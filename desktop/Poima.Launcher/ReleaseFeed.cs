// SPDX-License-Identifier: Apache-2.0
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Poima.Launcher;

internal enum ReleaseChannel { Preview, Stable }

internal sealed record EngineVersion(int Major, int Minor, int Patch, int Stage, int Iteration)
    : IComparable<EngineVersion>
{
    public static EngineVersion? Parse(string text)
    {
        var match = Regex.Match(text, @"\A(0|[1-9][0-9]{0,7})\.(0|[1-9][0-9]{0,7})\.(0|[1-9][0-9]{0,7})(?:-(alpha|beta|rc)\.([1-9][0-9]{0,7}))?\z",
            RegexOptions.CultureInvariant);
        if (!match.Success) return null;
        var stage = match.Groups[4].Value switch { "alpha" => 0, "beta" => 1, "rc" => 2, _ => 3 };
        return new(int.Parse(match.Groups[1].Value), int.Parse(match.Groups[2].Value), int.Parse(match.Groups[3].Value),
            stage, stage == 3 ? 0 : int.Parse(match.Groups[5].Value));
    }

    public bool Stable => Major != 0 && Stage == 3;

    public int CompareTo(EngineVersion? other) => other is null ? 1 : (Major, Minor, Patch, Stage, Iteration)
        .CompareTo((other.Major, other.Minor, other.Patch, other.Stage, other.Iteration));
}

internal sealed record ReleaseAsset(Uri Url, long Bytes, string Sha256);
internal sealed record EngineRelease(string Version, EngineVersion Order, ReleaseAsset Archive, ReleaseAsset Inventory);

internal sealed class ReleaseFeed(HttpClient client)
{
    private const string Repo = "OvershockStudios/poimaengine";
    internal const int MaxInventoryBytes = 4 * 1024 * 1024;
    internal const long MaxArchiveBytes = 2L * 1024 * 1024 * 1024;

    public static string Folder(string version) => $"Poima-{version}-Windows-x64";

    public static HttpClient NewClient()
    {
        var http = new HttpClient(new SocketsHttpHandler { ConnectTimeout = TimeSpan.FromSeconds(15) })
            { Timeout = Timeout.InfiniteTimeSpan };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("Poima-Launcher/1.0");
        return http;
    }

    public async Task<EngineRelease?> LatestAsync(ReleaseChannel channel, CancellationToken cancel)
    {
        EngineRelease? latest = null;
        for (var page = 1; page <= 20; ++page)
        {
            using var request = new HttpRequestMessage(HttpMethod.Get,
                $"https://api.github.com/repos/{Repo}/releases?per_page=100&page={page}");
            request.Headers.Accept.ParseAdd("application/vnd.github+json");
            request.Headers.Add("X-GitHub-Api-Version", "2022-11-28");
            using var response = await client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancel);
            response.EnsureSuccessStatusCode();
            var data = await ReadAsync(response, 8 * 1024 * 1024, cancel);
            using var document = JsonDocument.Parse(data);
            foreach (var release in document.RootElement.EnumerateArray())
            {
                if (release.GetProperty("draft").GetBoolean()) continue;
                var tag = release.GetProperty("tag_name").GetString() ?? "";
                if (!tag.StartsWith('v')) continue;
                var version = tag[1..];
                var order = EngineVersion.Parse(version);
                if (order is null || order.Stable != (channel == ReleaseChannel.Stable) ||
                    release.GetProperty("prerelease").GetBoolean() == order.Stable) continue;
                var prefix = Folder(version);
                var assets = release.GetProperty("assets");
                var zip = Asset(assets, tag, prefix + ".zip", MaxArchiveBytes);
                var list = Asset(assets, tag, prefix + ".manifest.json", MaxInventoryBytes);
                if (zip is not null && list is not null && (latest is null || order.CompareTo(latest.Order) > 0))
                    latest = new(version, order, zip, list);
            }
            if (!response.Headers.TryGetValues("Link", out var links) ||
                !links.Any(link => link.Contains("rel=\"next\"", StringComparison.Ordinal))) return latest;
        }
        throw new InvalidDataException("The release list exceeded the launcher's paging limit.");
    }

    private static ReleaseAsset? Asset(JsonElement assets, string tag, string name, long limit)
    {
        foreach (var asset in assets.EnumerateArray())
        {
            if (asset.GetProperty("name").GetString() != name || asset.GetProperty("state").GetString() != "uploaded")
                continue;
            var bytes = asset.GetProperty("size").GetInt64();
            var digest = asset.TryGetProperty("digest", out var value) ? value.GetString() : null;
            var expected = new Uri($"https://github.com/{Repo}/releases/download/{tag}/{name}");
            if (bytes > 0 && bytes <= limit && asset.GetProperty("browser_download_url").GetString() == expected.AbsoluteUri &&
                digest is not null && digest.StartsWith("sha256:", StringComparison.Ordinal) && IsSha256(digest[7..]))
                return new(expected, bytes, digest[7..]);
        }
        return null;
    }

    public async Task<byte[]> InventoryAsync(ReleaseAsset asset, CancellationToken cancel)
    {
        using var response = await client.GetAsync(asset.Url, HttpCompletionOption.ResponseHeadersRead, cancel);
        response.EnsureSuccessStatusCode();
        var bytes = await ReadAsync(response, (int)asset.Bytes, cancel);
        if (bytes.LongLength != asset.Bytes || Hex(SHA256.HashData(bytes)) != asset.Sha256)
            throw new InvalidDataException("The release inventory does not match GitHub's published checksum.");
        return bytes;
    }

    public async Task DownloadAsync(ReleaseAsset asset, string destination, IProgress<double>? progress, CancellationToken cancel)
    {
        using var response = await client.GetAsync(asset.Url, HttpCompletionOption.ResponseHeadersRead, cancel);
        response.EnsureSuccessStatusCode();
        await using var input = await response.Content.ReadAsStreamAsync(cancel);
        await using var output = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None,
            128 * 1024, FileOptions.Asynchronous);
        await CopyVerifiedAsync(input, output, asset.Bytes, asset.Sha256, progress, cancel);
    }

    private static async Task<byte[]> ReadAsync(HttpResponseMessage response, int limit, CancellationToken cancel)
    {
        await using var input = await response.Content.ReadAsStreamAsync(cancel);
        using var output = new MemoryStream();
        var buffer = new byte[32 * 1024];
        int read;
        while ((read = await input.ReadAsync(buffer, cancel)) != 0)
        {
            if (output.Length + read > limit) throw new InvalidDataException("The release response exceeded its size limit.");
            output.Write(buffer.AsSpan(0, read));
        }
        return output.ToArray();
    }

    internal static async Task CopyVerifiedAsync(Stream input, Stream output, long bytes, string sha256,
        IProgress<double>? progress, CancellationToken cancel)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buffer = new byte[128 * 1024];
        long copied = 0;
        var lastPercent = -1;
        int read;
        while ((read = await input.ReadAsync(buffer, cancel)) != 0)
        {
            cancel.ThrowIfCancellationRequested();
            copied += read;
            if (copied > bytes) throw new InvalidDataException("A file exceeded its published length.");
            hash.AppendData(buffer.AsSpan(0, read));
            await output.WriteAsync(buffer.AsMemory(0, read), cancel);
            if (progress is null) continue;
            var percent = (int)(100 * copied / bytes);
            if (lastPercent != percent)
            {
                progress.Report(percent / 100.0);
                lastPercent = percent;
            }
        }
        if (copied != bytes || Hex(hash.GetHashAndReset()) != sha256)
            throw new InvalidDataException("A file did not match its published length and SHA-256.");
    }

    internal static bool IsSha256(string text) => text.Length == 64 && text.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f');
    internal static string Hex(byte[] bytes) => Convert.ToHexStringLower(bytes);
}
