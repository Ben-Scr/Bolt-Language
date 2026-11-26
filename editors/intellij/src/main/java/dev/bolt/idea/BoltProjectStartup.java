package dev.bolt.idea;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.startup.StartupActivity;
public final class BoltProjectStartup implements StartupActivity.DumbAware {
    @Override public void runActivity(Project project){
        BoltProjectService.getInstance(project); // Register watched-file refreshes.
        BoltProjectModel.getInstance(project).refresh();
    }
}
