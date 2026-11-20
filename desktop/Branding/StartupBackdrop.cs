// SPDX-License-Identifier: Apache-2.0
namespace Poima.Branding;

internal static class StartupBackdrop
{
    public const uint Ground = 0xFF211016;
    public static readonly (float Position, uint Argb)[] Tint =
    [
        (0, 0xE02B0D13),
        (0.35f, 0xA332111B),
        (0.80f, 0xD9211016),
        (1, Ground)
    ];

    public static (double X, double Y, double Width, double Height) Crop(
        double imageWidth, double imageHeight, double width, double height)
    {
        var scale = Math.Max(width / imageWidth, height / imageHeight);
        var visibleWidth = width / scale;
        var visibleHeight = height / scale;
        return ((imageWidth - visibleWidth) * 0.5, (imageHeight - visibleHeight) * 0.58,
            visibleWidth, visibleHeight);
    }
}
