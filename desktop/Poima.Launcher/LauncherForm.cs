// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Text.Json;

namespace Poima.Launcher;

internal sealed class LauncherForm : Form
{
    private static readonly Color Surface = Color.FromArgb(25, 23, 26);
    private static readonly Color Panel = Color.FromArgb(38, 33, 38);
    private static readonly Color TextColor = Color.FromArgb(247, 232, 235);
    private static readonly Color Muted = Color.FromArgb(204, 188, 194);
    private static readonly Color Wine = Color.FromArgb(114, 47, 55);
    private static readonly Color Accent = Color.FromArgb(212, 106, 126);
    private readonly string userRoot;
    private readonly string preferences;
    private readonly string? projectsRoot;
    private readonly HttpClient http = ReleaseFeed.NewClient();
    private readonly TextBox location = new() { ReadOnly = true, AccessibleName = "Install location" };
    private readonly ComboBox channel = new() { DropDownStyle = ComboBoxStyle.DropDownList, AccessibleName = "Release channel" };
    private readonly Label current = new();
    private readonly Label status = new() { AutoSize = false, Height = 44, AccessibleName = "Install status" };
    private readonly ProgressBar progress = new() { Height = 6, AccessibleName = "Download progress" };
    private readonly Button update = Button("Check and install", primary: true);
    private readonly Button installPackage = Button("Install from ZIP…");
    private readonly Button browse = Button("Browse…");
    private readonly Button open = Button("Open Poima", primary: true);
    private readonly Button project = Button("Open project…");
    private readonly Button rollback = Button("Use previous version");
    private readonly Button cancel = Button("Cancel");
    private readonly Dictionary<string, string> pins;
    private CancellationTokenSource? pending;

    private sealed record Settings(string Location, ReleaseChannel Channel, Dictionary<string, string> Projects);

    public LauncherForm(bool portable)
    {
        userRoot = portable ? Path.Combine(AppContext.BaseDirectory, "PoimaData") :
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Poima");
        preferences = Path.Combine(userRoot, "installer.json");
        projectsRoot = portable ? Path.Combine(userRoot, "Projects") : null;
        Text = "Poima Launcher";
        Font = new Font("Segoe UI", 10);
        ForeColor = TextColor;
        BackColor = Surface;
        ClientSize = new Size(660, 445);
        MinimumSize = new Size(570, 489);
        StartPosition = FormStartPosition.CenterScreen;
        var settings = LoadSettings();
        pins = new(settings.Projects, StringComparer.OrdinalIgnoreCase);

        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill, Padding = new Padding(26, 22, 26, 18), ColumnCount = 1, RowCount = 8
        };
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 84));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 35));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 49));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 49));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 43));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 16));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 47));
        Controls.Add(layout);

        using var wordmark = typeof(LauncherForm).Assembly.GetManifestResourceStream("Poima.Launcher.Poima.png")!;
        using var source = Image.FromStream(wordmark);
        var logo = new PictureBox { Width = 145, Height = 54, SizeMode = PictureBoxSizeMode.Zoom, Image = new Bitmap(source) };
        layout.Controls.Add(logo, 0, 0);
        current.Dock = DockStyle.Fill;
        current.Font = new Font(Font, FontStyle.Bold);
        layout.Controls.Add(current, 0, 1);

        channel.DataSource = Enum.GetValues<ReleaseChannel>();
        channel.SelectedItem = settings.Channel;
        location.BackColor = Panel;
        location.ForeColor = TextColor;
        location.Text = settings.Location;
        location.Dock = DockStyle.Fill;
        layout.Controls.Add(Row("Install location", location, browse), 0, 2);
        layout.Controls.Add(Row("Release channel", channel, update, cancel), 0, 3);
        var local = new FlowLayoutPanel { Dock = DockStyle.Fill };
        local.Controls.Add(installPackage);
        layout.Controls.Add(local, 0, 4);

        status.Dock = DockStyle.Fill;
        status.ForeColor = Muted;
        layout.Controls.Add(status, 0, 5);
        progress.Dock = DockStyle.Fill;
        progress.Visible = false;
        layout.Controls.Add(progress, 0, 6);
        var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false };
        actions.Controls.AddRange([open, project, rollback]);
        layout.Controls.Add(actions, 0, 7);

        browse.Click += (_, _) => PickInstallFolder();
        channel.SelectedIndexChanged += (_, _) => RunWithStatus(SaveSettings);
        update.Click += async (_, _) => await UpdateAsync(automatic: false);
        installPackage.Click += async (_, _) => await PickPackageAsync();
        cancel.Click += (_, _) => pending?.Cancel();
        rollback.Click += (_, _) => RunWithStatus(() => { status.Text = $"Poima {Install.Rollback()} is active."; RefreshInstall(); });
        open.Click += (_, _) => RunWithStatus(() => Launch(Install.State().Active));
        project.Click += (_, _) => RunWithStatus(PickProject);
        FormClosing += (_, args) =>
        {
            if (pending is null) return;
            pending.Cancel();
            args.Cancel = true;
        };
        Disposed += (_, _) => { http.Dispose(); logo.Image?.Dispose(); };
        RunWithStatus(RefreshInstall);
        ActiveControl = update;
        Shown += async (_, _) => { if (open.Enabled && pending is null) await UpdateAsync(automatic: true); };
    }

    private static Button Button(string title, bool primary = false)
    {
        var background = primary ? Wine : Panel;
        var button = new Button
        {
            Text = title, Height = 33, AutoSize = true, Padding = new Padding(9, 0, 9, 0),
            FlatStyle = FlatStyle.Flat, BackColor = background, ForeColor = TextColor, UseVisualStyleBackColor = false
        };
        button.FlatAppearance.BorderColor = background;
        button.FlatAppearance.MouseOverBackColor = primary ? Color.FromArgb(133, 57, 67) : Color.FromArgb(54, 46, 54);
        return button;
    }

    private static TableLayoutPanel Row(string title, Control field, params Control[] actions)
    {
        var row = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = actions.Length + 2, RowCount = 1 };
        row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 144));
        row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        row.Controls.Add(new Label { Text = title, AutoEllipsis = true, TextAlign = ContentAlignment.TopLeft, Dock = DockStyle.Fill,
            ForeColor = Muted, Padding = new Padding(0, 6, 0, 0) });
        row.Controls.Add(field);
        foreach (var action in actions)
        {
            action.Anchor = AnchorStyles.Top;
            row.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            row.Controls.Add(action);
        }
        return row;
    }

    private EngineInstall Install => new(location.Text, new(http));

    private void RefreshInstall()
    {
        var install = Install;
        var state = install.State();
        var ready = install.Executable(state.Active) is not null;
        current.Text = ready ? $"Poima {state.Active}" : "Install Poima to get started";
        open.Visible = project.Visible = ready;
        open.Enabled = project.Enabled = ready;
        rollback.Visible = install.Executable(state.Previous) is not null;
        rollback.Enabled = pending is null;
        cancel.Visible = pending is not null;
    }

    private void PickInstallFolder()
    {
        using var dialog = new FolderBrowserDialog { Description = "Choose the folder for Poima engine installations.",
            UseDescriptionForTitle = true, SelectedPath = location.Text };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        location.Text = dialog.SelectedPath;
        RunWithStatus(() => { SaveSettings(); status.Text = ""; RefreshInstall(); });
    }

    private async Task PickPackageAsync()
    {
        using var dialog = new OpenFileDialog
        {
            Title = "Select a Poima engine ZIP", Filter = "Poima Windows engine|Poima-*-Windows-x64.zip",
            CheckFileExists = true, RestoreDirectory = true
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
            await UpdateAsync(automatic: false, archive: dialog.FileName);
    }

    private async Task UpdateAsync(bool automatic, string? archive = null)
    {
        using var work = new CancellationTokenSource(TimeSpan.FromMinutes(15));
        pending = work;
        try
        {
            browse.Enabled = channel.Enabled = update.Enabled = rollback.Enabled = installPackage.Enabled = false;
            cancel.Visible = progress.Visible = true;
            progress.Value = 0;
            status.ForeColor = Muted;
            if (!automatic) SaveSettings();
            var install = Install;
            var selected = (ReleaseChannel)channel.SelectedItem!;
            var messages = new Progress<string>(message => status.Text = message);
            var download = new Progress<double>(fraction => progress.Value = (int)Math.Round(fraction * 100));
            var result = await Task.Run(() => archive is null
                ? install.UpdateAsync(selected, automatic, messages, download, work.Token)
                : install.InstallPackageAsync(archive, messages, download, work.Token));
            status.Text = result.Version is null ? $"No {selected.ToString().ToLowerInvariant()} Windows releases are available." :
                result.Held ? $"Poima {result.Version} is available. Check and install to switch back." :
                result.Downloaded ? $"Poima {result.Version} is installed." : $"Poima {result.Version} is already installed.";
        }
        catch (OperationCanceledException) { status.Text = "Installation stopped. Your active engine is unchanged."; }
        catch (Exception error) when (Expected(error)) { Report(error); }
        finally
        {
            pending = null;
            browse.Enabled = channel.Enabled = update.Enabled = installPackage.Enabled = true;
            progress.Visible = false;
            RunWithStatus(RefreshInstall);
        }
    }

    private void PickProject()
    {
        using var dialog = new OpenFileDialog
        {
            Title = "Open a Poima project", Filter = "Poima projects|*.poimaproject;project.json", CheckFileExists = true
        };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        var manifest = Path.GetFullPath(dialog.FileName);
        var remembered = pins.TryGetValue(manifest, out var pinned);
        var version = remembered ? pinned : Install.State().Active;
        Launch(version, manifest);
        if (!remembered && version is not null) { pins[manifest] = version; SaveSettings(); }
    }

    private void Launch(string? version, string? manifest = null)
    {
        var editor = Install.Executable(version) ??
            throw new InvalidOperationException($"Poima {version ?? "is not installed"}. Choose its installation folder.");
        var start = new ProcessStartInfo(editor) { WorkingDirectory = Path.GetDirectoryName(editor)! };
        if (manifest is not null)
        {
            start.ArgumentList.Add("--creator");
            start.ArgumentList.Add(manifest);
        }
        else if (projectsRoot is not null)
        {
            start.ArgumentList.Add("--projects");
            start.ArgumentList.Add(projectsRoot);
        }
        using var launched = Process.Start(start) ?? throw new InvalidOperationException("Poima could not be started.");
    }

    private Settings LoadSettings()
    {
        try
        {
            var info = new FileInfo(preferences);
            if (info.Exists && info.Length <= 256 * 1024 &&
                JsonSerializer.Deserialize<Settings>(File.ReadAllBytes(preferences)) is { Projects: not null } saved &&
                Path.IsPathFullyQualified(saved.Location) && Enum.IsDefined(saved.Channel)) return saved;
        }
        catch (Exception error) when (Expected(error)) { }
        return new(Path.Combine(userRoot, "Engines"), ReleaseChannel.Preview, new());
    }

    private void SaveSettings()
    {
        Directory.CreateDirectory(userRoot);
        var temporary = Path.Combine(userRoot, $"installer-{Guid.NewGuid():N}.tmp");
        try
        {
            File.WriteAllBytes(temporary, JsonSerializer.SerializeToUtf8Bytes(
                new Settings(location.Text, (ReleaseChannel)channel.SelectedItem!, pins)));
            File.Move(temporary, preferences, overwrite: true);
        }
        finally { File.Delete(temporary); }
    }

    private void RunWithStatus(Action action)
    {
        status.ForeColor = Muted;
        try { action(); }
        catch (Exception error) when (Expected(error)) { Report(error); }
    }

    private void Report(Exception error) { status.ForeColor = Accent; status.Text = error.Message; }

    private static bool Expected(Exception error) => error is IOException or InvalidDataException or HttpRequestException or JsonException
        or UnauthorizedAccessException or InvalidOperationException or ArgumentException or FormatException or TimeoutException
        or KeyNotFoundException or System.ComponentModel.Win32Exception;
}
