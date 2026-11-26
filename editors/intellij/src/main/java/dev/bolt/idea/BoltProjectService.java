package dev.bolt.idea;

import com.intellij.codeInsight.daemon.DaemonCodeAnalyzer;
import com.intellij.openapi.Disposable;
import com.intellij.openapi.application.ApplicationManager;
import com.intellij.openapi.components.Service;
import com.intellij.openapi.editor.EditorFactory;
import com.intellij.openapi.editor.event.DocumentEvent;
import com.intellij.openapi.editor.event.DocumentListener;
import com.intellij.openapi.fileEditor.FileDocumentManager;
import com.intellij.openapi.fileEditor.FileEditorManagerListener;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.progress.ProgressManager;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.openapi.vfs.VirtualFileManager;
import com.intellij.openapi.vfs.newvfs.BulkFileListener;
import com.intellij.openapi.vfs.newvfs.events.VFileEvent;
import com.intellij.util.Alarm;
import dev.bolt.idea.core.*;
import java.io.IOException;
import java.nio.file.Path;
import java.util.*;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CancellationException;

/** Owns exactly one child process for a project, not one process per file/keystroke. */
@Service(Service.Level.PROJECT)
public final class BoltProjectService implements Disposable {
    private final Project project;
    private final BoltSession session = new BoltSession();
    private final AtomicLong diskEpoch = new AtomicLong();
    private final AtomicLong generation = new AtomicLong();
    private final Alarm refresh = new Alarm(Alarm.ThreadToUse.SWING_THREAD, this);
    private volatile boolean disposed;
    private volatile long retryAfter;
    private volatile String previousFailure = "";
    private final java.util.concurrent.ConcurrentHashMap<Path,SymbolSnapshot> symbolSnapshots=new java.util.concurrent.ConcurrentHashMap<>();
    private final java.util.concurrent.ConcurrentHashMap<BoltNavigation.VirtualSource,SymbolSnapshot> virtualSnapshots=new java.util.concurrent.ConcurrentHashMap<>();
    private record SymbolSnapshot(long generation, BoltSession.Analysis analysis, BoltSymbols symbols) {
        static SymbolSnapshot update(SymbolSnapshot previous, long generation, BoltSession.Analysis analysis) {
            if (previous != null && previous.generation == generation && previous.analysis == analysis) return previous;
            return new SymbolSnapshot(generation, analysis, new BoltSymbols(analysis));
        }
    }
    public BoltSymbols symbols(Path path,String text) {
        var value=symbolSnapshots.get(path.toAbsolutePath().normalize());return value!=null&&value.generation()==generation()&&value.analysis().text().equals(text)?value.symbols():null;
    }
    public BoltSymbols symbols(VirtualFile file,String text) {
        if(file.isInLocalFileSystem())return symbols(Path.of(file.getPath()),text);
        var source=file.getUserData(BoltNavigation.VIRTUAL_SOURCE);var value=source==null?null:virtualSnapshots.get(source);
        return value!=null&&value.generation()==generation()&&value.analysis().text().equals(text)?value.symbols():null;
    }
    private record QueryKey(Path file,long stamp,long generation,long epoch,String method,int offset,String extra){}
    private final Map<QueryKey,CompletableFuture<Object>> queries=new LinkedHashMap<>();
    public synchronized CompletableFuture<Object> query(BoltExternalAnnotator.Input input,String method,int offset,Map<String,Object> extra) {
        if(!input.virtualUri().isEmpty())return CompletableFuture.completedFuture(null);
        QueryKey key=new QueryKey(input.path(),input.stamp(),input.generation(),input.diskEpoch(),method,offset,Json.write(extra));
        var existing=queries.get(key);if(existing!=null)return existing;
        if(queries.size()>48)queries.clear();
        var future=new CompletableFuture<Object>();queries.put(key,future);
        ApplicationManager.getApplication().executeOnPooledThread(()->{
            Runnable cancel=()->{if(disposed||project.isDisposed()||generation()!=input.generation())throw new CancellationException();};
            try {cancel.run();if(!input.trusted()){future.complete(null);return;}
                var executable=CompilerLocator.locate(input.options().executable());session.setProfile(input.options().analysisProfile());
                var result=session.query(executable,input.workspace(),input.documents(),input.path(),input.diskEpoch(),method,offset,extra,cancel);
                cancel.run();future.complete(result);
            }catch(CancellationException e){future.cancel(false);}catch(Exception e){future.completeExceptionally(e);}
        });return future;
    }
    private record CompletionKey(Path file, long stamp, long generation, long epoch, int offset, String executable) { }
    private CompletionKey completionKey;
    private CompletableFuture<List<BoltCompletion.Item>> completionFuture;

    /** I/O always runs on a pooled thread. At most one current snapshot's result is retained. */
    public synchronized CompletableFuture<List<BoltCompletion.Item>> complete(BoltExternalAnnotator.Input input, int offset) {
        if(!input.virtualUri().isEmpty())return CompletableFuture.completedFuture(List.of());
        CompletionKey key = new CompletionKey(input.path(), input.stamp(), input.generation(), input.diskEpoch(), offset, input.options().executable());
        if (key.equals(completionKey) && completionFuture != null) return completionFuture;
        completionKey = key;
        var future = new CompletableFuture<List<BoltCompletion.Item>>(); completionFuture = future;
        ApplicationManager.getApplication().executeOnPooledThread(() -> {
            Runnable cancellation = () -> {
                if (disposed || project.isDisposed() || generation() != input.generation()) throw new CancellationException();
            };
            try {
                cancellation.run();
                if (!input.trusted() || !input.options().completion()) { future.complete(List.of()); return; }
                if (System.nanoTime() < retryAfter) throw new IOException(previousFailure);
                Path compiler = CompilerLocator.locate(input.options().executable());
            session.setProfile(input.options().analysisProfile());
                var items = session.complete(compiler, input.workspace(), input.documents(), input.path(), input.diskEpoch(), offset, cancellation);
                cancellation.run(); previousFailure = ""; retryAfter = 0;
                future.complete(items);
            } catch (CancellationException error) { future.cancel(false); }
            catch (IOException | RuntimeException error) {
                if (error instanceof IOException) {
                    previousFailure = Objects.toString(error.getMessage(), "I/O error");
                    retryAfter = System.nanoTime() + 2_000_000_000L;
                }
                future.completeExceptionally(error);
            }
        });
        return future;
    }

    public BoltProjectService(Project project) {
        this.project = project;
        ApplicationManager.getApplication().getMessageBus().connect(this)
            .subscribe(VirtualFileManager.VFS_CHANGES, new BulkFileListener() {
                @Override public void after(List<? extends VFileEvent> events) {
                    if (events.stream().anyMatch(e -> e.getPath().endsWith(".bolt") || e.getPath().endsWith("bolt.project") || e.getPath().endsWith("bolt.proj") || e.getPath().endsWith("bolt.conf") ||
                        (e.getFile() != null && e.getFile().isDirectory()))) {
                        diskEpoch.incrementAndGet(); generation.incrementAndGet(); scheduleRefresh();
                        BoltProjectModel.getInstance(project).refresh();
                    }
                }
            });
        EditorFactory.getInstance().getEventMulticaster().addDocumentListener(new DocumentListener() {
            @Override public void documentChanged(DocumentEvent event) {
                VirtualFile file = FileDocumentManager.getInstance().getFile(event.getDocument());
                if (file != null && "bolt".equalsIgnoreCase(file.getExtension())) { generation.incrementAndGet(); scheduleRefresh(); }
            }
        }, this);
        project.getMessageBus().connect(this).subscribe(FileEditorManagerListener.FILE_EDITOR_MANAGER,
            new FileEditorManagerListener() {
                @Override public void fileClosed(com.intellij.openapi.fileEditor.FileEditorManager manager, VirtualFile file) {
                    if ("bolt".equalsIgnoreCase(file.getExtension())) {
                        if(file.isInLocalFileSystem())symbolSnapshots.remove(Path.of(file.getPath()).toAbsolutePath().normalize());
                        else {var source=file.getUserData(BoltNavigation.VIRTUAL_SOURCE);if(source!=null)virtualSnapshots.remove(source);}
                        generation.incrementAndGet(); scheduleRefresh();
                    }
                }
            });
    }
    public static BoltProjectService getInstance(Project project) { return project.getService(BoltProjectService.class); }
    public long epoch() { return diskEpoch.get(); }
    public long generation() { return generation.get(); }
    private void scheduleRefresh() {
        if (disposed || project.isDisposed()) return;
        refresh.cancelAllRequests();
        refresh.addRequest(() -> {
            if (!disposed && !project.isDisposed()) DaemonCodeAnalyzer.getInstance(project).restart();
        }, 350);
    }
    public BoltSession.Analysis analyze(BoltExternalAnnotator.Input input) throws IOException {
        if (System.nanoTime() < retryAfter) throw new IOException(previousFailure);
        try {
            Path compiler = CompilerLocator.locate(input.options().executable());
            session.setProfile(input.options().analysisProfile());
            Runnable cancellation=() -> {
                ProgressManager.checkCanceled();
                if (disposed || project.isDisposed()) throw new com.intellij.openapi.progress.ProcessCanceledException();
            };
            var result = input.virtualUri().isEmpty()
                ?session.analyze(compiler,input.workspace(),input.documents(),input.path(),input.diskEpoch(),cancellation)
                :session.analyzeVirtual(compiler,input.workspace(),input.documents(),input.path(),input.diskEpoch(),input.virtualUri(),input.text(),cancellation);
            if(input.generation()==generation()) {
                if (input.virtualUri().isEmpty())
                    symbolSnapshots.compute(input.path().toAbsolutePath().normalize(), (key, previous) ->
                        SymbolSnapshot.update(previous, input.generation(), result));
                else
                    virtualSnapshots.compute(new BoltNavigation.VirtualSource(input.virtualUri(), input.path()), (key, previous) ->
                        SymbolSnapshot.update(previous, input.generation(), result));
            }
            previousFailure = ""; retryAfter = 0; return result;
        } catch (IOException e) {
            previousFailure = Objects.toString(e.getMessage(), e.getClass().getSimpleName());
            retryAfter = System.nanoTime() + 2_000_000_000L; throw e;
        }
    }
    public synchronized void restart() {
        completionKey = null; completionFuture = null;queries.clear();symbolSnapshots.clear();virtualSnapshots.clear();
        retryAfter = 0; previousFailure = ""; diskEpoch.incrementAndGet(); generation.incrementAndGet();
        ApplicationManager.getApplication().executeOnPooledThread(() -> {
            session.restart(); scheduleRefresh();BoltProjectModel.getInstance(project).refresh();
        });
    }
    @Override public void dispose() {
        disposed = true;
        // Never block the UI while a compiler finishes or shuts down.
        ApplicationManager.getApplication().executeOnPooledThread(session::close);
    }
}
