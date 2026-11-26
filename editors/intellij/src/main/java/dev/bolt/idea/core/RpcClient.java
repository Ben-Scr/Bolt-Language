package dev.bolt.idea.core;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.function.BiConsumer;

/** Bounded stdio JSON-RPC transport. No shell, sockets, downloads or global processes. */
public final class RpcClient implements AutoCloseable {
    private static final int MAX_FRAME = 8 * 1024 * 1024;
    private final Process process;
    private final OutputStream input;
    private final Map<Long, CompletableFuture<Object>> pending = new ConcurrentHashMap<>();
    private final AtomicLong ids = new AtomicLong();
    private final AtomicBoolean stopped = new AtomicBoolean();
    private final Object writeLock = new Object();
    private final BiConsumer<String, Object> notification;
    private final StringBuilder stderr = new StringBuilder();
    private final Thread reader;
    private final Thread errors;

    public RpcClient(List<String> command, Path directory, BiConsumer<String, Object> notification) throws IOException {
        this.notification = notification;
        ProcessBuilder builder = new ProcessBuilder(command);
        builder.directory(directory.toFile());
        process = builder.start(); input = process.getOutputStream();
        reader = daemon("Bolt LSP stdout", this::readLoop);
        errors = daemon("Bolt LSP stderr", () -> {
            try (Reader r = new InputStreamReader(process.getErrorStream(), StandardCharsets.UTF_8)) {
                char[] buffer = new char[1024]; int n;
                while ((n = r.read(buffer)) >= 0) synchronized (stderr) {
                    stderr.append(buffer, 0, n);
                    if (stderr.length() > 8192) stderr.delete(0, stderr.length() - 8192);
                }
            } catch (IOException ignored) { /* Process teardown closes the pipe. */ }
        });
    }
    private static Thread daemon(String name, Runnable task) {
        Thread t = new Thread(task, name); t.setDaemon(true); t.start(); return t;
    }
    public boolean isAlive() { return !stopped.get() && process.isAlive(); }
    public long processId() { return process.pid(); }
    public String stderrTail() { synchronized (stderr) { return stderr.toString(); } }
    public void notify(String method, Object params) throws IOException {
        send(Json.object("jsonrpc", "2.0", "method", method, "params", params));
    }
    public Object request(String method, Object params, Duration timeout, Runnable cancellation) throws IOException {
        if (pending.size() >= 128) throw new IOException("Too many outstanding Bolt requests");
        long id = ids.incrementAndGet(); CompletableFuture<Object> reply = new CompletableFuture<>();
        pending.put(id, reply);
        try {
            send(Json.object("jsonrpc", "2.0", "id", id, "method", method, "params", params));
            long deadline = System.nanoTime() + timeout.toNanos();
            while (true) {
                cancellation.run();
                long remaining = deadline - System.nanoTime();
                if (remaining <= 0) throw new IOException("Bolt language-server request timed out: " + method);
                try { return reply.get(Math.min(remaining, TimeUnit.MILLISECONDS.toNanos(50)), TimeUnit.NANOSECONDS); }
                catch (TimeoutException ignored) { /* Poll cancellation without holding an IDE read lock. */ }
                catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new IOException("Bolt request interrupted", e); }
                catch (ExecutionException e) {
                    String detail = stderrTail().strip();
                    throw new IOException(e.getCause().getMessage() + (detail.isEmpty() ? "" : "\nCompiler stderr: " + detail), e.getCause());
                }
            }
        } finally {
            pending.remove(id);
            if (!reply.isDone()) try { notify("$/cancelRequest", Json.object("id", id)); } catch (IOException ignored) { }
        }
    }
    private void send(Object message) throws IOException {
        byte[] body = Json.write(message).getBytes(StandardCharsets.UTF_8);
        if (body.length > MAX_FRAME) throw new IOException("Bolt LSP message exceeds 8 MiB");
        byte[] header = ("Content-Length: " + body.length + "\r\n\r\n").getBytes(StandardCharsets.US_ASCII);
        synchronized (writeLock) {
            if (stopped.get()) throw new IOException("Bolt language server is stopped");
            input.write(header); input.write(body); input.flush();
        }
    }
    private void readLoop() {
        try (InputStream out = new BufferedInputStream(process.getInputStream())) {
            while (true) {
                Object message = readMessage(out);
                if (message == null) throw new EOFException("Bolt language server closed stdout");
                if (!"2.0".equals(Json.get(message, "jsonrpc"))) throw new IOException("Invalid JSON-RPC version");
                Object id = Json.get(message, "id");
                String method = Json.string(Json.get(message, "method"));
                if (!method.isEmpty()) {
                    if (id != null) send(Json.object("jsonrpc", "2.0", "id", id,
                        "error", Json.object("code", -32601, "message", "Client method not supported: " + method)));
                    else notification.accept(method, Json.get(message, "params"));
                } else if (id instanceof Number n) {
                    CompletableFuture<Object> result = pending.get(n.longValue());
                    if (result != null) {
                        Object error = Json.get(message, "error");
                        if (error != null) result.completeExceptionally(new IOException(Json.string(Json.get(error, "message"))));
                        else result.complete(Json.get(message, "result"));
                    }
                }
            }
        } catch (Exception e) {
            stopped.set(true);
            for (CompletableFuture<Object> f : pending.values()) f.completeExceptionally(e);
            if (process.isAlive()) process.destroy();
        }
    }
    public static Object readMessage(InputStream input) throws IOException {
        int length = -1, headerBytes = 0;
        while (true) {
            ByteArrayOutputStream line = new ByteArrayOutputStream();
            int ch;
            while ((ch = input.read()) != -1 && ch != '\n') {
                if (++headerBytes > 8192) throw new IOException("LSP header exceeds 8 KiB");
                line.write(ch);
            }
            if (ch == -1) {
                if (headerBytes == 0 && length == -1) return null;
                throw new EOFException("Truncated LSP header");
            }
            if (++headerBytes > 8192) throw new IOException("LSP header exceeds 8 KiB");
            String text = line.toString(StandardCharsets.US_ASCII).trim();
            if (text.isEmpty()) break;
            int colon = text.indexOf(':');
            if (colon >= 0 && text.substring(0, colon).trim().equalsIgnoreCase("Content-Length")) {
                if (length >= 0) throw new IOException("Duplicate Content-Length");
                try { length = Integer.parseInt(text.substring(colon + 1).trim()); }
                catch (NumberFormatException e) { throw new IOException("Invalid Content-Length", e); }
                if (length < 0 || length > MAX_FRAME) throw new IOException("LSP frame exceeds limit");
            }
        }
        if (length < 0) throw new IOException("Missing Content-Length");
        byte[] body = input.readNBytes(length);
        if (body.length != length) throw new EOFException("Truncated LSP body");
        try { return Json.parse(new String(body, StandardCharsets.UTF_8)); }
        catch (IllegalArgumentException e) { throw new IOException("Malformed LSP JSON: " + e.getMessage(), e); }
    }
    @Override public void close() {
        if (!stopped.get() && process.isAlive()) {
            try {
                request("shutdown", null, Duration.ofMillis(500), () -> { });
                notify("exit", null);
            } catch (IOException ignored) { }
        }
        stopped.set(true);
        try { input.close(); } catch (IOException ignored) { }
        try {
            if (!process.waitFor(200, TimeUnit.MILLISECONDS)) process.destroy();
            if (!process.waitFor(300, TimeUnit.MILLISECONDS)) process.destroyForcibly();
        } catch (InterruptedException e) { Thread.currentThread().interrupt(); process.destroyForcibly(); }
        for (var f : pending.values()) f.completeExceptionally(new IOException("Bolt language server stopped"));
        pending.clear();
    }
}
