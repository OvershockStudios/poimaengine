// SPDX-License-Identifier: Apache-2.0
using Avalonia.Platform.Storage;
using Poima.AgentHost;
using SkiaSharp;

namespace Poima.Editor;

internal static class ReferenceImage
{
    private const int MaximumSourceBytes = 16 * 1024 * 1024;
    private const int MaximumSourcePixels = 24_000_000;

    public static async Task<AgentImage> ReadAsync(IStorageFile file, CancellationToken token)
    {
        await using var input = await file.OpenReadAsync();
        using var copy = new MemoryStream();
        var buffer = new byte[64 * 1024];
        int read;
        while ((read = await input.ReadAsync(buffer, token)) != 0)
        {
            if (copy.Length + read > MaximumSourceBytes)
                throw new InvalidDataException("Choose images no larger than 16 MiB.");
            copy.Write(buffer, 0, read);
        }
        return await Task.Run(() => Normalize(copy.GetBuffer().AsSpan(0, (int)copy.Length)), token);
    }

    private static AgentImage Normalize(ReadOnlySpan<byte> bytes)
    {
        using var data = SKData.CreateCopy(bytes);
        using var codec = SKCodec.Create(data);
        if (codec is null || codec.EncodedFormat is not (SKEncodedImageFormat.Png or SKEncodedImageFormat.Jpeg))
            throw new InvalidDataException("Choose a readable PNG or JPEG image.");
        var (width, height) = (codec.Info.Width, codec.Info.Height);
        if (width <= 0 || height <= 0 || (long)width * height > MaximumSourcePixels)
            throw new InvalidDataException("Choose images no larger than 24 megapixels.");

        using var srgb = SKColorSpace.CreateSrgb();
        var info = new SKImageInfo(width, height, SKColorType.Rgba8888, SKAlphaType.Premul, srgb);
        using var original = new SKBitmap(info);
        var pixels = original.GetPixels();
        if (pixels == IntPtr.Zero || codec.GetPixels(info, pixels) != SKCodecResult.Success)
            throw new InvalidDataException("The image could not be decoded completely.");
        original.SetImmutable();
        using var raster = SKImage.FromBitmap(original);

        // Normalize camera orientation and discard source metadata before sending.
        var origin = codec.EncodedOrigin;
        var transpose = origin is SKEncodedOrigin.LeftTop or SKEncodedOrigin.RightTop or SKEncodedOrigin.RightBottom or SKEncodedOrigin.LeftBottom;
        var (orientedWidth, orientedHeight) = transpose ? (height, width) : (width, height);
        var orientation = origin switch
        {
            SKEncodedOrigin.TopRight => new SKMatrix(-1, 0, width, 0, 1, 0, 0, 0, 1),
            SKEncodedOrigin.BottomRight => new SKMatrix(-1, 0, width, 0, -1, height, 0, 0, 1),
            SKEncodedOrigin.BottomLeft => new SKMatrix(1, 0, 0, 0, -1, height, 0, 0, 1),
            SKEncodedOrigin.LeftTop => new SKMatrix(0, 1, 0, 1, 0, 0, 0, 0, 1),
            SKEncodedOrigin.RightTop => new SKMatrix(0, -1, height, 1, 0, 0, 0, 0, 1),
            SKEncodedOrigin.RightBottom => new SKMatrix(0, -1, height, -1, 0, width, 0, 0, 1),
            SKEncodedOrigin.LeftBottom => new SKMatrix(0, 1, 0, -1, 0, width, 0, 0, 1),
            _ => SKMatrix.Identity
        };

        foreach (var edge in new[] { 1536, 1024, 640 })
        {
            var scale = Math.Min(1f, (float)edge / Math.Max(orientedWidth, orientedHeight));
            var targetWidth = Math.Max(1, (int)Math.Round(orientedWidth * scale));
            var targetHeight = Math.Max(1, (int)Math.Round(orientedHeight * scale));
            using var target = new SKBitmap(new SKImageInfo(targetWidth, targetHeight, info.ColorType, info.AlphaType, srgb));
            using var canvas = new SKCanvas(target);
            canvas.Clear(SKColors.Transparent);
            canvas.Scale((float)targetWidth / orientedWidth, (float)targetHeight / orientedHeight);
            canvas.Concat(orientation);
            canvas.DrawImage(raster, 0, 0, new SKSamplingOptions(SKFilterMode.Linear));
            using var encoded = target.Encode(codec.EncodedFormat, 90);
            if (encoded is not null && encoded.Size <= AgentImage.MaximumBytes)
                return new AgentImage(encoded.AsSpan());
        }
        throw new InvalidDataException("The image could not be prepared for chat.");
    }
}
