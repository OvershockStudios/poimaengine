// SPDX-License-Identifier: Apache-2.0
using Poima.Editor;

internal static class CreatorConversationContract
{
    public static void Verify()
    {
        var root = Path.Combine(Path.GetTempPath(), "poima-chat-store-" + Guid.NewGuid().ToString("N"));
        var a = Path.Combine(root, "Game", "project.poimaproject");
        var b = Path.Combine(root, "Game Copy", "project.poimaproject");
        var store = new CreatorConversationStore(a, "project-a", root);
        try
        {
            Check(store.Read() is null, "Fresh project had a saved chat");
            store.Write("known-chat");
            Check(new CreatorConversationStore(a, "project-a", root).Read() == "known-chat", "Saved chat did not survive reopen");
            Check(new CreatorConversationStore(b, "project-a", root).Read() is null &&
                new CreatorConversationStore(a, "project-b", root).Read() is null, "Chat leaked into a project copy or replacement");
            try { store.Write("invalid\nthread"); throw new InvalidOperationException("Accepted a malformed chat ID"); }
            catch (InvalidDataException) { }
            Check(store.Read() == "known-chat", "Failed update lost the saved chat");
            store.Write("new-chat");
            Check(store.Read() == "new-chat" && Directory.GetFiles(root).Length == 1, "Replacement or temporary cleanup failed");
            Console.WriteLine("Creator Codex chat survives reopen, isolates project copies and rejects malformed updates.");
        }
        finally { if (Directory.Exists(root)) Directory.Delete(root, recursive: true); }
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
