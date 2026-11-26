package dev.bolt.idea;
import com.intellij.openapi.application.ApplicationManager;
import com.intellij.openapi.components.*;
@Service(Service.Level.APP)
@State(name = "dev.bolt.idea.settings", storages = @Storage("bolt-idea.xml"))
public final class BoltSettings implements PersistentStateComponent<BoltSettings.Data> {
    public static final class Data {
        public String executable = "";
        public boolean diagnostics = true;
        public boolean semanticColors = true;
        public boolean completion = true;
        public String analysisProfile = "";
    }
    private volatile Data state = new Data();
    public static BoltSettings getInstance() { return ApplicationManager.getApplication().getService(BoltSettings.class); }
    @Override public Data getState() { return state; }
    @Override public void loadState(Data value) { state = value; }
    public record Options(String executable, boolean diagnostics, boolean semanticColors, boolean completion, String analysisProfile) { }
    public Options snapshot() { Data d = state; return new Options(d.executable == null ? "" : d.executable, d.diagnostics, d.semanticColors, d.completion, d.analysisProfile == null ? "" : d.analysisProfile); }
}
