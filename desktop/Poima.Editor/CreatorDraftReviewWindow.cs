// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace Poima.Editor;

internal sealed class CreatorDraftReviewWindow : Window
{
    public CreatorDraftReviewWindow(long revision, CreatorBuildTeam.ReviewedDraft[] drafts)
    {
        Title = "Review worker scenes";
        Width = 580; Height = 475; MinWidth = 420; MinHeight = 340;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter");
        FontSize = 14;
        Background = EditorTheme.Brush("#211016");
        Foreground = EditorTheme.Brush("#F7E8EB");
        UseLayoutRounding = true;
        var layout = new Grid { RowDefinitions = new RowDefinitions("Auto,*,Auto"), Margin = new Thickness(20), RowSpacing = 16 };
        layout.Children.Add(new TextBlock
        {
            Text = $"The combined build starts from revision {revision}. Choose which worker scenes to check together. Your current project stays open.",
            TextWrapping = TextWrapping.Wrap
        });
        var rows = new StackPanel { Spacing = 12 };
        var scroll = new ScrollViewer { Content = rows };
        Grid.SetRow(scroll, 1);
        layout.Children.Add(scroll);
        var build = new Button { Content = "Check combined game", IsEnabled = false, IsDefault = true };
        var cancel = new Button { Content = "Cancel", IsCancel = true };
        var actions = new StackPanel
        {
            Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Spacing = 8,
            Children = { cancel, build }
        };
        Grid.SetRow(actions, 2);
        layout.Children.Add(actions);
        var choices = new Dictionary<CheckBox, CreatorBuildTeam.ReviewedDraft>();
        foreach (var draft in drafts)
        {
            var choice = new CheckBox { Content = "Include " + draft.Name };
            ToolTip.SetTip(choice, draft.WorldHash);
            choices.Add(choice, draft);
            choice.IsCheckedChanged += (_, _) => build.IsEnabled = choices.Keys.Any(box => box.IsChecked == true);
            rows.Children.Add(new StackPanel
            {
                Spacing = 4,
                Children =
                {
                    choice,
                    new TextBlock { Text = $"Draft revision {draft.Revision} ({draft.WorldHash[..12]})", Foreground = EditorTheme.Brush("#EBC7CE") },
                    new TextBox { Text = draft.Report, IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, MaxHeight = 132 }
                }
            });
        }
        build.Click += (_, _) => Close(choices.Where(choice => choice.Key.IsChecked == true).Select(choice => choice.Value).ToArray());
        cancel.Click += (_, _) => Close();
        Content = layout;
    }
}
