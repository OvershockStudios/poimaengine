// SPDX-License-Identifier: Apache-2.0
using System.Net;
using Poima.Editor;

internal static class CodexInstallContract
{
    public static async Task VerifyAsync()
    {
        var root = Path.Combine(Path.GetTempPath(), "poima-codex-install-" + Guid.NewGuid().ToString("N"));
        using var cancel = new CancellationTokenSource();
        try
        {
            using (var http = new HttpClient(new Handler(declarePinnedLength: false)))
            {
                try
                {
                    await WindowsCodexInstall.InstallAsync(http, root, null, CancellationToken.None);
                    throw new InvalidOperationException("Accepted a truncated Codex download.");
                }
                catch (InvalidDataException) { }
            }
            CheckClean(root);
            using (var http = new HttpClient(new Handler(declarePinnedLength: true)))
            {
                try
                {
                    await WindowsCodexInstall.InstallAsync(http, root, new CancelProgress(cancel), cancel.Token);
                    throw new InvalidOperationException("Published an installation after cancellation.");
                }
                catch (OperationCanceledException) when (cancel.IsCancellationRequested) { }
            }
            CheckClean(root);
            Console.WriteLine("Pinned Codex download rejects an incomplete response and cleans up a cancelled transfer.");
        }
        finally { if (Directory.Exists(root)) Directory.Delete(root, recursive: true); }
    }

    private static void CheckClean(string root)
    {
        if (WindowsCodexInstall.Executable(root) is not null || Directory.EnumerateDirectories(root).Any())
            throw new InvalidOperationException("A failed Codex download left an installation or staging folder.");
    }

    private sealed class CancelProgress(CancellationTokenSource source) : IProgress<int>
    {
        public void Report(int value) => source.Cancel();
    }

    private sealed class Handler(bool declarePinnedLength) : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            if (request.RequestUri?.AbsoluteUri != WindowsCodexInstall.PackageUrl)
                throw new InvalidOperationException("Expected the exact official Codex release address.");
            var content = new ByteArrayContent(new byte[256 * 1024]);
            if (declarePinnedLength) content.Headers.ContentLength = WindowsCodexInstall.ArchiveBytes;
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = content });
        }
    }
}
