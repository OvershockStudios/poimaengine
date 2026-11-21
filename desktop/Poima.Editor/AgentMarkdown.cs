// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Documents;
using Avalonia.Controls.Primitives;
using Avalonia.Media;
using Markdig;
using Markdig.Extensions.Tables;
using Markdig.Syntax;
using Md = Markdig.Syntax.Inlines;

namespace Poima.Editor;

internal static class AgentMarkdown
{
    private static readonly MarkdownPipeline Pipeline = new MarkdownPipelineBuilder { MaximumNestingDepth = 24 }
        .DisableHtml().UsePipeTables().Build();
    private static readonly FontFamily CodeFont = new("Consolas");
    private static readonly IBrush Accent = EditorTheme.Brush("#F0CFD6");
    private static readonly IBrush Rule = EditorTheme.Brush("#69515A");
    private static readonly IBrush CodeBackground = EditorTheme.Brush("#1D171D");

    public static Control Format(string text)
    {
        var panel = new StackPanel { Spacing = 8 };
        Fill(panel, Markdown.Parse(text, Pipeline));
        return panel;
    }

    private static void Fill(Panel panel, ContainerBlock blocks)
    {
        foreach (var block in blocks)
        {
            switch (block)
            {
                case HeadingBlock heading:
                    var label = InlineText(heading);
                    label.FontSize = heading.Level == 1 ? 22 : heading.Level == 2 ? 19 : 16;
                    label.FontWeight = FontWeight.SemiBold;
                    panel.Children.Add(label);
                    break;
                case ParagraphBlock paragraph:
                    panel.Children.Add(InlineText(paragraph));
                    break;
                case CodeBlock code:
                    panel.Children.Add(new Border
                    {
                        Background = CodeBackground, Padding = new Thickness(10, 7),
                        Child = new ScrollViewer
                        {
                            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
                            VerticalScrollBarVisibility = ScrollBarVisibility.Disabled,
                            Content = new SelectableTextBlock
                            {
                                Text = code.Lines.ToString(), FontFamily = CodeFont, TextWrapping = TextWrapping.NoWrap
                            }
                        }
                    });
                    break;
                case QuoteBlock quote:
                    var quoted = new StackPanel { Spacing = 5 };
                    Fill(quoted, quote);
                    panel.Children.Add(new Border
                    {
                        BorderBrush = Rule, BorderThickness = new Thickness(2, 0, 0, 0),
                        Padding = new Thickness(10, 0, 0, 0), Child = quoted
                    });
                    break;
                case ListBlock list:
                    panel.Children.Add(List(list));
                    break;
                case Table table:
                    panel.Children.Add(TableGrid(table));
                    break;
                case ThematicBreakBlock:
                    panel.Children.Add(new Border { Height = 1, Background = Rule, Margin = new Thickness(0, 4) });
                    break;
                case ContainerBlock container:
                    Fill(panel, container);
                    break;
            }
        }
    }

    private static Control List(ListBlock list)
    {
        var items = new StackPanel { Spacing = 4 };
        var number = long.TryParse(list.OrderedStart, out var start) ? start : 1;
        foreach (var item in list.OfType<ListItemBlock>())
        {
            var content = new StackPanel { Spacing = 4 };
            Fill(content, item);
            var row = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*"), ColumnSpacing = 8 };
            row.Children.Add(new TextBlock { Text = list.IsOrdered ? $"{number++}." : "•", Foreground = Accent });
            Grid.SetColumn(content, 1);
            row.Children.Add(content);
            items.Children.Add(row);
        }
        return items;
    }

    private static Control TableGrid(Table table)
    {
        var grid = new Grid();
        for (var column = 0; column < table.ColumnDefinitions.Count; column++)
            grid.ColumnDefinitions.Add(new ColumnDefinition(GridLength.Auto));
        foreach (var row in table.OfType<TableRow>())
        {
            var index = grid.RowDefinitions.Count;
            grid.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
            var column = 0;
            foreach (var cell in row.OfType<TableCell>())
            {
                var content = new StackPanel { Spacing = 4, MinWidth = 72, MaxWidth = 280 };
                if (row.IsHeader)
                    TextElement.SetFontWeight(content, FontWeight.SemiBold);
                Fill(content, cell);
                var border = new Border
                {
                    Background = row.IsHeader ? CodeBackground : null,
                    BorderBrush = Rule, BorderThickness = new Thickness(0, 0, 1, 1),
                    Padding = new Thickness(8, 5), Child = content
                };
                Grid.SetRow(border, index);
                Grid.SetColumn(border, column++);
                grid.Children.Add(border);
            }
        }
        return new ScrollViewer
        {
            Content = grid,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            VerticalScrollBarVisibility = ScrollBarVisibility.Disabled
        };
    }

    private static SelectableTextBlock InlineText(LeafBlock block)
    {
        var text = new SelectableTextBlock { TextWrapping = TextWrapping.Wrap };
        if (block.Inline is { } children)
        {
            AddInlines(text.Inlines!, children);
            AutomationProperties.SetName(text, string.Concat(text.Inlines!.Select(PlainText)));
        }
        return text;
    }

    private static string? PlainText(Inline inline) => inline switch
    {
        Run run => run.Text,
        Span span => string.Concat(span.Inlines.Select(PlainText)),
        LineBreak => "\n",
        _ => null
    };

    private static void AddInlines(InlineCollection result, Md.ContainerInline input)
    {
        foreach (var inline in input)
        {
            switch (inline)
            {
                case Md.LiteralInline literal:
                    result.Add(new Run(literal.Content.ToString()));
                    break;
                case Md.CodeInline code:
                    result.Add(new Run(code.Content) { FontFamily = CodeFont, Foreground = Accent });
                    break;
                case Md.EmphasisInline emphasis:
                    var style = new Span();
                    if (emphasis.DelimiterCount >= 2) style.FontWeight = FontWeight.SemiBold;
                    else style.FontStyle = FontStyle.Italic;
                    AddInlines(style.Inlines, emphasis);
                    result.Add(style);
                    break;
                case Md.LinkInline link:
                    var label = new Span { Foreground = Accent, TextDecorations = TextDecorations.Underline };
                    if (link.IsImage) label.Inlines.Add(new Run("Image: "));
                    AddInlines(label.Inlines, link);
                    result.Add(label);
                    break;
                case Md.AutolinkInline link:
                    result.Add(new Run(link.Url) { Foreground = Accent, TextDecorations = TextDecorations.Underline });
                    break;
                case Md.LineBreakInline line:
                    if (line.IsHard) result.Add(new LineBreak());
                    else result.Add(new Run(" "));
                    break;
                case Md.HtmlEntityInline entity:
                    result.Add(new Run(entity.Transcoded.ToString()));
                    break;
                case Md.ContainerInline children:
                    AddInlines(result, children);
                    break;
            }
        }
    }
}
