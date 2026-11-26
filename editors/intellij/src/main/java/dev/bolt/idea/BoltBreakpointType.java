package dev.bolt.idea;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.xdebugger.breakpoints.*;
import org.jetbrains.annotations.NotNull;
import java.util.EnumSet;

public final class BoltBreakpointType extends XLineBreakpointType<BoltBreakpointType.Properties> {
    public static final class Properties extends XBreakpointProperties<Properties> {
        @Override public Properties getState(){return this;}
        @Override public void loadState(@NotNull Properties state) { }
    }
    public BoltBreakpointType(){super("bolt-line","Bolt Line Breakpoints");}
    @Override public EnumSet<StandardPanels> getVisibleStandardPanels(){return EnumSet.of(StandardPanels.SUSPEND_POLICY,StandardPanels.ACTIONS);}
    @Override public Properties createBreakpointProperties(@NotNull VirtualFile file,int line){return new Properties();}
    @Override public boolean canPutAt(@NotNull VirtualFile file,int line,@NotNull Project project){return file.getFileType()==BoltFileType.INSTANCE&&file.isWritable();}
}
