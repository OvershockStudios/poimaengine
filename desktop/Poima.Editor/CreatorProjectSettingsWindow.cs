// SPDX-License-Identifier: Apache-2.0
using System.Text.Json;
using System.Text.Json.Nodes;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Markup.Xaml.Styling;
using Avalonia.Media;

namespace Poima.Editor;

internal sealed class CreatorProjectSettingsWindow : Window
{
    public CreatorProjectSettingsWindow(JsonElement current)
    {
        var splash = new CheckBox
        {
            Content = "Show the Poima splash in exported games",
            IsChecked = current.GetProperty("startup_splash").GetBoolean()
        };
        AutomationProperties.SetName(splash, "Show the Poima splash in exported games");

        var currentRender = current.TryGetProperty("startup_render", out var render)
            ? JsonNode.Parse(render.GetRawText()) : null;
        var graphics = new StackPanel { Spacing = 8 };
        var lighting = AddChoice("Lighting", "Export lighting", "lighting_path",
            ("Forward", "forward"), ("Deferred", "deferred"));
        var antialiasing = AddChoice("Post-process antialiasing", "Export post-process antialiasing", "post_aa",
            ("Off", "none"), ("FXAA", "fxaa"));
        var reconstruction = AddChoice("Reconstruction", "Export reconstruction", "reconstruction",
            ("Off", "none"), ("FSR 3 Native AA", "fsr3_native"), ("FSR 3 Quality", "fsr3_quality"),
            ("FSR 3 Balanced", "fsr3_balanced"), ("FSR 3 Performance", "fsr3_performance"));
        var samples = AddChoice("World samples", "Export world samples", "samples",
            ("1 sample", 1), ("4 samples", 4));
        bool needsSpriteParticles = current.TryGetProperty("needs_sprite_particles", out var spriteParticles)
            && spriteParticles.GetBoolean();

        ComboBox AddChoice(string label, string automationName, string key, params (string Label, JsonNode Value)[] options)
        {
            var choice = new ComboBox { HorizontalAlignment = HorizontalAlignment.Stretch };
            var defaultItem = new ComboBoxItem { Content = "Default" };
            AutomationProperties.SetName(defaultItem, "Default");
            choice.Items.Add(defaultItem);
            foreach (var (name, value) in options)
            {
                var item = new ComboBoxItem { Content = name, Tag = value };
                AutomationProperties.SetName(item, name);
                choice.Items.Add(item);
            }
            var selected = Array.FindIndex(options, option => JsonNode.DeepEquals(option.Value, currentRender?[key]));
            choice.SelectedIndex = selected + 1;
            AutomationProperties.SetName(choice, automationName);
            graphics.Children.Add(new StackPanel
            {
                Spacing = 4, Children = { new TextBlock { Text = label }, choice }
            });
            return choice;
        }

        static JsonNode? Value(ComboBox choice) => (choice.SelectedItem as ComboBoxItem)?.Tag as JsonNode;

        var save = new Button { Content = "Save", IsDefault = true };
        save.Classes.Add("primary");
        var cancel = new Button { Content = "Cancel", IsCancel = true };
        var validation = new TextBlock
        {
            TextWrapping = TextWrapping.Wrap, Foreground = EditorTheme.Brush("#D46A7E")
        };
        AutomationProperties.SetName(validation, "Export graphics validation");
        void ValidateSelection()
        {
            bool deferred = Value(lighting)?.GetValue<string>() == "deferred";
            bool fxaa = Value(antialiasing)?.GetValue<string>() == "fxaa";
            bool useFsr = Value(reconstruction)?.GetValue<string>() is not (null or "none");
            bool multisample = Value(samples)?.GetValue<int>() == 4;
            string? conflict = null;
            if (fxaa && useFsr)
                conflict = "Choose either FXAA or FSR.";
            else if (multisample && (deferred || fxaa || useFsr || needsSpriteParticles))
                conflict = needsSpriteParticles ? "World particle sprites require Default or 1 world sample."
                    : "Deferred lighting, FXAA, and FSR require Default or 1 world sample.";
            validation.Text = conflict;
            validation.IsVisible = conflict is not null;
            save.IsEnabled = conflict is null;
        }
        foreach (var choice in new[] { lighting, antialiasing, reconstruction, samples })
            choice.SelectionChanged += (_, _) => ValidateSelection();
        ValidateSelection();

        Title = "Project settings";
        Width = 460;
        SizeToContent = SizeToContent.Height;
        CanResize = false;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        ShowInTaskbar = false;
        Content = new StackPanel
        {
            Spacing = 16, Margin = new Thickness(24), Children =
            {
                splash,
                new TextBlock
                {
                    Text = "Graphics at launch for standalone exports. Default uses the usual game settings.",
                    TextWrapping = TextWrapping.Wrap, Foreground = EditorTheme.Brush("#EBC7CE")
                },
                graphics,
                validation,
                new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right,
                    Spacing = 8, Children = { cancel, save } }
            }
        };
        Styles.Add(new StyleInclude(new Uri("avares://Poima.Editor/"))
        { Source = new Uri("avares://Poima.Editor/Themes/CreatorControls.axaml") });
        save.Click += (_, _) =>
        {
            if (!save.IsEnabled) return;
            var startupRender = new JsonObject();
            foreach (var (key, choice) in new[] { ("lighting_path", lighting), ("post_aa", antialiasing),
                ("reconstruction", reconstruction), ("samples", samples) })
            {
                if (Value(choice) is { } value) startupRender[key] = value.DeepClone();
            }
            Close(new JsonObject
            {
                ["startup_splash"] = splash.IsChecked == true,
                ["startup_render"] = startupRender.Count > 0 || currentRender is JsonObject { Count: 0 } ? startupRender : null
            });
        };
        cancel.Click += (_, _) => Close();
    }
}
