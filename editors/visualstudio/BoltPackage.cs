using EnvDTE;
using EnvDTE80;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Microsoft.VisualStudio.OLE.Interop;
using Newtonsoft.Json.Linq;
using System;
using System.Collections.Generic;
using System.Collections.Concurrent;
using System.ComponentModel.Design;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Forms = System.Windows.Forms;
using Task = System.Threading.Tasks.Task;

namespace Bolt.VisualStudio
{
    [PackageRegistration(UseManagedResourcesOnly = true, AllowsBackgroundLoading = true, RegisterUsing = RegistrationMethod.CodeBase)]
    [ProvideMenuResource("Menus.ctmenu", 1)]
    [ProvideOptionPage(typeof(BoltOptions), "Bolt", "General", 100, 101, true)]
    [Guid(PackageId)]
    public sealed class BoltPackage : AsyncPackage
    {
        internal const string PackageId = "56B30167-5122-4746-9CAD-13DA36200610";
        internal static BoltPackage Instance;
        internal BoltOptions Options => (BoltOptions)GetDialogPage(typeof(BoltOptions));
        private DTE2 ide;
        private IVsOutputWindowPane output;
        private IVsDebugger4 debugger;
        private ErrorListProvider diagnostics;
        private static readonly Regex Diagnostic = new Regex(@"^(.*)\((\d+),(\d+)\):\s*(error|warning|info)\s+(BLT\d+):\s*(.*)$", RegexOptions.IgnoreCase);
        private IVsRegisterPriorityCommandTarget routing;
        private uint routingCookie;
        private bool busy;
        private CancellationTokenSource compilation;
        internal JObject EffectiveSettings()
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            var settings = Options.Settings();
            string workspace = ide.Solution.FullName;
            if (!string.IsNullOrEmpty(workspace)) {
                var profiles = JObject.Parse(Options.WorkspaceProfiles);
                var profile = (string)profiles.GetValue(workspace, StringComparison.OrdinalIgnoreCase);
                if (profile != null) settings["profile"] = profile;
            }
            return settings;
        }
        private enum ActionId { Run = 0x100, Debug, Build, Check, NewClass, NewProject, Compiler, Profile, Restart, Info, Options, ConfiguredTask, Cancel }

        internal static async Task<BoltPackage> LoadAsync()
        {
            await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync();
            if (Instance != null) return Instance;
            var shell = (IVsShell)GetGlobalService(typeof(SVsShell));
            var id = new Guid(PackageId);
            ErrorHandler.ThrowOnFailure(shell.LoadPackage(ref id, out var package));
            return (BoltPackage)package;
        }
        protected override async Task InitializeAsync(CancellationToken token, IProgress<ServiceProgressData> progress)
        {
            await JoinableTaskFactory.SwitchToMainThreadAsync(token);
            ide = (DTE2)await GetServiceAsync(typeof(DTE));
            debugger = (IVsDebugger4)await GetServiceAsync(typeof(SVsShellDebugger));
            var window = (IVsOutputWindow)await GetServiceAsync(typeof(SVsOutputWindow));
            var id = new Guid(PackageId);
            window.CreatePane(ref id, "Bolt", 1, 0);
            window.GetPane(ref id, out output);
            diagnostics = new ErrorListProvider(this) { ProviderName = "Bolt Build", ProviderGuid = id };
            var service = (OleMenuCommandService)await GetServiceAsync(typeof(IMenuCommandService));
            foreach (ActionId action in Enum.GetValues(typeof(ActionId)))
            {
                var command = new OleMenuCommand((s, e) => Dispatch(action), new CommandID(new Guid("c11df32e-21e4-4bba-bfdf-98a976145388"), (int)action));
                command.BeforeQueryStatus += (s,e) => {
                    ThreadHelper.ThrowIfNotOnUIThread();
                    command.Visible = action > ActionId.Check || ActiveFile() != null;
                    if (action == ActionId.Cancel) command.Visible = compilation != null;
                    command.Enabled = !busy || action > ActionId.Check;
                };
                service.AddCommand(command);
            }
            routing = (IVsRegisterPriorityCommandTarget)await GetServiceAsync(typeof(SVsRegisterPriorityCommandTarget));
            ErrorHandler.ThrowOnFailure(routing.RegisterPriorityCommandTarget(0, new StartCommands(this), out routingCookie));
            Instance = this;
        }
        private bool CanStart(Guid group, uint id)
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            return group == VSConstants.GUID_VSStandardCommandSet97 &&
                (id == (uint)VSConstants.VSStd97CmdID.Start || id == (uint)VSConstants.VSStd97CmdID.StartNoDebug) &&
                ActiveFile() != null && ide.Debugger.CurrentMode == dbgDebugMode.dbgDesignMode;
        }
        private sealed class StartCommands : IOleCommandTarget
        {
            private readonly BoltPackage package;
            internal StartCommands(BoltPackage package) { this.package = package; }
            public int QueryStatus(ref Guid group, uint count, OLECMD[] commands, IntPtr text)
            {
                ThreadHelper.ThrowIfNotOnUIThread();
                if (count != 1 || !package.CanStart(group, commands[0].cmdID)) return (int)Microsoft.VisualStudio.OLE.Interop.Constants.OLECMDERR_E_NOTSUPPORTED;
                commands[0].cmdf = (uint)OLECMDF.OLECMDF_SUPPORTED | (package.busy ? 0u : (uint)OLECMDF.OLECMDF_ENABLED);
                return VSConstants.S_OK;
            }
            public int Exec(ref Guid group, uint id, uint options, IntPtr input, IntPtr outputArg)
            {
                ThreadHelper.ThrowIfNotOnUIThread();
                if (!package.CanStart(group, id)) return (int)Microsoft.VisualStudio.OLE.Interop.Constants.OLECMDERR_E_NOTSUPPORTED;
                package.Dispatch(id == (uint)VSConstants.VSStd97CmdID.Start ? ActionId.Debug : ActionId.Run);
                return VSConstants.S_OK;
            }
        }
        private string ActiveFile()
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            var file = ide.ActiveDocument?.FullName;
            return string.Equals(Path.GetExtension(file), ".bolt", StringComparison.OrdinalIgnoreCase) ? file : null;
        }
        internal void RunFile(string file) => Dispatch(ActionId.Run, file);
        private void Dispatch(ActionId action, string file = null)
        {
            JoinableTaskFactory.RunAsync(async () => {
                try { await ExecuteAsync(action, file); }
                catch (OperationCanceledException) { output.OutputStringThreadSafe("Bolt operation cancelled." + Environment.NewLine); }
                catch (Exception ex) {
                    await JoinableTaskFactory.SwitchToMainThreadAsync();
                    output.OutputStringThreadSafe(ex.Message + Environment.NewLine);
                    output.Activate();
                    VsShellUtilities.ShowMessageBox(this, ex.Message, "Bolt", OLEMSGICON.OLEMSGICON_WARNING, OLEMSGBUTTON.OLEMSGBUTTON_OK, OLEMSGDEFBUTTON.OLEMSGDEFBUTTON_FIRST);
                }
            }).FileAndForget("Bolt/Command");
        }
        private string TargetDirectory()
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            var selection = ide.ToolWindows.SolutionExplorer.SelectedItems as Array;
            string dir = null;
            if (selection?.Length > 0 && selection.GetValue(0) is UIHierarchyItem item && item.Object is ProjectItem projectItem)
                dir = projectItem.FileNames[1];
            if (!Directory.Exists(dir)) {
                var active = ActiveFile() ?? ide.Solution.FullName;
                dir = string.IsNullOrEmpty(active) || Directory.Exists(active) ? active : Path.GetDirectoryName(active);
            }
            using (var picker = new Forms.FolderBrowserDialog { SelectedPath = dir ?? "", Description = "Select a parent folder" })
                return picker.ShowDialog() == Forms.DialogResult.OK ? picker.SelectedPath : null;
        }
        private async Task ExecuteAsync(ActionId action, string file)
        {
            await JoinableTaskFactory.SwitchToMainThreadAsync();
            var options = Options;
            if (action == ActionId.Cancel) { compilation?.Cancel(); return; }
            if (action == ActionId.Options) { ShowOptionPage(typeof(BoltOptions)); return; }
            if (action == ActionId.Compiler) {
                using (var picker = new Forms.OpenFileDialog { Filter = "Bolt compiler|bolt.exe;bolt.cmd|Executables|*.exe", FileName = options.CompilerPath })
                    if (picker.ShowDialog() == Forms.DialogResult.OK) { options.CompilerPath = picker.FileName; options.SaveSettingsToStorage(); BoltClient.SettingsChanged(); }
                return;
            }
            if (action == ActionId.Profile) {
                int current = Enum.TryParse((string)EffectiveSettings()["profile"], out BuildProfile chosen) ? (int)chosen : 0;
                int selected = Choose("Bolt build and analysis profile", new[] { "Project default", "Debug", "Release" }, current);
                if (selected >= 0) {
                        string workspace = ide.Solution.FullName;
                        var value = (BuildProfile)selected;
                        if (string.IsNullOrEmpty(workspace)) options.Profile = value;
                        else {
                            var profiles = JObject.Parse(options.WorkspaceProfiles);
                            profiles[workspace] = value == BuildProfile.ProjectDefault ? "" : value.ToString();
                            options.WorkspaceProfiles = profiles.ToString(Newtonsoft.Json.Formatting.None);
                        }
                        options.SaveSettingsToStorage();
                        BoltClient.SettingsChanged();
                }
                return;
            }
            if (action == ActionId.Restart) { if (BoltClient.Active != null) await BoltClient.Active.RestartAsync(); return; }
            var settings = EffectiveSettings();
            if (action == ActionId.NewClass || action == ActionId.NewProject) {
                var dir = TargetDirectory();
                if (dir == null) return;
                string name = Microsoft.VisualBasic.Interaction.InputBox(action == ActionId.NewClass ? "Class name" : "New project folder", "Bolt").Trim();
                if (name == "") return;
                if (action == ActionId.NewClass) {
                    name = (string)JToken.Parse(await Tools.CaptureAsync(Tools.Adapter(settings, "className", name)));
                    if (name == null) throw new ArgumentException("Enter a valid Bolt class identifier.");
                    file = Path.Combine(dir, name + ".bolt");
                    using (var stream = new FileStream(file, FileMode.CreateNew, FileAccess.Write))
                    using (var writer = new StreamWriter(stream, new UTF8Encoding(false))) writer.Write("public class " + name + "\r\n{\r\n    \r\n}\r\n");
                } else {
                    if (!System.Text.RegularExpressions.Regex.IsMatch(name, @"^[A-Za-z0-9_][A-Za-z0-9._ -]*$") || name.EndsWith("."))
                        throw new ArgumentException("Enter a single project folder name.");
                    string dest = Path.Combine(dir, name);
                    await Tools.CaptureAsync(Tools.Adapter(settings, "init", dest));
                    file = Path.Combine(dest, "src", "Main.bolt");
                }
                ide.ItemOperations.OpenFile(file);
                return;
            }
            file = file ?? ActiveFile();
            output.Activate();
            ide.ToolWindows.OutputWindow.Parent.Activate();
            if (action == ActionId.Info) {
                output.OutputStringThreadSafe("Bolt Language Tools " + typeof(BoltPackage).Assembly.GetName().Version + Environment.NewLine +
                    "Palette: " + options.Palette + "; semantic highlighting: " + options.SemanticHighlighting + Environment.NewLine +
                    await Tools.CaptureAsync(Tools.Adapter(settings, "info")));
                return;
            }
            if (busy) return;
            busy = true;
            try {
                var boundary = ide.Solution.FullName;
                if (!string.IsNullOrEmpty(boundary) && !Directory.Exists(boundary)) boundary = Path.GetDirectoryName(boundary);
                if (action == ActionId.ConfiguredTask) {
                    boundary = string.IsNullOrEmpty(boundary) ? Path.GetDirectoryName(file) : boundary;
                    string config = Path.Combine(boundary ?? "", ".vscode", "tasks.json");
                    if (string.IsNullOrEmpty(boundary) || !File.Exists(config)) throw new InvalidOperationException("Open a folder containing .vscode/tasks.json.");
                    var tasks = (JObject.Parse(File.ReadAllText(config))["tasks"] as JArray)?.OfType<JObject>()
                        .Where(t => (string)t["type"] == "bolt" && new[] { "run", "build", "check" }.Contains((string)t["operation"])).ToList();
                    if (tasks == null || tasks.Count == 0) throw new InvalidOperationException("No Bolt tasks found in " + config);
                    int index = Choose("Run Bolt task", tasks.Select(t => (string)t["label"] ?? (string)t["operation"]).ToArray(), 0);
                    if (index < 0) return;
                    var task = tasks[index];
                    action = (ActionId)Enum.Parse(typeof(ActionId), (string)task["operation"], true);
                    foreach (string key in new[] { "args", "cwd", "profile" }) if (task[key] != null) settings[key] = task[key];
                    if (task["env"] is JObject vars) ((JObject)settings["env"]).Merge(vars);
                    settings["taskTarget"] = (string)task["target"] ?? (file == null ? "." : null);
                    file = file ?? Path.Combine(boundary, "Main.bolt");
                }
                if (file == null) throw new InvalidOperationException("Open a Bolt source file first.");
                var target = JObject.Parse(await Tools.CaptureAsync(Tools.Adapter(settings, "target", file, boundary ?? "")));
                string root = (string)target["root"];
                foreach (Document doc in ide.Documents) {
                    string path = doc.FullName;
                    if (doc.Saved || !(path.Equals(file, StringComparison.OrdinalIgnoreCase) ||
                        path.StartsWith(root.TrimEnd('\\','/') + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))) continue;
                    string name = Path.GetFileName(path).ToLowerInvariant();
                    if (name.EndsWith(".bolt") || name == "bolt.proj" || name == "bolt.conf" || name == "bolt.project")
                        if (doc.Save() != vsSaveStatus.vsSaveSucceeded) throw new IOException("Save the modified Bolt files before continuing.");
                }
                var plan = JObject.Parse(await Tools.CaptureAsync(Tools.Adapter(settings, action.ToString().ToLowerInvariant(), file, boundary ?? "")));
                var args = plan["args"].Values<string>().ToArray();
                string exe = (string)plan["exe"];
                if (action == ActionId.Run) { Launch(exe, args, root, (JObject)settings["env"], false); return; }
                var info = Tools.ProcessInfo(exe, args, root);
                foreach (var entry in ((JObject)settings["env"]).Properties()) info.EnvironmentVariables[entry.Name] = (string)entry.Value;
                diagnostics.Tasks.Clear();
                var matches = new ConcurrentQueue<Match>();
                compilation = new CancellationTokenSource();
                try {
                    await Tools.CaptureAsync(info, line => {
                        output.OutputStringThreadSafe(line + Environment.NewLine);
                        var match = Diagnostic.Match(line);
                        if (match.Success) matches.Enqueue(match);
                    }, compilation.Token);
                } finally {
                    compilation.Dispose();
                    compilation = null;
                    foreach (var match in matches) AddDiagnostic(match, root);
                }
                if (action == ActionId.Debug) {
                    var env = (JObject)settings["env"].DeepClone();
                    var bin = Path.GetDirectoryName(exe);
                    env["PATH"] = bin + ";" + ((string)env.GetValue("PATH", StringComparison.OrdinalIgnoreCase) ?? Environment.GetEnvironmentVariable("PATH"));
                    Launch((string)plan["artifact"], plan["programArgs"].Values<string>().ToArray(), (string)plan["cwd"], env, true);
                }
            } finally { busy = false; }
        }
        private static int Choose(string title, string[] choices, int selected)
        {
            using (var dialog = new Forms.Form { Text = title, Width = 380, Height = 130, StartPosition = Forms.FormStartPosition.CenterParent })
            using (var list = new Forms.ComboBox { Dock = Forms.DockStyle.Top, DropDownStyle = Forms.ComboBoxStyle.DropDownList })
            using (var ok = new Forms.Button { Text = "OK", Dock = Forms.DockStyle.Bottom, DialogResult = Forms.DialogResult.OK }) {
                list.Items.AddRange(choices);
                list.SelectedIndex = selected;
                dialog.Controls.Add(list); dialog.Controls.Add(ok); dialog.AcceptButton = ok;
                return dialog.ShowDialog() == Forms.DialogResult.OK ? list.SelectedIndex : -1;
            }
        }
        private void AddDiagnostic(Match match, string root)
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            var fields = match.Groups;
            if (!int.TryParse(fields[2].Value, out var line) || !int.TryParse(fields[3].Value, out var column)) return;
            var severity = fields[4].Value.ToLowerInvariant();
            var task = new ErrorTask {
                Category = TaskCategory.BuildCompile, ErrorCategory = severity == "error" ? TaskErrorCategory.Error :
                    severity == "warning" ? TaskErrorCategory.Warning : TaskErrorCategory.Message,
                Document = Path.Combine(root, fields[1].Value), Line = Math.Max(0, line - 1), Column = Math.Max(0, column - 1),
                Text = fields[5].Value + ": " + fields[6].Value
            };
            task.Navigate += (_, __) => {
                ThreadHelper.ThrowIfNotOnUIThread();
                if (!File.Exists(task.Document)) return;
                ide.ItemOperations.OpenFile(task.Document);
                ((TextSelection)ide.ActiveDocument.Selection).MoveToLineAndOffset(task.Line + 1, task.Column + 1);
            };
            diagnostics.Tasks.Add(task);
        }
        private void Launch(string exe, string[] args, string cwd, JObject env, bool debug)
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            var target = new VsDebugTargetInfo4 {
                dlo = (uint)DEBUG_LAUNCH_OPERATION.DLO_CreateProcess, bstrExe = exe, bstrCurDir = cwd,
                bstrArg = string.Join(" ", args.Select(Tools.Quote)),
                bstrEnv = env.HasValues ? string.Join("\0", env.Properties().Select(p => p.Name + "=" + (string)p.Value)) + "\0\0" : null,
                guidLaunchDebugEngine = VSConstants.DebugEnginesGuids.NativeOnly_guid,
                LaunchFlags = (uint)__VSDBGLAUNCHFLAGS2.DBGLAUNCH_MergeEnv | (debug ? 0u : (uint)__VSDBGLAUNCHFLAGS.DBGLAUNCH_NoDebug)
            };
            debugger.LaunchDebugTargets4(1, new[] {target}, new VsDebugTargetProcessInfo[1]);
        }
        protected override void Dispose(bool disposing)
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            if (disposing) {
                diagnostics?.Dispose();
                if (routingCookie != 0) routing.UnregisterPriorityCommandTarget(routingCookie);
            }
            base.Dispose(disposing);
        }
    }
}
