// SPDX-License-Identifier: Apache-2.0
using System.Net;
using Poima.Editor;

internal static class AgentInstallContract
{
    public static async Task VerifyAsync(WindowsAgentPackage package, string url, long archiveBytes)
    {
        var root = Path.Combine(Path.GetTempPath(), "poima-agent-install-" + Guid.NewGuid().ToString("N"));
        using var cancel = new CancellationTokenSource();
        try
        {
            using (var http = new HttpClient(new Handler(url, null)))
            {
                try
                {
                    await package.InstallAsync(http, root, null, CancellationToken.None);
                    throw new InvalidOperationException($"Accepted a truncated {package.Name} download.");
                }
                catch (InvalidDataException) { }
            }
            CheckClean(package, root);
            using (var http = new HttpClient(new Handler(url, archiveBytes)))
            {
                try
                {
                    await package.InstallAsync(http, root, new CancelProgress(cancel), cancel.Token);
                    throw new InvalidOperationException("Published an installation after cancellation.");
                }
                catch (OperationCanceledException) when (cancel.IsCancellationRequested) { }
            }
            CheckClean(package, root);
            Console.WriteLine($"Pinned {package.Name} download rejects an incomplete response and cleans up a cancelled transfer.");
        }
        finally { if (Directory.Exists(root)) Directory.Delete(root, recursive: true); }
    }

    private static void CheckClean(WindowsAgentPackage package, string root)
    {
        if (package.Executable(root) is not null || Directory.EnumerateDirectories(root).Any())
            throw new InvalidOperationException($"A failed {package.Name} download left an installation or staging folder.");
    }

    private sealed class CancelProgress(CancellationTokenSource source) : IProgress<int>
    {
        public void Report(int value) => source.Cancel();
    }

    private sealed class Handler(string url, long? length) : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            if (request.RequestUri?.AbsoluteUri != url)
                throw new InvalidOperationException("Expected the exact official release address.");
            var content = new ByteArrayContent(new byte[256 * 1024]);
            if (length is not null) content.Headers.ContentLength = length;
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = content });
        }
    }
}
