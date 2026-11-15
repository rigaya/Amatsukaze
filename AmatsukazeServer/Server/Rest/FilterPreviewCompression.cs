using System.IO;
using System.IO.Compression;
using ZstdSharp;

namespace Amatsukaze.Server.Rest
{
    public static class FilterPreviewCompression
    {
        public static byte[] Zstd(byte[] input)
        {
            using var compressor = new Compressor(1);
            return compressor.Wrap(input).ToArray();
        }

        public static byte[] Gzip(byte[] input)
        {
            using var result = new MemoryStream();
            using (var gzip = new GZipStream(result, CompressionLevel.Fastest, true)) gzip.Write(input);
            return result.ToArray();
        }
    }
}
