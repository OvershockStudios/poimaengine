// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Platform.Storage;
using Avalonia.VisualTree;
using Avalonia.Media.Imaging;
using System.Text.Json.Nodes;

namespace Poima.Editor;

public sealed partial class MainWindow
{
    private sealed record ComponentChoice(string Id, string Name) { public override string ToString() => Name; }
    private sealed record EntityChoice(string Id, string Name) { public override string ToString() => Name; }
    private string componentManifestPath = "";

    internal JsonObject RenderInspector(string destination)
    {
        if (Program.Options.Script is null) throw new InvalidOperationException("Inspector visual qualification requires an explicit script.");
        var panel = InspectorPanel();
        var path = Path.GetFullPath(destination);
        var project = Path.GetFullPath(projectRoot).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (path.StartsWith(project, StringComparison.OrdinalIgnoreCase) || Path.GetExtension(path).ToLowerInvariant() != ".png")
            throw new ArgumentException("Inspector output must be a new PNG outside the project directory.");
        var parent = new DirectoryInfo(Path.GetDirectoryName(path)!);
        if (!parent.Exists || File.Exists(path) || Directory.Exists(path)) throw new ArgumentException("Inspector output requires a new file in an existing directory.");
        for (var check = parent; check is not null; check = check.Parent)
            if ((check.Attributes & FileAttributes.ReparsePoint) != 0) throw new ArgumentException("Inspector output cannot traverse a reparse point.");
        var width = Math.Ceiling(panel.Bounds.Width * RenderScaling); var height = Math.Ceiling(panel.Bounds.Height * RenderScaling);
        if (!panel.IsMeasureValid || !panel.IsArrangeValid || !double.IsFinite(width) || !double.IsFinite(height) || width is < 32 or > 4096 || height is < 32 or > 4096 || width * height > 16777216)
            throw new InvalidOperationException("Inspector visual is unmeasured or exceeds qualification bounds.");
        using var image = new RenderTargetBitmap(new PixelSize((int)width, (int)height), new Vector(96 * RenderScaling, 96 * RenderScaling));
        image.Render(panel);
        using var encoded = new MemoryStream(); image.Save(encoded, new PngBitmapEncoderOptions()); encoded.Position = 0;
        using (var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None)) { encoded.CopyTo(output); output.Flush(true); }
        return new JsonObject { ["kind"] = "avalonia_visual_content", ["path"] = path, ["width"] = (int)width, ["height"] = (int)height,
            ["qualification"] = "Attached Inspector visual only; not an OS screenshot or physical-input qualification." };
    }
    private Control InspectorPanel()
    {
        var lifetime = (Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime)Application.Current!.ApplicationLifetime!;
        return lifetime.Windows.Where(window => window.IsVisible).SelectMany(window => window.GetVisualDescendants()).OfType<Control>()
            .Single(control => control.IsVisible && AutomationProperties.GetName(control) == "Inspector contents");
    }
    internal void ScrollInspector(string position)
    {
        if (Program.Options.Script is null) throw new InvalidOperationException("Inspector qualification requires an explicit script.");
        var scroll = InspectorPanel().GetVisualDescendants().OfType<ScrollViewer>().First();
        if (position == "top") scroll.ScrollToHome(); else if (position == "bottom") scroll.ScrollToEnd(); else throw new ArgumentException("Expected top or bottom.");
    }
    private Control BuildComponentTools()
    {
        var panel = new StackPanel { Spacing = 5, Margin = new Thickness(8) };
        var path = new TextBox { Text = componentManifestPath, PlaceholderText = "Generated .poima-components.json", MinWidth = 60 };
        AutomationProperties.SetName(path, "Components Manifest path");
        path.TextChanged += (_, _) => componentManifestPath = path.Text ?? "";
        var import = Button("Import schemas", () => Model.ImportComponentManifest(componentManifestPath));
        AutomationProperties.SetName(import, "Components Import schemas");
        var browse = Button("Browse", () => _ = BrowseComponentManifest(path));
        AutomationProperties.SetName(browse, "Components Browse manifest");
        var actions = new WrapPanel { Orientation = Orientation.Horizontal }; actions.Children.Add(import); actions.Children.Add(browse);
        var schemas = new ComboBox { HorizontalAlignment = HorizontalAlignment.Stretch, MinWidth = 70, PlaceholderText = "Choose component" };
        AutomationProperties.SetName(schemas, "Components Available schemas");
        var add = Button("Add component", () =>
        {
            if (schemas.SelectedItem is not ComponentChoice choice) throw new InvalidOperationException("Choose an imported component first.");
            Model.AddCustomComponent(choice.Id);
        });
        AutomationProperties.SetName(add, "Components Add component");
        var body = new StackPanel { Spacing = 5 }; body.Children.Add(path); body.Children.Add(actions); body.Children.Add(schemas); body.Children.Add(add);
        body.Children.Add(new TextBlock { Text = "Import component definitions, then add them to the selected object.", TextWrapping = Avalonia.Media.TextWrapping.Wrap, FontSize = 11 });
        var tools = new Expander { Header = "Custom components", Content = body, IsExpanded = false, HorizontalAlignment = HorizontalAlignment.Stretch };
        AutomationProperties.SetName(tools, "Components Tools"); panel.Children.Add(tools);
        string last = "";
        void Refresh()
        {
            import.IsEnabled = browse.IsEnabled = Model.RuntimeId is null;
            add.IsEnabled = Model.RuntimeId is null && Model.Selected is not null;
            var key = Model.Selected + ":" + Model.DraftGeneration + ":" + Model.Revision;
            if (key == last) return; last = key;
            var prior = (schemas.SelectedItem as ComponentChoice)?.Id;
            var available = Model.DraftSchemas.Where(pair => !Model.DraftComponents.ContainsKey("game:" + pair.Key))
                .Select(pair => new ComponentChoice(pair.Key, pair.Value!["name"]!.GetValue<string>())).OrderBy(choice => choice.Name, StringComparer.Ordinal).ThenBy(choice => choice.Id, StringComparer.Ordinal).ToArray();
            schemas.ItemsSource = available;
            schemas.SelectedItem = available.FirstOrDefault(choice => choice.Id == prior) ?? available.FirstOrDefault();
        }
        Observe(panel, Model, Refresh); Refresh(); return panel;
    }
    private async Task BrowseComponentManifest(TextBox target)
    {
        try
        {
            Model.RequireSceneEditable();
            var revision = Model.Revision; var selected = Model.Selected;
            var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions { Title = "Select generated component declarations", AllowMultiple = false,
                FileTypeFilter = new[] { new FilePickerFileType("Component declarations") { Patterns = new[] { "*.poima-components.json", "*.json" } } } });
            if (!IsVisible || disposed || Model.Revision != revision || Model.Selected != selected) return;
            Model.RequireSceneEditable();
            if (files.Count == 1 && files[0].TryGetLocalPath() is string path) target.Text = path;
        }
        catch (Exception error) { Model.Note(error.Message); }
    }
    private void BuildCustomComponent(StackPanel panel, string type, JsonObject source, JsonObject schema, Action changed)
    {
        var entity = Model.Selected; var generation = Model.DraftGeneration; var value = EditorModel.Clone(source);
        bool Current() => Model.Selected == entity && Model.DraftGeneration == generation;
        panel.Children.Add(new TextBlock { Text = "Authored values · restart Play to use changes", FontSize = 11, TextWrapping = Avalonia.Media.TextWrapping.Wrap });
        foreach (var field in schema["fields"]!.AsArray().OfType<JsonObject>())
        {
            var id = field["id"]!.GetValue<string>(); var key = type + ":" + id;
            var text = Model.FieldText(key, ComponentFields.Text(value[id]));
            var control = ComponentFieldControl(field, text, "Component " + schema["name"] + " " + field["name"], edited =>
            {
                if (!Current()) return;
                Model.SetFieldText(key, edited);
                try { value[id] = ComponentFields.Parse(field, edited); Model.SetComponent(type, value); Model.SetInvalid(key, false); }
                catch (InvalidOperationException) { Model.SetInvalid(key, true); }
                changed();
            });
            panel.Children.Add(control);
        }
        var remove = Button("Remove component", () => { if (Current()) Model.RemoveCustomComponent(type[5..]); });
        AutomationProperties.SetName(remove, "Component " + schema["name"] + " Remove"); panel.Children.Add(remove);
        var load = Button("Load paused live values", () =>
        {
            try { if (Current() && entity is not null) Components.Load(entity, type[5..]); }
            catch (Exception error) { Components.Fail(error); }
        });
        AutomationProperties.SetName(load, "Component " + schema["name"] + " Load live"); panel.Children.Add(load);
        // These controls stay attached while a draft is dirty; native/model guards
        // remain authoritative if Play starts or a remote selection changes.
        remove.IsEnabled = Model.RuntimeId is null;
    }
    private Control ComponentFieldControl(JsonObject field, string initial, string automation, Action<string> edited)
    {
        var fieldName = field["name"]!.GetValue<string>(); var kind = field["kind"]!.GetValue<string>(); var unit = field["unit"]?.GetValue<string>() ?? "";
        var row = new StackPanel { Spacing = 3, Margin = new Thickness(0, 2) };
        row.Children.Add(new TextBlock { Text = fieldName + (unit.Length == 0 ? "" : " · " + unit), TextWrapping = Avalonia.Media.TextWrapping.Wrap });
        var text = new TextBox { Text = initial, MinWidth = 65 }; AutomationProperties.SetName(text, automation);
        ToolTip.SetTip(text, kind + " · field " + field["id"]);
        var shown = initial;
        void Validate(string value)
        {
            try { _ = ComponentFields.Parse(field, value); text.BorderBrush = EditorTheme.Brush("#191919"); }
            catch (InvalidOperationException) { text.BorderBrush = EditorTheme.Brush("#BA6B60"); }
        }
        Validate(initial);
        text.TextChanged += (_, _) => { var value = text.Text ?? ""; if (shown == value) return; shown = value; Validate(value); edited(value); };
        row.Children.Add(text);
        if (kind == "entity")
        {
            var options = new List<EntityChoice> { new(ComponentFields.Unset, "None") };
            options.AddRange(Model.Entities.Select(entity => new EntityChoice(entity.Id, entity.Name + " · " + entity.Id[..8])));
            if (options.All(option => option.Id != initial)) options.Add(new(initial, "Current reference · " + initial));
            var picker = new ComboBox { ItemsSource = options, SelectedItem = options.FirstOrDefault(option => option.Id == initial), HorizontalAlignment = HorizontalAlignment.Stretch };
            AutomationProperties.SetName(picker, automation + " Entity");
            ToolTip.SetTip(picker, "Current authored objects. The native service validates references against the edited world or frozen runtime.");
            picker.SelectionChanged += (_, _) => { if (picker.SelectedItem is EntityChoice choice) text.Text = choice.Id; };
            row.Children.Add(picker);
        }
        return row;
    }
    private Control BuildLiveComponents()
    {
        var panel = new StackPanel { Spacing = 5, Margin = new Thickness(8) };
        var title = new TextBlock { TextWrapping = Avalonia.Media.TextWrapping.Wrap };
        var status = new TextBlock { TextWrapping = Avalonia.Media.TextWrapping.Wrap, FontSize = 11 };
        AutomationProperties.SetName(status, "Live component Status");
        var fields = new StackPanel { Spacing = 3 };
        var types = new ComboBox { HorizontalAlignment = HorizontalAlignment.Stretch, PlaceholderText = "Frozen runtime component" };
        AutomationProperties.SetName(types, "Live component Types");
        var discovery = new WrapPanel { Orientation = Orientation.Horizontal };
        var discover = Button("Refresh runtime types", () => { try { Components.RefreshTypes(); } catch (Exception error) { Components.Fail(error); } });
        AutomationProperties.SetName(discover, "Live component Refresh types"); discovery.Children.Add(discover);
        var load = Button("Load selected object", () =>
        {
            try
            {
                if (Components.SchemaSession != Model.RuntimeId || Model.Selected is null || types.SelectedItem is not ComponentChoice choice) throw new InvalidOperationException("Select an object and refresh the current runtime types first.");
                Components.Load(Model.Selected, choice.Id);
            }
            catch (Exception error) { Components.Fail(error); }
        });
        AutomationProperties.SetName(load, "Live component Load selected"); discovery.Children.Add(load);
        var actions = new WrapPanel { Orientation = Orientation.Horizontal };
        Button Action(string name, Action action)
        {
            var button = Button(name, () => { try { action(); } catch (Exception error) { Components.Fail(error); } });
            AutomationProperties.SetName(button, "Live component " + name); actions.Children.Add(button); return button;
        }
        var apply = Action("Apply", Components.Apply); var reload = Action("Reload", Components.Reload); Action("Discard", Components.Discard);
        panel.Children.Add(title); panel.Children.Add(status); panel.Children.Add(discovery); panel.Children.Add(types); panel.Children.Add(fields); panel.Children.Add(actions);
        long version = -1;
        void Refresh()
        {
            panel.IsVisible = Model.RuntimeId is not null || Components.Observation is not null;
            discover.IsEnabled = load.IsEnabled = Model.Paused;
            apply.IsEnabled = reload.IsEnabled = Model.Paused && Components.Observation is not null;
            if (version != Components.DraftVersion)
            {
                var prior = (types.SelectedItem as ComponentChoice)?.Id;
                var choices = Components.RuntimeSchemas.OfType<JsonObject>().Select(schema => new ComponentChoice(schema["id"]!.GetValue<string>(), schema["name"]!.GetValue<string>())).ToArray();
                types.ItemsSource = choices; types.SelectedItem = choices.FirstOrDefault(choice => choice.Id == prior) ?? choices.FirstOrDefault();
            }
            if (Components.Observation is not JsonObject observation)
            {
                title.Text = "Live components"; status.Text = Components.Error ?? "Pause, refresh frozen runtime types and load an object. Authored values remain separate.";
                if (version != Components.DraftVersion) fields.Children.Clear(); version = Components.DraftVersion; return;
            }
            title.Text = "Live component · " + observation["schema"]!["name"] + " · " + (Model.Entities.FirstOrDefault(entity => entity.Id == observation["id"]?.GetValue<string>())?.Name ?? observation["id"]?.ToString());
            status.Text = "Paused runtime values; authored values are unchanged. " + (Components.Dirty ? "Unapplied changes. " : "") +
                (Components.Conflict ? "Observation changed; reload explicitly or discard. " : "") + (Components.Error ?? "");
            apply.IsEnabled = reload.IsEnabled = Model.Paused;
            if (version == Components.DraftVersion) return; version = Components.DraftVersion; fields.Children.Clear();
            foreach (var field in observation["schema"]!["fields"]!.AsArray().OfType<JsonObject>())
            {
                var id = field["id"]!.GetValue<string>(); var captured = version;
                fields.Children.Add(ComponentFieldControl(field, Components.Values[id], "Live component " + field["name"], value =>
                { if (captured == Components.DraftVersion) Components.Set(id, value); }));
            }
        }
        EventHandler change = (_, _) => Refresh();
        panel.AttachedToVisualTree += (_, _) => { Components.Changed += change; Refresh(); };
        panel.DetachedFromVisualTree += (_, _) => Components.Changed -= change;
        Refresh(); return panel;
    }
}
