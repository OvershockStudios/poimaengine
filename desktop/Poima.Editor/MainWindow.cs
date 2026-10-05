// SPDX-License-Identifier: Apache-2.0
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Templates;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using Dock.Avalonia.Controls;
using Dock.Model.Controls;

namespace Poima.Editor;

public sealed class MainWindow : Window
{
    public EditorModel Model { get; }
    public SceneNavigation Navigation { get; }
    private readonly EditorLayoutStore layoutStore;
    public string? LayoutError { get; private set; }
    public string LayoutPath => layoutStore.FilePath;
    private readonly DockControl dock;
    private EditorDockFactory factory;
    private IRootDock layout;
    private readonly TextBlock status = new();
    private readonly Button start, step, undo, redo;
    private readonly string projectRoot;
    private readonly Dictionary<string, string> assetNames = new();
    private bool disposed;
    private bool qualificationClosing;
    public MainWindow(NativeHost host, string? layoutFile = null)
    {
        Title = $"{System.IO.Path.GetFileNameWithoutExtension(host.WorldPath)} — Poima";
        Width = 1440; Height = 900; MinWidth = 960; MinHeight = 620;
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter");
        FontSize = 12; UseLayoutRounding = true;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        Opened += (_, _) =>
        {
            var screen = Screens.ScreenFromWindow(this);
            if (screen is null) return;
            Width = Math.Min(Width, screen.WorkingArea.Width / RenderScaling * .94);
            Height = Math.Min(Height, screen.WorkingArea.Height / RenderScaling * .9);
            Position = new PixelPoint(screen.WorkingArea.X + (int)((screen.WorkingArea.Width-Width*RenderScaling)/2),
                screen.WorkingArea.Y + (int)((screen.WorkingArea.Height-Height*RenderScaling)/2));
        };
        Application.Current!.Styles.Add(new EditorTheme());
        Application.Current.DataTemplates.Add(new EditorPanelTemplate());
        Model = new EditorModel(host);
        Navigation = new SceneNavigation(Model);
        Navigation.Error += text => status.Text = text;
        projectRoot = FindProjectRoot(host.WorldPath);
        layoutStore = new EditorLayoutStore(projectRoot, layoutFile);
        Opened += (_, _) => { if (layoutStore.TryLoad(out var saved, out var error)) Run(() => RestoreLayout(saved!)); else if (error is not null) { LayoutError = error; Model.Note(error); } };
        factory = new EditorDockFactory(BuildPanel); layout = factory.CreateLayout(); factory.InitLayout(layout);
        dock = new DockControl { Factory = factory, Layout = layout, InitializeFactory = false, InitializeLayout = false };
        var root = new Grid { RowDefinitions = new RowDefinitions("22,30,*,20") };
        var menu = BuildMenu(); Grid.SetRow(menu, 0); root.Children.Add(menu);
        var toolbar = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto,*"), Background = EditorTheme.Brush("#222327") };
        var left = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2, Margin = new Thickness(5, 3) };
        var create = Button("Create", () => {}, "plus");
        create.ContextMenu = new ContextMenu { ItemsSource = new[] { CreateItem("Empty object", "Entity"), CreateItem("Cube", "Cube"), CreateItem("Camera", "Camera"), CreateItem("Point light", "Light") } };
        create.Click += (_, _) => create.ContextMenu.Open(create);
        left.Children.Add(create);
        left.Children.Add(new Border { Width = 1, Height = 16, Margin = new Thickness(5, 2), Background = EditorTheme.Brush("#3B3C42") });
        undo = ToolButton("Undo", () => Model.History(false), "undo"); redo = ToolButton("Redo", () => Model.History(true), "redo");
        left.Children.Add(undo); left.Children.Add(redo); toolbar.Children.Add(left);
        var transport = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 1, Margin = new Thickness(0, 3) };
        start = Button("", Model.PlayStop, "play"); ToolTip.SetTip(start, "Start simulation paused / stop simulation");
        start.Width = 30; start.Classes.Add("transport");
        AutomationProperties.SetName(start, "Start or stop simulation");
        step = Button("", () => Model.Step(), "step"); ToolTip.SetTip(step, "Advance simulation by one fixed tick"); AutomationProperties.SetName(step, "Step simulation");
        step.Width = 30; step.Classes.Add("transport");
        transport.Children.Add(start); transport.Children.Add(step); Grid.SetColumn(transport, 1); toolbar.Children.Add(transport);
        var projectLabel = Label(System.IO.Path.GetFileName(projectRoot), true); projectLabel.Margin = new Thickness(8); projectLabel.HorizontalAlignment = HorizontalAlignment.Right;
        Grid.SetColumn(projectLabel, 2); toolbar.Children.Add(projectLabel); Grid.SetRow(toolbar, 1); root.Children.Add(toolbar);
        Grid.SetRow(dock, 2); root.Children.Add(dock);
        status.FontSize = 11; status.Foreground = EditorTheme.Brush("#A0A0A5");
        var footer = new Border { Background = EditorTheme.Brush("#222327"), BorderBrush = EditorTheme.Brush("#1B1C20"), BorderThickness = new Thickness(0, 1, 0, 0), Padding = new Thickness(8, 1), Child = status };
        Grid.SetRow(footer, 3); root.Children.Add(footer); Content = root;
        Model.Changed += UpdateToolbar; UpdateToolbar(null, EventArgs.Empty);
        KeyDown += (_, e) =>
        {
            if (e.Source is TextBox) return;
            if (e.Key == Key.F && e.KeyModifiers == KeyModifiers.None) { Run(Navigation.FrameSelection); e.Handled = true; }
            if (!Navigation.Flying && e.KeyModifiers == KeyModifiers.None && e.Key is Key.Q or Key.W or Key.E or Key.R)
            {
                Run(() => Navigation.ConfigureGizmo(e.Key switch { Key.Q => "none", Key.W => "move", Key.E => "rotate", _ => "scale" }));
                e.Handled = true;
            }
            if (e.KeyModifiers == KeyModifiers.Control && e.Key is Key.Z or Key.Y)
            { Run(() => Model.History(e.Key == Key.Y)); e.Handled = true; }
        };
        Closing += (_, e) =>
        {
            if (!qualificationClosing && Model.Dirty)
            {
                e.Cancel = true;
                const string message = "Apply or Reload the Inspector changes before closing.";
                Model.Note(message); status.Text = message;
            }
            if (!e.Cancel) { Navigation.Cancel(); try { SaveLayout(); } catch (Exception error) { LayoutError = error.Message; } }
        };
        Closed += (_, _) => { if (!disposed) { disposed = true; Model.Changed -= UpdateToolbar; CloseLayout(); Model.Dispose(); } };
    }
    internal void CloseQualification() { qualificationClosing = true; Close(); }
    public JsonObject InspectDraft() => Model.InspectDraft();
    private MenuItem CreateItem(string label, string kind)
    {
        var item = new MenuItem { Header = label }; item.Click += (_, _) => Run(() => Model.Create(kind)); return item;
    }
    private Button ToolButton(string label, Action action, string icon)
    {
        var button = Button("", action, icon); button.Classes.Add("toolbar-button"); button.Width = 26;
        AutomationProperties.SetName(button, label); ToolTip.SetTip(button, label); return button;
    }
    private static TextBox SearchField(string placeholder) => new() {
        PlaceholderText = placeholder, FontSize = 12, MinHeight = 20, Height = 20,
        InnerLeftContent = new Border { Margin = new Thickness(5, 0, 0, 0), Child = EditorIcons.Make("search", 12) }
    };
    public void SelectEntity(string id) => Model.Select(id);
    public void FloatPanel(string name)
    {
        if (!factory.Panels.TryGetValue(name, out var panel)) throw new ArgumentException("Unknown editor panel.", nameof(name));
        Navigation.Cancel();
        factory.FloatDockable(panel);
    }
    private static string FindProjectRoot(string world)
    {
        var fallback = System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(world))!;
        var directory = new DirectoryInfo(fallback);
        for (int depth = 0; depth < 16 && directory is not null; ++depth, directory = directory.Parent)
            if (File.Exists(System.IO.Path.Combine(directory.FullName, "project.json"))) return directory.FullName;
        return fallback;
    }
    public void ResetLayout()
    {
        CloseLayout(); factory = new EditorDockFactory(BuildPanel); layout = factory.CreateLayout(); factory.InitLayout(layout);
        dock.Factory = factory; dock.Layout = layout;
    }
    public JsonObject InspectLayout() => EditorLayoutStore.Encode(factory.CaptureLayout(layout));
    public void SaveLayout()
    {
        if (!layoutStore.TrySave(factory.CaptureLayout(layout), out var error)) { LayoutError = error; throw new IOException(error); }
        LayoutError = null;
    }
    public void LoadLayout()
    {
        if (!layoutStore.TryLoad(out var saved, out var error)) { LayoutError = error; throw new IOException(error ?? "No saved layout exists yet."); }
        RestoreLayout(saved!); LayoutError = null;
    }
    private EditorFloatBounds ClampFloating(EditorFloatBounds bounds)
    {
        var origin = new PixelPoint((int)bounds.X, (int)bounds.Y);
        var screen = Screens.ScreenFromPoint(origin) ?? Screens.ScreenFromWindow(this) ?? Screens.Primary;
        if (screen is null) return new EditorFloatBounds(Position.X+40,Position.Y+40,Math.Min(bounds.Width,800),Math.Min(bounds.Height,600));
        var area = screen.WorkingArea; var scale = screen.Scaling;
        var width = Math.Clamp(bounds.Width, Math.Min(240,area.Width/scale), area.Width/scale);
        var height = Math.Clamp(bounds.Height, Math.Min(160,area.Height/scale), area.Height/scale);
        return new EditorFloatBounds(Math.Clamp(bounds.X,area.X,Math.Max(area.X,area.Right-width*scale)),
            Math.Clamp(bounds.Y,area.Y,Math.Max(area.Y,area.Bottom-height*scale)),width,height);
    }
    private void RestoreLayout(EditorLayoutDocument saved)
    {
        var next = new EditorDockFactory(BuildPanel); var restored = next.RestoreLayout(saved, ClampFloating);
        CloseLayout(); factory = next; layout = restored; factory.InitLayout(layout); dock.Factory = factory; dock.Layout = layout;
    }
    private void CloseLayout() { Navigation.Cancel(); if (layout.Close.CanExecute(null)) layout.Close.Execute(null); }
    private void UpdateToolbar(object? sender, EventArgs args)
    {
        status.Text = Model.Host.LastError is string error ? "Scene: " + error : $"{(Model.RuntimeId is null ? "Ready" : $"Simulation paused · tick {Model.Tick}")}     ·     {Model.Entities.Count} objects     ·     Revision {Model.Revision}";
        start.Content = EditorIcons.Make(Model.RuntimeId is null ? "play" : "stop");
        step.IsEnabled = Model.RuntimeId is not null;
        undo.IsEnabled = Model.UndoDepth > 0 && Model.RuntimeId is null;
        redo.IsEnabled = Model.RedoDepth > 0 && Model.RuntimeId is null;
    }
    private void Run(Action action)
    {
        try { action(); }
        catch (Exception e) { Model.Note(e.Message); status.Text = e.Message; }
    }
    private Button Button(string text, Action action, string? icon = null)
    {
        Control content = Label(text);
        if (icon is not null)
        {
            var panel = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5 };
            panel.Children.Add(EditorIcons.Make(icon)); if (text.Length != 0) panel.Children.Add(content); content = panel;
        }
        var button = new Button { Content = content }; button.Classes.Add("editor-button"); button.Click += (_, _) => Run(action); return button;
    }
    private static TextBlock Label(string text, bool muted = false) => new() { Text = text, VerticalAlignment = VerticalAlignment.Center, Foreground = EditorTheme.Brush(muted ? "#A0A0A5" : "#D6D6D9"), TextTrimming = TextTrimming.CharacterEllipsis };
    private Menu BuildMenu()
    {
        MenuItem Item(string title, Action action) { var item = new MenuItem { Header = title }; item.Click += (_, _) => Run(action); return item; }
        return new Menu { Background = EditorTheme.Brush("#222327"), ItemsSource = new[]
        {
            new MenuItem { Header = "_File", ItemsSource = new[] { Item("Import model…", () => _ = PickModel()), Item("Refresh", Model.Refresh), Item("Close", Close) } },
            new MenuItem { Header = "_Edit", ItemsSource = new[] { Item("Undo", () => Model.History(false)), Item("Redo", () => Model.History(true)), Item("Apply Inspector", Model.Apply), Item("Reload Inspector", Model.Reload), Item("Delete selected", Model.Delete), Item("Frame selected", Navigation.FrameSelection) } },
            new MenuItem { Header = "_GameObject", ItemsSource = new[] { Item("Create Empty", () => Model.Create("Entity")), Item("3D Object / Cube", () => Model.Create("Cube")), Item("Camera", () => Model.Create("Camera")), Item("Point Light", () => Model.Create("Light")) } },
            new MenuItem { Header = "_Window", ItemsSource = new[] { Item("Save layout", SaveLayout), Item("Restore saved layout", LoadLayout), Item("Reset layout", ResetLayout) } },
            new MenuItem { Header = "_Help", ItemsSource = new[] { Item("Prototype capabilities", () => Model.Note("Dock tabs can split or float. Inspector uses guarded Apply. Simulation starts paused; Step advances one tick. glTF/GLB model import is supported. Asset previews are type icons, not rendered thumbnails.")) } }
        } };
    }
    private Control BuildPanel(string name) => name switch
    {
        "Hierarchy" => BuildHierarchy(), "Inspector" => BuildInspector(), "Project" => BuildProject(),
        "Console" => BuildConsole(), _ => BuildScene()
    };
    private static void Observe(Control view, EditorModel model, Action refresh)
    {
        EventHandler listener = (_, _) => refresh();
        view.AttachedToVisualTree += (_, _) => { model.Changed += listener; refresh(); };
        view.DetachedFromVisualTree += (_, _) => model.Changed -= listener;
    }
    private Control BuildScene()
    {
        var grid = new Grid { RowDefinitions = new RowDefinitions("24,*") };
        var controls = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 3 };
        var modes = new Dictionary<string, Button>();
        foreach (var (mode, title, key) in new[] { ("none", "Select", "Q"), ("move", "Move", "W"), ("rotate", "Rotate", "E"), ("scale", "Scale", "R") })
        {
            var button = Button(title, () => Navigation.ConfigureGizmo(mode));
            AutomationProperties.SetName(button, "Scene "+title); ToolTip.SetTip(button, title+" tool ("+key+")");
            modes.Add(mode, button); controls.Children.Add(button);
        }
        var space = Button("World", () => Navigation.ConfigureGizmo(Navigation.GizmoMode, Navigation.GizmoSpace == "world" ? "local" : "world"));
        AutomationProperties.SetName(space, "Scene transform space"); ToolTip.SetTip(space, "Toggle world/local axes. Ctrl snaps: 0.25 units, 15 degrees, 0.1 scale.");
        controls.Children.Add(space);
        controls.Children.Add(Button("Frame", Navigation.FrameSelection));
        ToolTip.SetTip(controls, "Q select · W move · E rotate · R scale · Ctrl snap · RMB + WASD fly · MMB pan · Alt orbit · F frame");
        void SyncTools(object? sender, EventArgs args)
        {
            foreach (var pair in modes)
                pair.Value.Background = EditorTheme.Brush(pair.Key == Navigation.GizmoMode ? "#34547B" : "#28292D");
            space.Content = Label(Navigation.GizmoSpace == "local" ? "Local" : "World");
        }
        Navigation.Changed += SyncTools; SyncTools(null, EventArgs.Empty);
        grid.DetachedFromVisualTree += (_, _) => Navigation.Changed -= SyncTools;
        grid.AttachedToVisualTree += (_, _) => { Navigation.Changed -= SyncTools; Navigation.Changed += SyncTools; SyncTools(null, EventArgs.Empty); };
        var bar = new Border { Background = EditorTheme.Brush("#28292D"), Padding = new Thickness(8, 1), Child = controls };
        grid.Children.Add(bar);
        var view = new VulkanView(Model.Host, Navigation); Grid.SetRow(view, 1); grid.Children.Add(view); return grid;
    }
    private sealed record HierarchyItem(EntityRow Entity, int Depth, int Index);
    private Control BuildHierarchy()
    {
        var root = new Grid { RowDefinitions = new RowDefinitions("28,*") };
        var search = SearchField("Search"); search.Margin = new Thickness(5, 4);
        AutomationProperties.SetName(search, "Search hierarchy"); root.Children.Add(search);
        var list = new ListBox { SelectionMode = SelectionMode.Single }; Grid.SetRow(list, 1); root.Children.Add(list);
        var syncing = false;
        var collapsed = new HashSet<string>();
        list.ItemTemplate = new FuncDataTemplate<HierarchyItem>((row, _) =>
        {
            if (row is null) return new TextBlock();
            var content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4, Margin = new Thickness(5 + row.Depth * 14, 0, 4, 0), Height = 20 };
            if (Model.Entities.Any(x => x.Parent == row.Entity.Id))
            {
                var expand = new Button { Content = EditorIcons.Make(collapsed.Contains(row.Entity.Id) ? "chevron" : "expanded", 10), Width = 12, MinWidth = 0, MinHeight = 16, Padding = new Thickness(0), BorderThickness = new Thickness(0), Background = Brushes.Transparent };
                AutomationProperties.SetName(expand, "Expand or collapse " + row.Entity.Name);
                expand.Click += (_, e) => { if (!collapsed.Add(row.Entity.Id)) collapsed.Remove(row.Entity.Id); Refresh(); e.Handled = true; };
                content.Children.Add(expand);
            }
            else content.Children.Add(new Border { Width = 12 });
            content.Children.Add(EditorIcons.Make(row.Entity.Icon, 14)); content.Children.Add(Label(row.Entity.Name));
            return new Border { Background = Model.Selected == row.Entity.Id ? EditorTheme.Brush("#34547B") : row.Index % 2 == 0 ? EditorTheme.Brush("#2B2C30") : EditorTheme.Brush("#27282C"), Child = content };
        });
        void Refresh()
        {
            var rows = new List<HierarchyItem>(); var visited = new HashSet<string>();
            var lookup = Model.Entities.ToLookup(e => e.Parent ?? "");
            void Append(EntityRow entity, int depth)
            {
                if (!visited.Add(entity.Id)) return;
                if (string.IsNullOrEmpty(search.Text) || entity.Name.Contains(search.Text, StringComparison.OrdinalIgnoreCase)) rows.Add(new(entity, depth, rows.Count));
                if (!collapsed.Contains(entity.Id) || !string.IsNullOrEmpty(search.Text)) foreach (var child in lookup[entity.Id]) Append(child, depth + 1);
            }
            foreach (var entity in lookup[""]) Append(entity, 0);
            syncing = true; list.ItemsSource = rows; list.SelectedItem = rows.FirstOrDefault(x => x.Entity.Id == Model.Selected); syncing = false;
        }
        search.TextChanged += (_, _) => Refresh();
        list.SelectionChanged += (_, _) => { if (!syncing && list.SelectedItem is HierarchyItem row) Run(() => { Model.Select(row.Entity.Id); }); };
        var create = new MenuItem { Header = "Create Cube" }; create.Click += (_, _) => Run(() => Model.Create("Cube"));
        var delete = new MenuItem { Header = "Delete selected" }; delete.Click += (_, _) => Run(Model.Delete);
        list.ContextMenu = new ContextMenu { ItemsSource = new[] { create, delete } };
        Observe(root, Model, Refresh); Refresh(); return root;
    }
    private Control BuildConsole()
    {
        var root = new Grid { RowDefinitions = new RowDefinitions("29,*") };
        root.Children.Add(Button("Clear", () => { Model.Log.Clear(); Model.Note("Console cleared."); }));
        var list = new ListBox { ItemTemplate = new FuncDataTemplate<string>((text, _) => new TextBlock { Text = text, Margin = new Thickness(8, 3), TextWrapping = TextWrapping.Wrap }) };
        Grid.SetRow(list, 1); root.Children.Add(list); Observe(root, Model, () => list.ItemsSource = Model.Log.ToArray()); return root;
    }
    private Control BuildInspector()
    {
        var root = new Grid { RowDefinitions = new RowDefinitions("*,Auto") };
        var fields = new StackPanel { Spacing = 0 }; root.Children.Add(new ScrollViewer { Content = fields, HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled });
        var footer = new StackPanel { Spacing = 5, Margin = new Thickness(8) };
        var message = new TextBlock { TextWrapping = TextWrapping.Wrap, FontSize = 12 };
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5 };
        actions.Children.Add(Button("Apply", Model.Apply)); actions.Children.Add(Button("Reload", Model.Reload));
        footer.Children.Add(message); footer.Children.Add(actions); Grid.SetRow(footer, 1); root.Children.Add(footer);
        string last = "";
        void Banner()
        {
            footer.IsVisible = Model.Dirty || Model.Conflict;
            message.Text = Model.Conflict ? "Changed externally. Reload or review your edits." : "Unapplied changes";
            message.Foreground = EditorTheme.Brush(Model.Conflict ? "#E2B476" : "#9B9B9B");
        }
        void Refresh()
        {
            Banner();
            var key = $"{Model.Selected}:{Model.DraftGeneration}";
            if (key == last) return; last = key; fields.Children.Clear();
            if (Model.Selected is null) { fields.Children.Add(new TextBlock { Text = "Select an object to inspect its components.", Margin = new Thickness(16), Foreground = EditorTheme.Brush("#939393"), TextWrapping = TextWrapping.Wrap }); return; }
            var row = Model.Entities.FirstOrDefault(x => x.Id == Model.Selected);
            var name = new TextBox { Text = Model.DraftName, FontWeight = FontWeight.SemiBold };
            AutomationProperties.SetName(name, "Object name"); ObserveInspectorText(name, text => { Model.SetName(text); Banner(); });
            var header = new Grid { ColumnDefinitions = new ColumnDefinitions("28,*"), Margin = new Thickness(8, 10, 8, 10) };
            header.Children.Add(EditorIcons.Make(row?.Icon ?? "entity", 20)); Grid.SetColumn(name, 1); header.Children.Add(name); fields.Children.Add(header);
            foreach (var pair in Model.DraftComponents.OrderBy(x => x.Key == "Transform" ? "" : x.Key).ToArray())
            {
                if (pair.Value is not JsonObject component) continue;
                var section = new StackPanel { Spacing = 2, Margin = new Thickness(8, 5, 8, 7) };
                if (pair.Key == "Transform") BuildTransform(section, component, Banner);
                else if (pair.Key is "Camera" or "MeshRenderer" or "PbrMaterial" or "Light") BuildTypedComponent(section, pair.Key, component, Banner);
                else
                {
                    var json = new TextBox { Text = Model.FieldText(pair.Key, component.ToJsonString(new JsonSerializerOptions { WriteIndented = true })), AcceptsReturn = true, FontSize = 12, MinHeight = 80, FontFamily = new FontFamily("Consolas"), TextWrapping = TextWrapping.Wrap };
                    AutomationProperties.SetName(json, pair.Key + " component JSON");
                    var type = pair.Key;
                    ObserveInspectorText(json, text =>
                    {
                        Model.SetFieldText(type, text);
                        try { var value = JsonNode.Parse(text) as JsonObject ?? throw new FormatException(); Model.SetComponent(type, value); Model.SetInvalid(type, false); }
                        catch (Exception) { Model.SetInvalid(type, true); }
                        Banner();
                    });
                    section.Children.Add(Label("Component data · JSON", true)); section.Children.Add(json);
                }
                var title = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
                title.Children.Add(EditorIcons.Make(pair.Key switch { "Transform" => "transform", "Camera" => "camera", "Light" => "light", "MeshRenderer" => "cube", _ => "entity" }, 13));
                title.Children.Add(Label(pair.Key switch { "MeshRenderer" => "Mesh Renderer", "PbrMaterial" => "Material", "PbrTextures" => "Material Textures", _ => pair.Key }));
                fields.Children.Add(new Expander { Header = title, IsExpanded = pair.Key is "Transform" or "Camera" or "MeshRenderer" or "PbrMaterial" or "Light", Content = section, HorizontalAlignment = HorizontalAlignment.Stretch, Background = EditorTheme.Brush("#28292D"), BorderBrush = EditorTheme.Brush("#202125"), BorderThickness = new Thickness(0, 1, 0, 0), Padding = new Thickness(0) });
            }
        }
        Observe(root, Model, Refresh); Refresh(); return root;
    }
    private void ObserveInspectorText(TextBox control, Action<string> edited)
    {
        // Avalonia may deliver the initial TextChanged after this subscription.
        // Display formatting must never round authored values, round-trip an
        // untouched quaternion through Euler angles, or materialize defaults.
        // Compare text rather than focus so paste, automation and invalid raw
        // user edits follow the same path. Retired controls cannot edit a new draft.
        var presented = control.Text ?? "";
        var selected = Model.Selected;
        var generation = Model.DraftGeneration;
        control.TextChanged += (_, _) =>
        {
            if (Model.Selected != selected || Model.DraftGeneration != generation) return;
            var text = control.Text ?? "";
            if (string.Equals(text, presented, StringComparison.Ordinal)) return;
            presented = text;
            edited(text);
        };
    }
    private void BuildTransform(StackPanel panel, JsonObject source, Action changed)
    {
        var transform = EditorModel.Clone(source);
        double[] Read(string field) => transform[field]!.AsArray().Select(n => n!.GetValue<double>()).ToArray();
        var q = Read("rotation");
        var angles = new[] { Math.Atan2(2*(q[3]*q[0]+q[1]*q[2]), 1-2*(q[0]*q[0]+q[1]*q[1])), Math.Asin(Math.Clamp(2*(q[3]*q[1]-q[2]*q[0]), -1, 1)), Math.Atan2(2*(q[3]*q[2]+q[0]*q[1]), 1-2*(q[1]*q[1]+q[2]*q[2])) }.Select(v => v*180/Math.PI).ToArray();
        foreach (var (field, label) in new[] { ("position", "Position"), ("rotation", "Rotation"), ("scale", "Scale") })
        {
            var values = field == "rotation" ? angles : Read(field);
            var row = new Grid { ColumnDefinitions = new ColumnDefinitions("65,*,*,*"), ColumnSpacing = 4 };
            row.Children.Add(Label(label));
            for (int axis = 0; axis < 3; axis++)
            {
                var index = axis;
                var cell = new Grid { ColumnDefinitions = new ColumnDefinitions("11,*") };
                cell.Children.Add(new TextBlock { Text = "XYZ"[axis].ToString(), Foreground = EditorTheme.Brush(new[] { "#CD8888", "#91B58A", "#8FA8CA" }[axis]), FontSize = 11, VerticalAlignment = VerticalAlignment.Center });
                var edit = new TextBox { Text = Model.FieldText(field + index, values[axis].ToString("0.###", CultureInfo.InvariantCulture)), MinWidth = 32, FontSize = 12 };
                edit.BorderBrush = EditorTheme.Brush(Model.HasInvalid(field + index) ? "#BA6B60" : "#3B3C42");
                AutomationProperties.SetName(edit, $"{label} {"XYZ"[axis]}{(field == "rotation" ? " degrees" : "")}");
                ObserveInspectorText(edit, text =>
                {
                    Model.SetFieldText(field + index, text);
                    var valid = double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var value) && double.IsFinite(value);
                    Model.SetInvalid(field + index, !valid);
                    edit.BorderBrush = EditorTheme.Brush(valid ? "#3B3C42" : "#BA6B60");
                    if (valid)
                    {
                        values[index] = value;
                        double[] result = values;
                        if (field == "rotation")
                        {
                            var x=angles[0]*Math.PI/360; var y=angles[1]*Math.PI/360; var z=angles[2]*Math.PI/360;
                            var sx=Math.Sin(x); var cx=Math.Cos(x); var sy=Math.Sin(y); var cy=Math.Cos(y); var sz=Math.Sin(z); var cz=Math.Cos(z);
                            result = [sx*cy*cz-cx*sy*sz, cx*sy*cz+sx*cy*sz, cx*cy*sz-sx*sy*cz, cx*cy*cz+sx*sy*sz];
                        }
                        transform[field] = new JsonArray(result.Select(v => (JsonNode?)JsonValue.Create(v)).ToArray());
                        Model.SetComponent("Transform", transform);
                    }
                    changed();
                });
                Grid.SetColumn(edit, 1); cell.Children.Add(edit); Grid.SetColumn(cell, axis+1); row.Children.Add(cell);
            }
            panel.Children.Add(row);
        }
    }
    private void BuildTypedComponent(StackPanel panel, string type, JsonObject source, Action changed)
    {
        // Edit a complete copy: optional fields remain absent until explicitly
        // edited, and unrelated values survive each typed field change.
        var value = EditorModel.Clone(source);
        var selectedEntity = Model.Selected;
        var draftGeneration = Model.DraftGeneration;
        bool CurrentDraft() => Model.Selected == selectedEntity && Model.DraftGeneration == draftGeneration;
        double Number(JsonObject owner, string key, double fallback = 0) => owner[key]?.GetValue<double>() ?? fallback;
        void Commit()
        {
            bool relations = true;
            if (type == "Camera") relations = Number(value, "far") > Number(value, "near");
            if (type == "Light")
            {
                var kind = value["kind"]!.GetValue<string>();
                if (kind == "spot") relations = Number(value, "inner_angle") < Number(value, "outer_angle", 45);
                if (value["shadow"] is JsonObject shadow)
                {
                    var near = Number(shadow, "near", .05);
                    relations &= Number(shadow, "distance", 80) > near;
                    if (shadow["enabled"]?.GetValue<bool>() == true)
                    {
                        relations &= kind != "spot" || Number(value, "outer_angle", 45) <= 89.5;
                        relations &= kind == "directional" || Number(value, "range") == 0 || Number(value, "range") > near;
                    }
                }
            }
            Model.SetInvalid(type + ".relations", !relations);
            Model.SetComponent(type, value); changed();
        }
        Grid Row(string text, Control control)
        {
            var row = new Grid { ColumnDefinitions = new ColumnDefinitions("112,*"), ColumnSpacing = 5 };
            var label = Label(text); label.FontSize = 12; row.Children.Add(label); Grid.SetColumn(control, 1); row.Children.Add(control); return row;
        }
        TextBox Numeric(string label, string key, double current, double min, double max, Action<double> set)
        {
            var edit = new TextBox { Text = Model.FieldText(type + ".input." + key, current.ToString("G9", CultureInfo.InvariantCulture)), FontSize = 12, MinWidth = 35 };
            edit.BorderBrush = EditorTheme.Brush(Model.HasInvalid(type + ".input." + key) ? "#BA6B60" : "#191919");
            AutomationProperties.SetName(edit, type + " " + label);
            ToolTip.SetTip(edit, $"{label}: {min:G} to {max:G}");
            ObserveInspectorText(edit, text =>
            {
                Model.SetFieldText(type + ".input." + key, text);
                var valid = double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var number) && double.IsFinite(number) && number >= min && number <= max;
                Model.SetInvalid(type + ".input." + key, !valid);
                edit.BorderBrush = EditorTheme.Brush(valid ? "#191919" : "#BA6B60");
                if (valid) { set(number); Commit(); } else changed();
            });
            return edit;
        }
        void Scalar(string label, string key, double min, double max, double fallback = 0, JsonObject? owner = null, string prefix = "")
        {
            owner ??= value; var target = owner;
            panel.Children.Add(Row(label, Numeric(label, prefix + key, Number(target, key, fallback), min, max, n => target[key] = n)));
        }
        void Toggle(string label, string key, JsonObject? owner = null)
        {
            owner ??= value; var target = owner;
            var control = new CheckBox { IsChecked = target[key]?.GetValue<bool>() ?? false, MinHeight = 22, VerticalAlignment = VerticalAlignment.Center };
            AutomationProperties.SetName(control, type + " " + label);
            var presented = control.IsChecked == true;
            control.IsCheckedChanged += (_, _) =>
            {
                var current = control.IsChecked == true;
                if (!CurrentDraft() || current == presented) return;
                presented = current; target[key] = current; Commit();
            };
            panel.Children.Add(Row(label, control));
        }
        void ColorField(string label, string key)
        {
            var channels = value[key]!.AsArray().Select(n => n!.GetValue<double>()).ToArray();
            var swatch = new Border { Height = 18, CornerRadius = new CornerRadius(2), BorderThickness = new Thickness(1), BorderBrush = EditorTheme.Brush("#181818"), Margin = new Thickness(0, 1) };
            void UpdateSwatch()
            {
                byte Encode(double linear) => (byte)Math.Round(255 * (linear <= .0031308 ? linear * 12.92 : 1.055 * Math.Pow(linear, 1/2.4) - .055));
                swatch.Background = new SolidColorBrush(Color.FromRgb(Encode(channels[0]), Encode(channels[1]), Encode(channels[2])));
            }
            UpdateSwatch(); ToolTip.SetTip(swatch, "sRGB preview of the linear RGB values below"); panel.Children.Add(Row(label, swatch));
            var rgb = new Grid { ColumnDefinitions = new ColumnDefinitions("112,*,*,*"), ColumnSpacing = 5 }; rgb.Children.Add(Label("Linear RGB", true));
            for (int axis = 0; axis < 3; ++axis)
            {
                var index = axis;
                var input = Numeric(label + " " + "RGB"[axis], key + axis, channels[axis], 0, 1, n =>
                {
                    channels[index] = n;
                    value[key] = new JsonArray(channels.Select(c => (JsonNode?)JsonValue.Create(c)).ToArray()); UpdateSwatch();
                });
                Grid.SetColumn(input, axis + 1); rgb.Children.Add(input);
            }
            panel.Children.Add(rgb);
        }
        void Draw()
        {
            panel.Children.Clear();
            switch (type)
            {
                case "Camera":
                    Scalar("Vertical FOV (°)", "vertical_fov", 5, 150);
                    Scalar("Near clip (m)", "near", .001, 1e7);
                    Scalar("Far clip (m)", "far", .001, 1e7);
                    panel.Children.Add(Label("Far clip must exceed near clip.", true));
                    break;
                case "MeshRenderer":
                    panel.Children.Add(Row("Mesh", Label("Box"))); Toggle("Visible", "visible"); ColorField("Albedo", "albedo");
                    break;
                case "PbrMaterial":
                    ColorField("Base color", "base_color"); Scalar("Metallic", "metallic", 0, 1); Scalar("Roughness", "roughness", 0, 1);
                    ColorField("Emission", "emissive"); Toggle("Double sided", "double_sided");
                    break;
                case "Light":
                    var currentKind = value["kind"]!.GetValue<string>();
                    var kind = new ComboBox { ItemsSource = new[] { "directional", "point", "spot" }, SelectedItem = currentKind, MinHeight = 24, Padding = new Thickness(5, 2), HorizontalAlignment = HorizontalAlignment.Stretch };
                    AutomationProperties.SetName(kind, "Light type");
                    kind.SelectionChanged += (_, _) =>
                    {
                        if (!CurrentDraft() || kind.SelectedItem is not string selected || selected == value["kind"]!.GetValue<string>()) return;
                        if (Model.HasInvalid(type + ".input.")) { kind.SelectedItem = value["kind"]!.GetValue<string>(); Model.Note("Fix invalid Light fields before changing its type."); return; }
                        value["kind"] = selected;
                        if (selected == "directional") value.Remove("range");
                        if (selected != "spot") { value.Remove("inner_angle"); value.Remove("outer_angle"); }
                        Commit(); Draw();
                    };
                    panel.Children.Add(Row("Type", kind)); Toggle("Enabled", "enabled"); ColorField("Color", "color");
                    Scalar(currentKind == "directional" ? "Intensity (lux)" : "Intensity (cd)", "intensity", 0, 1e9);
                    if (currentKind != "directional") { Scalar("Range (m)", "range", 0, 1e9); panel.Children.Add(Label("Range 0 = no distance cutoff", true)); }
                    if (currentKind == "spot") { Scalar("Inner angle (°)", "inner_angle", 0, 90); Scalar("Outer angle (°)", "outer_angle", 0, 90, 45); panel.Children.Add(Label("Half-angles: 0 ≤ inner < outer ≤ 90°", true)); }
                    if (value["shadow"] is JsonObject shadow)
                    {
                        panel.Children.Add(new Border { Height = 1, Background = EditorTheme.Brush("#222222"), Margin = new Thickness(0, 5) });
                        Toggle("Cast shadows", "enabled", shadow);
                        Scalar("Shadow near (m)", "near", .001, 100, .05, shadow, "shadow.");
                        Scalar("Distance (m)", "distance", .001, 100000, 80, shadow, "shadow.");
                        Scalar("Depth bias", "bias", 0, .1, .0005, shadow, "shadow.");
                        Scalar("Normal bias (m)", "normal_bias", 0, 1, .01, shadow, "shadow.");
                    }
                    else panel.Children.Add(Button("Configure shadows", () =>
                    {
                        if (Model.HasInvalid(type + ".input.")) throw new InvalidOperationException("Fix invalid Light fields first.");
                        value["shadow"] = new JsonObject { ["enabled"] = false }; Commit(); Draw();
                    }));
                    break;
            }
        }
        Draw();
    }
    private async System.Threading.Tasks.Task PickModel()
    {
        try
        {
            var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions { Title = "Import glTF model", AllowMultiple = false, FileTypeFilter = new[] { new FilePickerFileType("glTF model") { Patterns = new[] { "*.gltf", "*.glb" } } } });
            if (files.Count == 0) return;
            var path = files[0].TryGetLocalPath(); if (path is null) throw new InvalidOperationException("Import needs a local file.");
            ImportModel(path);
        }
        catch (Exception e) { Model.Note(e.Message); }
    }
    private void ImportModel(string path)
    {
        var imported = Model.Import(path); assetNames[imported["asset"]!.GetValue<string>()] = System.IO.Path.GetFileNameWithoutExtension(path);
        Model.Note("Model is available in the world asset store. Select its .pmodel file to inspect or instantiate it.");
    }
    private sealed record FileEntry(string Path, string Name, string Type, bool Directory);
    private static readonly HashSet<string> ExcludedDirectories = new(StringComparer.OrdinalIgnoreCase) { ".git", ".cache", "build", "bin", "obj", "node_modules" };
    private static bool SafeDirectory(string path) => (File.GetAttributes(path) & FileAttributes.ReparsePoint) == 0 && !ExcludedDirectories.Contains(System.IO.Path.GetFileName(path));
    private static bool StorageSidecar(string path) => new[] { ".lock", ".pending", ".previous" }.Any(suffix => path.EndsWith(suffix, StringComparison.OrdinalIgnoreCase));
    private Control BuildProject()
    {
        var root = new Grid { RowDefinitions = new RowDefinitions("27,*,Auto") };
        var toolbar = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,Auto,*,Auto"), ColumnSpacing = 5, Margin = new Thickness(5, 3) };
        toolbar.Children.Add(Button("Import", () => _ = PickModel()));
        var search = SearchField("Search folder"); AutomationProperties.SetName(search, "Search project files"); Grid.SetColumn(search, 2); toolbar.Children.Add(search);
        var mode = ToolButton("Switch to grid view", () => {}, "grid"); Grid.SetColumn(mode, 3); toolbar.Children.Add(mode); root.Children.Add(toolbar);
        var body = new Grid { ColumnDefinitions = new ColumnDefinitions("175,4,*") };
        var tree = new TreeView { Background = EditorTheme.Brush("#28292D") };
        body.Children.Add(tree); var splitter = new GridSplitter { Width = 4, Background = EditorTheme.Brush("#222327") }; Grid.SetColumn(splitter, 1); body.Children.Add(splitter);
        var listing = new Grid { RowDefinitions = new RowDefinitions("22,*") };
        var breadcrumb = Label("", true); breadcrumb.Margin = new Thickness(8, 2); listing.Children.Add(breadcrumb);
        var presenter = new ContentControl(); Grid.SetRow(presenter, 1); listing.Children.Add(presenter); Grid.SetColumn(listing, 2); body.Children.Add(listing);
        Grid.SetRow(body, 1); root.Children.Add(body);
        var details = new StackPanel { Spacing = 4, Margin = new Thickness(8, 4) };
        var description = new TextBlock { FontSize = 12, Foreground = EditorTheme.Brush("#999999"), TextWrapping = TextWrapping.Wrap };
        var instantiate = Button("Instantiate model", () => { }); instantiate.IsVisible = false;
        details.Children.Add(description); details.Children.Add(instantiate); Grid.SetRow(details, 2); root.Children.Add(details);
        string current = Directory.Exists(System.IO.Path.Combine(projectRoot, "Assets")) ? System.IO.Path.Combine(projectRoot, "Assets") : projectRoot;
        bool grid = false; string? modelAsset = null; string modelName = "Model";
        instantiate.Click += (_, _) => { if (modelAsset is not null) Run(() => Model.Instantiate(modelAsset, modelName)); };
        void Select(FileEntry item)
        {
            modelAsset = null; instantiate.IsVisible = false;
            if (item.Directory) { description.Text = $"{item.Name}  ·  Folder  ·  Double-click to open"; return; }
            description.Text = $"{item.Name}  ·  {item.Type}  ·  {new FileInfo(item.Path).Length:N0} bytes";
            if (System.IO.Path.GetExtension(item.Path).Equals(".pmodel", StringComparison.OrdinalIgnoreCase))
                Run(() =>
                {
                    var asset = System.IO.Path.GetFileNameWithoutExtension(item.Path);
                    var metadata = Model.Host.Call("asset.inspect", new() { ["asset"] = asset });
                    modelAsset = asset; modelName = assetNames.GetValueOrDefault(asset, "Model");
                    description.Text = $"{modelName}  ·  {metadata["nodes"]} nodes  ·  {metadata["triangles"]} triangles  ·  {metadata["animations"]} clips";
                    instantiate.IsVisible = true;
                });
            else if (item.Type == "glTF model") description.Text += "  ·  Double-click to import";
        }
        void Open(FileEntry item) { if (item.Directory) { current = item.Path; Refresh(); } else if (item.Type == "glTF model") { Run(() => ImportModel(item.Path)); Refresh(); } }
        void Refresh()
        {
            try
            {
                var relative = System.IO.Path.GetRelativePath(projectRoot, current);
                breadcrumb.Text = System.IO.Path.GetFileName(projectRoot) + (relative == "." ? "" : "  ›  " + relative.Replace(System.IO.Path.DirectorySeparatorChar.ToString(), "  ›  "));
                var entries = Directory.EnumerateFileSystemEntries(current).Take(1025).Where(path =>
                    (File.GetAttributes(path) & FileAttributes.ReparsePoint) == 0 && !StorageSidecar(path) && (!Directory.Exists(path) || SafeDirectory(path)))
                    .Select(path =>
                    {
                        var directory = Directory.Exists(path); var extension = System.IO.Path.GetExtension(path).ToLowerInvariant();
                        var name = System.IO.Path.GetFileName(path);
                        if (extension == ".pmodel") name = assetNames.GetValueOrDefault(System.IO.Path.GetFileNameWithoutExtension(path), "Model · " + name[..Math.Min(8, name.Length)]);
                        return new FileEntry(path, name, directory ? "Folder" : extension is ".glb" or ".gltf" ? "glTF model" : extension.TrimStart('.'), directory);
                    }).Where(item => string.IsNullOrEmpty(search.Text) || item.Name.Contains(search.Text, StringComparison.OrdinalIgnoreCase))
                    .OrderByDescending(x => x.Directory).ThenBy(x => x.Name, StringComparer.OrdinalIgnoreCase).Take(1024).ToList();
                if (grid)
                {
                    var wrap = new WrapPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(6) };
                    foreach (var item in entries)
                    {
                        var content = new StackPanel { Spacing = 5, Width = 84 }; content.Children.Add(EditorIcons.Make(item.Directory ? "folder" : EditorIcons.FileKind(System.IO.Path.GetExtension(item.Path)), 34));
                        content.Children.Add(new TextBlock { Text = item.Name, TextAlignment = TextAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis, FontSize = 12 });
                        var tile = new Button { Content = content, Margin = new Thickness(3), Padding = new Thickness(6), Background = Brushes.Transparent, BorderThickness = new Thickness(0) };
                        ToolTip.SetTip(tile, item.Path); tile.Click += (_, _) => Run(() => Select(item)); tile.DoubleTapped += (_, _) => Run(() => Open(item)); wrap.Children.Add(tile);
                    }
                    presenter.Content = new ScrollViewer { Content = wrap };
                }
                else
                {
                    var files = new ListBox { ItemsSource = entries, ItemTemplate = new FuncDataTemplate<FileEntry>((item, _) =>
                    {
                        if (item is null) return new TextBlock();
                        var row = new Grid { ColumnDefinitions = new ColumnDefinitions("21,*,100"), Margin = new Thickness(8, 0), Height = 20 };
                        row.Children.Add(EditorIcons.Make(item.Directory ? "folder" : EditorIcons.FileKind(System.IO.Path.GetExtension(item.Path)), 15));
                        var label = Label(item.Name); Grid.SetColumn(label, 1); row.Children.Add(label); var kind = Label(item.Type, true); Grid.SetColumn(kind, 2); row.Children.Add(kind); return row;
                    }) };
                    files.SelectionChanged += (_, _) => { if (files.SelectedItem is FileEntry item) Run(() => Select(item)); };
                    files.DoubleTapped += (_, _) => { if (files.SelectedItem is FileEntry item) Run(() => Open(item)); };
                    presenter.Content = files;
                }
                if (entries.Count == 1024) description.Text = "Showing at most 1,024 entries. Browse a narrower folder.";
            }
            catch (Exception e) { description.Text = e.Message; }
        }
        TreeViewItem Folder(string path)
        {
            var header = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5 }; header.Children.Add(EditorIcons.Make("folder", 14)); header.Children.Add(Label(path == projectRoot ? System.IO.Path.GetFileName(projectRoot) : System.IO.Path.GetFileName(path)));
            var item = new TreeViewItem { Header = header, Tag = path, ItemsSource = new[] { new TreeViewItem { Header = "…" } } };
            bool loaded = false;
            item.PropertyChanged += (_, e) =>
            {
                if (e.Property != TreeViewItem.IsExpandedProperty || !item.IsExpanded || loaded) return;
                loaded = true;
                try { item.ItemsSource = Directory.EnumerateDirectories(path).Take(257).Where(SafeDirectory).Take(256).OrderBy(x => x).Select(Folder).ToArray(); }
                catch (Exception error) { item.ItemsSource = Array.Empty<TreeViewItem>(); description.Text = error.Message; }
            };
            return item;
        }
        var top = Folder(projectRoot); tree.ItemsSource = new[] { top }; top.IsExpanded = true;
        tree.SelectionChanged += (_, _) => { if (tree.SelectedItem is TreeViewItem item && item.Tag is string path) { current = path; Refresh(); } };
        var refresh = Button("Refresh", Refresh); Grid.SetColumn(refresh, 1); toolbar.Children.Add(refresh);
        search.TextChanged += (_, _) => Refresh(); mode.Click += (_, _) =>
        {
            grid = !grid; mode.Content = EditorIcons.Make(grid ? "list" : "grid");
            var label = grid ? "Switch to list view" : "Switch to grid view";
            ToolTip.SetTip(mode, label); AutomationProperties.SetName(mode, label); Refresh();
        };
        Refresh(); return root;
    }
}
