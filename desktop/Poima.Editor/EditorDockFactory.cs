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
        Panels.Clear();
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
    /// <summary>Builds the restored model only. InitLayout attaches owners and presents its floating windows.</summary>
    public IRootDock RestoreLayout(EditorLayoutDocument document, Func<EditorFloatBounds, EditorFloatBounds>? clamp = null)
    {
        EditorLayoutStore.Validate(document);
        if (document.Floating.Count != 0 && clamp is null)
            throw new ArgumentException("Restoring floating windows requires current-display bounds clamping.", nameof(clamp));
        // Validate every clamped rectangle before creating any panel or window.
        var bounds = document.Floating.Select(window => clamp!(window.Bounds)).ToArray();
        foreach (var value in bounds) EditorLayoutStore.ValidateBounds(value);
        Panels.Clear();
        IDockable Restore(EditorLayoutNode node)
        {
            if (node is EditorTabLayout tabs)
            {
                var panels = tabs.Panels.Select(Panel).ToArray();
                var active = panels.Single(panel => panel.Id == tabs.Active);
                return new DocumentDock { Proportion = tabs.Proportion, CanCreateDocument = false, EnableWindowDrag = true,
                    VisibleDockables = CreateList<IDockable>(panels), ActiveDockable = active, DefaultDockable = active };
            }
            var split = (EditorSplitLayout)node;
            var children = new List<IDockable>();
            foreach (var child in split.Children)
            {
                if (children.Count != 0) children.Add(new ProportionalDockSplitter());
                children.Add(Restore(child));
            }
            return new ProportionalDock { Proportion = split.Proportion,
                Orientation = split.Orientation == "horizontal" ? Orientation.Horizontal : Orientation.Vertical,
                VisibleDockables = CreateList(children.ToArray()) };
        }
        IRootDock Root(EditorLayoutNode? node, string id)
        {
            var result = CreateRootDock(); result.Id = id; result.Title = "Poima"; result.IsCollapsable = false;
            result.VisibleDockables = CreateList<IDockable>();
            if (node is not null)
            {
                var child = Restore(node); result.VisibleDockables.Add(child);
                result.ActiveDockable = child; result.DefaultDockable = child;
            }
            return result;
        }
        var root = Root(document.Main, "PoimaRoot"); root.Windows = CreateList<IDockWindow>();
        for (var i = 0; i < document.Floating.Count; ++i)
        {
            var floating = Root(document.Floating[i].Content, "PoimaFloating"+i);
            var window = CreateDockWindow(); window.Id = nameof(IDockWindow); window.Title = "Poima";
            window.X = bounds[i].X; window.Y = bounds[i].Y; window.Width = bounds[i].Width; window.Height = bounds[i].Height;
            window.WindowState = DockWindowState.Normal; window.Layout = floating; floating.Window = window;
            root.Windows.Add(window);
        }
        return root;
    }

    /// <summary>Capture before CloseLayout: Dock clears floating collections during teardown.</summary>
    public EditorLayoutDocument CaptureLayout(IRootDock root)
    {
        var visited = new HashSet<IDockable>(ReferenceEqualityComparer.Instance);
        double Weight(IDockable dock) => double.IsFinite(dock.Proportion) && dock.Proportion > 0 ? dock.Proportion : 1;
        EditorLayoutNode Reweight(EditorLayoutNode node, double weight) => node switch
        {
            EditorTabLayout tabs => new EditorTabLayout(weight, tabs.Panels, tabs.Active),
            EditorSplitLayout split => new EditorSplitLayout(weight, split.Orientation, split.Children),
            _ => throw new InvalidDataException("Unsupported layout node.")
        };
        EditorLayoutNode? Capture(IDockable item, int depth)
        {
            if (depth > 16 || visited.Count >= 64 || !visited.Add(item))
                throw new InvalidDataException("Live layout is cyclic or exceeds supported bounds.");
            if (item is EditorPanel panel) return new EditorTabLayout(Weight(panel), new[] { panel.Id }, panel.Id);
            if (item is not IDock dock) throw new InvalidDataException("Unsupported live dock item: "+item.GetType().Name);
            var children = dock.VisibleDockables?.Where(child => child is not IProportionalDockSplitter).ToArray() ?? [];
            if (children.Length == 0) return null;
            if (dock is IDocumentDock or IToolDock)
            {
                if (children.Any(child => child is not EditorPanel)) throw new InvalidDataException("Unsupported nested tab group.");
                var names = children.Select(child => child.Id).ToArray();
                var active = dock.ActiveDockable?.Id;
                return new EditorTabLayout(Weight(dock), names, active is not null && names.Contains(active) ? active : names[0]);
            }
            var captured = children.Select(child => Capture(child, depth+1)).Where(child => child is not null).Cast<EditorLayoutNode>().ToArray();
            if (captured.Length == 0) return null;
            if (captured.Length == 1) return Reweight(captured[0], Weight(dock));
            if (dock is not IProportionalDock split) throw new InvalidDataException("Only proportional splits are supported in saved layouts.");
            return new EditorSplitLayout(Weight(split), split.Orientation == Orientation.Horizontal ? "horizontal" : "vertical", captured);
        }
        var windows = new List<EditorFloatingLayout>();
        var seenWindows = new HashSet<IDockWindow>(ReferenceEqualityComparer.Instance);
        void Floating(IRootDock parent, int depth)
        {
            if (depth > 5) throw new InvalidDataException("Floating window nesting exceeds supported bounds.");
            if (parent.Windows is null) return;
            foreach (var window in parent.Windows)
            {
                if (!seenWindows.Add(window) || seenWindows.Count > 5 || window.Layout is null)
                    throw new InvalidDataException("Invalid floating window collection.");
                window.Save();
                var node = Capture(window.Layout, 0) ?? throw new InvalidDataException("Cannot save an empty floating window.");
                windows.Add(new(node, new(window.X, window.Y, window.Width, window.Height)));
                Floating(window.Layout, depth+1);
            }
        }
        var main = Capture(root, 0); Floating(root, 0);
        var document = new EditorLayoutDocument(main, windows); EditorLayoutStore.Validate(document); return document;
    }
    public override void InitLayout(IDockable layout)
    {
        HostWindowLocator = new Dictionary<string, Func<IHostWindow?>> { [nameof(IDockWindow)] = () => new HostWindow() };
        base.InitLayout(layout);
    }
}
