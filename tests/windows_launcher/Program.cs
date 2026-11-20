// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Net;
using System.Security.Cryptography;
using System.Text.Json;
using Poima.Launcher;

if (args is ["--local", var firstArchive, var nextArchive, var root])
{
    await LocalInstallTests.RunAsync(firstArchive, nextArchive, root);
    return;
}

if (args.Length != 2) throw new ArgumentException("Expected a release asset folder and a new installation root.");
Directory.CreateDirectory(args[1]);
Require(!Directory.EnumerateFileSystemEntries(args[1]).Any(), "The test installation root must be empty.");
using var deadline = new CancellationTokenSource(TimeSpan.FromMinutes(8));
var handler = new FixtureFeed(args[0]);
using var fixture = new HttpClient(handler);
var feed = new ReleaseFeed(fixture);
var install = new EngineInstall(args[1], feed);
var cancel = deadline.Token;
var results = new List<string>();

var first = await install.UpdateAsync(ReleaseChannel.Preview, false, null, null, cancel);
Require(first is { Version: "0.0.296", Downloaded: true } && install.State().Active == first.Version, "First preview installation");
Require(await NativeVersion(first.Version) == "0.0.296", "First installed engine runs");
results.Add("first_preview_install_and_native_engine");

var unchanged = await install.UpdateAsync(ReleaseChannel.Preview, true, null, null, cancel);
Require(unchanged is { Downloaded: false, Version: "0.0.296" } && handler.ArchiveRequests == 1 &&
    handler.InventoryRequests == 1, "Unchanged tag made no asset requests");
results.Add("no_download_for_same_version");

var missing = await install.UpdateAsync(ReleaseChannel.Stable, false, null, null, cancel);
Require(missing.Version is null && install.State().Active == first.Version, "Unavailable stable channel preserved preview");
results.Add("empty_stable_channel_preserves_active");

handler.ShowNext = true;
handler.StableMetadata = true;
Require((await feed.LatestAsync(ReleaseChannel.Stable, cancel))?.Version == "1.0.0", "Stable selects its own release");
Require((await feed.LatestAsync(ReleaseChannel.Preview, cancel))?.Version == "0.0.297", "Preview traverses release pages");
handler.StableMetadata = false;
results.Add("stable_and_preview_selection_with_pagination");

using (var interrupted = CancellationTokenSource.CreateLinkedTokenSource(cancel))
{
    try
    {
        await install.UpdateAsync(ReleaseChannel.Preview, false, null,
            new ImmediateProgress<double>(_ => interrupted.Cancel()), interrupted.Token);
        throw new InvalidOperationException("Cancel during archive transfer unexpectedly completed.");
    }
    catch (OperationCanceledException) when (interrupted.IsCancellationRequested) { }
}
Require(install.State().Active == first.Version && install.Executable("0.0.297") is null &&
    !Directory.EnumerateFileSystemEntries(Path.Combine(args[1], ".poima-staging")).Any(), "Leaving no partial activation or staging");
results.Add("interrupted_download_keeps_active_and_cleans_staging");

handler.BadArchive = true;
try
{
    await install.UpdateAsync(ReleaseChannel.Preview, false, null, null, cancel);
    throw new InvalidOperationException("A corrupt archive was accepted.");
}
catch (InvalidDataException) { }
Require(install.State().Active == first.Version && install.Executable("0.0.297") is null, "Corrupt download never becomes active");
handler.BadArchive = false;
results.Add("corrupt_archive_rejected");

handler.BadFileInventory = true;
try
{
    await install.UpdateAsync(ReleaseChannel.Preview, false, null, null, cancel);
    throw new InvalidOperationException("A valid archive with a bad inventory was accepted.");
}
catch (InvalidDataException) { }
Require(install.State().Active == first.Version && install.Executable("0.0.297") is null &&
    !Directory.EnumerateFileSystemEntries(Path.Combine(args[1], ".poima-staging")).Any(), "Failed extracted-file check preserves first engine");
handler.BadFileInventory = false;
results.Add("individual_file_digest_rejected_after_archive_passed");

var updated = await install.UpdateAsync(ReleaseChannel.Preview, true, null, null, cancel);
Require(updated is { Version: "0.0.297", Downloaded: true } &&
    install.State() is { Active: "0.0.297", Previous: "0.0.296" }, "Changed tag activates after verification");
Require(await NativeVersion(updated.Version) == "0.0.297" && await NativeVersion(first.Version) == "0.0.296",
    "Both installed native engines run");
results.Add("update_preserves_working_prior_engine");

Require(install.Rollback() == first.Version && install.State().Active == "0.0.296", "Rollback restores previous engine");
var held = await install.UpdateAsync(ReleaseChannel.Preview, true, null, null, cancel);
Require(held.Held && install.State().Active == first.Version, "Automatic update respects rollback");
var downloads = handler.ArchiveRequests;
var retry = await install.UpdateAsync(ReleaseChannel.Preview, false, null, null, cancel);
Require(retry is { Version: "0.0.297", Downloaded: false } && handler.ArchiveRequests == downloads &&
    install.State().Active == "0.0.297", "Explicit reactivation uses verified cached install");
results.Add("rollback_persists_until_explicit_reactivation");

foreach (var path in new[] { "../poima.exe", "/poima.exe", "C:/poima.exe", "runtime\\poima.exe", "NUL.txt",
    "foo./poima.exe", "foo//poima.exe", "foo/open:stream", "com¹.txt", ".poima-install.json" })
    Require(!PackageInventory.SafePath(path), $"Windows path validation rejected {path}");
Require(PackageInventory.SafePath("samples/Collection Room/runtime/bin/poima.exe"), "Real path allowed");
results.Add("windows_archive_path_bounds");

Console.WriteLine(JsonSerializer.Serialize(new
{
    passed = true, tests = results, archive_requests = handler.ArchiveRequests,
    inventory_requests = handler.InventoryRequests, file_counts = handler.FileCounts,
    active = install.State().Active, previous = install.State().Previous,
    scope = "Windows installation and native process startup with a controlled GitHub HTTP fixture and original archives"
}));

async Task<string> NativeVersion(string? version)
{
    var editor = install.Executable(version) ?? throw new InvalidOperationException("Expected an installed executable.");
    using var process = Process.Start(new ProcessStartInfo(Path.Combine(Path.GetDirectoryName(editor)!, "poima.exe"), "version")
    {
        UseShellExecute = false, RedirectStandardOutput = true, CreateNoWindow = true
    })!;
    var text = await process.StandardOutput.ReadToEndAsync(cancel);
    await process.WaitForExitAsync(cancel);
    Require(process.ExitCode == 0, "Native CLI exits successfully");
    using var reply = JsonDocument.Parse(text);
    Require(reply.RootElement.GetProperty("status").GetString() == "ok", "Native CLI returns a successful reply");
    return reply.RootElement.GetProperty("result").GetProperty("version").GetString()!;
}

static void Require(bool condition, string detail)
{
    if (!condition) throw new InvalidOperationException(detail);
}

sealed class ImmediateProgress<T>(Action<T> report) : IProgress<T>
{
    public void Report(T value) => report(value);
}

sealed class FixtureFeed : HttpMessageHandler
{
    private const string Repository = "OvershockStudios/poimaengine";
    private readonly string assets;
    private readonly Dictionary<string, (long Bytes, string Sha)> archives = new();
    private readonly Dictionary<string, byte[]> inventories = new();

    public bool ShowNext { get; set; }
    public bool StableMetadata { get; set; }
    public bool BadArchive { get; set; }
    public bool BadFileInventory { get; set; }
    public int ArchiveRequests { get; private set; }
    public int InventoryRequests { get; private set; }
    public Dictionary<string, int> FileCounts { get; } = new();

    public FixtureFeed(string assets)
    {
        this.assets = assets;
        foreach (var version in new[] { "0.0.296", "0.0.297" })
        {
            var prefix = ReleaseFeed.Folder(version);
            var bytes = File.ReadAllBytes(Path.Combine(assets, prefix + ".manifest.json"));
            using var document = JsonDocument.Parse(bytes);
            var archive = document.RootElement.GetProperty("archive");
            archives[version] = (archive.GetProperty("bytes").GetInt64(), archive.GetProperty("sha256").GetString()!);
            inventories[version] = bytes;
            FileCounts[version] = document.RootElement.GetProperty("files").GetArrayLength();
        }
    }

    protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancel)
    {
        cancel.ThrowIfCancellationRequested();
        var uri = request.RequestUri!;
        HttpResponseMessage response;
        if (uri.Host == "api.github.com")
        {
            var page = uri.Query.EndsWith("page=1", StringComparison.Ordinal);
            var releases = new List<object>();
            if (page)
            {
                releases.Add(Release("0.0.296", "0.0.296", true));
                if (StableMetadata) releases.Add(Release("1.0.0", "0.0.296", false));
            }
            else if (ShowNext) releases.Add(Release("0.0.297", "0.0.297", true));
            response = new(HttpStatusCode.OK) { Content = new ByteArrayContent(JsonSerializer.SerializeToUtf8Bytes(releases)) };
            if (page && ShowNext)
                response.Headers.TryAddWithoutValidation("Link", $"<https://api.github.com/repos/{Repository}/releases?per_page=100&page=2>; rel=\"next\"");
        }
        else
        {
            if (uri.Host != "github.com") throw new InvalidOperationException("Unexpected release host.");
            var tag = uri.Segments[^2].Trim('/');
            var version = tag[1..];
            if (uri.AbsolutePath.EndsWith(".manifest.json", StringComparison.Ordinal))
            {
                ++InventoryRequests;
                response = new(HttpStatusCode.OK) { Content = new ByteArrayContent(Inventory(version)) };
            }
            else
            {
                ++ArchiveRequests;
                Stream content = BadArchive ? new MemoryStream([1, 2, 3]) :
                    File.OpenRead(Path.Combine(assets, ReleaseFeed.Folder(version) + ".zip"));
                response = new(HttpStatusCode.OK) { Content = new StreamContent(content) };
            }
        }
        response.RequestMessage = request;
        return Task.FromResult(response);
    }

    private byte[] Inventory(string version)
    {
        if (!BadFileInventory || version != "0.0.297") return inventories[version];
        var node = System.Text.Json.Nodes.JsonNode.Parse(inventories[version])!;
        node["files"]![0]!["sha256"] = new string('0', 64);
        return JsonSerializer.SerializeToUtf8Bytes(node);
    }

    private object Release(string version, string source, bool preview)
    {
        var prefix = ReleaseFeed.Folder(version);
        var inventory = Inventory(source);
        return new
        {
            tag_name = "v" + version, draft = false, prerelease = preview,
            assets = new[]
            {
                Asset(prefix + ".zip", archives[source].Bytes, archives[source].Sha),
                Asset(prefix + ".manifest.json", inventory.LongLength, ReleaseFeed.Hex(SHA256.HashData(inventory)))
            }
        };

        object Asset(string name, long size, string sha) => new
        {
            name, state = "uploaded", size, digest = "sha256:" + sha,
            browser_download_url = $"https://github.com/{Repository}/releases/download/v{version}/{name}"
        };
    }
}
