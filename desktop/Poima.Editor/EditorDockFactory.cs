// SPDX-License-Identifier: Apache-2.0
using System;
using System.Collections.Generic;
using Avalonia.Controls;
using Avalonia.Controls.Templates;
using Dock.Avalonia.Controls;
using Dock.Model.Controls;
using Dock.Model.Core;
using Dock.Model.Mvvm;
using Dock.Model.Mvvm.Controls;

namespace Poima.Editor;

public sealed class EditorPanel : Document
{
    public required Func<Control> BuildView { get; init; }
}
public sealed class EditorPanelTemplate : IDataTemplate
{
    public bool Match(object? data) => data is EditorPanel;
    public Control? Build(object? data) => (data as EditorPanel)?.BuildView();
}
public sealed class EditorDockFactory : Factory
{
    private readonly Func<string, Control> build;
    public Dictionary<string, EditorPanel> Panels { get; } = new(StringComparer.Ordinal);
    public EditorDockFactory(Func<string, Control> build) { this.build = build; }
    private EditorPanel Panel(string name)
    {
        var panel = new EditorPanel { Id = name, Title = name, CanClose = false, CanPin = false, BuildView = () => build(name) };
        Panels[name] = panel; return panel;
    }
    public override IRootDock CreateLayout()
    {
        var hierarchy = Panel("Hierarchy"); var scene = Panel("Scene"); var inspector = Panel("Inspector");
        var project = Panel("Project"); var console = Panel("Console");
        DocumentDock Pane(double proportion, params EditorPanel[] panels) => new()
        {
            Proportion = proportion, CanCreateDocument = false, EnableWindowDrag = true,
            VisibleDockables = CreateList<IDockable>(panels), ActiveDockable = panels[0]
        };
        var upper = new ProportionalDock
        {
            Orientation = Orientation.Horizontal, Proportion = .72,
            VisibleDockables = CreateList<IDockable>(Pane(.23, hierarchy), new ProportionalDockSplitter(), Pane(.77, scene))
        };
        var left = new ProportionalDock
        {
            Orientation = Orientation.Vertical, Proportion = .75,
            VisibleDockables = CreateList<IDockable>(upper, new ProportionalDockSplitter(), Pane(.28, project, console))
        };
        var main = new ProportionalDock
        {
            Orientation = Orientation.Horizontal,
            VisibleDockables = CreateList<IDockable>(left, new ProportionalDockSplitter(), Pane(.25, inspector))
        };
        var root = CreateRootDock(); root.Id = "PoimaRoot"; root.Title = "Poima"; root.IsCollapsable = false;
        root.VisibleDockables = CreateList<IDockable>(main); root.ActiveDockable = main; root.DefaultDockable = main;
        return root;
    }
    public override void InitLayout(IDockable layout)
    {
        HostWindowLocator = new Dictionary<string, Func<IHostWindow?>> { [nameof(IDockWindow)] = () => new HostWindow() };
        base.InitLayout(layout);
    }
}
