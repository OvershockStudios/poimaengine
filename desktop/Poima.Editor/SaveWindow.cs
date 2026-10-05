// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using System.Text.Json.Nodes;

namespace Poima.Editor;

public sealed class SaveWindow : Window
{
    private readonly EditorModel editor;
    private readonly SaveEditorModel model;
    private bool forceClose, retired, syncing;
    private readonly TextBlock message = new() { TextWrapping = TextWrapping.Wrap };
    public SaveWindow(EditorModel editor, SaveEditorModel model)
    {
        this.editor = editor; this.model = model;
        Title = "Save / Load — Poima"; Width = 610; Height = 640; MinWidth = 420; MinHeight = 420;
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter"); FontSize = 12;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        var root = new StackPanel { Spacing = 10, Margin = new Thickness(16) };
        root.Children.Add(new TextBlock { Text = "Save / Load", FontSize = 18 });
        root.Children.Add(Note("Save Play state, or return to a checkpoint. Your authored scene stays unchanged."));
        var rootField = Field("Root", model.RootDraft); Observe(rootField, model.SetRoot);
        var browse = IconButton("Browse root", "folder", () => _ = Browse(rootField));
        ToolTip.SetTip(browse, "Choose a checkpoint storage folder");
        var refresh = IconButton("Refresh configuration", "refresh", model.RefreshConfiguration);
        ToolTip.SetTip(refresh, "Reload the configured folder and discard the folder draft");
        root.Children.Add(Row("Storage folder", Inline(rootField, browse, refresh)));
        var configure = Button("Configure root", () => model.Configure()); configure.Content = "Use folder";
        ToolTip.SetTip(configure, "Use this existing folder for this editor session");
        var clear = Button("Clear root", () => model.Configure(true)); clear.Content = "Forget folder";
        ToolTip.SetTip(clear, "Disconnect this session's storage folder. Saved files stay on disk.");
        var storageActions = new WrapPanel(); storageActions.Children.Add(configure); storageActions.Children.Add(clear); root.Children.Add(storageActions);
        var configuration = Summary("Configuration status"); root.Children.Add(configuration);
        root.Children.Add(new Separator());
        var slotField = Field("Slot", model.SlotDraft); Observe(slotField, model.SetSlot);
        ToolTip.SetTip(slotField, "1–64 lowercase letters, digits, - or _");
        var inspect = IconButton("Inspect slot", "refresh", model.InspectSlot);
        ToolTip.SetTip(inspect, "Inspect this slot and the current Play state before saving or loading");
        root.Children.Add(Row("Slot", Inline(slotField, inspect)));
        var status = Summary("Slot status"); root.Children.Add(status);
        var observation = Summary("Observation status"); root.Children.Add(observation);
        var recoverySection = new StackPanel { Spacing = 5 };
        var recoveryReason = Note(""); recoverySection.Children.Add(recoveryReason);
        var recovery = Check("Allow recovered load", "Load the verified older checkpoint", model.SetAllowRecovery);
        var acknowledge = Check("Acknowledge payload recovery", "Save a new checkpoint and keep the damaged file", model.SetAcknowledgeRecovery);
        recoverySection.Children.Add(recovery); recoverySection.Children.Add(acknowledge); root.Children.Add(recoverySection);
        var configuredGameplay = Check("Use configured gameplay", "Use configured C# gameplay when loading", model.SetUseConfiguredGameplay);
        ToolTip.SetTip(configuredGameplay, "While stopped, load with the trusted launch profile observed by Inspect. Uncheck to load without gameplay. The save must match the original assembly and type.");
        root.Children.Add(configuredGameplay);
        var gameplayStatus = Summary("Gameplay mode"); root.Children.Add(gameplayStatus);
        ToolTip.SetTip(gameplayStatus, "An active run supplies its gameplay module. Stop first to choose no gameplay. Saves require the exact original assembly and type.");
        var actions = new WrapPanel();
        var write = Button("Write checkpoint", model.Write); write.Content = "Save checkpoint";
        ToolTip.SetTip(write, "Save the paused simulation using the inspected slot and Play state");
        var load = Button("Load checkpoint", model.Load);
        ToolTip.SetTip(load, "Replace the current Play state with the inspected checkpoint; keep the authored scene");
        actions.Children.Add(write); actions.Children.Add(load); root.Children.Add(actions);
        var lastResult = Summary("Last result"); root.Children.Add(lastResult);
        var pendingSection = new StackPanel { Spacing = 5 };
        var pendingStatus = Summary("Pending status"); pendingSection.Children.Add(pendingStatus);
        var pendingActions = new WrapPanel();
        var retry = Button("Retry pending", model.RetryPending); var dismiss = Button("Dismiss pending", model.DiscardPending);
        ToolTip.SetTip(retry, "Retry the same operation ID, without creating another save or load");
        ToolTip.SetTip(dismiss, "Abandon this retained retry. Inspect again before creating another operation.");
        pendingActions.Children.Add(retry); pendingActions.Children.Add(dismiss); pendingSection.Children.Add(pendingActions); root.Children.Add(pendingSection);
        AutomationProperties.SetName(message, "Save Message"); root.Children.Add(message);
        var technical = Readout("Technical details");
        var details = new Expander { Header = "Details", Content = technical, IsExpanded = false };
        AutomationProperties.SetName(details, "Save Details"); root.Children.Add(details);
        Content = new ScrollViewer { Content = root, HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled };
        void Sync()
        {
            syncing = true;
            try
            {
                if (rootField.Text != model.RootDraft) rootField.Text = model.RootDraft;
                if (slotField.Text != model.SlotDraft) slotField.Text = model.SlotDraft;
                configuration.Text = model.ConfigConflict ? "Folder changed elsewhere. Refresh to review it." :
                    model.RootDirty ? "Folder change not applied." : model.ConfigObservation["root"] is null ? "Choose a folder to get started." : "Folder ready.";
                var seen = model.RuntimeObservation;
                observation.Text = seen is null ? "Inspect the slot before saving or loading." :
                    (seen["session_id"] is null ? "Inspected while Play was stopped." : $"Inspected Play at tick {seen["tick"]}.") +
                    (model.ObservationStale ? " State changed — inspect again before a new operation." : model.Playing ? " Pause to save or load." : "");
                var slot = model.SlotObservation;
                status.Text = slot is null ? "" : slot["exists"]?.GetValue<bool>() != true ? "Empty slot — ready for its first checkpoint." :
                    model.ObservedRecovery ? "The latest checkpoint is damaged. An older checkpoint is available." : "Checkpoint available.";
                recoverySection.IsVisible = model.ObservedRecovery;
                recoveryReason.Text = slot?["manifest_recovered"]?.GetValue<bool>() == true ?
                    "The slot index was recovered. Loading is available; use a new slot for further saves." :
                    "Recovery keeps the damaged file for inspection. Choose explicitly before loading or saving.";
                ToolTip.SetTip(recoveryReason, slot?["recovery_reason"]?.ToString());
                recovery.IsChecked = model.AllowRecovery; acknowledge.IsChecked = model.AcknowledgeRecovery;
                recovery.IsEnabled = model.ObservedRecovery; acknowledge.IsEnabled = model.ObservedRecovery && slot?["manifest_recovered"]?.GetValue<bool>() != true;
                configuredGameplay.IsChecked = model.UseConfiguredGameplay; configuredGameplay.IsVisible = editor.RuntimeId is null;
                configuredGameplay.IsEnabled = editor.RuntimeId is null;
                var profile = model.GameplayObservation?["profile"];
                gameplayStatus.Text = editor.RuntimeId is not null ? "Gameplay: use the active run's configuration." :
                    !model.UseConfiguredGameplay ? "Gameplay: none." : profile is null ? "Gameplay: no launch profile observed." : $"Gameplay: {profile["type"]}";
                configure.IsEnabled = clear.IsEnabled = !model.Playing;
                write.IsEnabled = model.CanWrite; load.IsEnabled = model.CanLoad;
                retry.IsEnabled = model.Pending is not null && !model.Playing; dismiss.IsEnabled = model.Pending is not null;
                pendingSection.IsVisible = model.Pending is not null;
                pendingStatus.Text = "An operation needs attention. Retry it unchanged, or dismiss it before starting another.";
                lastResult.IsVisible = model.LastResult is not null;
                lastResult.Text = model.LastResult is not { } result ? "" : result["slot"] is null ? "Storage folder updated." :
                    result["tick"] is not null ? $"Loaded {result["slot"]} at tick {result["tick"]}." : $"Saved checkpoint to {result["slot"]}.";
                technical.Text = $"Storage generation: observed {model.Generation}, current {model.CurrentGeneration}\n" +
                    (seen is null ? "No runtime observation." : $"Authored revision: {seen["authored_revision"]}\nSession: {seen["session_id"]?.ToString() ?? "stopped"}\nTick: {seen["tick"]} · gameplay revision: {seen["gameplay_revision"]}") +
                    (slot is null ? "" : $"\nSlot {slot["slot"]}: generation {slot["generation"]}\nSelected: {slot["selected"]?["generation"]} · {slot["selected"]?["bytes"]} bytes\nPrevious: {slot["previous"]?["generation"]?.ToString() ?? "none"}\nQuarantined: {slot["quarantined"]?["generation"]?.ToString() ?? "none"}") +
                    (model.ObservedRecovery ? $"\nRecovery: {slot?["recovery_reason"]}" : "") +
                    (profile is null ? "" : $"\nTrusted gameplay: {profile["type"]}\n{profile["assembly"]}") +
                    (model.Pending is not { } pending ? "" : $"\nPending {pending["method"]}\nOperation: {pending["params"]?["request_id"]}") +
                    (model.LastResult is not { } previous ? "" : $"\nLast completed generation: {previous["generation"]}" + (previous["replayed"]?.GetValue<bool>() == true ? " · replayed receipt" : ""));
                message.Text = model.Error ?? ""; message.IsVisible = model.Error is not null;
            }
            finally { syncing = false; }
        }
        EventHandler changed = (_, _) => Sync();
        model.Changed += changed; editor.PlaybackChanged += changed;
        Closed += (_, _) => { retired = true; model.Changed -= changed; editor.PlaybackChanged -= changed; };
        Closing += (_, args) =>
        {
            if (!forceClose && model.Dirty) { args.Cancel = true; model.Fail(new InvalidOperationException("Configure or refresh the folder draft, and retry or dismiss the pending operation before closing.")); }
        };
        Opened += (_, _) => { var screen = Screens.ScreenFromWindow(this); if (screen is not null) Height = Math.Min(Height, screen.WorkingArea.Height / RenderScaling * .9); };
        Sync();
    }
    private static TextBlock Note(string text) => new() { Text = text, TextWrapping = TextWrapping.Wrap, Foreground = EditorTheme.Brush("#A0A0A5") };
    private static TextBlock Summary(string name)
    {
        var value = new TextBlock { TextWrapping = TextWrapping.Wrap };
        AutomationProperties.SetName(value, "Save " + name); return value;
    }
    private static Grid Inline(Control field, params Control[] actions)
    {
        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("*" + string.Concat(actions.Select(_ => ",Auto"))) };
        Grid.SetColumn(field, 0); grid.Children.Add(field);
        for (var i = 0; i < actions.Length; ++i)
        {
            actions[i].Margin = new Thickness(5, 0, 0, 0);
            Grid.SetColumn(actions[i], i + 1); grid.Children.Add(actions[i]);
        }
        return grid;
    }
    private static TextBox Readout(string name)
    {
        var value = new TextBox { IsReadOnly = true, TextWrapping = TextWrapping.Wrap, FontSize = 12 };
        AutomationProperties.SetName(value, "Save " + name); return value;
    }
    private static TextBox Field(string name, string text)
    {
        var field = new TextBox { Text = text, MinWidth = 80, FontSize = 12 }; AutomationProperties.SetName(field, "Save " + name); return field;
    }
    private static StackPanel Row(string title, Control control)
    {
        var row = new StackPanel { Spacing = 3 }; row.Children.Add(new TextBlock { Text = title, TextWrapping = TextWrapping.Wrap }); row.Children.Add(control); return row;
    }
    private void Observe(TextBox control, Action<string> edit)
    {
        control.TextChanged += (_, _) => { if (!retired && !syncing) edit(control.Text ?? ""); };
    }
    private Button Button(string name, Action action)
    {
        var button = new Button { Content = name, Margin = new Thickness(0, 3, 6, 3) }; button.Classes.Add("editor-button");
        AutomationProperties.SetName(button, "Save " + name); ToolTip.SetTip(button, name);
        button.Click += (_, _) => { try { action(); } catch (Exception error) { model.Fail(error); } }; return button;
    }
    private Button IconButton(string name, string icon, Action action)
    {
        var button = Button(name, action); button.Content = EditorIcons.Make(icon); return button;
    }
    private CheckBox Check(string name, string label, Action<bool> action)
    {
        var check = new CheckBox { Content = new TextBlock { Text = label, TextWrapping = TextWrapping.Wrap } };
        AutomationProperties.SetName(check, "Save " + name); ToolTip.SetTip(check, label);
        check.IsCheckedChanged += (_, _) => { if (retired || syncing) return; try { action(check.IsChecked == true); } catch (Exception error) { model.Fail(error); } };
        return check;
    }
    private async Task Browse(TextBox target)
    {
        var observedDraft = model.RootDraft;
        var observedGeneration = model.Generation;
        var observedCurrentGeneration = model.CurrentGeneration;
        try
        {
            var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = "Choose existing checkpoint storage folder", AllowMultiple = false });
            if (folders.Count == 1 && folders[0].TryGetLocalPath() is string path && IsVisible)
            {
                if (model.RootDraft != observedDraft || model.Generation != observedGeneration || model.CurrentGeneration != observedCurrentGeneration)
                    throw new InvalidOperationException("The storage draft or configuration changed while the picker was open. The current draft was retained; choose the folder again.");
                target.Text = path;
            }
        }
        catch (Exception error) { model.Fail(error); }
    }
    public void CloseForOwner() { forceClose = true; Close(); }
    internal void ScrollForQualification(string position)
    {
        if (Program.Options.Script is null || Content is not ScrollViewer scroll) throw new InvalidOperationException("Save visual qualification requires an explicit script.");
        if (position == "top") scroll.ScrollToHome(); else if (position == "bottom") scroll.ScrollToEnd(); else throw new ArgumentException("Save scroll position must be top or bottom.");
    }
    internal JsonObject RenderForQualification(string destination, string projectRoot)
    {
        if (Program.Options.Script is null || !IsVisible || !IsMeasureValid || !IsArrangeValid)
            throw new InvalidOperationException("Save visual qualification requires a visible, measured script-owned tool window.");
        var path = Path.GetFullPath(destination);
        var project = Path.GetFullPath(projectRoot).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (path.StartsWith(project, StringComparison.OrdinalIgnoreCase) || Path.GetExtension(path).ToLowerInvariant() != ".png")
            throw new ArgumentException("Save visual output must be a new PNG outside the project directory.");
        var parent = new DirectoryInfo(Path.GetDirectoryName(path)!);
        if (!parent.Exists || File.Exists(path) || Directory.Exists(path)) throw new ArgumentException("Save visual output requires a new file in an existing directory.");
        for (var check = parent; check is not null; check = check.Parent)
            if ((check.Attributes & FileAttributes.ReparsePoint) != 0) throw new ArgumentException("Save visual output cannot traverse a reparse point.");
        var width = Math.Ceiling(Bounds.Width * RenderScaling); var height = Math.Ceiling(Bounds.Height * RenderScaling);
        if (!double.IsFinite(width) || !double.IsFinite(height) || width is < 32 or > 4096 || height is < 32 or > 4096 || width * height > 16777216)
            throw new InvalidOperationException("Save visual dimensions exceed the qualification budget.");
        using var image = new RenderTargetBitmap(new PixelSize((int)width, (int)height), new Vector(96 * RenderScaling, 96 * RenderScaling));
        image.Render(this);
        using var encoded = new MemoryStream(); image.Save(encoded); encoded.Position = 0;
        using (var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None)) { encoded.CopyTo(output); output.Flush(true); }
        return new() { ["kind"] = "avalonia_visual_content", ["path"] = path, ["width"] = (int)width, ["height"] = (int)height,
            ["scale"] = RenderScaling, ["scroll_y"] = (Content as ScrollViewer)?.Offset.Y ?? 0,
            ["qualification"] = "Attached Save / Load client visual only; not an OS screenshot, display/compositor or physical-input qualification." };
    }
}
