// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Text.Json;

namespace Poima.Editor;

internal sealed record ProjectLocation(string World, string Root, string Endpoint);

internal static class ProjectFiles
{
    public const string Extension = ".poimaproject";
    public const string DefaultManifestName = "project" + Extension;
    public const string LegacyManifestName = "project.json";

    public static bool IsManifest(string path) =>
        string.Equals(Path.GetExtension(path), Extension, StringComparison.OrdinalIgnoreCase) ||
        string.Equals(Path.GetFileName(path), LegacyManifestName, StringComparison.OrdinalIgnoreCase);

    public static async Task<ProjectLocation> Open(string manifest)
    {
        var project = await Command("inspect", Path.GetFullPath(manifest));
        var root = Path.GetDirectoryName(project.GetProperty("manifest").GetString())!;
        var world = Path.GetFullPath(project.GetProperty("entry").GetProperty("world").GetString()!, root);
        return new(world, root, "poima-" + project.GetProperty("project_id").GetString());
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
