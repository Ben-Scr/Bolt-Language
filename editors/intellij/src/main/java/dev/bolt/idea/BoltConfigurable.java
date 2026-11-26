package dev.bolt.idea;

import com.intellij.openapi.options.Configurable;
import com.intellij.openapi.project.ProjectManager;
import dev.bolt.idea.core.CompilerLocator;
import javax.swing.*;
import java.awt.*;
import java.io.IOException;

public final class BoltConfigurable implements Configurable {
    private JPanel panel;
    private JTextField executable;
    private JComboBox<String> profile;
    private JCheckBox diagnostics, semantic, completion;
    @Override public String getDisplayName() { return "Bolt"; }
    @Override public JComponent createComponent() {
        panel = new JPanel(new GridBagLayout()); GridBagConstraints c = new GridBagConstraints();
        c.gridx = 0; c.gridy = 0; c.anchor = GridBagConstraints.WEST; c.insets = new Insets(6,6,6,6);
        panel.add(new JLabel("Bolt executable:"), c);
        c.gridx = 1; c.weightx = 1; c.fill = GridBagConstraints.HORIZONTAL;
        executable = new JTextField(40); panel.add(executable, c);
        c.gridx = 2; c.weightx = 0; c.fill = GridBagConstraints.NONE;
        JButton detect = new JButton("Detect"); panel.add(detect, c);
        JLabel status = new JLabel("Leave empty to search BOLT_COMPILER, PATH and standard per-user install locations.");
        detect.addActionListener(e -> {
            try { executable.setText(CompilerLocator.locate("").toString()); status.setText("Detected installed native compiler."); }
            catch (IOException error) { status.setText(error.getMessage()); }
        });
        c.gridy++; c.gridx = 0; c.gridwidth = 3; panel.add(status, c);
        c.gridy++; JPanel profileRow = new JPanel(new FlowLayout(FlowLayout.LEFT, 0, 0));
        profileRow.add(new JLabel("Conditional-compilation analysis profile: "));
        profile = new JComboBox<>(new String[]{"Project default", "Debug", "Release"}); profileRow.add(profile);panel.add(profileRow, c);
        c.gridy++; diagnostics = new JCheckBox("Show compiler diagnostics for unsaved .bolt files"); panel.add(diagnostics, c);
        c.gridy++; semantic = new JCheckBox("Semantic colors from bolt lsp (types, generic parameters, methods and fields)"); panel.add(semantic, c);
        c.gridy++; completion = new JCheckBox("Member completion after . and -> (and Code > Code Completion > Basic)"); panel.add(completion, c);
        c.gridy++; panel.add(new JLabel("Lexical colors work without Bolt. The language server runs only in trusted projects."), c);
        c.gridy++; panel.add(new JLabel("Colors: Editor > Color Scheme > Bolt. Builds/runs: use Bolt Run/Build configurations or the terminal."), c);
        c.gridy++; c.weighty = 1; c.fill = GridBagConstraints.BOTH; panel.add(new JPanel(), c);
        reset(); return panel;
    }
    @Override public boolean isModified() {
        if (panel == null) return false;
        var state = BoltSettings.getInstance().snapshot();
        return !executable.getText().trim().equals(state.executable()) || diagnostics.isSelected() != state.diagnostics()
            || semantic.isSelected() != state.semanticColors() || completion.isSelected() != state.completion() || !selectedProfile().equals(state.analysisProfile());
    }
    @Override public void apply() {
        if (panel == null) return;
        BoltSettings.Data state = new BoltSettings.Data(); state.executable = executable.getText().trim();
        state.diagnostics = diagnostics.isSelected(); state.semanticColors = semantic.isSelected(); state.completion = completion.isSelected(); state.analysisProfile = selectedProfile();
        BoltSettings.getInstance().loadState(state);
        for (var project : ProjectManager.getInstance().getOpenProjects()) if (!project.isDisposed()) BoltProjectService.getInstance(project).restart();
    }
    @Override public void reset() {
        if (panel == null) return;
        var state = BoltSettings.getInstance().snapshot(); executable.setText(state.executable());
        diagnostics.setSelected(state.diagnostics()); semantic.setSelected(state.semanticColors()); completion.setSelected(state.completion()); profile.setSelectedItem(state.analysisProfile().isEmpty() ? "Project default" : state.analysisProfile());
    }
    private String selectedProfile() { return profile == null || profile.getSelectedIndex() == 0 ? "" : (String) profile.getSelectedItem(); }
    @Override public void disposeUIResources() { panel = null; executable = null; diagnostics = null; semantic = null; completion = null; profile = null; }
}
