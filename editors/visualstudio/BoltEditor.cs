using Microsoft.VisualStudio.PlatformUI;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.Text.Classification;
using Microsoft.VisualStudio.Text.Editor;
using Microsoft.VisualStudio.Text.Formatting;
using Microsoft.VisualStudio.Text.Tagging;
using Microsoft.VisualStudio.Utilities;
using System;
using System.Collections.Generic;
using System.ComponentModel.Composition;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace Bolt.VisualStudio
{
    internal sealed class EditorHighlightData
    {
        public string Uri { get; set; }
        public string Hash { get; set; }
        public string[][] Palette { get; set; }
        internal int[] Paint { get; private set; }
        // Decode (gap, length, palette index) in place for binary search by snapshot offset.
        public int[] Runs {
            set {
                if (value.Length % 3 != 0) throw new ArgumentException("Incomplete Bolt color span.");
                int end = 0;
                for (int i = 0; i < value.Length; i += 3) {
                    value[i] = checked(end + value[i]);
                    end = value[i+1] = checked(value[i] + value[i+1]);
                }
                Paint = value;
            }
        }
        public int[][] Folds { get; set; }
        public int[] Entries { get; set; }
    }

    [Export(typeof(IViewTaggerProvider)), ContentType("Bolt"), TagType(typeof(ClassificationTag)),
     TagType(typeof(IOutliningRegionTag)), TagType(typeof(IntraTextAdornmentTag))]
    internal sealed class BoltTags : IViewTaggerProvider
    {
        [Import] internal ITextDocumentFactoryService Documents = null;
        [Import] internal IClassificationTypeRegistryService Types = null;
        [Import] internal IClassificationFormatMapService Formats = null;
        public ITagger<T> CreateTagger<T>(ITextView view, ITextBuffer buffer) where T : ITag
        {
            if (buffer != view.TextBuffer || !(view is IWpfTextView wpf)) return null;
            return view.Properties.GetOrCreateSingletonProperty(() => new BoltEditor(wpf, this)) as ITagger<T>;
        }
    }

    internal sealed class BoltEditor : ITagger<ClassificationTag>, ITagger<IOutliningRegionTag>, ITagger<IntraTextAdornmentTag>
    {
        // Views are removed on close. All access to this list is on the VS UI thread.
        private static readonly List<BoltEditor> views = new List<BoltEditor>();
        private readonly IWpfTextView view;
        private readonly BoltTags services;
        private readonly ITextDocument document;
        private readonly Dictionary<string, ClassificationTag> styles = new Dictionary<string, ClassificationTag>();
        private ITextSnapshot painted;
        private EditorHighlightData highlights, pending;
        private readonly List<ITrackingPoint> closers = new List<ITrackingPoint>();
        public event EventHandler<SnapshotSpanEventArgs> TagsChanged;

        internal BoltEditor(IWpfTextView view, BoltTags services)
        {
            this.view = view;
            this.services = services;
            services.Documents.TryGetTextDocument(view.TextBuffer, out document);
            views.Add(this);
            view.Closed += Closed;
            view.VisualElement.PreviewTextInput += Typing;
            view.TextBuffer.Changed += Changed;
            VSColorTheme.ThemeChanged += ThemeChanged;
            var rpc = BoltClient.Active?.Rpc;
            if (rpc != null && Uri.TryCreate(document?.FilePath, UriKind.Absolute, out var uri))
                ThreadHelper.JoinableTaskFactory.RunAsync(() => rpc.InvokeWithParameterObjectAsync("bolt/editorRefresh",
                    new { textDocument = new { uri = uri.AbsoluteUri } })).FileAndForget("Bolt/Refresh");
        }
        private void Closed(object sender, EventArgs e)
        {
            views.Remove(this);
            view.VisualElement.PreviewTextInput -= Typing;
            view.TextBuffer.Changed -= Changed;
            VSColorTheme.ThemeChanged -= ThemeChanged;
            view.Closed -= Closed;
        }
        private void ThemeChanged(ThemeChangedEventArgs e)
        {
            styles.Clear();
            Invalidate();
        }
        private void Invalidate()
        {
            var snapshot = view.TextSnapshot;
            TagsChanged?.Invoke(this, new SnapshotSpanEventArgs(new SnapshotSpan(snapshot, 0, snapshot.Length)));
        }
        private static readonly ConditionalWeakTable<ITextSnapshot, string> hashes = new ConditionalWeakTable<ITextSnapshot, string>();
        private static string SnapshotHash(ITextSnapshot snapshot) => hashes.GetValue(snapshot, Hash);
        private static string Hash(ITextSnapshot snapshot)
        {
            using (var sha = SHA256.Create()) return Convert.ToBase64String(sha.ComputeHash(Encoding.Unicode.GetBytes(snapshot.GetText())));
        }
        internal static async Task PublishAsync(EditorHighlightData payload)
        {
            if (!Uri.TryCreate(payload.Uri, UriKind.Absolute, out var uri) || !uri.IsFile) return;
            await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync();
            var targets = views.Where(v => string.Equals(v.document?.FilePath, uri.LocalPath, StringComparison.OrdinalIgnoreCase))
                .Select(v => (Editor: v, Snapshot: v.view.TextSnapshot)).ToArray();
            foreach (var target in targets) target.Editor.pending = payload;
            foreach (var target in targets) {
                var editor = target.Editor;
                var fingerprint = await Task.Run(() => SnapshotHash(target.Snapshot)).ConfigureAwait(false);
                await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync();
                if (editor.view.IsClosed || editor.pending != payload || target.Snapshot != editor.view.TextSnapshot ||
                    fingerprint != payload.Hash) continue;
                editor.painted = target.Snapshot;
                editor.highlights = payload;
                editor.Invalidate();
            }
        }
        private ClassificationTag Style(string hex)
        {
            if (styles.TryGetValue(hex, out var tag)) return tag;
            var name = "Bolt " + hex;
            var type = services.Types.GetClassificationType(name) ?? services.Types.CreateClassificationType(name,
                new[] { services.Types.GetClassificationType("text") });
            var map = services.Formats.GetClassificationFormatMap(view);
            var brush = new SolidColorBrush((Color)ColorConverter.ConvertFromString(hex));
            brush.Freeze();
            var props = map.DefaultTextProperties.SetForegroundBrush(brush);
            if (map.CurrentPriorityOrder.Contains(type)) map.SetExplicitTextProperties(type, props);
            else map.AddExplicitTextProperties(type, props);
            return styles[hex] = new ClassificationTag(type);
        }
        IEnumerable<ITagSpan<ClassificationTag>> ITagger<ClassificationTag>.GetTags(NormalizedSnapshotSpanCollection spans)
        {
            if (spans.Count == 0 || painted != spans[0].Snapshot || SystemParameters.HighContrast) yield break;
            var scheme = BoltPackage.Instance?.Options.Palette ?? ColorScheme.Automatic;
            var color = VSColorTheme.GetThemedColor(EnvironmentColors.ToolWindowBackgroundColorKey);
            int theme = scheme == ColorScheme.Dark || (scheme == ColorScheme.Automatic && color.GetBrightness() < 0.5f) ? 0 : 1;
            var paint = highlights.Paint;
            foreach (var span in spans) {
                int low = 0, high = paint.Length / 3;
                while (low < high) {
                    int mid = low + (high - low) / 2;
                    if (paint[3*mid+1] <= span.Start.Position) low = mid + 1; else high = mid;
                }
                for (int i = 3 * low; i < paint.Length && paint[i] < span.End.Position; i += 3) {
                    int a = paint[i], b = paint[i+1];
                    yield return new TagSpan<ClassificationTag>(new SnapshotSpan(painted, a, b-a), Style(highlights.Palette[paint[i+2]][theme]));
                }
            }
        }
        IEnumerable<ITagSpan<IOutliningRegionTag>> ITagger<IOutliningRegionTag>.GetTags(NormalizedSnapshotSpanCollection spans)
        {
            if (spans.Count == 0 || painted != spans[0].Snapshot) yield break;
            foreach (var range in highlights.Folds) {
                int a = (int)range[0], b = (int)range[1] + 1;
                var span = new SnapshotSpan(painted, a, b-a);
                if (spans.IntersectsWith(span)) yield return new TagSpan<IOutliningRegionTag>(span, new OutliningRegionTag(false, false, "{ … }", null));
            }
        }
        IEnumerable<ITagSpan<IntraTextAdornmentTag>> ITagger<IntraTextAdornmentTag>.GetTags(NormalizedSnapshotSpanCollection spans)
        {
            if (spans.Count == 0 || painted != spans[0].Snapshot || document == null) yield break;
            foreach (int line in highlights.Entries) {
                var point = painted.GetLineFromLineNumber(line).Start;
                var span = new SnapshotSpan(point, 0);
                if (!spans.Any(s => s.Start <= point && point <= s.End)) continue;
                var button = new Button { Content = "▶", ToolTip = "Run Bolt project", Padding = new Thickness(2,0,2,0),
                    Focusable = false, VerticalAlignment = VerticalAlignment.Center };
                button.Click += (_,e) => { BoltPackage.Instance?.RunFile(document.FilePath); e.Handled = true; };
                yield return new TagSpan<IntraTextAdornmentTag>(span, new IntraTextAdornmentTag(button, null, PositionAffinity.Successor));
            }
        }
        private void Typing(object sender, TextCompositionEventArgs e)
        {
            if (e.Text != ">" || !view.Selection.IsEmpty) return;
            var point = view.Caret.Position.BufferPosition;
            var mark = closers.FindLast(p => p.GetPosition(point.Snapshot) == point.Position);
            if (mark != null && point.Position < point.Snapshot.Length && point.GetChar() == '>') {
                closers.Remove(mark); view.Caret.MoveTo(point + 1); e.Handled = true;
            }
        }
        private void Changed(object sender, TextContentChangedEventArgs e)
        {
            closers.RemoveAll(p => { var pos = p.GetPosition(e.After); return pos >= e.After.Length || e.After[pos] != '>'; });
            if (e.Changes.Count != 1 || e.Changes[0].NewText != "<" || e.Changes[0].OldLength != 0 || document == null) return;
            if (!view.Options.GetOptionValue<bool>(DefaultTextViewOptions.BraceCompletionEnabledOptionName)) return;
            var after = e.After;
            int pos = e.Changes[0].NewEnd;
            if (pos < after.Length && (char.IsLetterOrDigit(after[pos]) || "_<=".Contains(after[pos]))) return;
            ThreadHelper.JoinableTaskFactory.RunAsync(async () => {
                await Task.Delay(150);
                await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync();
                bool Current() => !view.IsClosed && view.HasAggregateFocus && view.TextSnapshot == after &&
                    view.Selection.IsEmpty && view.Caret.Position.BufferPosition.Position == pos;
                var rpc = BoltClient.Active?.Rpc;
                if (!Current() || rpc == null || !Uri.TryCreate(document.FilePath, UriKind.Absolute, out var uri)) return;
                var line = after.GetLineFromPosition(pos);
                var request = new { textDocument = new { uri = uri.AbsoluteUri }, position = new { line = line.LineNumber,
                    character = pos - line.Start.Position }, hash = await Task.Run(() => SnapshotHash(after)) };
                if (!Current()) return;
                if (await rpc.InvokeWithParameterObjectAsync<bool>("bolt/generic", request) && Current()) {
                    var updated = view.TextBuffer.Insert(pos, ">");
                    closers.Add(updated.CreateTrackingPoint(pos, PointTrackingMode.Positive));
                    view.Caret.MoveTo(new SnapshotPoint(updated, pos));
                }
            }).FileAndForget("Bolt/GenericPair");
        }
    }
}
