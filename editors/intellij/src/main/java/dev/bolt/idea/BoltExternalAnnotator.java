package dev.bolt.idea;

import com.intellij.lang.annotation.*;
import com.intellij.openapi.editor.Document;
import com.intellij.openapi.editor.Editor;
import com.intellij.openapi.editor.colors.TextAttributesKey;
import com.intellij.openapi.editor.colors.EditorColorsManager;
import com.intellij.openapi.editor.markup.TextAttributes;
import com.intellij.openapi.editor.markup.EffectType;
import com.intellij.openapi.editor.colors.CodeInsightColors;
import com.intellij.openapi.fileEditor.FileDocumentManager;
import com.intellij.openapi.fileEditor.FileEditorManager;
import com.intellij.openapi.project.DumbAware;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.util.TextRange;
import com.intellij.openapi.util.Key;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.psi.PsiDocumentManager;
import com.intellij.psi.PsiFile;
import dev.bolt.idea.core.*;
import java.io.IOException;
import java.nio.file.Path;
import java.util.*;

/** Native analysis runs outside the IDE read lock; results never target a stale document. */
public final class BoltExternalAnnotator extends ExternalAnnotator<BoltExternalAnnotator.Input, BoltExternalAnnotator.Output> implements DumbAware {
    public record Input(Project project, Path workspace, Path path, long stamp, String text,
                        Map<Path,String> documents, long diskEpoch, long generation, BoltSettings.Options options, boolean trusted, String virtualUri) { }
    public record Output(Input input, BoltSession.Analysis analysis, String failure) { }
    private record DocumentText(long stamp, String text) { }
    private static final Key<DocumentText> SNAPSHOT = Key.create("bolt.document.text");
    private static String snapshotText(Document document) {
        long stamp = document.getModificationStamp();
        var cached = document.getUserData(SNAPSHOT);
        if (cached != null && cached.stamp() == stamp) return cached.text();
        String text = document.getText();
        document.putUserData(SNAPSHOT, new DocumentText(stamp, text));
        return text;
    }
    @Override public Input collectInformation(PsiFile file) { return collect(file); }
    @Override public Input collectInformation(PsiFile file, Editor editor, boolean hasErrors) { return collect(file); }
    static Input collect(PsiFile file) {
        Project project = file.getProject(); VirtualFile virtual = file.getVirtualFile();
        if (project.isDisposed() || virtual == null) return null;
        if(!virtual.isInLocalFileSystem()) {
            var source=virtual.getUserData(BoltNavigation.VIRTUAL_SOURCE);if(source==null)return null;
            var origin=BoltNavigation.targetFile(project,BoltSession.uri(source.context()),Map.of());
            var context=origin==null?null:collect(origin);if(context==null)return null;
            var document=PsiDocumentManager.getInstance(project).getDocument(file);if(document==null)return null;
            return new Input(project,context.workspace(),context.path(),document.getModificationStamp(),snapshotText(document),
                context.documents(),context.diskEpoch(),context.generation(),context.options(),context.trusted(),source.uri());
        }
        BoltSettings.Options options = BoltSettings.getInstance().snapshot();
        if (!options.diagnostics() && !options.semanticColors() && !options.completion()) return null;
        Document document = PsiDocumentManager.getInstance(project).getDocument(file);
        if (document == null) return null;
        Path path = Path.of(virtual.getPath()).toAbsolutePath().normalize();
        Path workspace = project.getBasePath() == null ? path.getParent() : Path.of(project.getBasePath());
        String text = snapshotText(document);
        Map<Path,String> sources = new LinkedHashMap<>(); sources.put(path, text);
        FileDocumentManager manager = FileDocumentManager.getInstance();
        // Include other open files and unsaved documents. The server loads the rest
        // from the project folder; unsaved edits must never be replaced by disk text.
        for (VirtualFile open : FileEditorManager.getInstance(project).getOpenFiles()) {
            if (open.isInLocalFileSystem() && "bolt".equalsIgnoreCase(open.getExtension())) {
                Document d = manager.getCachedDocument(open);
                if (d != null && d != document) sources.put(Path.of(open.getPath()).toAbsolutePath().normalize(), snapshotText(d));
            }
        }
        for (Document d : manager.getUnsavedDocuments()) {
            VirtualFile vf = manager.getFile(d);
            if (vf == null || !vf.isInLocalFileSystem() || !"bolt".equalsIgnoreCase(vf.getExtension())) continue;
            Path p = Path.of(vf.getPath()).toAbsolutePath().normalize();
            if (p.startsWith(workspace)) sources.putIfAbsent(p, snapshotText(d));
        }
        return new Input(project, workspace, path, document.getModificationStamp(), text,
            Collections.unmodifiableMap(sources), BoltProjectService.getInstance(project).epoch(),
            BoltProjectService.getInstance(project).generation(), options, BoltTrust.isTrusted(project),"");
    }
    @Override public Output doAnnotate(Input input) {
        if (input == null || input.project().isDisposed()) return null;
        if (!input.trusted()) return new Output(input, null,
            "Bolt analysis is disabled in Safe Mode. Trust this project, then use Tools > Restart Bolt Language Server. Syntax colors remain available.");
        try { return new Output(input, BoltProjectService.getInstance(input.project()).analyze(input), null); }
        catch (IOException | java.nio.file.InvalidPathException e) { return new Output(input, null,
            "Bolt language server: " + Objects.toString(e.getMessage(), "I/O error") +
            "\nConfigure the native executable in Settings > Tools > Bolt. Syntax colors remain available."); }
        // ProcessCanceledException must propagate to IDEA rather than becoming a fake compiler error.
    }
    @Override public void apply(PsiFile file, Output output, AnnotationHolder holder) {
        if (output == null || !file.isValid()) return;
        Document doc = PsiDocumentManager.getInstance(file.getProject()).getDocument(file);
        Input input = output.input();
        if (doc == null || doc.getModificationStamp() != input.stamp() || !snapshotText(doc).equals(input.text())
            || BoltProjectService.getInstance(file.getProject()).generation() != input.generation()) return;
        int length = input.text().length();
        if (output.failure() != null) {
            holder.newAnnotation(HighlightSeverity.WEAK_WARNING, output.failure())
                .range(range(0, 1, length)).create(); return;
        }
        var analysis = output.analysis();
        boolean ordered = true;
        int lastEnd = 0;
        if (input.options().semanticColors()) for (var span : analysis.spans()) {
            if (span.start() < lastEnd || span.end() < span.start()) ordered = false;
            lastEnd = span.end();
            if (span.end() <= span.start() || span.end() > length) continue;
            TextAttributesKey key = BoltColors.semantic(span.kind(), input.text(), span.start(), span.end());
            if (key != null) holder.newSilentAnnotation(HighlightSeverity.INFORMATION)
                .range(new TextRange(span.start(), span.end())).textAttributes(key).create();
        }
        if (input.options().diagnostics()) for (var problem : analysis.problems()) {
            HighlightSeverity severity = switch (problem.severity()) {
                case 2 -> HighlightSeverity.WARNING;
                case 3 -> HighlightSeverity.WEAK_WARNING;
                case 4 -> HighlightSeverity.INFORMATION;
                default -> HighlightSeverity.ERROR;
            };
            String message = (problem.code().isBlank() ? "" : problem.code() + ": ") + problem.message();
            var diagnostic = holder.newAnnotation(severity, message).range(range(problem.start(), problem.end(), length));
            if (input.options().semanticColors()) for (int i = ordered ? firstEndingAfter(analysis.spans(), problem.start()) : 0;
                                                       i < analysis.spans().size(); i++) {
                var span = analysis.spans().get(i);
                if (ordered && span.start() > problem.start()) break;
                if (span.start() > problem.start() || span.end() <= problem.start()) continue;
                if (span.end() > length || span.end() <= span.start()) continue;
                var key = BoltColors.semantic(span.kind(), input.text(), span.start(), span.end());
                if (key == null) continue;
                var scheme = EditorColorsManager.getInstance().getGlobalScheme();
                TextAttributes normal = scheme.getAttributes(key);
                TextAttributes error = scheme.getAttributes(severity == HighlightSeverity.ERROR ? CodeInsightColors.ERRORS_ATTRIBUTES : CodeInsightColors.WARNINGS_ATTRIBUTES);
                if (normal != null && error != null) {
                    TextAttributes merged = normal.clone();
                    merged.setEffectType(EffectType.WAVE_UNDERSCORE);
                    merged.setEffectColor(error.getEffectColor() != null ? error.getEffectColor() : error.getForegroundColor());
                    diagnostic.enforcedTextAttributes(merged);
                }
                break;
            }
            diagnostic.create();
        }
    }
    private static int firstEndingAfter(List<BoltSession.Span> spans, int offset) {
        int lo = 0, hi = spans.size();
        while (lo < hi) {
            int mid = (lo + hi) >>> 1;
            if (spans.get(mid).end() <= offset) lo = mid + 1; else hi = mid;
        }
        return lo;
    }
    private static TextRange range(int start, int end, int length) {
        start = Math.max(0, Math.min(start, length)); end = Math.max(start, Math.min(end, length));
        if (end == start && length > 0) { if (start == length) start--; else end++; }
        return new TextRange(start, end);
    }
}
