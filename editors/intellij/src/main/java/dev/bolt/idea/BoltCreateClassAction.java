package dev.bolt.idea;

import com.intellij.ide.actions.CreateFileFromTemplateAction;
import com.intellij.ide.actions.CreateFileFromTemplateDialog;
import com.intellij.openapi.project.DumbAware;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.ui.InputValidatorEx;
import com.intellij.psi.PsiDirectory;
import com.intellij.psi.PsiFile;
import java.util.Set;

/** Creates a Bolt source file using the platform's template, undo and editor support. */
public final class BoltCreateClassAction extends CreateFileFromTemplateAction implements DumbAware {
    // Match Parser::identifier(); syntax highlighting also colors contextual keywords.
    private static final Set<String> RESERVED_NAMES = Set.of(
        "event", "implicit", "explicit", "class", "struct", "abstract", "async", "await", "try", "catch",
        "finally", "throw", "require", "extern", "char", "interface", "operator", "enum", "concept",
        "namespace", "import", "public", "private", "protected", "static", "const", "var", "sealed", "virtual", "override",
        "if", "else", "elif", "while", "for", "return", "break", "continue", "true", "false", "this", "null",
        "default", "sizeof", "new", "delete", "void", "byte", "short", "ushort", "int", "uint", "long", "ulong",
        "float", "double", "bool", "string");

    public BoltCreateClassAction() {
        super("Bolt Class", "Create a new Bolt class", BoltFileType.INSTANCE.getIcon());
    }

    @Override protected void buildDialog(Project project, PsiDirectory directory,
                                         CreateFileFromTemplateDialog.Builder builder) {
        builder.setTitle("New Bolt Class")
            .addKind("Class", BoltFileType.INSTANCE.getIcon(), "Bolt Class")
            .setValidator(new InputValidatorEx() {
                @Override public boolean checkInput(String input) { return getErrorText(input) == null; }
                @Override public boolean canClose(String input) { return checkInput(input); }
                @Override public String getErrorText(String input) {
                    String name = className(input);
                    if (!name.matches("[A-Za-z_][A-Za-z0-9_]*"))
                        return "Enter a class name, e.g. Player (optionally ending in .bolt).";
                    if (RESERVED_NAMES.contains(name))
                        return "A Bolt keyword cannot be used as a class name.";
                    return null;
                }
            });
    }

    @Override protected PsiFile createFile(String name, String templateName, PsiDirectory directory) {
        // Pass only the identifier as NAME, even when a file extension was entered.
        return super.createFile(className(name), templateName, directory);
    }

    @Override protected String getActionName(PsiDirectory directory, String newName, String templateName) {
        return "Create Bolt Class: " + className(newName);
    }

    private static String className(String input) {
        String name = input.trim();
        return name.endsWith(".bolt") ? name.substring(0, name.length() - 5) : name;
    }
}
