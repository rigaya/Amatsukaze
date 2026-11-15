using System.IO;
using System.IO.Compression;
using Amatsukaze.Server.Rest;
using Xunit;
using ZstdSharp;

namespace AmatsukazeServerTest;

public class FilterPreviewCompressionTests
{
    [Fact]
    public void 圧縮したYUVをそれぞれ元に戻せる()
    {
        var input = new byte[1440 * 1080 * 3 / 2];
        for (var i = 0; i < input.Length; i++) input[i] = (byte)(i * 13 + i / 997);
        using var decoder = new Decompressor();
        Assert.Equal(input, decoder.Unwrap(FilterPreviewCompression.Zstd(input)).ToArray());
        using var stream = new GZipStream(new MemoryStream(FilterPreviewCompression.Gzip(input)), CompressionMode.Decompress);
        using var result = new MemoryStream();
        stream.CopyTo(result);
        Assert.Equal(input, result.ToArray());
    }
}
