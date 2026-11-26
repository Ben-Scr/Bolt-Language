package dev.bolt.idea;
import com.intellij.openapi.options.SettingsEditor;
import com.intellij.openapi.options.ConfigurationException;
import com.intellij.util.ui.FormBuilder;
import javax.swing.*;
public final class BoltRunEditor extends SettingsEditor<BoltRunConfiguration>{
    private final JTextField project=new JTextField(),directory=new JTextField(),adapter=new JTextField(),pid=new JTextField(),remote=new JTextField();
    private final JComboBox<String> mode=new JComboBox<>(new String[]{"Launch","Attach","Remote"});
    private final JTextArea sourceMap=new JTextArea(2,40);
    private final JCheckBox terminal=new JCheckBox("Interactive debug console input"),exceptions=new JCheckBox("Break on Bolt throw, rethrow and runtime failure");
    private final JComboBox<String> profile=new JComboBox<>(new String[]{"Project default","Debug","Release"});
    private final JTextArea args=new JTextArea(4,40),env=new JTextArea(3,40);
    private final JCheckBox override=new JCheckBox("Override bolt.conf arguments (empty means no arguments)");
    private final JPanel panel=FormBuilder.createFormBuilder().addLabeledComponent("Project / bolt.proj / .bolt file",project).addLabeledComponent("Build profile",profile).addLabeledComponent("Working directory (blank = project default)",directory).addComponent(override).addLabeledComponent("Arguments (one per line, no shell quoting)",new JScrollPane(args)).addLabeledComponent("Environment (NAME=value per line)",new JScrollPane(env)).addLabeledComponent("LLDB-DAP executable (blank = auto-detect)",adapter).addLabeledComponent("Debug connection (Attach/Remote: project path is the symbol executable)",mode).addLabeledComponent("Attach process ID",pid).addLabeledComponent("Remote host:port (lldb-server gdbserver)",remote).addLabeledComponent("Source mappings (remote-path=local-path)",new JScrollPane(sourceMap)).addComponent(terminal).addComponent(exceptions).addComponent(new JLabel("Debug uses LLVM, the Debug profile and LLDB-DAP. Run uses the selected build profile.")).getPanel();
    @Override protected void resetEditorFrom(BoltRunConfiguration c){project.setText(c.projectFile);profile.setSelectedItem(c.profile.isBlank()?"Project default":c.profile);directory.setText(c.directory);args.setText(c.arguments);env.setText(c.environment);override.setSelected(c.overrideArguments);adapter.setText(c.debugAdapter);mode.setSelectedItem(c.debugMode);pid.setText(c.debugPid);remote.setText(c.debugRemote);sourceMap.setText(c.debugSourceMap);terminal.setSelected(c.debugTerminal);exceptions.setSelected(c.debugExceptions);adapter.setEnabled(!c.buildOnly);args.setEnabled(!c.buildOnly);directory.setEnabled(!c.buildOnly);override.setEnabled(!c.buildOnly);}
    @Override protected void applyEditorTo(BoltRunConfiguration c)throws ConfigurationException{c.projectFile=project.getText().strip();c.profile=profile.getSelectedIndex()==0?"":String.valueOf(profile.getSelectedItem());c.directory=directory.getText().strip();c.arguments=args.getText();c.environment=env.getText();c.overrideArguments=override.isSelected();c.debugAdapter=adapter.getText().strip();c.debugMode=String.valueOf(mode.getSelectedItem());c.debugPid=pid.getText().strip();c.debugRemote=remote.getText().strip();c.debugSourceMap=sourceMap.getText();c.debugTerminal=terminal.isSelected();c.debugExceptions=exceptions.isSelected();try{c.parseEnvironment();c.debugOptions();}catch(IllegalArgumentException e){throw new ConfigurationException(e.getMessage());}}
    @Override protected JComponent createEditor(){return panel;}
}
