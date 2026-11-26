// SPDX-License-Identifier: Apache-2.0
using System.Text.Json;
using System.Text.Json.Nodes;
using Poima.Launcher;

internal static class LocalInstallTests
{
    public static async Task RunAsync(string firstArchive, string nextArchive, string root)
    {
        Directory.CreateDirectory(root);
        Require(!Directory.EnumerateFileSystemEntries(root).Any(), "Expected an empty test root.");
        var first = Version(firstArchive);
        var next = Version(nextArchive);
        Require(EngineVersion.Parse(next)!.CompareTo(EngineVersion.Parse(first)) > 0, "Use two increasing engine versions.");
        var fixtures = Directory.CreateDirectory(Path.Combine(root, "fixtures")).FullName;
        var installed = Path.Combine(root, "installed");
        using var deadline = new CancellationTokenSource(TimeSpan.FromMinutes(8));
        var cancel = deadline.Token;
        using var blockedNetwork = new OfflineOnly();
        using var http = new HttpClient(blockedNetwork);
        var install = new EngineInstall(installed, new(http));
        var result = await install.InstallPackageAsync(firstArchive, null, null, cancel);
        Require(result is { Downloaded: true } && result.Version == first && install.Executable(first) is not null,
            "First package did not activate.");
        Require(install.State() == new InstallState(first), "First installation state differs.");
        var unchanged = await install.InstallPackageAsync(firstArchive, null, null, cancel);
        Require(!unchanged.Downloaded && unchanged.Version == first, "The same package should reuse the installation.");

        var invalid = Path.Combine(fixtures, Path.GetFileName(nextArchive));
        await File.WriteAllTextAsync(invalid, "missing companion", cancel);
        await ExpectInvalidAsync(invalid, "matching .manifest.json");
        var originalManifest = Path.ChangeExtension(nextArchive, ".manifest.json");
        var copiedManifest = Path.ChangeExtension(invalid, ".manifest.json");
        File.Copy(originalManifest, copiedManifest);
        await ExpectInvalidAsync(invalid, "length differs");
        File.Copy(nextArchive, invalid, overwrite: true);
        using (var damaged = new FileStream(invalid, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
        {
            var firstByte = damaged.ReadByte();
            damaged.Position = 0;
            damaged.WriteByte((byte)(firstByte ^ 1));
        }
        await ExpectInvalidAsync(invalid, "SHA-256");
        File.Copy(nextArchive, invalid, overwrite: true);
        var written = JsonNode.Parse(await File.ReadAllTextAsync(copiedManifest, cancel))!;
        written["files"]![0]!["sha256"] = new string('0', 64);
        await File.WriteAllTextAsync(copiedManifest, written.ToJsonString(), cancel);
        await ExpectInvalidAsync(invalid, "SHA-256");

        using (var interrupted = CancellationTokenSource.CreateLinkedTokenSource(cancel))
        {
            try
            {
                await install.InstallPackageAsync(nextArchive, null,
                    new ImmediateProgress<double>(_ => interrupted.Cancel()), interrupted.Token);
                throw new InvalidOperationException("Cancelling the local copy completed installation.");
            }
            catch (OperationCanceledException) when (interrupted.IsCancellationRequested) { }
        }
        VerifyUnchanged();
        var update = await install.InstallPackageAsync(nextArchive, null, null, cancel);
        Require(update is { Downloaded: true } && update.Version == next && install.State() == new InstallState(next, first),
            "Local update did not preserve the original as the previous version.");
        Require(install.Rollback() == first && install.Executable(next) is not null, "Local rollback failed.");
        var reactivated = await install.InstallPackageAsync(nextArchive, null, null, cancel);
        Require(!reactivated.Downloaded && install.State() == new InstallState(next, first), "Local cached reactivation failed.");
        var previous = install.Installed(first)!;
        var pin = new ProjectEnginePin(first, previous.ArchiveSha256);
        Require(pin.ArchiveSha256 is not null, "The installed package has an archive identity.");
        Directory.Move(Path.GetDirectoryName(previous.Editor)!, Path.Combine(root, "removed-project-engine"));
        try
        {
            await install.InstallPackageAsync(nextArchive, null, null, cancel, pin);
            throw new InvalidOperationException("The wrong version was accepted for the pinned project.");
        }
        catch (InvalidDataException error) when (error.Message.Contains($"needs the Poima {first}", StringComparison.Ordinal)) { }
        Require(install.ProjectEngine(pin) is null && install.State() == new InstallState(next, first),
            "A different package changed the remembered engine or the active selection.");
        var restored = await install.InstallPackageAsync(firstArchive, null, null, cancel, pin);
        Require(restored is { Downloaded: true } && restored.Version == first &&
            install.ProjectEngine(pin) == previous && install.State() == new InstallState(next, first),
            "ZIP recovery did not restore the exact project engine or affected the active selection.");
        ExpectEmptyStaging();
        Require(blockedNetwork.Requests == 0, "Installing from files unexpectedly attempted HTTP.");
        Console.WriteLine(JsonSerializer.Serialize(new
        {
            passed = true, first, active = next, previous = first, http_requests = blockedNetwork.Requests,
            cases = new[] { "install", "reuse", "missing_manifest", "wrong_length", "corrupt_zip",
                "corrupt_member_inventory", "cancel", "update", "rollback", "reactivate",
                "wrong_project_version_rejected", "exact_project_zip_restored_without_activating" }
        }));

        async Task ExpectInvalidAsync(string archive, string message)
        {
            try
            {
                await install.InstallPackageAsync(archive, null, null, cancel);
                throw new InvalidOperationException("Invalid local package installed.");
            }
            catch (IOException error) when (error.Message.Contains(message, StringComparison.Ordinal)) { }
            catch (InvalidDataException error) when (error.Message.Contains(message, StringComparison.Ordinal)) { }
            VerifyUnchanged();
        }

        void VerifyUnchanged()
        {
            Require(install.State() == new InstallState(first) && install.Executable(first) is not null &&
                install.Executable(next) is null, "Rejected package changed the active installation.");
            ExpectEmptyStaging();
        }

        void ExpectEmptyStaging() => Require(!Directory.EnumerateFileSystemEntries(Path.Combine(installed, ".poima-staging")).Any(),
            "An installer operation left staging files.");
    }

    private static string Version(string archive)
    {
        using var document = JsonDocument.Parse(File.ReadAllBytes(Path.ChangeExtension(archive, ".manifest.json")));
        return document.RootElement.GetProperty("engine_version").GetString()!;
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private sealed class OfflineOnly : HttpMessageHandler
    {
        public int Requests { get; private set; }
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            ++Requests;
            throw new InvalidOperationException("Unexpected network request during local installation.");
        }
    }
}
