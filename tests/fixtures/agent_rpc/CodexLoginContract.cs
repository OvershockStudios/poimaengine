// SPDX-License-Identifier: Apache-2.0
using Poima.AgentHost;

internal static class CodexLoginContract
{
    public static async Task VerifyAsync(string client, string engine, string directory, bool useFixture)
    {
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(25));
        var token = deadline.Token;
        Task<CodexSession> Connect() => CodexSession.ConnectAsync(client, engine, "auth-contract",
            directory, token, receiveQuestions: true);
        const string mode = "POIMA_TEST_CODEX_SIGN_IN";
        var saved = Environment.GetEnvironmentVariable(mode);
        try
        {
            if (useFixture) Environment.SetEnvironmentVariable(mode, "cancel");
            await using (var codex = await Connect())
            {
                Check(await codex.RequiresSignInAsync(token), "Expected a signed-out Codex session.");
                var login = await codex.StartBrowserLoginAsync(token);
                Check(login.AuthorizationPage.IdnHost is "auth.openai.com" or "chatgpt.com", "Expected official browser authorization.");
                var cancel = await codex.CancelBrowserLoginAsync(login.Id, token);
                Check(cancel?["status"]?.GetValue<string>() == "canceled", "Expected the matching pending login to cancel.");
                Check(await codex.RequiresSignInAsync(token), "Cancelling login must not create an account.");
            }
            if (useFixture)
            {
                Environment.SetEnvironmentVariable(mode, "invalid-url");
                await using var codex = await Connect();
                try
                {
                    await codex.StartBrowserLoginAsync(token);
                    throw new InvalidOperationException("Accepted a non-OpenAI sign-in URL.");
                }
                catch (InvalidDataException) { }
            }
        }
        finally { if (useFixture) Environment.SetEnvironmentVariable(mode, saved); }
        Console.WriteLine(useFixture ? "Codex account read, validated browser login, correlated cancellation and rejected sign-in URL passed." :
            "Signed-out Codex account read, official browser login start and matching cancellation passed.");
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
