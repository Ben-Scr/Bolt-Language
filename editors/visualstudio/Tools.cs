using Microsoft.VisualStudio.Shell;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace Bolt.VisualStudio
{
    public enum BuildProfile { ProjectDefault, Debug, Release }
    public enum ColorScheme { Automatic, Dark, Light }
    [System.Runtime.InteropServices.Guid("7aacc2ee-32e6-4a9d-9f21-c0c290b1526b")]
    public sealed class BoltOptions : DialogPage
    {
        [Category("Toolchain"), DisplayName("Compiler path"), Description("Absolute path to bolt.exe. Leave empty for SDK discovery.")]
        public string CompilerPath { get; set; } = "";
        [Category("Toolchain"), DisplayName("Build and analysis profile")]
        public BuildProfile Profile { get; set; }
        [Category("Editor"), DisplayName("Bolt C# colors")]
        public ColorScheme Palette { get; set; }
        [Category("Editor"), DisplayName("Semantic highlighting"), DefaultValue(true)]
        public bool SemanticHighlighting { get; set; } = true;
        [Browsable(false)]
        public string WorkspaceProfiles { get; set; } = "{}";
        [Category("Run"), Description("JSON array of strings. Leave empty to use project arguments.")]
        public string Arguments { get; set; } = "";
        [Category("Run"), DisplayName("Environment"), Description("JSON object with environment variable names and string values.")]
        public string EnvironmentVariables { get; set; } = "{}";
        [Category("Run"), DisplayName("Working directory"), Description("Absolute or relative to the project.")]
        public string WorkingDirectory { get; set; } = "";
        internal JObject Settings()
        {
            var args = string.IsNullOrWhiteSpace(Arguments) ? null : JArray.Parse(Arguments);
            if (args != null && args.Any(a => a.Type != JTokenType.String))
                throw new ArgumentException("Run arguments must be a JSON array of strings.");
            var env = JObject.Parse(string.IsNullOrWhiteSpace(EnvironmentVariables) ? "{}" : EnvironmentVariables);
            if (env.Properties().Any(p => p.Name.Length == 0 || p.Name.IndexOfAny(new[] {'=', '\0'}) >= 0 || p.Value.Type != JTokenType.String))
                throw new ArgumentException("Environment variables require names and string values.");
            return new JObject { ["compilerPath"] = CompilerPath, ["profile"] = Profile == BuildProfile.ProjectDefault ? "" : Profile.ToString(),
                ["cwd"] = WorkingDirectory, ["args"] = args, ["env"] = env, ["semanticHighlighting"] = SemanticHighlighting };
        }
        protected override void OnApply(PageApplyEventArgs e)
        {
            try { Settings(); }
            catch (Exception ex) { System.Windows.Forms.MessageBox.Show(ex.Message, "Bolt"); e.ApplyBehavior = ApplyKind.Cancel; return; }
            base.OnApply(e);
            BoltClient.SettingsChanged();
        }
    }

    internal static class Tools
    {
        // CommandLineToArgvW treats backslashes specially before quotes and the closing delimiter.
        internal static string Quote(string arg)
        {
            var result = new StringBuilder("\"");
            int slashes = 0;
            foreach (char c in arg)
            {
                if (c == '\\') { slashes++; continue; }
                if (c == '"') result.Append('\\', slashes + 1);
                result.Append('\\', slashes).Append(c);
                slashes = 0;
            }
            return result.Append('\\', slashes * 2).Append('"').ToString();
        }
        internal static ProcessStartInfo ProcessInfo(string exe, string[] args, string cwd)
        {
            return new ProcessStartInfo(exe, string.Join(" ", args.Select(Quote))) {
                WorkingDirectory = cwd, UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true, StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8
            };
        }
        internal static ProcessStartInfo Adapter(JObject settings, params string[] args)
        {
            var dir = Path.Combine(Path.GetDirectoryName(typeof(Tools).Assembly.Location), "Server");
            var info = ProcessInfo(Path.Combine(dir, "node.exe"), new[] { Path.Combine(dir, "adapter.cjs") }.Concat(args).ToArray(), dir);
            info.EnvironmentVariables["BOLT_VS_OPTIONS"] = settings.ToString(Formatting.None);
            info.EnvironmentVariables.Remove("NODE_OPTIONS");
            return info;
        }
        internal static async Task<string> CaptureAsync(ProcessStartInfo info, Action<string> log = null, CancellationToken token = default)
        {
            using (var process = new Process { StartInfo = info, EnableRaisingEvents = true })
            {
                var done = new TaskCompletionSource<int>(TaskCreationOptions.RunContinuationsAsynchronously);
                process.Exited += (_, __) => done.TrySetResult(process.ExitCode);
                process.Start();
                var output = ReadLinesAsync(process.StandardOutput, log);
                var error = ReadLinesAsync(process.StandardError, log);
                using (token.Register(() => {
                    if (!process.HasExited) {
                        var stop = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "taskkill.exe"),
                            "/T /F /PID " + process.Id) { UseShellExecute = false, CreateNoWindow = true };
                        using (Process.Start(stop)) { }
                    }
                })) await Task.WhenAll(output, error, done.Task).ConfigureAwait(false);
                token.ThrowIfCancellationRequested();
                int code = await done.Task.ConfigureAwait(false);
                var stderr = (await error.ConfigureAwait(false)).Trim();
                if (code != 0) throw new InvalidOperationException(stderr.Length > 0 ? stderr : "Bolt exited with code " + code);
                return await output.ConfigureAwait(false);
            }
        }
        private static async Task<string> ReadLinesAsync(StreamReader reader, Action<string> log)
        {
            var result = new StringBuilder();
            string line;
            while ((line = await reader.ReadLineAsync().ConfigureAwait(false)) != null)
            {
                log?.Invoke(line);
                result.AppendLine(line);
            }
            return result.ToString();
        }
    }
}
