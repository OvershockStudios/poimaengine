// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Drawing.Drawing2D;
using System.Text.Json;

namespace Poima.Launcher;

internal sealed class LauncherForm : Form
{
    private static readonly Color Surface = Color.FromArgb(33, 16, 22);
    private static readonly Color Panel = Color.FromArgb(43, 26, 35);
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
    private readonly Button update = Button("Install Poima", primary: true);
    private readonly Button installPackage = Button("Install from ZIP…");
    private readonly Button browse = Button("Browse…");
    private readonly Button open = Button("Open Poima", primary: true);
    private readonly Button project = Button("Open project…");
    private readonly Button rollback = Button("Use previous version");
    private readonly Button cancel = Button("Cancel");
    private readonly Button showOptions = Button("Installation options");
    private readonly ToolTip tooltips = new();
    private readonly Bitmap settingsIcon = DrawIcon((g, pen) =>
    {
        g.DrawLine(pen, 3, 5, 17, 5);
        g.DrawLine(pen, 3, 10, 17, 10);
        g.DrawLine(pen, 3, 15, 17, 15);
        g.DrawLine(pen, 7, 3, 7, 7);
        g.DrawLine(pen, 13, 8, 13, 12);
        g.DrawLine(pen, 8, 13, 8, 17);
    });
    private readonly Bitmap folderIcon = DrawIcon((g, pen) => g.DrawLines(pen,
        (Point[])[new(2, 5), new(8, 5), new(10, 7), new(18, 7), new(18, 16), new(2, 16), new(2, 5)]));
    private readonly Bitmap updateIcon = DrawIcon((g, pen) =>
    {
        g.DrawArc(pen, 4, 4, 12, 12, 2, 308);
        g.DrawLines(pen, (Point[])[new(15, 3), new(15, 7), new(11, 7)]);
    });
    private readonly TableLayoutPanel options = new()
    {
        Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 3, AutoSize = true,
        AutoSizeMode = AutoSizeMode.GrowAndShrink, Visible = false
    };
    private readonly Dictionary<string, string> pins;
    private CancellationTokenSource? pending;
    private bool closeAfterInstall;

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
        ClientSize = new Size(620, 380);
        MinimumSize = new Size(560, 392);
        StartPosition = FormStartPosition.CenterScreen;
        var settings = LoadSettings();
        pins = new(settings.Projects, StringComparer.OrdinalIgnoreCase);

        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill, Padding = new Padding(26, 22, 26, 18), ColumnCount = 1, RowCount = 6
        };
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 70));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 44));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 46));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 16));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        Controls.Add(layout);

        using var wordmark = typeof(LauncherForm).Assembly.GetManifestResourceStream("Poima.Launcher.Poima.png")!;
        using var source = Image.FromStream(wordmark);
        var logo = new PictureBox { Width = 145, Height = 54, SizeMode = PictureBoxSizeMode.Zoom, Image = new Bitmap(source) };
        var header = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1 };
        header.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        header.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        header.Controls.Add(logo, 0, 0);
        SetIcon(showOptions, settingsIcon, "Installation options");
        showOptions.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        header.Controls.Add(showOptions, 1, 0);
        layout.Controls.Add(header, 0, 0);
        current.Dock = DockStyle.Fill;
        current.Font = new Font(Font.FontFamily, 15, FontStyle.Bold);
        layout.Controls.Add(current, 0, 1);

        var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false };
        SetIcon(project, folderIcon, "Open project…");
        actions.Controls.AddRange([open, update, project, cancel]);
        layout.Controls.Add(actions, 0, 2);
        status.Dock = DockStyle.Fill;
        status.ForeColor = Muted;
        layout.Controls.Add(status, 0, 3);
        progress.Dock = DockStyle.Fill;
        progress.Visible = false;
        layout.Controls.Add(progress, 0, 4);
        channel.DataSource = Enum.GetValues<ReleaseChannel>();
        channel.SelectedItem = settings.Channel;
        location.BackColor = Panel;
        location.ForeColor = TextColor;
        location.Text = settings.Location;
        location.Dock = DockStyle.Fill;
        SetIcon(browse, folderIcon, "Browse…");
        options.RowStyles.Add(new RowStyle(SizeType.Absolute, 38));
        options.RowStyles.Add(new RowStyle(SizeType.Absolute, 38));
        options.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
        options.Controls.Add(Row("Install location", location, browse), 0, 0);
        options.Controls.Add(Row("Release channel", channel), 0, 1);
        var local = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false };
        local.Controls.AddRange([installPackage, rollback]);
        options.Controls.Add(local, 0, 2);
        layout.Controls.Add(options, 0, 5);

        showOptions.Click += (_, _) => ShowInstallationOptions(!options.Visible);
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
            closeAfterInstall = true;
            pending.Cancel();
            args.Cancel = true;
        };
        Disposed += (_, _) =>
        {
            http.Dispose(); tooltips.Dispose(); logo.Image?.Dispose();
            settingsIcon.Dispose(); folderIcon.Dispose(); updateIcon.Dispose();
        };
        RunWithStatus(RefreshInstall);
        ActiveControl = open.Enabled ? open : update;
        Shown += async (_, _) => { if (open.Enabled && pending is null) await UpdateAsync(automatic: true); };
    }

    private static Button Button(string title, bool primary = false)
    {
        var button = new Button
        {
            Text = title, Height = 33, AutoSize = true, Padding = new Padding(9, 0, 9, 0),
            FlatStyle = FlatStyle.Flat, ForeColor = TextColor, UseVisualStyleBackColor = false
        };
        StyleAction(button, primary);
        return button;
    }

    private static void StyleAction(Button button, bool primary)
    {
        var background = primary ? Wine : Panel;
        button.BackColor = background;
        button.FlatAppearance.BorderColor = background;
        button.FlatAppearance.MouseOverBackColor = primary ? Color.FromArgb(133, 57, 67) : Color.FromArgb(54, 46, 54);
    }

    private void ShowInstallationOptions(bool visible)
    {
        options.Visible = visible;
        tooltips.SetToolTip(showOptions, visible ? "Hide installation options" : "Installation options");
    }

    private void SetIcon(Button button, Bitmap icon, string name)
    {
        button.Text = "";
        button.AutoSize = false;
        button.Size = new Size(38, 33);
        button.Image = icon;
        button.AccessibleName = name;
        button.BackColor = Surface;
        button.FlatAppearance.BorderSize = 0;
        button.FlatAppearance.MouseOverBackColor = Panel;
        tooltips.SetToolTip(button, name);
    }

    private static Bitmap DrawIcon(Action<Graphics, Pen> draw)
    {
        var icon = new Bitmap(20, 20);
        using var graphics = Graphics.FromImage(icon);
        using var pen = new Pen(TextColor, 1.6F) { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round };
        graphics.SmoothingMode = SmoothingMode.AntiAlias;
        draw(graphics, pen);
        return icon;
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
        current.Text = ready ? $"Poima {state.Active}" : "Set up Poima on this PC";
        if (ready) SetIcon(update, updateIcon, "Check for updates");
        else
        {
            update.Text = "Install Poima";
            update.AccessibleName = update.Text;
            update.Image = null;
            update.AutoSize = true;
            update.FlatAppearance.BorderSize = 1;
            tooltips.SetToolTip(update, "");
            StyleAction(update, primary: true);
        }
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
            showOptions.Enabled = false;
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
                result.Held ? $"Poima {result.Version} is available. Check for updates to switch back." :
                result.Downloaded ? $"Poima {result.Version} is installed." : $"Poima {result.Version} is already installed.";
            if (result.Downloaded) ShowInstallationOptions(false);
        }
        catch (OperationCanceledException) { status.Text = "Installation stopped. Your active engine is unchanged."; }
        catch (Exception error) when (Expected(error)) { Report(error); }
        finally
        {
            pending = null;
            browse.Enabled = channel.Enabled = update.Enabled = installPackage.Enabled = showOptions.Enabled = true;
            progress.Visible = false;
            RunWithStatus(RefreshInstall);
            if (closeAfterInstall) BeginInvoke(Close);
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
