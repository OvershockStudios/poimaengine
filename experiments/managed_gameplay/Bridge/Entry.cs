// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace Poima.ManagedLab;

public static class Entry
{
    private static readonly JsonSerializerOptions JsonOptions = new() { PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower };

    public static int Run(nint arguments, int argumentBytes)
    {
        try
        {
            if (argumentBytes != nint.Size) throw new ArgumentException("Native bridge argument size mismatch.");
            string path = Marshal.PtrToStringUTF8(Marshal.ReadIntPtr(arguments)) ?? throw new ArgumentException("Missing kernel path.");
            Console.InputEncoding = Encoding.UTF8;
            Console.OutputEncoding = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false);
            var kernel = new NativeKernel(path);
            using var host = new Host(kernel);
            string? line;
            while ((line = Console.ReadLine()) is not null)
                if (Execute(host, line)) break;
            return 0;
        }
        catch (Exception error)
        {
            Respond("startup", null, error.Message, 0);
            return 4;
        }
    }

    // A separate frame lets exception/type/local references from a previous
    // command disappear before the next explicit unload-collection check.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static bool Execute(Host host, string line)
    {
        int separator = line.IndexOf(' ');
        string command = separator < 0 ? line : line[..separator];
        string argument = separator < 0 ? "" : line[(separator + 1)..];
        string? failure = null;
        long started = Stopwatch.GetTimestamp();
        try
        {
            switch (command)
            {
                case "load": case "save": case "restore":
                    if (argument.Length == 0) throw new ArgumentException("A path is required; do not quote it inside this protocol.");
                    if (command == "load") host.Load(argument);
                    else if (command == "save") host.Save(argument);
                    else host.Restore(argument);
                    break;
                case "step":
                    if (!ulong.TryParse(argument, NumberStyles.None, CultureInfo.InvariantCulture, out ulong ticks))
                        throw new ArgumentException("Expected unsigned decimal ticks.");
                    host.Step(ticks);
                    break;
                case "inspect": case "collect": case "quit": case "release-probe":
                    if (argument.Length != 0) throw new ArgumentException("Command takes no argument.");
                    if (command == "collect") host.CollectRetired();
                    if (command == "release-probe") Poima.Lab.LifetimeProbe.Release();
                    if (command == "quit") { host.Dispose(); host.CollectRetired(); }
                    break;
                default: throw new ArgumentException("Commands: load PATH, step N, inspect, collect, save PATH, restore PATH, quit.");
            }
        }
        catch (Exception error) { failure = error.Message; }
        double elapsed = Stopwatch.GetElapsedTime(started).TotalMilliseconds;
        Respond(command, host.Inspect(), failure, elapsed);
        return command == "quit" && failure is null;
    }

    private static void Respond(string command, object? state, string? failure, double milliseconds)
    {
        var reply = new
        {
            protocol_version = 1, request_id = (string?)null, command = "managed-lab." + command,
            status = failure is null ? "ok" : "error",
            result = new { operation_ms = milliseconds, state },
            diagnostics = failure is null ? Array.Empty<object>() : [new { code = "managed_lab_failure", message = failure }]
        };
        Console.WriteLine(JsonSerializer.Serialize(reply, JsonOptions));
        Console.Out.Flush();
    }
}
