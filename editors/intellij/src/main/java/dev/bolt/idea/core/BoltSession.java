package dev.bolt.idea.core;

import java.io.*;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.time.Duration;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.ReentrantLock;

/** One persistent native language server per IDEA project; tested independently of the IDE. */
public final class BoltSession implements AutoCloseable {
    public record Span(int start, int end, String kind) { }
    public record Problem(int start, int end, int severity, String code, String message) { }
    public record Analysis(String text, List<Span> spans, List<Problem> problems, Object symbols) {
        public Analysis(String text,List<Span> spans,List<Problem> problems){this(text,spans,problems,null);}
    }
    private static final class OpenDocument {
        final String text;
        final int version, utf8Bytes;
        private LspPositions positions;
        OpenDocument(String text, int version) {
            this.text = text;
            this.version = version;
            this.utf8Bytes = text.getBytes(StandardCharsets.UTF_8).length;
        }
        // Accessed under the session lock. Replaced along with the document on edits.
        LspPositions positions() {
            if (positions == null) positions = new LspPositions(text);
            return positions;
        }
    }
    private record Diagnostics(long version, List<?> items) { }
    private final ReentrantLock lock = new ReentrantLock();
    private final Map<String, OpenDocument> opened = new LinkedHashMap<>();
    private final Map<String, Diagnostics> diagnostics = new ConcurrentHashMap<>();
    private final Map<String, Analysis> cache = new HashMap<>();
    private RpcClient rpc;
    private Path executable, root;
    private List<String> legend = List.of();
    private long epoch = Long.MIN_VALUE;
    private boolean disposed;
    private String analysisProfile = "";

    public Analysis analyze(Path executable, Path workspace, Map<Path, String> documents, Path file,
                            long diskEpoch, Runnable cancellation) throws IOException {
        acquire(cancellation);
        try {
            String target = synchronize(executable, workspace, documents, file, diskEpoch, cancellation);
            Analysis cached = cache.get(target);
            if (cached != null) return cached;
            // The response is also an ordering barrier: this server publishes diagnostics
            // synchronously before answering the next semantic-token request.
            Object tokens = rpc.request("textDocument/semanticTokens/full",
                Json.object("textDocument", Json.object("uri", target)), Duration.ofSeconds(20), cancellation);
            OpenDocument document = opened.get(target);
            String text = document.text; LspPositions positions = document.positions();
            List<Span> spans = parseTokens(tokens, positions, text.length());
            List<Problem> problems = new ArrayList<>();
            Diagnostics current = diagnostics.get(target);
            if (current != null && (current.version() < 0 || current.version() == document.version)) {
                for (Object item : current.items()) {
                    Object range = Json.get(item, "range");
                    int start = positions.offset(Json.get(range, "start")), end = positions.offset(Json.get(range, "end"));
                    if (start < 0 || end < start) continue;
                    String code = Objects.toString(Json.get(item, "code"), "");
                    String message = Json.string(Json.get(item, "message"));
                    for (Object related : Json.list(Json.get(item, "relatedInformation"))) {
                        String detail = Json.string(Json.get(related, "message"));
                        Object location = Json.get(related, "location");
                        String where = Json.string(Json.get(location, "uri"));
                        long line = Json.integer(Json.get(Json.get(Json.get(location, "range"), "start"), "line"), -1);
                        if (!detail.isBlank()) message += "\n" + detail + (where.isEmpty() ? "" : " (" + where + ":" + (line + 1) + ")");
                    }
                    problems.add(new Problem(start, end, (int) Json.integer(Json.get(item, "severity"), 1), code, message));
                }
            }
            Object symbolIndex=rpc.request("textDocument/boltSymbolIndex",Json.object("textDocument",Json.object("uri",target)),Duration.ofSeconds(10),cancellation);
            Analysis result = new Analysis(text, List.copyOf(spans), List.copyOf(problems), symbolIndex);
            cache.put(target, result); return result;
        } finally { lock.unlock(); }
    }
    /** Readonly embedded source analysis uses the originating physical document's project. */
    public Analysis analyzeVirtual(Path executable,Path workspace,Map<Path,String> documents,Path context,
                                   long diskEpoch,String virtualUri,String expectedText,Runnable cancellation)throws IOException {
        if(!virtualUri.startsWith("bolt-stdlib:///"))throw new IOException("Not an embedded Bolt source URI");
        acquire(cancellation);
        try {
            String target=synchronize(executable,workspace,documents,context,diskEpoch,cancellation);
            String cacheKey=target+"\n"+virtualUri;
            Analysis cached=cache.get(cacheKey);if(cached!=null&&cached.text().equals(expectedText))return cached;
            Object document=rpc.request("bolt/virtualDocument",Json.object("uri",virtualUri,
                "textDocument",Json.object("uri",target)),Duration.ofSeconds(20),cancellation);
            String text=Json.string(Json.get(document,"text"));
            if(!text.equals(expectedText))throw new IOException("Embedded source changed; reopen its definition");
            var positions=new LspPositions(text);List<Span> spans=new ArrayList<>();
            for(var token:Json.list(Json.get(document,"tokens"))) {
                int line=(int)Json.integer(Json.get(token,"line"),-1),column=(int)Json.integer(Json.get(token,"start"),-1);
                long length=Json.integer(Json.get(token,"length"),-1);
                if(line<0||column<0||length<=0||column+length>Integer.MAX_VALUE)continue;
                int start=positions.offset(line,column),end=positions.offset(line,(int)(column+length));
                if(start>=0&&end>start&&end<=text.length())spans.add(new Span(start,end,Json.string(Json.get(token,"kind"))));
            }
            Analysis result=new Analysis(text,List.copyOf(spans),List.of(),Json.get(document,"symbolIndex"));
            cache.put(cacheKey,result);return result;
        }finally {lock.unlock();}
    }

    /** Called under lock by analysis and completion; keeps exactly the caller's unsaved snapshots. */
    private String synchronize(Path executable, Path workspace, Map<Path,String> documents, Path file,
                               long diskEpoch, Runnable cancellation) throws IOException {
            if (disposed) throw new IOException("Bolt project service has been disposed");
            ensureServer(executable.toRealPath(), workspace.toAbsolutePath().normalize(), cancellation);
            Map<String, OpenDocument> source = new LinkedHashMap<>();
            long sourceBytes = 0;
            for (var d : documents.entrySet()) {
                String uri = uri(d.getKey());
                OpenDocument previous = opened.get(uri);
                OpenDocument next = previous != null && previous.text.equals(d.getValue()) ? previous
                    : new OpenDocument(d.getValue(), previous == null ? 1 : previous.version + 1);
                int bytes = next.utf8Bytes;
                sourceBytes += bytes;
                if (sourceBytes > 32L * 1024 * 1024) throw new IOException("Open Bolt documents exceed the 32 MiB analysis budget.");
                if (bytes > 4 * 1024 * 1024)
                    throw new IOException("Bolt language-server documents are limited to 4 MiB.");
                source.put(uri, next);
            }
            if (source.size() > 256) throw new IOException("Bolt supports at most 256 simultaneously open documents.");
            String target = uri(file);
            if (!source.containsKey(target)) throw new IOException("Missing current document snapshot");
            boolean changed = false;
            for (String old : List.copyOf(opened.keySet())) if (!source.containsKey(old)) {
                cache.clear();
                rpc.notify("textDocument/didClose", Json.object("textDocument", Json.object("uri", old)));
                opened.remove(old); diagnostics.remove(old); changed = true;
            }
            for (var d : source.entrySet()) {
                cancellation.run(); OpenDocument previous = opened.get(d.getKey());
                OpenDocument next = d.getValue();
                if (previous == next) continue;
                cache.clear();
                if (previous == null) rpc.notify("textDocument/didOpen", Json.object("textDocument", Json.object(
                    "uri", d.getKey(), "languageId", "bolt", "version", next.version, "text", next.text)));
                else rpc.notify("textDocument/didChange", Json.object("textDocument", Json.object("uri", d.getKey(), "version", next.version),
                    "contentChanges", List.of(Json.object("text", next.text))));
                opened.put(d.getKey(), next); changed = true;
            }
            if (epoch != diskEpoch) {
                rpc.notify("workspace/didChangeWatchedFiles", Json.object("changes", List.of()));
                epoch = diskEpoch; changed = true;
            }
            if (changed) cache.clear();
            return target;
    }
    public List<BoltCompletion.Item> complete(Path executable, Path workspace, Map<Path,String> documents,
                                               Path file, long diskEpoch, int offset, Runnable cancellation) throws IOException {
        acquire(cancellation);
        try {
            String target = synchronize(executable, workspace, documents, file, diskEpoch, cancellation);
            OpenDocument document = opened.get(target);
            String text = document.text;
            if (offset < 0 || offset > text.length()) throw new IOException("Completion offset is outside the document");
            if (!BoltCompletion.isCode(text, offset)) return List.of();
            Object response = rpc.request("textDocument/completion", Json.object(
                "textDocument", Json.object("uri", target),
                "position", document.positions().position(offset),
                "context", Json.object("triggerKind", 1)), Duration.ofSeconds(10), cancellation);
            cancellation.run();
            return BoltCompletion.items(response);
        } finally { lock.unlock(); }
    }
    /** Version-coherent semantic request; never execute this on the IDE event thread. */
    public Object query(Path executable,Path workspace,Map<Path,String> documents,Path file,long epoch,
                        String method,int offset,Map<String,Object> extra,Runnable cancellation)throws IOException {
        acquire(cancellation);
        try {
            String target=synchronize(executable,workspace,documents,file,epoch,cancellation);
            OpenDocument document = opened.get(target);
            String text=document.text;
            if(offset<0||offset>text.length())throw new IOException("Offset outside source");
            Map<String,Object> params=new LinkedHashMap<>(extra);
            params.put("textDocument",Json.object("uri",target));params.put("position",document.positions().position(offset));
            Object result=rpc.request(method,params,Duration.ofSeconds(15),cancellation);cancellation.run();return result;
        } finally {lock.unlock();}
    }
    private List<Span> parseTokens(Object tokens, LspPositions positions, int length) throws IOException {
        List<?> data = Json.list(Json.get(tokens, "data"));
        if (data.size() % 5 != 0) throw new IOException("Malformed semantic-token response");
        List<Span> result = new ArrayList<>(); long line = 0, column = 0;
        for (int i = 0; i < data.size(); i += 5) {
            long dl = Json.integer(data.get(i), -1), dc = Json.integer(data.get(i + 1), -1);
            long size = Json.integer(data.get(i + 2), -1), kind = Json.integer(data.get(i + 3), -1);
            if (dl < 0 || dc < 0 || size <= 0) continue;
            line += dl; column = dl == 0 ? column + dc : dc;
            if (line > Integer.MAX_VALUE || column > Integer.MAX_VALUE || size > Integer.MAX_VALUE) continue;
            int start = positions.offset((int) line, (int) column);
            int end = column + size <= Integer.MAX_VALUE ? positions.offset((int) line, (int) (column + size)) : -1;
            if (start < 0 || end < start || end > length || kind < 0 || kind >= legend.size()) continue;
            result.add(new Span(start, end, legend.get((int) kind)));
        }
        return result;
    }
    private void ensureServer(Path requested, Path workspace, Runnable cancellation) throws IOException {
        if (rpc != null && rpc.isAlive() && requested.equals(executable) && workspace.equals(root)) return;
        reset(); executable = requested; root = workspace;
        if (!Files.isDirectory(workspace)) throw new IOException("Bolt workspace directory is missing: " + workspace);
        rpc = new RpcClient(List.of(requested.toString(), "lsp"), workspace, (method, params) -> {
            if (method.equals("textDocument/publishDiagnostics")) {
                String uri = canonicalUri(Json.string(Json.get(params, "uri")));
                diagnostics.put(uri, new Diagnostics(Json.integer(Json.get(params, "version"), -1), List.copyOf(Json.list(Json.get(params, "diagnostics")))));
            }
        });
        try {
            Object initialized = rpc.request("initialize", Json.object(
                "processId", ProcessHandle.current().pid(), "clientInfo", Json.object("name", "Bolt for IntelliJ", "version", "0.9.0"),
                "rootUri", uri(workspace), "workspaceFolders", List.of(Json.object("uri", uri(workspace), "name", workspace.getFileName() == null ? workspace.toString() : workspace.getFileName().toString())),
                "capabilities", Json.object("general", Json.object("positionEncodings", List.of("utf-16")),
                    "textDocument", Json.object("publishDiagnostics", Json.object("versionSupport", true),
                        "completion", Json.object("completionItem", Json.object("snippetSupport", false, "labelDetailsSupport", true)))),
                "initializationOptions", Json.object("boltSemanticNotifications", false, "boltProfile", analysisProfile)), Duration.ofSeconds(10), cancellation);
            Object capabilities = Json.get(initialized, "capabilities");
            String encoding = Json.string(Json.get(capabilities, "positionEncoding"));
            if (!encoding.isEmpty() && !encoding.equals("utf-16")) throw new IOException("Bolt server must use UTF-16 positions");
            legend = Json.list(Json.get(Json.get(Json.get(capabilities, "semanticTokensProvider"), "legend"), "tokenTypes"))
                .stream().map(Json::string).toList();
            if (legend.isEmpty()) throw new IOException("Selected compiler has no semantic-token LSP support");
            rpc.notify("initialized", Json.object());
        } catch (IOException | RuntimeException e) { reset(); throw e; }
    }
    private void acquire(Runnable cancellation) throws IOException {
        try { while (!lock.tryLock(50, TimeUnit.MILLISECONDS)) cancellation.run(); }
        catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new IOException("Bolt analysis interrupted", e); }
        try { cancellation.run(); } catch (RuntimeException e) { lock.unlock(); throw e; }
    }
    public void setProfile(String value) {
        String requested = value == null ? "" : value;
        if (!requested.isEmpty() && !requested.equals("Debug") && !requested.equals("Release"))
            throw new IllegalArgumentException("Bolt analysis profile must be Debug, Release or project default");
        lock.lock();
        try { if (!requested.equals(analysisProfile)) { reset(); analysisProfile = requested; } }
        finally { lock.unlock(); }
    }
    public long processId() { lock.lock(); try { return rpc == null ? -1 : rpc.processId(); } finally { lock.unlock(); } }
    public void restart() { lock.lock(); try { reset(); } finally { lock.unlock(); } }
    private void reset() {
        if (rpc != null) rpc.close(); rpc = null; opened.clear(); diagnostics.clear(); cache.clear();
        epoch = Long.MIN_VALUE; legend = List.of();
    }
    @Override public void close() { lock.lock(); try { disposed = true; reset(); } finally { lock.unlock(); } }
    public static String uri(Path path) {
        Path resolved = path.toAbsolutePath().normalize();
        try { if (Files.exists(resolved)) resolved = resolved.toRealPath(); } catch (IOException ignored) { }
        String s = resolved.toUri().toASCIIString();
        // Both file:/path and file:///path are legal URIs. Bolt expects file://.
        if (s.startsWith("file:/") && !s.startsWith("file://")) s = "file://" + s.substring(5);
        return canonicalUri(s);
    }
    private static String canonicalUri(String uri) {
        try {
            URI parsed = URI.create(uri);
            Path p = Path.of(parsed).toAbsolutePath().normalize();
            String s = p.toUri().toASCIIString();
            if (s.startsWith("file:/") && !s.startsWith("file://")) s = "file://" + s.substring(5);
            // The server normalizes Windows drive letters through filesystem paths.
            if (System.getProperty("os.name", "").toLowerCase(Locale.ROOT).contains("win")) s = s.toLowerCase(Locale.ROOT);
            return s;
        } catch (IllegalArgumentException e) { return uri; }
    }
}
