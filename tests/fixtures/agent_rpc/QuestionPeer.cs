// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;

// A deterministic Codex app-server peer for the desktop question workflow.
static class QuestionPeer
{
    public static async Task RunAsync()
    {
        const string thread = "question-fixture";
        var turn = "";
        while (await Console.In.ReadLineAsync() is { } line)
        {
            var call = JsonNode.Parse(line)!;
            var id = call["id"]?.DeepClone();
            var method = call["method"]?.GetValue<string>();
            if (method is null)
            {
                if (id?.ToJsonString() == "701" && call["error"] is not null)
                {
                    await Send(Question(702, thread));
                    continue;
                }
                if (id?.ToJsonString() != "702" ||
                    call["result"]?["answers"]?["task"]?["answers"]?[0]?.GetValue<string>() != "Repair a valve" ||
                    call["result"]?["answers"]?["tone"]?["answers"]?[0]?.GetValue<string>() != "After the rain, 水")
                    throw new InvalidDataException("The question reply did not preserve the request ID and exact answers.");
                await Send(new JsonObject { ["method"] = "serverRequest/resolved",
                    ["params"] = new JsonObject { ["threadId"] = thread, ["requestId"] = 702 } });
                await Send(new JsonObject { ["method"] = "item/agentMessage/delta",
                    ["params"] = new JsonObject { ["threadId"] = thread, ["delta"] = "QUESTIONS_ACCEPTED" } });
                await Finish("completed");
                continue;
            }

            JsonObject Response(JsonNode? result) => new() { ["id"] = id, ["result"] = result };
            switch (method)
            {
                case "initialize":
                    if (call["params"]?["capabilities"]?["experimentalApi"]?.GetValue<bool>() != true)
                        throw new InvalidDataException("Desktop did not enable supported experimental requests.");
                    await Send(Response(new JsonObject()));
                    break;
                case "initialized": break;
                case "model/list":
                    await Send(Response(new JsonObject { ["data"] = new JsonArray(
                        new JsonObject { ["model"] = "question-fixture", ["isDefault"] = true }) }));
                    break;
                case "thread/start":
                    await Send(Response(new JsonObject { ["thread"] = new JsonObject { ["id"] = thread } }));
                    break;
                case "turn/start":
                    turn = Guid.NewGuid().ToString("N");
                    await Send(Response(new JsonObject { ["turn"] = new JsonObject { ["id"] = turn } }));
                    await Send(new JsonObject { ["method"] = "turn/started",
                        ["params"] = new JsonObject { ["threadId"] = thread, ["turn"] = new JsonObject { ["id"] = turn } } });
                    await Send(Question(701, "other-thread"));
                    break;
                case "turn/interrupt":
                    await Send(Response(new JsonObject()));
                    await Finish("interrupted");
                    break;
                default: throw new InvalidDataException("Unexpected question fixture method.");
            }
        }

        JsonObject Question(int id, string target) => new()
        {
            ["id"] = id, ["method"] = "item/tool/requestUserInput",
            ["params"] = new JsonObject
            {
                ["threadId"] = target, ["turnId"] = turn, ["itemId"] = "test-input", ["isBlocking"] = true,
                ["questions"] = JsonNode.Parse("""
                    [{"id":"task","header":"Activity","question":"What should the player do?","options":[
                        {"label":"Repair a valve","description":"Restore the flow of water."},
                        {"label":"Plant a seed","description":"Grow a plant."}
                    ]},{"id":"tone","header":"Mood","question":"How should this feel?","isOther":true,"options":[
                        {"label":"Calm","description":"A quiet room after the rain."},
                        {"label":"Tense","description":"A storm is coming."}
                    ]}]
                    """)
            }
        };

        Task Finish(string status) => Send(new JsonObject { ["method"] = "turn/completed",
            ["params"] = new JsonObject { ["threadId"] = thread,
                ["turn"] = new JsonObject { ["id"] = turn, ["status"] = status } } });
    }

    private static async Task Send(JsonObject message)
    {
        await Console.Out.WriteLineAsync(message.ToJsonString());
        await Console.Out.FlushAsync();
    }
}
