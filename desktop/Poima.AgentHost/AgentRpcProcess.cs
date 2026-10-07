// SPDX-License-Identifier: Apache-2.0
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;
using System.Text.Json.Nodes;
using System.Threading.Channels;

namespace Poima.AgentHost;

/// <summary>A failed exchange does not prove that the agent or engine rolled back.</summary>
public sealed class AgentExchangeException(string message, bool outcomeUnknown, Exception? inner = null)
    : Exception(message, inner)
{
    public bool OutcomeUnknown { get; } = outcomeUnknown;
}

public sealed class AgentRpcException(JsonNode error) : Exception(error["message"]?.GetValue<string>() ?? "Agent request failed.")
{
    public JsonNode Error { get; } = error.DeepClone();
}

/// <summary>
/// One owned stdio JSON-RPC process. No shell expansion, automatic retries, logging,
/// provider authentication, or assumptions that cancellation undoes side effects.
/// The owner must consume Events to service provider requests. Event overflow
/// closes the connection explicitly instead of blocking unrelated RPC responses.
/// </summary>
public sealed class AgentRpcProcess : IAsyncDisposable
{
    public const int MaximumFrameCharacters = 16 * 1024 * 1024;
    private readonly Process process;
    private readonly CancellationTokenSource lifetime = new();
    private readonly SemaphoreSlim writer = new(1, 1);
    private readonly object stateLock = new();
    private readonly ConcurrentDictionary<long, TaskCompletionSource<JsonNode?>> pending = new();
    private readonly Channel<JsonObject> events = Channel.CreateBounded<JsonObject>(new BoundedChannelOptions(64)
    {
        SingleReader = true, SingleWriter = true, FullMode = BoundedChannelFullMode.Wait
    });
    private readonly StringBuilder diagnostics = new();
    private readonly Task readerTask, errorTask, exitTask;
    private long nextId;
    private bool terminal;
    private Task? disposal;
    public ChannelReader<JsonObject> Events => events.Reader;
    public string DiagnosticTail { get { lock (diagnostics) return diagnostics.ToString(); } }

    public AgentRpcProcess(string executable, IEnumerable<string> arguments, string workingDirectory)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(executable);
        if (!Directory.Exists(workingDirectory)) throw new DirectoryNotFoundException(workingDirectory);
        var info = new ProcessStartInfo(executable)
        {
            WorkingDirectory = Path.GetFullPath(workingDirectory), UseShellExecute = false,
            RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true,
            StandardInputEncoding = new UTF8Encoding(false, true),
            StandardOutputEncoding = new UTF8Encoding(false, true),
            StandardErrorEncoding = new UTF8Encoding(false, true), CreateNoWindow = true
        };
        foreach (var argument in arguments) info.ArgumentList.Add(argument);
        process = new Process { StartInfo = info };
        try
        {
            if (!process.Start()) throw new IOException("Agent process did not start.");
        }
        catch { process.Dispose(); throw; }
        readerTask = ReadMessagesAsync();
        errorTask = DrainErrorsAsync();
        exitTask = ObserveExitAsync();
    }

    public async Task<JsonNode?> RequestAsync(string method, JsonObject? parameters = null, CancellationToken cancellation = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(method);
        cancellation.ThrowIfCancellationRequested();
        long id;
        var source = new TaskCompletionSource<JsonNode?>(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (stateLock)
        {
            if (terminal) throw new AgentExchangeException("Agent connection is closed.", false);
            id = checked(++nextId);
            pending[id] = source;
        }
        bool sent = false;
        try
        {
            await WriteAsync(new JsonObject { ["id"] = id, ["method"] = method, ["params"] = parameters?.DeepClone() ?? new JsonObject() },
                () => sent = true, cancellation);
            return await source.Task.WaitAsync(cancellation);
        }
        catch (AgentRpcException) { throw; }
        catch (Exception error)
        {
            throw new AgentExchangeException(sent ? "Agent response unavailable; the request outcome is unknown." : "Agent request was not sent.", sent, error);
        }
        finally { pending.TryRemove(id, out _); }
    }

    public Task NotifyAsync(string method, JsonObject? parameters = null, CancellationToken cancellation = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(method);
        return WriteAsync(new JsonObject { ["method"] = method, ["params"] = parameters?.DeepClone() ?? new JsonObject() }, null, cancellation);
    }

    /// <summary>Reply to a provider request using its exact string/integer ID.</summary>
    public Task ReplyAsync(JsonNode id, JsonNode? result, CancellationToken cancellation = default)
    {
        if (!ValidId(id)) throw new ArgumentException("A string or integer request ID is required.", nameof(id));
        return WriteAsync(new JsonObject { ["id"] = id.DeepClone(), ["result"] = result?.DeepClone() }, null, cancellation);
    }

    public Task RejectAsync(JsonNode id, string message, CancellationToken cancellation = default)
    {
        if (!ValidId(id)) throw new ArgumentException("A string or integer request ID is required.", nameof(id));
        return WriteAsync(new JsonObject { ["id"] = id.DeepClone(), ["error"] = new JsonObject { ["code"] = -32601, ["message"] = message } }, null, cancellation);
    }

    private async Task WriteAsync(JsonObject message, Action? beginning, CancellationToken cancellation)
    {
        var text = message.ToJsonString();
        if (text.Length > MaximumFrameCharacters) throw new ArgumentException("Agent request exceeds the frame limit.");
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellation, lifetime.Token);
        await writer.WaitAsync(linked.Token);
        var started = false;
        try
        {
            lock (stateLock) if (terminal) throw new IOException("Agent connection is closed.");
            beginning?.Invoke(); // A failed partial write may still have reached the peer.
            started = true;
            await process.StandardInput.WriteLineAsync(text.AsMemory(), linked.Token);
            await process.StandardInput.FlushAsync(linked.Token);
        }
        catch (Exception error)
        {
            // Once writing starts the peer may have an incomplete frame. Never
            // allow a later request to append to it or flush canceled content.
            if (started) Finish(error);
            throw;
        }
        finally { writer.Release(); }
    }

    private static bool ValidId(JsonNode? value) => value is JsonValue id && (id.TryGetValue<string>(out _) || id.TryGetValue<long>(out _));

    private async Task ReadMessagesAsync()
    {
        var buffer = new char[4096];
        var frame = new StringBuilder();
        try
        {
            while (true)
            {
                var count = await process.StandardOutput.ReadAsync(buffer.AsMemory(), lifetime.Token);
                if (count == 0)
                {
                    if (frame.Length != 0) throw new InvalidDataException("Agent stream ended inside a frame.");
                    break;
                }
                for (var i = 0; i < count; i++)
                {
                    if (buffer[i] == '\n')
                    {
                        var line = frame.ToString(); frame.Clear();
                        await DispatchAsync(line);
                    }
                    else
                    {
                        if (frame.Length >= MaximumFrameCharacters) throw new InvalidDataException("Agent frame exceeds the limit.");
                        frame.Append(buffer[i]);
                    }
                }
            }
            Finish(new EndOfStreamException("Agent output closed."));
        }
        catch (OperationCanceledException) when (lifetime.IsCancellationRequested) { }
        catch (Exception error) { Finish(error); }
    }

    private Task DispatchAsync(string line)
    {
        var message = JsonNode.Parse(line, documentOptions: new() { MaxDepth = 64 }) as JsonObject
            ?? throw new InvalidDataException("Agent frame must be an object.");
        if (message.TryGetPropertyValue("jsonrpc", out var version) && version?.GetValue<string>() != "2.0")
            throw new InvalidDataException("Unsupported JSON-RPC version.");
        if (message.ContainsKey("method"))
        {
            if (message["method"] is not JsonValue method || !method.TryGetValue<string>(out var name) || string.IsNullOrEmpty(name)
                || message.ContainsKey("result") || message.ContainsKey("error")
                || (message.ContainsKey("id") && !ValidId(message["id"])))
                throw new InvalidDataException("Invalid agent request or notification.");
            if (!events.Writer.TryWrite(message))
                throw new InvalidDataException("Agent event buffer is full; consume events continuously.");
            return Task.CompletedTask;
        }
        if (message.ContainsKey("result") == message.ContainsKey("error") || !ValidId(message["id"]))
            throw new InvalidDataException("Invalid agent response.");
        if (message["error"] is JsonNode error && (error is not JsonObject || error["message"] is not JsonValue text
            || !text.TryGetValue<string>(out _) || error["code"] is not JsonValue code || !code.TryGetValue<long>(out _)))
            throw new InvalidDataException("Invalid agent error.");
        if (message.ContainsKey("error") && message["error"] is null) throw new InvalidDataException("Null agent error.");
        // A timed-out request can still finish. Its late response must never be
        // mistaken for a newer call or cause another outbound request.
        if (message["id"] is JsonValue id && id.TryGetValue<long>(out var number) && pending.TryRemove(number, out var source))
        {
            if (message["error"] is JsonNode problem) source.TrySetException(new AgentRpcException(problem));
            else source.TrySetResult(message["result"]?.DeepClone());
        }
        return Task.CompletedTask;
    }

    private async Task DrainErrorsAsync()
    {
        var buffer = new char[2048];
        try
        {
            while (true)
            {
                var count = await process.StandardError.ReadAsync(buffer.AsMemory(), lifetime.Token);
                if (count == 0) break;
                lock (diagnostics)
                {
                    diagnostics.Append(buffer, 0, count);
                    if (diagnostics.Length > 8192) diagnostics.Remove(0, diagnostics.Length - 8192);
                }
            }
        }
        catch (OperationCanceledException) when (lifetime.IsCancellationRequested) { }
        catch (Exception error) { Finish(error); }
    }

    private async Task ObserveExitAsync()
    {
        try
        {
            await process.WaitForExitAsync(lifetime.Token);
            // stdout may still contain a final response. The reader owns normal
            // completion, so process exit cannot discard buffered messages.
            await readerTask;
        }
        catch (OperationCanceledException) when (lifetime.IsCancellationRequested) { }
        catch (Exception error) { Finish(error); }
    }

    private void Finish(Exception error)
    {
        lock (stateLock)
        {
            if (terminal) return;
            terminal = true;
            foreach (var entry in pending)
                if (pending.TryRemove(entry.Key, out var source)) source.TrySetException(error);
            events.Writer.TryComplete(error);
        }
        lifetime.Cancel();
    }

    public ValueTask DisposeAsync()
    {
        lock (stateLock) return new ValueTask(disposal ??= DisposeCoreAsync());
    }

    private async Task DisposeCoreAsync()
    {
        Finish(new ObjectDisposedException(nameof(AgentRpcProcess)));
        try
        {
            // Cancellation does not join a pending StreamWriter operation.
            // Closing it concurrently can throw and abandon the child process.
            await writer.WaitAsync();
            try { process.StandardInput.Close(); }
            catch (Exception error) when (error is IOException or InvalidOperationException) { }
            finally { writer.Release(); }
            if (!process.HasExited)
            {
                using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
                try { await process.WaitForExitAsync(timeout.Token); }
                catch (OperationCanceledException)
                {
                    try { process.Kill(entireProcessTree: true); }
                    catch (InvalidOperationException) when (process.HasExited) { }
                    await process.WaitForExitAsync();
                }
            }
            await Task.WhenAll(readerTask, errorTask, exitTask);
        }
        finally { process.Dispose(); }
    }
}
