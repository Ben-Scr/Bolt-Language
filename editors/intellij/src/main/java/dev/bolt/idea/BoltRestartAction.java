package dev.bolt.idea;
import com.intellij.openapi.actionSystem.*;
import com.intellij.openapi.project.DumbAwareAction;
public final class BoltRestartAction extends DumbAwareAction {
    @Override public ActionUpdateThread getActionUpdateThread() { return ActionUpdateThread.BGT; }
    @Override public void update(AnActionEvent event) { event.getPresentation().setEnabledAndVisible(event.getProject() != null); }
    @Override public void actionPerformed(AnActionEvent event) {
        if (event.getProject() != null) BoltProjectService.getInstance(event.getProject()).restart();
    }
}
