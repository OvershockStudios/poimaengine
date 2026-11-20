// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Text.Json;

namespace Poima.Editor;

internal sealed record ProjectLocation(string Manifest, string Name, string World, string Root, string Endpoint,
    long Revision, string Camera, string? InputProfile, bool Audio, JsonElement? Gameplay);

internal static class ProjectFiles
{
    public const string Extension = ".poimaproject";
    public const string DefaultManifestName = "project" + Extension;
    public const string LegacyManifestName = "project.json";
    public static string PreviewSample => Path.Combine(AppContext.BaseDirectory, "samples", "Collection Room", "game.json");
    public static string PreviewRuntime => Path.Combine(Path.GetDirectoryName(PreviewSample)!, "runtime");

    public static bool IsManifest(string path) =>
        string.Equals(Path.GetExtension(path), Extension, StringComparison.OrdinalIgnoreCase) ||
        string.Equals(Path.GetFileName(path), LegacyManifestName, StringComparison.OrdinalIgnoreCase);

    public static async Task<ProjectLocation> Open(string manifest)
    {
        var project = await Command("inspect", Path.GetFullPath(manifest));
        var path = project.GetProperty("manifest").GetString()!;
        var root = Path.GetDirectoryName(path)!;
        var entry = project.GetProperty("entry");
        var world = Path.GetFullPath(entry.GetProperty("world").GetString()!, root);
        return new(path, project.GetProperty("name").GetString()!, world, root,
            "poima-" + project.GetProperty("project_id").GetString(), project.GetProperty("revision").GetInt64(),
            entry.GetProperty("camera").GetString()!, project.GetProperty("input_profile").GetString(),
            project.GetProperty("audio").GetBoolean(), project.TryGetProperty("gameplay", out var gameplay) ? gameplay : null);
    }

    public static Process Start(string executable, params string[] arguments)
    {
        var info = new ProcessStartInfo(executable)
        {
            WorkingDirectory = AppContext.BaseDirectory, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true
        };
        foreach (var argument in arguments) info.ArgumentList.Add(argument);
        return Process.Start(info) ?? throw new InvalidOperationException("Poima could not start.");
    }

    public static async Task<JsonElement> Command(params string[] arguments)
    {
        using var process = Start(Path.Combine(AppContext.BaseDirectory, "poima.exe"), ["project", .. arguments]);
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
}
