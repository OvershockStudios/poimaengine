// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Text.Json;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Templates;
using Avalonia.Layout;
using Avalonia.Media;

namespace Poima.Editor;

internal sealed record BuildHistoryEntry(string Directory, string Checkpoint, string Snapshot, PlaytestAttempt Attempt)
{
    public static BuildHistoryEntry Read(JsonElement entry) => new(
        entry.GetProperty("directory").GetString()!, entry.GetProperty("checkpoint").GetString()!,
        entry.GetProperty("snapshot_sha256").GetString()!, PlaytestAttempt.FromHistory(entry));

    public string Label => $"Revision {Attempt.Revision} {(Attempt.Passed ? "passed" : "failed")} ({Attempt.Build[..12]})";

    public string Date => Directory.Length >= 18 && DateTime.TryParseExact(Directory[..18], "yyyyMMdd-HHmmssfff",
        CultureInfo.InvariantCulture, DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal, out var time)
        ? time.ToLocalTime().ToString("g", CultureInfo.CurrentCulture) : Directory;
}

internal sealed class BuildHistoryWindow : Window
{
    private readonly Func<string?, CancellationToken, Task<JsonElement>> readPage;
    private readonly string? latestBuild;
    private readonly long latestRevision;
    private readonly CancellationTokenSource lifetime = new();
    private readonly Stack<string?> newerPages = new();
    private readonly ListBox list = new();
    private readonly TextBlock status = Text("");
    private readonly Button newer = new() { Content = "Newer" };
    private readonly Button older = new() { Content = "Older" };
    private readonly Button view = new() { Content = "View checks" };
    private readonly Button restore = new() { Content = "Restore this version as copy" };
    private string? current, next;
    private bool loading;

    public BuildHistoryWindow(Func<string?, CancellationToken, Task<JsonElement>> readPage, string? latestBuild, long latestRevision)
    {
        this.readPage = readPage;
        this.latestBuild = latestBuild;
        this.latestRevision = latestRevision;
        Title = "Build history";
        Width = 640; Height = 445; MinWidth = 430; MinHeight = 345;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        Background = EditorTheme.Brush("#19191C");
        Foreground = EditorTheme.Brush("#F2EDEE");
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter");
        FontSize = 14; UseLayoutRounding = true;
        restore.Background = EditorTheme.Brush("#722F37");
        restore.Foreground = EditorTheme.Brush("#F7E8EB");

        list.ItemTemplate = new FuncDataTemplate<BuildHistoryEntry>((entry, _) =>
        {
            if (entry is null) return new TextBlock();
            var row = new StackPanel { Spacing = 4, Margin = new Thickness(2, 6) };
            row.Children.Add(new TextBlock { Text = entry.Label, FontWeight = FontWeight.SemiBold });
            row.Children.Add(Text($"{entry.Date}   {entry.Attempt.PassedGroups} of {entry.Attempt.TotalGroups} check groups"));
            return row;
        });
        AutomationProperties.SetName(list, "Build versions");
        list.ContainerPrepared += (_, args) =>
        {
            if (args.Container is ListBoxItem { Content: BuildHistoryEntry entry })
                AutomationProperties.SetName(args.Container, entry.Label);
        };
        list.SelectionChanged += (_, _) => UpdateControls();
        newer.Click += async (_, _) =>
        {
            if (newerPages.TryPeek(out var before)) await LoadAsync(before, () => newerPages.Pop());
        };
        older.Click += async (_, _) =>
        {
            if (next is { } before) await LoadAsync(before, () => newerPages.Push(current));
        };
        view.Click += async (_, _) => await ViewAsync();
        restore.Click += (_, _) =>
        {
            if (!loading && list.SelectedItem is BuildHistoryEntry entry && entry.Attempt.Retained) Close(entry);
        };
        var done = new Button { Content = "Done", IsCancel = true };
        done.Click += (_, _) => Close();
        Opened += async (_, _) => await LoadAsync(null, () => { });
        Closed += (_, _) => { lifetime.Cancel(); lifetime.Dispose(); };

        var header = new TextBlock { Text = Title, FontSize = 23, FontWeight = FontWeight.SemiBold };
        var footer = new StackPanel { Spacing = 8 };
        footer.Children.Add(status);
        footer.Children.Add(new WrapPanel { ItemSpacing = 8, LineSpacing = 8, Children = { newer, older, view, restore, done } });
        var layout = new Grid { RowDefinitions = new RowDefinitions("Auto,*,Auto"), RowSpacing = 14, Margin = new Thickness(20) };
        layout.Children.Add(header);
        Grid.SetRow(list, 1); layout.Children.Add(list);
        Grid.SetRow(footer, 2); layout.Children.Add(footer);
        Content = layout;
        UpdateControls();
    }

    private async Task LoadAsync(string? before, Action navigated)
    {
        if (loading) return;
        var token = lifetime.Token;
        loading = true; status.Text = "Loading build history…"; UpdateControls();
        try
        {
            var page = await readPage(before, token);
            if (token.IsCancellationRequested) return;
            var entries = page.GetProperty("entries").EnumerateArray().Select(BuildHistoryEntry.Read).ToArray();
            var skipped = page.GetProperty("skipped").GetInt32();
            navigated();
            current = before;
            next = page.GetProperty("next_before").GetString();
            list.ItemsSource = entries;
            list.SelectedIndex = entries.Length == 0 ? -1 : 0;
            status.Text = skipped > 0 ? $"{skipped} unavailable or unfinished entries on this page." :
                entries.Length == 0 ? "No completed playtests here." : "Choose a build to review its checks or restore a passing version.";
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (Exception failure) { if (!token.IsCancellationRequested) status.Text = failure.Message; }
        finally { loading = false; if (!token.IsCancellationRequested) UpdateControls(); }
    }

    private async Task ViewAsync()
    {
        if (loading || list.SelectedItem is not BuildHistoryEntry entry) return;
        var token = lifetime.Token;
        loading = true; UpdateControls();
        try
        {
            using var report = await entry.Attempt.ReadReportAsync();
            if (!token.IsCancellationRequested)
                await new PlaytestResultsWindow(entry.Attempt, report.RootElement, latestBuild, latestRevision).ShowDialog(this);
        }
        catch (Exception failure) { if (!token.IsCancellationRequested) status.Text = failure.Message; }
        finally { loading = false; if (!token.IsCancellationRequested) UpdateControls(); }
    }

    private void UpdateControls()
    {
        list.IsEnabled = !loading;
        newer.IsEnabled = !loading && newerPages.Count != 0;
        older.IsEnabled = !loading && next is not null;
        view.IsEnabled = !loading && list.SelectedItem is BuildHistoryEntry;
        restore.IsEnabled = !loading && list.SelectedItem is BuildHistoryEntry { Attempt.Retained: true };
    }

    private static TextBlock Text(string text) => new()
    {
        Text = text, TextWrapping = TextWrapping.Wrap, Foreground = EditorTheme.Brush("#B4ACB2")
    };
}
