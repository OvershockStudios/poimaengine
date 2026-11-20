// SPDX-License-Identifier: Apache-2.0
using System.Text.Json.Nodes;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace Poima.Editor;

internal sealed class CodexQuestionCard : StackPanel
{
    private sealed record Option(string Label, string Description);
    private sealed record Question(string Id, string Header, string Text, bool Other, bool Secret, Option[] Options);

    private readonly List<(Question Question, RadioButton[] Options, RadioButton? Other, TextBox? Text)> fields = [];
    private readonly Button submit = new() { Content = "Send answers", IsEnabled = false };
    public JsonNode RequestId { get; }
    public string TurnId { get; }

    private CodexQuestionCard(JsonNode id, string turnId, IReadOnlyList<Question> questions)
    {
        RequestId = id.DeepClone();
        TurnId = turnId;
        Spacing = 10;
        Margin = new Thickness(0, 5);
        Children.Add(new TextBlock { Text = "Your choices", FontWeight = FontWeight.SemiBold });
        for (var index = 0; index < questions.Count; index++)
            AddQuestion(questions[index], index + 1);
        submit.Background = EditorTheme.Brush("#722F37");
        submit.Foreground = EditorTheme.Brush("#F7E8EB");
        submit.HorizontalAlignment = HorizontalAlignment.Left;
        Children.Add(submit);
    }

    public static CodexQuestionCard? Create(JsonNode id, JsonObject? request)
    {
        if (request is null || ReadText(request["turnId"], 256) is not { } turnId ||
            request["questions"] is not JsonArray items || items.Count is < 1 or > 8) return null;

        var questions = new List<Question>(items.Count);
        var questionIds = new HashSet<string>(StringComparer.Ordinal);
        foreach (var item in items)
        {
            if (item is not JsonObject row || ReadText(row["id"], 256) is not { } key || !questionIds.Add(key) ||
                ReadText(row["question"], 2048) is not { } text) return null;
            var options = new List<Option>();
            if (row["options"] is JsonNode raw)
            {
                if (raw is not JsonArray candidates || candidates.Count > 12) return null;
                var labels = new HashSet<string>(StringComparer.Ordinal);
                foreach (var choice in candidates)
                {
                    if (choice is not JsonObject option || ReadText(option["label"], 256) is not { } label || !labels.Add(label))
                        return null;
                    options.Add(new(label, ReadText(option["description"], 2048) ?? ""));
                }
            }
            questions.Add(new(key, ReadText(row["header"], 128) ?? "", text, Flag(row["isOther"]),
                Flag(row["isSecret"]), options.ToArray()));
        }
        return new(id, turnId, questions);
    }

    public void OnSubmit(Func<JsonObject, Task> send)
    {
        submit.Click += async (_, _) =>
        {
            if (!IsEffectivelyEnabled || Answers() is not { } answers) return;
            IsEnabled = false;
            foreach (var (question, _, _, text) in fields)
                if (question.Secret && text is not null) text.Text = "";
            await send(new JsonObject { ["answers"] = answers });
        };
    }

    private static string? ReadText(JsonNode? item, int limit) =>
        item is JsonValue value && value.TryGetValue<string>(out var text) &&
        !string.IsNullOrWhiteSpace(text) && text.Length <= limit ? text : null;

    private static bool Flag(JsonNode? item) => item is JsonValue value && value.TryGetValue<bool>(out var flag) && flag;

    private void AddQuestion(Question question, int index)
    {
        var group = "AgentQuestion-" + Guid.NewGuid().ToString("N");
        var stack = new StackPanel { Spacing = 5 };
        if (question.Header.Length != 0)
            stack.Children.Add(new TextBlock { Text = question.Header, FontWeight = FontWeight.SemiBold });
        var label = new TextBlock { Text = question.Text, TextWrapping = TextWrapping.Wrap };
        stack.Children.Add(label);
        var options = new RadioButton[question.Options.Length];
        for (var i = 0; i < options.Length; ++i)
        {
            var choice = question.Options[i];
            var button = options[i] = new RadioButton { GroupName = group, HorizontalAlignment = HorizontalAlignment.Stretch,
                Content = new StackPanel { Children =
                {
                    new TextBlock { Text = choice.Label, TextWrapping = TextWrapping.Wrap },
                    new TextBlock { Text = choice.Description, TextWrapping = TextWrapping.Wrap, FontSize = 12,
                        Foreground = EditorTheme.Brush("#B4ACB2"), IsVisible = choice.Description.Length != 0 }
                }} };
            AutomationProperties.SetAutomationId(button, $"agent-question-{index}-choice-{i + 1}");
            AutomationProperties.SetName(button, choice.Label);
            AutomationProperties.SetHelpText(button, choice.Description);
            button.IsCheckedChanged += (_, _) => UpdateSubmit();
            stack.Children.Add(button);
        }

        RadioButton? other = null;
        TextBox? text = null;
        if (question.Other || options.Length == 0)
        {
            text = new TextBox { PlaceholderText = question.Secret ? "Private answer" : "Your answer",
                MaxLength = 4096, IsVisible = options.Length == 0 };
            if (question.Secret) text.PasswordChar = '•';
            AutomationProperties.SetAutomationId(text, $"agent-question-{index}-answer");
            AutomationProperties.SetName(text, question.Header.Length != 0 ? question.Header : "Your answer");
            text.TextChanged += (_, _) => UpdateSubmit();
            if (options.Length != 0)
            {
                other = new RadioButton { GroupName = group, Content = "Write my own answer" };
                AutomationProperties.SetAutomationId(other, $"agent-question-{index}-other");
                var answer = text;
                other.IsCheckedChanged += (_, _) =>
                {
                    answer.IsVisible = other.IsChecked == true;
                    if (answer.IsVisible) answer.Focus();
                    UpdateSubmit();
                };
                stack.Children.Add(other);
            }
            stack.Children.Add(text);
        }
        fields.Add((question, options, other, text));
        Children.Add(new Border { BorderBrush = EditorTheme.Brush("#41383E"), BorderThickness = new Thickness(1),
            Padding = new Thickness(10), CornerRadius = new CornerRadius(6), Child = stack });
    }

    private static string? Selected(Question question, RadioButton[] options, RadioButton? other, TextBox? text)
    {
        if (options.Length == 0 || other?.IsChecked == true) return text?.Text?.Trim();
        var selected = Array.FindIndex(options, option => option.IsChecked == true);
        return selected < 0 ? null : question.Options[selected].Label;
    }

    private void UpdateSubmit() => submit.IsEnabled = fields.All(
        row => !string.IsNullOrWhiteSpace(Selected(row.Question, row.Options, row.Other, row.Text)));

    private JsonObject? Answers()
    {
        var result = new JsonObject();
        foreach (var (question, options, other, text) in fields)
        {
            var answer = Selected(question, options, other, text);
            if (string.IsNullOrWhiteSpace(answer)) return null;
            result[question.Id] = new JsonObject { ["answers"] = new JsonArray(JsonValue.Create(answer)) };
        }
        return result;
    }
}
