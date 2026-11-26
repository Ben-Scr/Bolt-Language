package dev.bolt.idea;

import com.intellij.ide.util.projectWizard.ModuleBuilder;
import com.intellij.ide.util.projectWizard.WizardContext;
import com.intellij.ide.wizard.AbstractNewProjectWizardStep;
import com.intellij.ide.wizard.NewProjectWizardBaseData;
import com.intellij.ide.wizard.NewProjectWizardStep;
import com.intellij.ide.wizard.UIWizardUtil;
import com.intellij.ide.wizard.language.LanguageGeneratorNewProjectWizard;
import com.intellij.openapi.module.GeneralModuleType;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.util.IconLoader;
import com.intellij.openapi.vfs.LocalFileSystem;
import dev.bolt.idea.core.Json;
import org.jetbrains.annotations.NotNull;

import javax.swing.Icon;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;

public final class BoltNewProjectWizard implements LanguageGeneratorNewProjectWizard {
    @Override public @NotNull String getName() { return "Bolt"; }
    @Override public @NotNull Icon getIcon() { return IconLoader.getIcon("/icons/bolt-logo.png", getClass()); }
    @Override public int getOrdinal() { return 300; }
    @Override public boolean isEnabled(@NotNull WizardContext context) { return context.isCreatingNewProject(); }

    @Override
    public @NotNull NewProjectWizardStep createStep(@NotNull NewProjectWizardStep parent) {
        return new AbstractNewProjectWizardStep(parent) {
            @Override
            public void setupProject(@NotNull Project project) {
                NewProjectWizardBaseData settings = NewProjectWizardBaseData.getBaseData(this);
                Path root = Path.of(settings.getContentEntryPath());
                String name = settings.getName();
                try {
                    createStarterFiles(root, name);
                } catch (IOException e) {
                    throw new UncheckedIOException("Unable to create the Bolt project in " + root, e);
                }

                LocalFileSystem.getInstance().refreshAndFindFileByNioFile(root.resolve("src/Main.bolt"));
                ModuleBuilder module = GeneralModuleType.INSTANCE.createModuleBuilder();
                module.setName(name);
                module.setContentEntryPath(root.toString());
                module.setModuleFilePath(root.resolve(".idea").resolve(name + ".iml").toString());
                UIWizardUtil.setupProjectFromBuilder(this, project, module);
            }
        };
    }

    private static void createStarterFiles(Path root, String name) throws IOException {
        Files.createDirectories(root.resolve("src"));
        writeIfMissing(root.resolve("bolt.proj"), "format = 1\n\n[project]\nname = " + Json.write(name) + "\n");
        writeIfMissing(root.resolve("src/Main.bolt"), """
            public static class Program
            {
            public:
                void Main()
                {
                    Print("Hello from Bolt!");
                }
            }
            """);
        writeIfMissing(root.resolve(".gitignore"), "/bin/\n/obj/\n/.bolt/\n/.idea/\n");
    }

    private static void writeIfMissing(Path file, String text) throws IOException {
        try {
            Files.writeString(file, text, StandardOpenOption.CREATE_NEW);
        } catch (FileAlreadyExistsException e) {
            if (!Files.isRegularFile(file)) throw e;
        }
    }
}
