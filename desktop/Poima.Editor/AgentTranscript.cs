// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using Avalonia.Automation.Peers;
using Avalonia.Automation.Provider;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input.Platform;
using Avalonia.Media;
using Avalonia.Threading;

namespace Poima.Editor;

internal sealed class AgentTranscript : UserControl
{
    private const int MaximumCharacters = 100000;
    private readonly StackPanel messages = new() { Spacing = 8, Margin = new Thickness(3, 6, 8, 6) };
    private readonly ScrollViewer scroll;
    private readonly DispatcherTimer refresh = new() { Interval = TimeSpan.FromMilliseconds(45) };
    private readonly List<Entry> entries = [];
    private int length;

    private sealed class Entry(bool markdown)
    {
        public bool Markdown { get; } = markdown;
        public string Text { get; set; } = "";
        public ContentControl View { get; } = new();
        public bool Dirty { get; set; } = true;
    }

    public AgentTranscript()
    {
        scroll = new ScrollViewer { Content = messages, VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        Content = scroll;
        var copy = new MenuItem { Header = "Copy source text" };
        copy.Click += async (_, _) =>
        {
            if (TopLevel.GetTopLevel(this)?.Clipboard is { } clipboard)
                await clipboard.SetTextAsync(Text);
        };
        ContextMenu = new ContextMenu { Items = { copy } };
        refresh.Tick += (_, _) => RenderPending();
        DetachedFromVisualTree += (_, _) => refresh.Stop();
        AttachedToVisualTree += (_, _) => RenderPending();
    }

    public string Text => string.Concat(entries.Select(entry => entry.Text));

    public void Clear()
    {
        refresh.Stop();
        entries.Clear();
        messages.Children.Clear();
        length = 0;
    }

    public void SetMarkdown(string text)
    {
        if (entries.Count == 1 && entries[0].Markdown && text.StartsWith(entries[0].Text, StringComparison.Ordinal))
            Append(text[entries[0].Text.Length..], markdown: true);
        else
        {
            Clear();
            Append(text, markdown: true);
        }
    }

    public void Append(string text, bool markdown = false)
    {
        if (text.Length == 0) return;
        if (entries.Count == 0 || entries[^1].Markdown != markdown)
        {
            var entry = new Entry(markdown);
            entries.Add(entry);
            messages.Children.Add(entry.View);
        }
        var last = entries[^1];
        last.Text += text;
        last.Dirty = true;
        length += text.Length;
        while (length > MaximumCharacters)
        {
            var first = entries[0];
            if (first.Text.Length <= length - MaximumCharacters)
            {
                messages.Children.Remove(first.View);
                entries.RemoveAt(0);
                length -= first.Text.Length;
            }
            else
            {
                first.Text = first.Text[(length - MaximumCharacters)..];
                first.Dirty = true;
                length = MaximumCharacters;
            }
        }
        if (!refresh.IsEnabled) refresh.Start();
    }

    private void RenderPending()
    {
        refresh.Stop();
        var following = scroll.Offset.Y + scroll.Viewport.Height >= scroll.Extent.Height - 24;
        foreach (var entry in entries)
        {
            if (!entry.Dirty) continue;
            entry.Dirty = false;
            entry.View.Content = entry.Markdown ? AgentMarkdown.Format(entry.Text) :
                new SelectableTextBlock { Text = entry.Text.Trim(), TextWrapping = TextWrapping.Wrap };
            entry.View.IsVisible = !string.IsNullOrWhiteSpace(entry.Text);
        }
        if (following) Dispatcher.UIThread.Post(scroll.ScrollToEnd, DispatcherPriority.Loaded);
    }

    protected override AutomationPeer OnCreateAutomationPeer() => new TranscriptPeer(this);

    private sealed class TranscriptPeer(AgentTranscript owner) : ControlAutomationPeer(owner), IValueProvider
    {
        public bool IsReadOnly => true;
        public string Value => owner.Text;
        public void SetValue(string? value) => throw new InvalidOperationException("The transcript is read-only.");
        protected override string GetClassNameCore() => nameof(AgentTranscript);
        protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.Document;
    }
}
