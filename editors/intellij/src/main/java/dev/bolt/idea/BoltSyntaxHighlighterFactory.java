package dev.bolt.idea;
import com.intellij.openapi.fileTypes.*;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.VirtualFile;
public final class BoltSyntaxHighlighterFactory extends SyntaxHighlighterFactory {
    @Override public SyntaxHighlighter getSyntaxHighlighter(Project project, VirtualFile file) { return new BoltSyntaxHighlighter(); }
}
