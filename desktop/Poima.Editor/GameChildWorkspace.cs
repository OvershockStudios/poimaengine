// SPDX-License-Identifier: Apache-2.0
using System.Security.Cryptography;
using System.Text.Json;

namespace Poima.Editor;

internal sealed class GameChildWorkspace : IDisposable
{
    private const int WindowsPathLimit = 260;
    private const int ManifestLimit = 4 * 1024 * 1024;
    private readonly DirectoryInfo folder;

    public string Manifest => Path.Combine(folder.FullName, "game", "game.json");
    public string Suite => Path.Combine(folder.FullName, "suite.json");
    public string Report => Path.Combine(folder.FullName, "report.json");
    public string BundleHash { get; }

    private GameChildWorkspace(DirectoryInfo folder, string hash) => (this.folder, BundleHash) = (folder, hash);

    public static async Task<GameChildWorkspace?> PrepareAsync(string manifest, string? suite, string? report,
        string? expectedBundle, CancellationToken cancellation)
    {
        if (!OperatingSystem.IsWindows()) return null;
        manifest = Path.GetFullPath(manifest);
        var source = Path.GetDirectoryName(manifest)!;
        var bytes = await ReadManifestAsync(manifest, cancellation);
        using var document = JsonDocument.Parse(bytes, new JsonDocumentOptions { AllowDuplicateProperties = false });
        var files = document.RootElement.GetProperty("files");
        if (!TooLong(manifest) && !TooLong(suite) && !TooLong(report) && !LongPayload(source, files)) return null;

        var hash = Convert.ToHexStringLower(SHA256.HashData(bytes));
        if (expectedBundle is not null && hash != expectedBundle)
            throw new InvalidDataException("The selected game bundle changed before launch.");
        // Use the installed executable to validate both inventories before running the bundled executable.
        var validated = await ProjectFiles.InspectGameAsync(manifest, hash, cancellation);
        var stage = new GameChildWorkspace(Directory.CreateTempSubdirectory("PoimaGame-"), hash);
        try
        {
            var destination = Path.GetDirectoryName(stage.Manifest)!;
            var inventory = validated.GetProperty("files");
            if (TooLong(stage.Manifest) || TooLong(stage.Suite) || TooLong(stage.Report) || LongPayload(destination, inventory))
                throw new IOException("The temporary directory path is too long to start this game.");
            await Task.Run(() =>
            {
                Directory.CreateDirectory(destination);
                foreach (var entry in inventory.EnumerateArray())
                {
                    cancellation.ThrowIfCancellationRequested();
                    var name = entry.GetProperty("path").GetString()!;
                    var target = Path.Combine(destination, name);
                    Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                    File.Copy(Path.Combine(source, name), target);
                }
                File.WriteAllBytes(stage.Manifest, bytes);
                if (suite is not null) File.Copy(suite, stage.Suite);
            }, cancellation);
            await ProjectFiles.InspectGameAsync(stage.Manifest, hash, cancellation);
            var current = await ReadManifestAsync(manifest, cancellation);
            if (!current.AsSpan().SequenceEqual(bytes))
                throw new InvalidDataException("The game bundle changed while preparing it for launch.");
            return stage;
        }
        catch
        {
            stage.Dispose();
            throw;
        }
    }

    private static bool TooLong(string? path) => path is not null && Path.GetFullPath(path).Length >= WindowsPathLimit;

    private static bool LongPayload(string root, JsonElement files)
    {
        foreach (var item in files.EnumerateArray())
        {
            var name = item.GetProperty("path").GetString()!;
            if (root.Length + 1 + name.Length >= WindowsPathLimit &&
                !name.StartsWith("runtime/share/poima/licenses/", StringComparison.Ordinal)) return true;
        }
        return false;
    }

    private static async Task<byte[]> ReadManifestAsync(string path, CancellationToken cancellation)
    {
        await using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 8192, useAsync: true);
        if (stream.Length is < 1 or > ManifestLimit) throw new InvalidDataException("The game manifest has an invalid size.");
        var bytes = new byte[(int)stream.Length];
        await stream.ReadExactlyAsync(bytes, cancellation);
        if (stream.ReadByte() != -1) throw new IOException("The game manifest changed during the read.");
        return bytes;
    }

    public async Task PublishReportAsync(string destination, CancellationToken cancellation)
    {
        var pending = Path.Combine(Path.GetDirectoryName(destination)!, ".checks-" + Guid.NewGuid().ToString("N"));
        try
        {
            await using (var input = new FileStream(Report, FileMode.Open, FileAccess.Read, FileShare.Read, 8192, useAsync: true))
            await using (var output = new FileStream(pending, FileMode.CreateNew, FileAccess.Write, FileShare.None, 8192, useAsync: true))
            {
                if (input.Length is < 1 or > ManifestLimit) throw new InvalidDataException("The game report has an invalid size.");
                await input.CopyToAsync(output, cancellation);
                output.Flush(flushToDisk: true);
            }
            cancellation.ThrowIfCancellationRequested();
            File.Move(pending, destination, overwrite: false);
        }
        finally { if (File.Exists(pending)) File.Delete(pending); }
    }

    public void Dispose()
    {
        try { folder.Delete(recursive: true); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { Console.Error.WriteLine("Could not remove temporary game files. " + error.Message); }
    }
}
