// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using System.Diagnostics;
using System.Text.Json;

namespace Poima.Editor;

internal sealed class LauncherWindow : Window
{
    private static readonly string Root = AppContext.BaseDirectory;
    private readonly Action<string,string> openWorld;
    private readonly TextBox projectName = new() { PlaceholderText = "Project name", MaxLength = 96, MinHeight = 34 };
    private readonly StackPanel actions = new() { Spacing = 10 };
    private readonly TextBlock status = new() { TextWrapping = TextWrapping.Wrap };

    public LauncherWindow(Action<string,string> openWorld)
    {
        this.openWorld = openWorld;
        Title = "Poima";
        Width = 510;
        MinWidth = 420;
        MaxWidth = 640;
        SizeToContent = SizeToContent.Height;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        FontFamily = new FontFamily("avares://Poima.Editor/Assets#Inter");
        FontSize = 14;
        Background = EditorTheme.Brush("#19191C");
        Foreground = EditorTheme.Brush("#F2EDEE");
        AutomationProperties.SetName(projectName, "Project name");

        var wordmark = new TextBlock { Text = "Poima", FontWeight = FontWeight.SemiBold, FontSize = 32 };
        var create = ActionButton("Create project", CreateProject);
        create.Background = EditorTheme.Brush("#722F37");
        create.Foreground = EditorTheme.Brush("#F7E8EB");
        var row = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), ColumnSpacing = 8 };
        row.Children.Add(projectName);
        Grid.SetColumn(create, 1);
        row.Children.Add(create);
        actions.Children.Add(row);
        actions.Children.Add(ActionButton("Open project…", PickProject));
        if (File.Exists(SampleManifest))
            actions.Children.Add(ActionButton("Play Collection Room", PlaySample));

        var content = new StackPanel { Spacing = 24, Margin = new Thickness(30) };
        content.Children.Add(wordmark);
        content.Children.Add(actions);
        status.IsVisible = false;
        content.Children.Add(status);
        Content = content;
    }

    private static string SampleManifest => Path.Combine(Root, "samples", "Collection Room", "game.json");

    private Button ActionButton(string label, Func<Task> run)
    {
        var button = new Button { Content = label, Padding = new Thickness(14, 8),
            HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Center };
        button.Click += async (_, _) =>
        {
            actions.IsEnabled = false;
            status.IsVisible = false;
            try { await run(); }
            catch (Exception error)
            {
                status.Text = error.Message;
                status.Foreground = EditorTheme.Brush("#D46A7E");
                status.IsVisible = true;
            }
            finally { actions.IsEnabled = true; }
        };
        return button;
    }

    private async Task CreateProject()
    {
        var name = projectName.Text?.Trim();
        if (string.IsNullOrWhiteSpace(name) || name is "." or ".." || name.EndsWith('.') ||
            name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
            throw new ArgumentException("Enter a valid project folder name.");
        var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
            { Title = "Choose where to create the project", AllowMultiple = false });
        if (folders.Count == 0) return;
        var parent = folders[0].TryGetLocalPath() ?? throw new InvalidOperationException("Choose a local folder.");
        var created = await ProjectCommand("create", Path.Combine(parent, name), "--name", name);
        await OpenProject(created.GetProperty("manifest").GetString()!);
    }

    private async Task PickProject()
    {
        var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Open a Poima project", AllowMultiple = false,
            FileTypeFilter = [new FilePickerFileType("Poima project") { Patterns = ["project.json"] }]
        });
        if (files.Count != 0)
            await OpenProject(files[0].TryGetLocalPath() ?? throw new InvalidOperationException("Choose a local project."));
    }

    private async Task OpenProject(string manifest)
    {
        var project = await ProjectCommand("inspect", manifest);
        var entry = project.GetProperty("entry").GetProperty("world").GetString()!;
        var endpoint = "poima-" + project.GetProperty("project_id").GetString();
        openWorld(Path.GetFullPath(entry, Path.GetDirectoryName(manifest)!), endpoint);
    }

    private static Process Start(string executable, params string[] arguments)
    {
        var info = new ProcessStartInfo(executable)
        {
            WorkingDirectory = Root, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true
        };
        foreach (var argument in arguments) info.ArgumentList.Add(argument);
        return Process.Start(info) ?? throw new InvalidOperationException("Poima could not start.");
    }

    private static async Task<JsonElement> ProjectCommand(params string[] arguments)
    {
        using var process = Start(Path.Combine(Root, "poima.exe"), ["project", .. arguments]);
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        var body = await output;
        var stderr = await error;
        if (body.Length == 0) throw new InvalidOperationException(stderr.Length > 0 ? stderr : "Poima did not return a project result.");
        using var response = JsonDocument.Parse(body);
        if (process.ExitCode != 0)
            throw new InvalidOperationException(response.RootElement.GetProperty("diagnostics")[0].GetProperty("message").GetString());
        return response.RootElement.GetProperty("result").Clone();
    }

    private static async Task PlaySample()
    {
        var executable = Path.Combine(Path.GetDirectoryName(SampleManifest)!, "runtime", "bin", "poima.exe");
        using var process = Start(executable, "game", "run", SampleManifest, "--default-save-root", "true");
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        if (process.ExitCode == 0)
        {
            await output; await error;
            return;
        }
        var stderr = await error;
        throw new InvalidOperationException(stderr.Length > 0 ? stderr : await output);
    }
}
