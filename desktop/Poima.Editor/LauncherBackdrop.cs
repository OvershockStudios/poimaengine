// SPDX-License-Identifier: Apache-2.0
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Poima.Branding;

namespace Poima.Editor;

internal sealed class LauncherBackdrop : Control, IDisposable
{
    private readonly Bitmap landscape = Load("Landscape.jpg");
    private readonly LinearGradientBrush overlay = new()
    {
        StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative),
        EndPoint = new RelativePoint(0, 1, RelativeUnit.Relative),
        GradientStops = new GradientStops()
    };

    public LauncherBackdrop()
    {
        foreach (var stop in StartupBackdrop.Tint)
            overlay.GradientStops.Add(new GradientStop(Color.FromUInt32(stop.Argb), stop.Position));
    }

    internal static Bitmap Load(string asset)
    {
        using var stream = AssetLoader.Open(new Uri($"avares://Poima.Editor/Assets/{asset}"));
        return new Bitmap(stream);
    }

    public override void Render(DrawingContext context)
    {
        var bounds = new Rect(Bounds.Size);
        if (bounds.Width <= 0 || bounds.Height <= 0) return;
        var size = landscape.Size;
        var crop = StartupBackdrop.Crop(size.Width, size.Height, bounds.Width, bounds.Height);
        context.DrawImage(landscape, new Rect(crop.X, crop.Y, crop.Width, crop.Height), bounds);
        context.FillRectangle(overlay, bounds);
    }

    public void Dispose() => landscape.Dispose();
}
