// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;

namespace Poima.Editor;

internal sealed class LauncherWindow : Window
{
    private readonly Func<ProjectLocation, bool, Task> openProject;
    private readonly string projectsDirectory;
    private readonly TextBox projectName = new() { PlaceholderText = "Project name", MaxLength = 96, MinHeight = 34 };
    private readonly StackPanel actions = new() { Spacing = 10 };
    private readonly TextBlock status = new() { TextWrapping = TextWrapping.Wrap };
    private readonly CheckBox advanced = new() { Content = "Open in the full editor" };

    public LauncherWindow(Func<ProjectLocation, bool, Task> openProject, string? projectsDirectory = null)
    {
        this.openProject = openProject;
        this.projectsDirectory = projectsDirectory ?? Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "Poima Projects");
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
        var create = ActionButton("Create project", () => CreateProject(this.projectsDirectory));
        create.Background = EditorTheme.Brush("#722F37");
        create.Foreground = EditorTheme.Brush("#F7E8EB");
        var row = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), ColumnSpacing = 8 };
        row.Children.Add(projectName);
        Grid.SetColumn(create, 1);
        row.Children.Add(create);
        actions.Children.Add(row);
        actions.Children.Add(new TextBlock { Text = this.projectsDirectory, TextWrapping = TextWrapping.Wrap,
            FontSize = 12, Foreground = EditorTheme.Brush("#B4ACB2") });
        var projects = LocalProjects();
        if (projects.Length != 0)
        {
            var recent = new ComboBox { ItemsSource = projects, SelectedIndex = 0, HorizontalAlignment = HorizontalAlignment.Stretch };
            AutomationProperties.SetName(recent, "Local projects");
            actions.Children.Add(recent);
            actions.Children.Add(ActionButton("Open selected project", async () =>
            {
                if (recent.SelectedItem is LocalProject item) await Open(item.Manifest);
            }));
        }
        actions.Children.Add(ActionButton("Open project from disk…", PickProject));
        actions.Children.Add(advanced);
        actions.Children.Add(new Expander { Header = "Project location", Content = ActionButton("Create in another folder…", ChooseFolder) });
        if (OperatingSystem.IsWindows() && WindowsGameplayTools.HasAuthoringPack && Program.LauncherDevelopmentProfiles is null)
            actions.Children.Add(ActionButton("Set up gameplay builds", () => new GameplaySetupWindow().ShowDialog<bool>(this)));
        if (File.Exists(ProjectFiles.PreviewSample))
            actions.Children.Add(ActionButton("Play Collection Room", PlaySample));

        var content = new StackPanel { Spacing = 24, Margin = new Thickness(30) };
        content.Children.Add(wordmark);
        content.Children.Add(actions);
        status.IsVisible = !string.IsNullOrEmpty(status.Text);
        content.Children.Add(status);
        Content = content;
    }

    private sealed record LocalProject(string Name, string Manifest)
    {
        public override string ToString() => Name;
    }

    private LocalProject[] LocalProjects()
    {
        try
        {
            if (!Directory.Exists(projectsDirectory)) return [];
            return new DirectoryInfo(projectsDirectory).EnumerateDirectories().Take(200)
                .Where(directory => (directory.Attributes & FileAttributes.ReparsePoint) == 0)
                .OrderByDescending(directory => directory.LastWriteTimeUtc)
                .Select(directory => new LocalProject(directory.Name, Path.Combine(directory.FullName, ProjectFiles.DefaultManifestName)))
                .Where(project => File.Exists(project.Manifest)).Take(30).ToArray();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            status.Text = "Couldn’t list local projects. " + error.Message;
            return [];
        }
    }

    private Button ActionButton(string label, Func<Task> run)
    {
        var button = new Button { Content = label, Padding = new Thickness(14, 8),
            HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Center };
        AutomationProperties.SetName(button, label);
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

    private async Task CreateProject(string parent)
    {
        var name = projectName.Text?.Trim();
        if (string.IsNullOrWhiteSpace(name) || name is "." or ".." || name.EndsWith('.') ||
            name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
            throw new ArgumentException("Enter a valid project folder name.");
        Directory.CreateDirectory(parent);
        string[] arguments = ["create", Path.Combine(parent, name), "--name", name];
        if (WindowsGameplayTools.HasAuthoringPack) arguments = [.. arguments, "--gameplay"];
        var created = await ProjectFiles.Command(arguments);
        await Open(created.GetProperty("manifest").GetString()!);
    }

    private async Task Open(string manifest)
    {
        var selected = await ProjectFiles.Open(manifest);
        if (IsVisible) await openProject(selected, advanced.IsChecked == true);
    }

    private async Task ChooseFolder()
    {
        var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
            { Title = "Choose where to create the project", AllowMultiple = false });
        if (folders.Count == 0) return;
        var parent = folders[0].TryGetLocalPath() ?? throw new InvalidOperationException("Choose a local folder.");
        await CreateProject(parent);
    }

    private async Task PickProject()
    {
        var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Open a Poima project", AllowMultiple = false,
            FileTypeFilter = [new FilePickerFileType("Poima project") { Patterns = ["*" + ProjectFiles.Extension, ProjectFiles.LegacyManifestName] }]
        });
        if (files.Count != 0)
            await Open(files[0].TryGetLocalPath() ?? throw new InvalidOperationException("Choose a local project."));
    }

    private static async Task PlaySample()
    {
        var executable = Path.Combine(ProjectFiles.PreviewRuntime, "bin", "poima.exe");
        using var process = ProjectFiles.Start(executable, "game", "run", ProjectFiles.PreviewSample, "--default-save-root", "true");
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
