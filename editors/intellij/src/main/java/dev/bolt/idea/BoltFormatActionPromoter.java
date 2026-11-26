package dev.bolt.idea;

import com.intellij.openapi.actionSystem.ActionPromoter;
import com.intellij.openapi.actionSystem.AnAction;
import com.intellij.openapi.actionSystem.DataContext;
import java.util.List;

/** Gives the Bolt chord priority over IntelliJ's single-stroke Ctrl+K Commit action. */
public final class BoltFormatActionPromoter implements ActionPromoter {
    @Override public List<AnAction> promote(List<? extends AnAction> actions, DataContext context) {
        for (AnAction action : actions) {
            if (action instanceof BoltFormatFileAction) {
                // The dispatcher still runs update(), so disabled Bolt contexts fall
                // through to the normal shortcut without taking over Commit globally.
                return List.of(action);
            }
        }
        return List.of();
    }
}
