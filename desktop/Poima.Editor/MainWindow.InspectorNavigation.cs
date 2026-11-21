// SPDX-License-Identifier: Apache-2.0
using System;
using System.Linq;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;

namespace Poima.Editor;

public sealed partial class MainWindow
{
    private Control BuildInspectorNavigation()
    {
        var row = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("Auto,Auto,*,Auto,*"),
            Margin = new Thickness(4, 2),
            ColumnSpacing = 3
        };
        var history = new StackPanel { Orientation = Orientation.Horizontal };
        var back = ToolButton("Inspector selection back", () => Model.NavigateSelection(forward: false), "back");
        var forward = ToolButton("Inspector selection forward", () => Model.NavigateSelection(forward: true), "forward");
        history.Children.Add(back);
        history.Children.Add(forward);
        row.Children.Add(history);

        var ancestorMenu = new ContextMenu();
        var ancestors = ToolButton("Show Inspector ancestors", () => { }, "context");
        ancestors.ContextMenu = ancestorMenu;
        ancestors.Click += (_, _) => ancestorMenu.Open(ancestors);
        Grid.SetColumn(ancestors, 1);
        row.Children.Add(ancestors);

        EntityRow? parent = null;
        var parentLabel = Label("", muted: true);
        var parentButton = Button("", () =>
        {
            if (parent is not null) Model.Select(parent.Id);
        });
        parentButton.Classes.Add("toolbar-button");
        parentButton.Content = parentLabel;
        parentButton.HorizontalAlignment = HorizontalAlignment.Stretch;
        AutomationProperties.SetName(parentButton, "Inspect parent object");
        Grid.SetColumn(parentButton, 2);
        row.Children.Add(parentButton);

        var separator = EditorIcons.Make("chevron", 10);
        separator.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(separator, 3);
        row.Children.Add(separator);

        var current = Label("");
        row.Children.Add(current);

        (string? Selection, long Revision)? shown = null;
        void Refresh()
        {
            back.IsEnabled = Model.CanNavigateSelection(forward: false);
            forward.IsEnabled = Model.CanNavigateSelection(forward: true);
            parentButton.IsEnabled = ancestors.IsEnabled = !Model.SelectionNeedsRefresh;

            var context = (Model.Selected, Model.Revision);
            if (shown == context) return;
            shown = context;
            var path = Model.SelectionPath();
            parent = path.Count > 1 ? path[^2] : null;
            parentButton.IsVisible = separator.IsVisible = parent is not null;
            parentLabel.Text = parent?.Name;
            ToolTip.SetTip(parentButton, parent is null ? null : "Inspect " + parent.Name);

            ancestors.IsVisible = path.Count > 2;
            ancestorMenu.ItemsSource = path.Take(Math.Max(0, path.Count - 2)).Select(entity =>
            {
                var item = new MenuItem { Header = entity.Name };
                item.Click += (_, _) => Run(() => Model.Select(entity.Id));
                return item;
            }).ToArray();

            current.Text = path.Count > 0 ? path[^1].Name : "No object selected";
            ToolTip.SetTip(current, path.Count > 0 ? string.Join(" › ", path.Select(entity => entity.Name)) : null);
            Grid.SetColumn(current, parent is null ? 2 : 4);
            Grid.SetColumnSpan(current, parent is null ? 3 : 1);
        }
        Observe(row, Model, Refresh);
        Refresh();
        return row;
    }
}
