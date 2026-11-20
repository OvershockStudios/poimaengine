// SPDX-License-Identifier: Apache-2.0
using System.Security.Cryptography;
using System.Text;

namespace Poima.Editor;

internal sealed class CreatorConversationStore
{
    private const int MaximumIdLength = 128;
    public string FilePath { get; }

    public CreatorConversationStore(string manifest, string projectId, string? directory = null)
    {
        var path = Path.GetFullPath(manifest);
        if (OperatingSystem.IsWindows()) path = path.ToUpperInvariant();
        var key = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(projectId + "\n" + path)));
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        var folder = directory ?? Path.Combine(local, "Poima", "Creator", "Conversation-v1");
        FilePath = Path.Combine(folder, key + ".codex.txt");
    }

    public string? Read()
    {
        if (!File.Exists(FilePath)) return null;
        if (File.GetAttributes(FilePath).HasFlag(FileAttributes.ReparsePoint))
            throw new IOException("The saved Codex chat path is a link.");
        using var input = File.OpenRead(FilePath);
        if (input.Length is < 1 or > MaximumIdLength) throw new InvalidDataException("The saved Codex chat ID is invalid.");
        var bytes = new byte[(int)input.Length];
        input.ReadExactly(bytes);
        var id = Encoding.ASCII.GetString(bytes);
        if (!ValidId(id)) throw new InvalidDataException("The saved Codex chat ID is invalid.");
        return id;
    }

    public void Write(string id)
    {
        if (!ValidId(id)) throw new InvalidDataException("Codex returned an unsupported chat ID.");
        var folder = Path.GetDirectoryName(FilePath)!;
        Directory.CreateDirectory(folder);
        var temporary = Path.Combine(folder, ".codex-chat-" + Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            using (var output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                output.Write(Encoding.ASCII.GetBytes(id));
                output.Flush(flushToDisk: true);
            }
            File.Move(temporary, FilePath, overwrite: true);
        }
        finally { File.Delete(temporary); }
    }

    private static bool ValidId(string id) => id.Length is >= 1 and <= MaximumIdLength &&
        id.All(c => char.IsAsciiLetterOrDigit(c) || c is '-' or '_');
}
