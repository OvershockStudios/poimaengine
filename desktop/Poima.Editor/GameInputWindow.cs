// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Templates;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using System.Text.Json.Nodes;

namespace Poima.Editor;

// Preferences can be staged while stopped. Native input remains the authority
// for configuration, assignment, neutral gating and device lifetime.
public sealed class GameInputWindow : Window
{
    private readonly EditorModel editor;
    private readonly GameInput input;
    private readonly ComboBox profile = new() { ItemsSource = new[] { "Keyboard and mouse", "Keyboard, mouse and gamepad", "Profile file" } };
    private readonly ComboBox policy = new() { ItemsSource = new[] { "Disabled", "Only connected gamepad", "Explicit device" } };
    private readonly ComboBox device = new() { ItemTemplate = new FuncDataTemplate<Device>((value, _) => new TextBlock { Text = value?.Label ?? "" }) };
    private readonly TextBox path = new() { PlaceholderText = "Choose an existing .poima-input.json file" };
    private readonly TextBlock status = Note(""), message = Note(""), format = Note("");
    private readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromMilliseconds(500) };
    private bool retired;
    private sealed record Device(uint Id, string Label) { public override string ToString() => Label; }
    public GameInputWindow(EditorModel editor, GameInput input)
    {
        this.editor = editor; this.input = input;
        Title = "Game Input — Poima"; Width = 560; Height = 640; MinWidth = 420; MinHeight = 440;
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter"); FontSize = 12;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        var root = new StackPanel { Spacing = 10, Margin = new Thickness(16) };
        root.Children.Add(Note("Choose bindings and a gamepad. Click the Game view to take control; Escape or gamepad Start releases it."));
        Add(root, "Bindings", profile, "Bindings");
        Add(root, "Profile file", path, "Profile path");
        root.Children.Add(Button("Browse profile", async () => await Browse()));
        Add(root, "Gamepad assignment", policy, "Assignment");
        Add(root, "Device", device, "Device");
        root.Children.Add(Button("Refresh devices", RefreshDevices));
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        actions.Children.Add(Button("Apply", Apply)); actions.Children.Add(Button("Reload selection", Reload)); root.Children.Add(actions);
        AutomationProperties.SetName(format, "Game Input Profile status"); root.Children.Add(format);
        AutomationProperties.SetName(status, "Game Input Device status"); root.Children.Add(status);
        AutomationProperties.SetName(message, "Game Input Message"); root.Children.Add(message);
        Content = new ScrollViewer { Content = root };
        profile.SelectionChanged += (_, _) => path.IsEnabled = profile.SelectedIndex == 2;
        policy.SelectionChanged += (_, _) => device.IsEnabled = policy.SelectedIndex == 2;
        Reload(); Guard(RefreshDevices);
        timer.Tick += (_, _) => Guard(Observe); timer.Start();
        Closed += (_, _) => { retired = true; timer.Stop(); };
        Opened += (_, _) => { var screen = Screens.ScreenFromWindow(this); if (screen is not null) { Height = Math.Min(Height, screen.WorkingArea.Height / RenderScaling * .9); Width = Math.Min(Width, screen.WorkingArea.Width / RenderScaling * .95); } };
    }
    private static TextBlock Note(string text) => new() { Text = text, TextWrapping = TextWrapping.Wrap, Foreground = EditorTheme.Brush("#B4B6BF") };
    private static void Add(StackPanel root, string label, Control control, string name)
    {
        root.Children.Add(new TextBlock { Text = label }); control.HorizontalAlignment = HorizontalAlignment.Stretch;
        AutomationProperties.SetName(control, "Game Input " + name); root.Children.Add(control);
    }
    private Button Button(string label, Action action)
    {
        var button = new Button { Content = label, HorizontalAlignment = HorizontalAlignment.Left };
        button.Classes.Add("editor-button"); AutomationProperties.SetName(button, "Game Input " + label);
        button.Click += (_, _) => Guard(action); return button;
    }
    private void Guard(Action action) { try { action(); } catch (Exception error) { if (!retired) { message.Text = error.Message; editor.Note(error.Message); } } }
    private void Reload()
    {
        profile.SelectedIndex = input.ProfilePath is not null ? 2 : input.Defaults == "keyboard_mouse_gamepad" ? 1 : 0;
        path.Text = input.ProfilePath ?? "";
        policy.SelectedIndex = input.GamepadMode switch { "only_connected" => 1, "explicit" => 2, _ => 0 };
        device.ItemsSource = new[] { new Device(input.GamepadId, input.GamepadId == 0 ? "Choose a device" : "Selected device · " + input.GamepadId) };
        device.SelectedIndex = 0; message.Text = ""; Observe();
    }
    private void RefreshDevices()
    {
        var selected = (device.SelectedItem as Device)?.Id ?? input.GamepadId;
        var result = editor.Host.Call("desktop.input.devices");
        var choices = result["devices"]!.AsArray().Select(value => new Device(value!["id"]!.GetValue<uint>(), value["name"]!.GetValue<string>() + " · " + value["id"])).ToList();
        if (selected != 0 && !choices.Any(value => value.Id == selected)) choices.Insert(0, new(selected, "Disconnected device · " + selected));
        choices.Insert(0, new(0, choices.Count == 0 ? "No gamepads connected" : "Choose a device"));
        device.ItemsSource = choices; device.SelectedItem = choices.First(value => value.Id == selected);
        message.Text = result["available"]?.GetValue<bool>() == false ? "Gamepad support is unavailable in this build." : "Device list refreshed.";
        Observe();
    }
    private void Apply()
    {
        var defaults = profile.SelectedIndex == 1 ? "keyboard_mouse_gamepad" : "keyboard_mouse";
        var mode = policy.SelectedIndex switch { 1 => "only_connected", 2 => "explicit", _ => "disabled" };
        input.SelectConfiguration(defaults, profile.SelectedIndex == 2 ? path.Text?.Trim() ?? "" : null, mode,
            mode == "explicit" ? (device.SelectedItem as Device)?.Id ?? 0 : 0);
        message.Text = input.ConfigurationPending ? "Saved for the next Game capture." : "Applied. Click the Game view to take control.";
        Observe();
    }
    private async Task Browse()
    {
        try
        {
            var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions { Title = "Choose input profile", AllowMultiple = false,
                FileTypeFilter = new[] { new FilePickerFileType("Poima input profile") { Patterns = new[] { "*.poima-input.json" } } } });
            if (retired || files.Count == 0) return;
            path.Text = files[0].TryGetLocalPath() ?? throw new InvalidOperationException("Choose a local input profile."); profile.SelectedIndex = 2;
        }
        catch (Exception error) { if (!retired) message.Text = error.Message; }
    }
    private void Observe()
    {
        var state = editor.Host.State["input"];
        var pad = state?["gamepad"];
        format.Text = (input.ConfigurationPending ? "For next Game capture: " : "Selected bindings: ") + input.ProfileFormat
            + (input.ProfilePath is string source ? " · " + Path.GetFileName(source) : " · built-in");
        if (state?["configured"]?.GetValue<bool>() != true) { status.Text = "No active runtime input configuration."; return; }
        if (pad?["policy"]?.GetValue<string>() == "disabled") { status.Text = "Gamepad disabled."; return; }
        var connected = pad?["connected"]?.GetValue<bool>() == true;
        var active = pad?["active"]?.GetValue<bool>() == true;
        var assignment = pad?["policy"]?.GetValue<string>() == "explicit" ? "Explicit device" : "Only connected gamepad";
        status.Text = assignment + " · " + (pad?["name"]?.GetValue<string>() ?? "No assigned gamepad")
            + "\n" + (connected ? "Connected" : "Disconnected")
            + (connected && active ? pad?["armed"]?.GetValue<bool>() == true ? " · Armed" : " · Waiting for neutral controls" : "")
            + " · " + (active ? "Active" : "Inactive")
            + "\n" + pad?["detail"]?.GetValue<string>();
        if (pad?["error"]?.GetValue<string>() is string error) status.Text += "\n" + error;
    }
    internal JsonObject RenderForQualification(string destination, string projectRoot)
    {
        if (Program.Options.Script is null || !IsVisible || !IsMeasureValid || !IsArrangeValid) throw new InvalidOperationException("Game Input render requires a measured script-owned window.");
        var outputPath = Path.GetFullPath(destination);
        var project = Path.GetFullPath(projectRoot).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (outputPath.StartsWith(project, StringComparison.OrdinalIgnoreCase) || Path.GetExtension(outputPath).ToLowerInvariant() != ".png") throw new ArgumentException("Game Input render must be a new PNG outside the project.");
        var parent = new DirectoryInfo(Path.GetDirectoryName(outputPath)!);
        if (!parent.Exists) throw new ArgumentException("Output directory must exist.");
        for (var check = parent; check is not null; check = check.Parent) if ((check.Attributes & FileAttributes.ReparsePoint) != 0) throw new ArgumentException("Output cannot traverse reparse points.");
        var width = Math.Ceiling(Bounds.Width * RenderScaling); var height = Math.Ceiling(Bounds.Height * RenderScaling);
        if (width is < 32 or > 4096 || height is < 32 or > 4096) throw new InvalidOperationException("Game Input render exceeds size budget.");
        using var image = new RenderTargetBitmap(new PixelSize((int)width, (int)height), new Vector(96 * RenderScaling, 96 * RenderScaling));
        image.Render(this); using var encoded = new MemoryStream(); image.Save(encoded, PngBitmapEncoderOptions.Default); encoded.Position = 0;
        using var output = new FileStream(outputPath, FileMode.CreateNew, FileAccess.Write, FileShare.None); encoded.CopyTo(output); output.Flush(true);
        return new() { ["kind"] = "avalonia_visual_content", ["path"] = outputPath, ["width"] = (int)width, ["height"] = (int)height,
            ["qualification"] = "Attached Game Input visual content only; not an OS screenshot or physical device qualification." };
    }
    public JsonObject Inspect()
    {
        Observe();
        return new() { ["bindings"] = profile.SelectedIndex, ["assignment"] = policy.SelectedIndex,
        ["device"] = (device.SelectedItem as Device)?.Id ?? 0, ["path"] = path.Text, ["message"] = message.Text,
        ["profile_status"] = format.Text, ["device_status"] = status.Text, ["input"] = input.Inspect() };
    }
}
