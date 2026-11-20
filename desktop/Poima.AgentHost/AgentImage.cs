// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;

namespace Poima.AgentHost;

public sealed class AgentImage
{
    public const int MaximumCount = 3;
    public const int MaximumBytes = 2 * 1024 * 1024;

    private readonly string mimeType, data;

    public AgentImage(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length > MaximumBytes) throw new ArgumentException("An image exceeds 2 MiB.", nameof(bytes));
        mimeType = bytes.StartsWith(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 }) ? "image/png"
            : bytes.StartsWith(new byte[] { 255, 216, 255 }) ? "image/jpeg"
            : throw new ArgumentException("Choose a PNG or JPEG image.", nameof(bytes));
        data = Convert.ToBase64String(bytes);
    }

    internal JsonObject CodexInput() => new() { ["type"] = "image", ["url"] = $"data:{mimeType};base64,{data}" };
    internal JsonObject AcpInput() => new() { ["type"] = "image", ["mimeType"] = mimeType, ["data"] = data };

    internal static void CheckCount(IReadOnlyList<AgentImage> images)
    {
        if (images.Count > MaximumCount) throw new ArgumentException("Attach up to three images per message.", nameof(images));
    }
}
