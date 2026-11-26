package dev.bolt.idea;
import com.intellij.openapi.project.Project;
import java.lang.reflect.*;
/** The trust API moved packages. Fail closed rather than launch a process in safe mode. */
final class BoltTrust {
    static boolean isTrusted(Project project) {
        for (String name : new String[]{"com.intellij.ide.trustedProjects.TrustedProjects", "com.intellij.ide.impl.TrustedProjects"}) {
            for (String methodName : new String[]{"isProjectTrusted", "isTrusted"}) {
                try {
                    Class<?> api = Class.forName(name, true, BoltTrust.class.getClassLoader());
                    Method method = api.getMethod(methodName, Project.class);
                    Object instance = Modifier.isStatic(method.getModifiers()) ? null : api.getField("INSTANCE").get(null);
                    return Boolean.TRUE.equals(method.invoke(instance, project));
                } catch (ReflectiveOperationException | LinkageError ignored) { }
            }
        }
        return false;
    }
    private BoltTrust() { }
}
