// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;
using Poima.AgentHost;

namespace Poima.Editor;

internal static class WindowsGameplayBuildCheck
{
    private const int BuildTimeoutMilliseconds = 240_000;

    public static async Task RunAsync(WindowsBuildToolchain tools, CancellationToken cancellation)
    {
        var scratch = Directory.CreateTempSubdirectory("poima-build-check-");
        try
        {
            var created = await ProjectFiles.Command(cancellation, "create", Path.Combine(scratch.FullName, "Starter"),
                "--name", "Build Check", "--gameplay").ConfigureAwait(false);
            var project = await ProjectFiles.Open(created.GetProperty("manifest").GetString()!).ConfigureAwait(false);
            var options = await WindowsGameplayTools.ConfigureAsync(LaunchOptions.Parse([project.Manifest], project), project, tools)
                .ConfigureAwait(false);
            var profile = options.ActiveDevelopmentProfiles ?? throw new InvalidOperationException(options.GameplayToolStatus);
            cancellation.ThrowIfCancellationRequested();
            using var deadline = new CancellationTokenSource(TimeSpan.FromMilliseconds(BuildTimeoutMilliseconds + 30_000));
            await using var owner = new AgentRpcProcess(Path.Combine(AppContext.BaseDirectory, "poima.exe"),
                ["world", project.World, "--development-profiles", profile], scratch.FullName, requireJsonRpcVersion: true);

            async Task<JsonNode> Call(string method, JsonObject parameters)
            {
                try
                {
                    return await owner.RequestAsync(method, parameters, deadline.Token).ConfigureAwait(false)
                        ?? throw new InvalidDataException("Poima returned an empty build check result.");
                }
                catch (Exception error) when (deadline.IsCancellationRequested &&
                    error is OperationCanceledException or AgentExchangeException)
                { throw new TimeoutException("The starter build check timed out.", error); }
            }

            var accepted = await Call("development.publish", new()
            {
                ["request_id"] = EditorModel.NewId(), ["profile"] = WindowsGameplayTools.ProfileName,
                ["project"] = "gameplay-src/Game.csproj", ["type"] = "ProjectGameplay.Game",
                ["output"] = "build-check", ["timeout_ms"] = BuildTimeoutMilliseconds
            }).ConfigureAwait(false);
            var job = accepted["job_id"] ?? throw new InvalidDataException("Poima did not return a build job.");
            var cancelRequested = false;
            JsonNode snapshot;
            while (true)
            {
                if (!cancelRequested && cancellation.IsCancellationRequested)
                {
                    await Call("development.cancel", new() { ["job_id"] = job.DeepClone() }).ConfigureAwait(false);
                    cancelRequested = true;
                }
                snapshot = await Call("development.inspect", new() { ["job_id"] = job.DeepClone(), ["include_output"] = false })
                    .ConfigureAwait(false);
                if (snapshot["terminal"]!.GetValue<bool>()) break;
                await Task.Delay(250).ConfigureAwait(false);
            }
            cancellation.ThrowIfCancellationRequested();
            if (snapshot["state"]?.GetValue<string>() != "succeeded")
            {
                var feedback = await Call("development.diagnostics", new() { ["job_id"] = job.DeepClone(), ["limit"] = 8 })
                    .ConfigureAwait(false);
                var terminal = await Call("development.inspect", new() { ["job_id"] = job.DeepClone() }).ConfigureAwait(false);
                throw new InvalidOperationException(DescribeFailure(terminal, feedback));
            }

            var artifact = snapshot["result"] ?? throw new InvalidDataException("The build completed without a native gameplay artifact.");
            var session = EditorModel.NewId();
            await Call("runtime.start", new() { ["session_id"] = session, ["revision"] = project.Revision }).ConfigureAwait(false);
            var loaded = await Call("runtime.gameplay.load_native", new()
            {
                ["session_id"] = session, ["request_id"] = EditorModel.NewId(), ["expected_tick"] = 0,
                ["expected_revision"] = 0, ["descriptor"] = artifact["descriptor"]!.DeepClone(),
                ["expected_descriptor_sha256"] = artifact["descriptor_sha256"]!.DeepClone()
            }).ConfigureAwait(false);
            var module = loaded["module"];
            if (module?["backend"]?.GetValue<string>() != "native_aot" || module["values"]?["Uses"]?.GetValue<int>() != 0)
                throw new InvalidOperationException("The native starter did not initialize its input counter.");
            await Call("runtime.step", new()
            {
                ["session_id"] = session, ["request_id"] = EditorModel.NewId(), ["expected_tick"] = 0, ["ticks"] = 1,
                ["inputs"] = new JsonArray(new JsonObject { ["entity"] = "00000000000000000000000000000002", ["use"] = true })
            }).ConfigureAwait(false);
            var observed = await Call("runtime.gameplay.inspect", new() { ["session_id"] = session, ["tick"] = 1 }).ConfigureAwait(false);
            if (observed["module"]?["values"]?["Uses"]?.GetValue<int>() != 1)
                throw new InvalidOperationException("The compiled starter did not respond to gameplay input.");
            cancellation.ThrowIfCancellationRequested();
        }
        finally { scratch.Delete(recursive: true); }
    }

    private static string DescribeFailure(JsonNode snapshot, JsonNode feedback)
    {
        var diagnostic = feedback["diagnostics"]!.AsArray().FirstOrDefault(item => item?["severity"]?.GetValue<string>() == "error");
        var cause = diagnostic is null ? snapshot["error"]?.GetValue<string>() :
            $"{diagnostic["code"]?.GetValue<string>()} {diagnostic["message"]?.GetValue<string>()}";
        if (string.IsNullOrWhiteSpace(cause))
            cause = snapshot["stderr"]?.GetValue<string>().Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).LastOrDefault();
        if (string.IsNullOrWhiteSpace(cause) && snapshot["exit_code"]?.GetValue<int>() is { } exitCode)
            cause = $"The compiler returned exit code {exitCode}.";
        if (string.IsNullOrWhiteSpace(cause)) cause = $"Build state: {snapshot["state"]?.GetValue<string>()}.";
        return "The starter build could not finish. " + (cause.Length > 450 ? cause[..450] + "…" : cause);
    }
}
