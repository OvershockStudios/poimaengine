// SPDX-License-Identifier: Apache-2.0
using System.Collections.ObjectModel;
using System.Text.Json.Nodes;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using Avalonia.Threading;

namespace Poima.Editor;

internal sealed record CreatorAsset(string Name, string Id, string Kind)
{
    public string Description => Kind switch { "model" => "3D model", "audio" => "audio clip", _ => "image" };
    public override string ToString() => $"{Name} ({Description})";
}

internal sealed class CreatorAssetWindow : Window
{
    private sealed record SourceKind(string Label, string Kind, string Method, string[] Patterns,
        string? ColorSpace = null, bool GeometryOnly = false)
    {
        public override string ToString() => Label;
    }

    private static readonly SourceKind[] SourceKinds =
    [
        new("3D models", "model", "asset.import", ["*.gltf", "*.glb", "*.fbx"]),
        new("FBX geometry", "model", "asset.import", ["*.fbx"], GeometryOnly: true),
        new("Color textures", "image", "asset.image.import", ["*.png", "*.jpg", "*.jpeg"], "srgb"),
        new("Data textures", "image", "asset.image.import", ["*.png", "*.jpg", "*.jpeg"], "linear"),
        new("WAV audio", "audio", "asset.audio.import", ["*.wav"])
    ];

    private readonly NativeHost host;
    private readonly ObservableCollection<CreatorAsset> imported = [];
    private readonly ComboBox kind = new() { ItemsSource = SourceKinds, SelectedIndex = 0 };
    private readonly ListBox assets = new() { SelectionMode = SelectionMode.Multiple };
    private readonly TextBlock status = new() { TextWrapping = TextWrapping.Wrap };
    private readonly Button pick = new() { Content = "Choose files…" };
    private readonly Button use = new() { Content = "Add to chat draft" };
    private readonly Button refresh = new() { Content = "Refresh" };
    private readonly Button more = new() { Content = "Load more" };
    private string? nextPage;
    private bool busy, closed;

    public CreatorAssetWindow(NativeHost host)
    {
        this.host = host;
        Title = "Import assets";
        Width = 580; Height = 430; MinWidth = 420; MinHeight = 350;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter");
        FontSize = 14; UseLayoutRounding = true;
        Background = EditorTheme.Brush("#19191C");
        Foreground = EditorTheme.Brush("#F2EDEE");
        use.Background = EditorTheme.Brush("#722F37");
        use.Foreground = EditorTheme.Brush("#F7E8EB");
        status.Foreground = EditorTheme.Brush("#B4ACB2");
        assets.ItemsSource = imported;
        AutomationProperties.SetName(kind, "Asset type");
        AutomationProperties.SetName(assets, "Imported assets");
        AutomationProperties.SetAutomationId(status, "creator-import-status");
        assets.ContainerPrepared += (_, args) =>
        {
            if (args.Container is ListBoxItem { Content: CreatorAsset asset })
                AutomationProperties.SetName(args.Container, asset.ToString());
        };
        pick.Click += async (_, _) => await PickAsync();
        Opened += (_, _) => ReadPage(reset: true);
        refresh.Click += (_, _) => ReadPage(reset: true);
        more.Click += (_, _) => ReadPage(reset: false);
        assets.SelectionChanged += (_, _) => UpdateControls();
        use.Click += (_, _) =>
        {
            if (!busy && assets.SelectedItems?.OfType<CreatorAsset>().ToArray() is { Length: > 0 } selected) Close(selected);
        };
        var done = new Button { Content = "Done", IsCancel = true };
        done.Click += (_, _) => Close();
        Closed += (_, _) => closed = true;

        var layout = new Grid { RowDefinitions = new RowDefinitions("Auto,*,Auto,Auto"), RowSpacing = 12, Margin = new Thickness(20) };
        layout.Children.Add(new WrapPanel { ItemSpacing = 8, LineSpacing = 6, Children = { kind, pick, refresh, more } });
        Grid.SetRow(assets, 1); layout.Children.Add(assets);
        var messages = new ScrollViewer { Content = status, MaxHeight = 96 };
        Grid.SetRow(messages, 2); layout.Children.Add(messages);
        var actions = new WrapPanel { ItemSpacing = 8, HorizontalAlignment = HorizontalAlignment.Right, Children = { use, done } };
        Grid.SetRow(actions, 3); layout.Children.Add(actions);
        Content = layout;
        UpdateControls();
    }

    private void ReadPage(bool reset)
    {
        if (busy || (!reset && nextPage is null)) return;
        try
        {
            var request = reset ? new JsonObject() : new JsonObject { ["after"] = nextPage };
            var result = host.Call("asset.catalog", request);
            var entries = result["entries"]!.AsArray().Select(value =>
            {
                var id = value!["asset"]!.GetValue<string>();
                return new CreatorAsset(value["label"]?.GetValue<string>() ?? id[..12], id, value["kind"]!.GetValue<string>());
            }).ToArray();
            nextPage = result["next_after"]?.GetValue<string>();
            if (reset) imported.Clear();
            foreach (var entry in entries) Include(entry);
            status.Text = imported.Count == 0 ? "Choose local files to use in this project." : "Select assets to add to your chat draft.";
        }
        catch (Exception error) { status.Text = error.Message; }
        UpdateControls();
    }

    private CreatorAsset Include(CreatorAsset entry)
    {
        // An import can name an asset already present on the page.
        var existing = imported.FirstOrDefault(asset => asset.Id == entry.Id && asset.Kind == entry.Kind);
        if (existing is null) imported.Add(entry);
        else if (existing != entry) imported[imported.IndexOf(existing)] = entry;
        return entry;
    }

    private async Task PickAsync()
    {
        if (busy || kind.SelectedItem is not SourceKind selected) return;
        busy = true;
        UpdateControls();
        try
        {
            var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
            {
                Title = "Choose assets", AllowMultiple = true,
                FileTypeFilter = [new FilePickerFileType(selected.Label) { Patterns = selected.Patterns }]
            });
            var failures = new List<string>();
            var added = new List<CreatorAsset>();
            foreach (var file in files)
            {
                status.Text = $"Importing {file.Name}…";
                await Dispatcher.Yield();
                if (closed) return;
                try
                {
                    var path = file.TryGetLocalPath() ?? throw new InvalidOperationException("Choose a local file.");
                    var request = new JsonObject { ["source"] = path };
                    if (selected.ColorSpace is { } color) request["color_space"] = color;
                    if (selected.GeometryOnly) request["fbx_geometry_only"] = true;
                    var id = host.Call(selected.Method, request)["asset"]?.GetValue<string>()
                        ?? throw new InvalidOperationException("Poima did not return a cooked asset ID.");
                    var name = selected.ColorSpace switch
                    {
                        "srgb" => $"{file.Name} (sRGB)", "linear" => $"{file.Name} (linear)", _ => file.Name
                    };
                    try
                    {
                        name = host.Call("asset.label", new() { ["asset"] = id, ["kind"] = selected.Kind, ["label"] = name })
                            ["label"]!.GetValue<string>();
                    }
                    catch (Exception error) { failures.Add($"{file.Name} imported. Couldn’t save its name. {error.Message}"); }
                    var asset = Include(new CreatorAsset(name, id, selected.Kind));
                    if (!added.Contains(asset)) added.Add(asset);
                }
                catch (Exception error) { failures.Add($"{file.Name} — {error.Message}"); }
            }
            if (closed || files.Count == 0) return;
            assets.SelectedItems?.Clear();
            foreach (var asset in added) assets.SelectedItems?.Add(asset);
            status.Text = failures.Count == 0 ? "Files are ready to use in this project." : string.Join('\n', failures);
        }
        catch (Exception error) { if (!closed) status.Text = error.Message; }
        finally { busy = false; if (!closed) UpdateControls(); }
    }

    private void UpdateControls()
    {
        pick.IsEnabled = kind.IsEnabled = assets.IsEnabled = !busy;
        refresh.IsEnabled = !busy;
        more.IsVisible = nextPage is not null;
        more.IsEnabled = !busy && nextPage is not null;
        use.IsEnabled = !busy && assets.SelectedItems?.Count > 0;
    }
}
