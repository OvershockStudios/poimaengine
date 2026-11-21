// SPDX-License-Identifier: Apache-2.0
using System.Security.Cryptography;
using System.Text.Json;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace Poima.Editor;

internal sealed record PlaytestAttempt(string Report, string Sha256, string Build, long Revision, int PassedGroups,
    int TotalGroups, bool Passed, bool Retained)
{
    public static PlaytestAttempt FromHistory(JsonElement result) => new(
        result.GetProperty("report").GetString()!, result.GetProperty("report_sha256").GetString()!,
        result.GetProperty("bundle_sha256").GetString()!, result.GetProperty("revision").GetInt64(),
        result.GetProperty("groups_passed").GetInt32(), result.GetProperty("groups_total").GetInt32(),
        result.GetProperty("passed").GetBoolean(), result.GetProperty("retained").GetBoolean());

    public string Summary => Passed
        ? $"Revision {Revision} passed all {TotalGroups} check groups."
        : $"Revision {Revision} passed {PassedGroups} of {TotalGroups} check groups.";

    public async Task<JsonDocument> ReadReportAsync()
    {
        await using var stream = new FileStream(Report, FileMode.Open, FileAccess.Read, FileShare.Read,
            bufferSize: 8192, FileOptions.Asynchronous | FileOptions.SequentialScan);
        var size = stream.Length;
        if (size is < 1 or > 4 * 1024 * 1024) throw new InvalidDataException("The playtest report has an invalid size.");
        var bytes = new byte[(int)size];
        await stream.ReadExactlyAsync(bytes);
        if (stream.ReadByte() != -1 || !Convert.ToHexString(SHA256.HashData(bytes)).Equals(Sha256, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("The playtest report changed. Reopen the project to refresh its results.");
        return JsonDocument.Parse(bytes);
    }
}

internal sealed class PlaytestResultsWindow : Window
{
    public PlaytestResultsWindow(PlaytestAttempt attempt, JsonElement report, string? workingBuild, long workingRevision,
        bool approveBuild = false)
    {
        Title = "Playtest results";
        Width = 620;
        Height = 565;
        MinWidth = 420;
        MinHeight = 335;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        Background = EditorTheme.Brush("#19191C");
        Foreground = EditorTheme.Brush("#F2EDEE");
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter");
        FontSize = 14;
        UseLayoutRounding = true;

        var header = new StackPanel { Spacing = 6 };
        header.Children.Add(new TextBlock { Text = "Playtest results", FontSize = 23, FontWeight = FontWeight.SemiBold });
        header.Children.Add(Text(attempt.Summary));
        header.Children.Add(Text($"Tested build {attempt.Build[..12]}", muted: true));
        if (approveBuild)
            header.Children.Add(Text(workingBuild is null
                ? "Approve this build to make it playable from the creator."
                : $"Approve this build to replace the current playable build ({workingBuild[..12]}).", muted: true));
        else if (!attempt.Retained && workingBuild is not null)
            header.Children.Add(Text($"Revision {workingRevision} is still playable ({workingBuild[..12]}).", muted: true));
        else if (attempt.Passed && !attempt.Retained)
            header.Children.Add(Text("Use Export build to save a playable copy.", muted: true));

        var groups = new StackPanel { Spacing = 8, Margin = new Thickness(0, 0, 8, 0) };
        foreach (var group in report.GetProperty("groups").EnumerateArray())
        {
            var passed = group.GetProperty("passed").GetBoolean();
            var checks = new StackPanel { Spacing = 8 };
            foreach (var check in group.GetProperty("checks").EnumerateArray())
            {
                var checkPassed = check.GetProperty("passed").GetBoolean();
                var label = check.GetProperty("label").GetString();
                checks.Children.Add(Text($"{label} ({(checkPassed ? "passed" : "failed")})"));
                if (!checkPassed)
                {
                    var observed = Observation(check);
                    checks.Children.Add(Text($"Tick {check.GetProperty("tick").GetInt64()}. {observed}", muted: true));
                }
            }
            if (group.TryGetProperty("issue", out var issue))
                checks.Children.Add(Text($"Playtest stopped. {issue.GetProperty("message").GetString()}", muted: true));
            var name = $"{group.GetProperty("name").GetString()} ({(passed ? "passed" : "failed")})";
            var section = new Expander
            {
                Header = Text(name),
                Content = checks,
                IsExpanded = !passed,
                HorizontalAlignment = HorizontalAlignment.Stretch,
                HorizontalContentAlignment = HorizontalAlignment.Stretch,
                Background = EditorTheme.Brush("#232125"),
            };
            AutomationProperties.SetName(section, name);
            groups.Children.Add(section);
        }
        var scroll = new ScrollViewer { Content = groups, HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled };
        var done = new Button { Content = approveBuild ? "Keep for later" : "Done", IsCancel = true };
        done.Click += (_, _) => Close();
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8,
            HorizontalAlignment = HorizontalAlignment.Right, Children = { done } };
        if (approveBuild)
        {
            var approve = new Button { Content = "Approve this build" };
            approve.Click += (_, _) => Close(true);
            actions.Children.Add(approve);
        }
        var layout = new Grid { RowDefinitions = new RowDefinitions("Auto,*,Auto"), RowSpacing = 16, Margin = new Thickness(20) };
        layout.Children.Add(header);
        Grid.SetRow(scroll, 1);
        layout.Children.Add(scroll);
        Grid.SetRow(actions, 2);
        layout.Children.Add(actions);
        Content = layout;
    }

    private static TextBlock Text(string text, bool muted = false) => new()
    {
        Text = text, TextWrapping = TextWrapping.Wrap,
        Foreground = EditorTheme.Brush(muted ? "#B4ACB2" : "#F2EDEE"),
    };

    private static string Observation(JsonElement check)
    {
        if (!check.GetProperty("observed").GetBoolean()) return "The expected value was missing.";
        if (check.GetProperty("actual_omitted").GetBoolean()) return "The recorded value was too large to show.";
        var actual = check.GetProperty("actual").GetRawText();
        return $"Observed {(actual.Length > 200 ? actual[..200] + "…" : actual)}";
    }
}
