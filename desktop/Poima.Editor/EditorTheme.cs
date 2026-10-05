// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Media;
using Avalonia.Styling;
using Avalonia.Markup.Xaml.Styling;

namespace Poima.Editor;

public sealed class EditorTheme : Styles
{
    public static IBrush Brush(string value) => new SolidColorBrush(Color.Parse(value));
    public EditorTheme()
    {
        Add(new StyleInclude(new System.Uri("avares://Poima.Editor/"))
        { Source = new System.Uri("avares://Poima.Editor/Themes/EditorControls.axaml") });
    }
}

public static class EditorIcons
{
    // Original 16-unit line drawings; no emoji glyphs or borrowed Unity assets.
    public static Control Make(string kind, double size = 16)
    {
        var (path, color) = kind switch
        {
            "folder" => ("M1,4 L6,4 L8,6 L15,6 L15,14 L1,14 Z M1,6 L15,6", "#C5B779"),
            "cube" => ("M8,1 L14,4.5 L14,11.5 L8,15 L2,11.5 L2,4.5 Z M2,4.5 L8,8 L14,4.5 M8,8 L8,15", "#91B5D2"),
            "camera" => ("M1,4 L10,4 L10,13 L1,13 Z M10,7 L15,4 L15,13 L10,10 M3,2 L8,2", "#B7BBC2"),
            "light" => ("M5,12 L11,12 M6,15 L10,15 M5,9 C0,3 5,0 8,1 C13,0 16,5 11,9 L11,10 L5,10 Z", "#E7CA74"),
            "audio" => ("M1,6 L4,6 L8,2 L8,14 L4,10 L1,10 Z M11,5 Q15,8 11,11", "#B0C699"),
            "audio_muted" => ("M1,6 L4,6 L8,2 L8,14 L4,10 L1,10 Z M11,5 L15,11 M15,5 L11,11", "#B7BBC2"),
            "rig" => ("M6,2 L10,2 L10,6 L6,6 Z M8,6 L8,11 M3,8 L13,8 M8,11 L4,15 M8,11 L12,15", "#C8AACD"),
            "image" => ("M1,2 L15,2 L15,14 L1,14 Z M2,12 L6,7 L9,10 L12,6 L15,10 M4,4 L5,4 L5,5 L4,5 Z", "#A2BC95"),
            "play" => ("M4,2 L13,8 L4,14 Z", "#D8D8D8"),
            "pause" => ("M3,2 L6,2 L6,14 L3,14 Z M10,2 L13,2 L13,14 L10,14 Z", "#D8D8D8"),
            "stop" => ("M3,3 L13,3 L13,13 L3,13 Z", "#D8D8D8"),
            "step" => ("M2,3 L10,8 L2,13 Z M13,2 L13,14", "#D8D8D8"),
            "plus" => ("M8,2 L8,14 M2,8 L14,8", "#D8D8D8"),
            "select" => ("M3,1 L3,12 L6,9 L9,15 L11,14 L8,8 L13,8 Z", "#C8CAD0"),
            "move" => ("M8,1 L8,15 M1,8 L15,8 M5,4 L8,1 L11,4 M5,12 L8,15 L11,12 M4,5 L1,8 L4,11 M12,5 L15,8 L12,11", "#C8CAD0"),
            "rotate" => ("M13,5 A6,6 0 1 0 14,10 M10,5 L14,5 L14,1", "#C8CAD0"),
            "scale" => ("M2,10 L6,10 L6,14 L2,14 Z M6,10 L14,2 M8,2 L14,2 L14,8", "#C8CAD0"),
            "world" => ("M15,8 A7,7 0 1 1 1,8 A7,7 0 1 1 15,8 M1,8 L15,8 M8,1 C3,5 3,11 8,15 C13,11 13,5 8,1 Z", "#C8CAD0"),
            "local" => ("M3,11 L3,2 M1,4 L3,2 L5,4 M5,13 L14,13 M12,11 L14,13 L12,15 M5,10 L12,3 M8,3 L12,3 L12,7 M1,11 L5,11 L5,15 L1,15 Z", "#C8CAD0"),
            "frame" => ("M1,5 L1,1 L5,1 M11,1 L15,1 L15,5 M15,11 L15,15 L11,15 M5,15 L1,15 L1,11 M5,8 L11,8 M8,5 L8,11", "#C8CAD0"),
            "import" => ("M8,1 L8,10 M4,6 L8,10 L12,6 M2,10 L2,14 L14,14 L14,10", "#C8CAD0"),
            "search" => ("M11,7 A4,4 0 1 1 3,7 A4,4 0 1 1 11,7 M10,10 L15,15", "#AAAAAA"),
            "chevron" => ("M5,3 L10,8 L5,13", "#AAAAAA"),
            "expanded" => ("M3,5 L8,10 L13,5", "#AAAAAA"),
            "undo" => ("M6,3 L2,7 L6,11 M2,7 L10,7 Q14,7 14,12", "#C8CAD0"),
            "redo" => ("M10,3 L14,7 L10,11 M14,7 L6,7 Q2,7 2,12", "#C8CAD0"),
            "grid" => ("M2,2 L6,2 L6,6 L2,6 Z M10,2 L14,2 L14,6 L10,6 Z M2,10 L6,10 L6,14 L2,14 Z M10,10 L14,10 L14,14 L10,14 Z", "#C8CAD0"),
            "list" => ("M2,3 L4,3 M6,3 L14,3 M2,8 L4,8 M6,8 L14,8 M2,13 L4,13 M6,13 L14,13", "#C8CAD0"),
            "refresh" => ("M13,6 A5,5 0 1 0 13,11 M10,6 L14,6 L14,2", "#C8CAD0"),
            "home" => ("M1,7 L8,1 L15,7 M3,6 L3,14 L7,14 L7,10 L10,10 L10,14 L13,14 L13,6", "#C8CAD0"),
            "settings" => ("M2,4 L14,4 M2,12 L14,12 M5,1 L5,7 M11,9 L11,15", "#C8CAD0"),
            "scene" => ("M1,12 L5,5 L9,10 L12,7 L15,12 Z M11,2 L13,2 L13,4 L11,4 Z", "#C8CAD0"),
            "transform" => ("M8,15 L8,2 M5,5 L8,2 L11,5 M8,8 L15,8 M12,5 L15,8 L12,11 M8,8 L2,14 M2,10 L2,14 L6,14", "#C8CAD0"),
            "context" => ("M3,3 L13,3 M3,8 L13,8 M3,13 L13,13", "#C8CAD0"),
            "code" => ("M5,3 L1,8 L5,13 M11,3 L15,8 L11,13 M9,1 L7,15", "#92B9AE"),
            _ => ("M3,1 L10,1 L14,5 L14,15 L3,15 Z M10,1 L10,5 L14,5 M5,8 L12,8 M5,11 L12,11", "#AFAFAF")
        };
        return new Viewbox { Width = size, Height = size, Child = new Avalonia.Controls.Shapes.Path { Width = 16, Height = 16, Data = Geometry.Parse(path), Stroke = EditorTheme.Brush(color), StrokeThickness = 1.2, StrokeLineCap = PenLineCap.Round, StrokeJoin = PenLineJoin.Round } };
    }
    public static string FileKind(string extension) => extension.ToLowerInvariant() switch
    {
        ".gltf" or ".glb" or ".pmodel" => "cube",
        ".png" or ".jpg" or ".jpeg" or ".pimage" => "image",
        ".wav" or ".paudio" => "audio",
        ".cs" or ".cpp" or ".hpp" => "code",
        _ => "entity"
    };
}
