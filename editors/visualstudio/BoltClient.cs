using Microsoft.VisualStudio.LanguageServer.Client;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Threading;
using Microsoft.VisualStudio.Utilities;
using StreamJsonRpc;
using System;
using System.Collections.Generic;
using System.ComponentModel.Composition;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;

namespace Bolt.VisualStudio
{
    internal static class BoltContent
    {
        [Export, Name("Bolt"), BaseDefinition(CodeRemoteContentDefinition.CodeRemoteContentTypeName)]
        internal static ContentTypeDefinition Type;
        [Export, FileExtension(".bolt"), ContentType("Bolt")]
        internal static FileExtensionToContentTypeDefinition File;
    }

    [Export(typeof(ILanguageClient)), ContentType("Bolt")]
    public sealed class BoltClient : ILanguageClient, ILanguageClientCustomMessage2
    {
        internal static BoltClient Active;
        internal JsonRpc Rpc;
        private readonly SemaphoreSlim restarting = new SemaphoreSlim(1);
        public string Name => "Bolt";
        public IEnumerable<string> ConfigurationSections => null;
        public object InitializationOptions => null;
        public IEnumerable<string> FilesToWatch => new[] { "**/*.bolt", "**/bolt.proj", "**/bolt.project", "**/bolt.conf" };
        public bool ShowNotificationOnInitializeFailed => true;
        public event AsyncEventHandler<EventArgs> StartAsync;
        public event AsyncEventHandler<EventArgs> StopAsync;
        public object MiddleLayer => null;
        public object CustomMessageTarget { get; } = new EditorMessages();
        public Task AttachForCustomMessageAsync(JsonRpc rpc) { Rpc = rpc; return Task.CompletedTask; }
        public async Task OnLoadedAsync() { Active = this; await StartAsync.InvokeAsync(this, EventArgs.Empty); }
        public Task<InitializationFailureContext> OnServerInitializeFailedAsync(ILanguageClientInitializationInfo info)
            => Task.FromResult(new InitializationFailureContext { FailureMessage = "Bolt editor startup failed. See the Visual Studio activity log." });
        public Task OnServerInitializedAsync() => Task.CompletedTask;
        public async Task<Connection> ActivateAsync(CancellationToken token)
        {
            var package = await BoltPackage.LoadAsync();
            await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync(token);
            var info = Tools.Adapter(package.EffectiveSettings(), "lsp");
            info.RedirectStandardInput = true;
            var process = new Process { StartInfo = info };
            process.ErrorDataReceived += (_, e) => { if (e.Data != null) ActivityLog.LogWarning("Bolt", e.Data); };
            process.Start();
            process.BeginErrorReadLine();
            return new Connection(process.StandardOutput.BaseStream, process.StandardInput.BaseStream);
        }
        private sealed class EditorMessages
        {
            [JsonRpcMethod("bolt/editor", UseSingleObjectParameterDeserialization = true)]
            public Task AcceptEditorDataAsync(EditorHighlightData payload) => BoltEditor.PublishAsync(payload);
        }
        internal static void SettingsChanged()
        {
            if (Active != null) ThreadHelper.JoinableTaskFactory.RunAsync(Active.RestartAsync).FileAndForget("Bolt/Restart");
        }
        internal async Task RestartAsync()
        {
            await restarting.WaitAsync();
            try { await StopAsync.InvokeAsync(this, EventArgs.Empty); Rpc = null; await StartAsync.InvokeAsync(this, EventArgs.Empty); }
            finally { restarting.Release(); }
        }
    }
}
