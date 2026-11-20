// SPDX-License-Identifier: Apache-2.0
using System.Formats.Tar;
using System.IO.Compression;

namespace Poima.Editor;

internal static class WindowsCodexInstall
{
    // Keep this release, its publisher digest and its layout bounds together.
    public const string Version = "0.171.0";
    internal const long ArchiveBytes = 214_982_795;
    private const long PayloadBytes = 625_178_849;
    private const int PayloadFiles = 46;
    internal const string ArchiveSha256 = "669941eff23dd8470fbf85d919a03d80c4e3d35b441e3fcd361c0f8630945be8";
    internal const string PackageUrl = $"https://releases.openai.com/codex/releases/{Version}/codex-package-x86_64-pc-windows-msvc.tar.gz";
    private const string Marker = ".poima-codex-installed";

    public static WindowsAgentPackage Package { get; } = new("Codex", "codex", Version, PackageUrl,
        ArchiveBytes, ArchiveSha256, Marker, "bin/codex.exe",
        new Dictionary<string, long> { ["bin/codex.exe"] = 407_421_232 }, UnpackAsync);

    private static async Task UnpackAsync(Stream archive, string payload, CancellationToken cancellation)
    {
        using var compressed = new GZipStream(archive, CompressionMode.Decompress, leaveOpen: true);
        using var tar = new TarReader(compressed, leaveOpen: true);
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        long total = 0;
        while (await tar.GetNextEntryAsync(copyData: false, cancellation).ConfigureAwait(false) is { } entry)
        {
            if (entry.EntryType == TarEntryType.Directory) continue;
            if (entry.EntryType is not (TarEntryType.RegularFile or TarEntryType.V7RegularFile) ||
                entry.DataStream is null || !SafeName(entry.Name) || !names.Add(entry.Name) || names.Count > PayloadFiles ||
                entry.Length < 0 || entry.Length > PayloadBytes - total)
                throw new InvalidDataException("The Codex package contains an unexpected entry.");
            total += entry.Length;
            var file = Path.Combine(payload, entry.Name.Replace('/', Path.DirectorySeparatorChar));
            Directory.CreateDirectory(Path.GetDirectoryName(file)!);
            await using var output = new FileStream(file, FileMode.CreateNew, FileAccess.Write, FileShare.None,
                128 * 1024, FileOptions.Asynchronous);
            await entry.DataStream.CopyToAsync(output, cancellation).ConfigureAwait(false);
            if (output.Length != entry.Length) throw new InvalidDataException("A packaged Codex file is incomplete.");
        }
        if (names.Count != PayloadFiles || total != PayloadBytes ||
            !new[] { "codex-package.json", "bin/codex.exe", "bin/codex-exec-server.exe", "codex-path/rg.exe",
                "codex-resources/codex-windows-sandbox-setup.exe" }.All(names.Contains))
            throw new InvalidDataException("The Codex package is incomplete.");
    }

    private static bool SafeName(string name)
    {
        if (name == "codex-package.json") return true;
        var parts = name.Split('/');
        return name.Length <= 200 && parts.Length >= 2 && (parts[0] is "bin" or "codex-path" or "codex-resources") &&
            parts.All(part => part.Length > 0 && char.IsAsciiLetterOrDigit(part[0]) && part[^1] != '.' &&
                part.All(c => char.IsAsciiLetterOrDigit(c) || c is '_' or '-' or '.'));
    }
}
