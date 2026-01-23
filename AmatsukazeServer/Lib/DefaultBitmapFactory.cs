using System;
using System.IO;
using SixLabors.ImageSharp;
using SixLabors.ImageSharp.PixelFormats;
using SixLabors.ImageSharp.Formats.Jpeg;
using SixLabors.ImageSharp.Formats.Png;

namespace Amatsukaze.Lib
{
    /// <summary>
    /// ImageSharpを使用したビットマップファクトリの実装
    /// Linuxでも使用可能な画像処理を提供します
    /// </summary>
    public class DefaultBitmapFactory : IBitmapFactory
    {
        /// <summary>
        /// 内部で使用するImage型のラッパークラス
        /// 画像データと関連メタデータを保持します
        /// </summary>
        public class ImageWrapper
        {
            public Image<Rgb24> Image { get; }
            public int Width => Image.Width;
            public int Height => Image.Height;

            public ImageWrapper(Image<Rgb24> image)
            {
                Image = image;
            }
        }

        /// <summary>
        /// バイト配列からビットマップを作成します
        /// </summary>
        public object CreateBitmapFromByteArray(byte[] buffer)
        {
            using (var memoryStream = new MemoryStream(buffer))
            {
                return new ImageWrapper(Image.Load<Rgb24>(memoryStream));
            }
        }

        /// <summary>
        /// ストリームからビットマップを作成します
        /// </summary>
        public object CreateBitmapFromStream(Stream stream)
        {
            return new ImageWrapper(Image.Load<Rgb24>(stream));
        }

        /// <summary>
        /// ファイルからビットマップを作成します
        /// </summary>
        public object CreateBitmapFromFile(string filePath)
        {
            return new ImageWrapper(Image.Load<Rgb24>(filePath));
        }

        /// <summary>
        /// RGBバッファからビットマップを作成します
        /// </summary>
        public object CreateBitmapFromRgb(byte[] buffer, int width, int height, int stride)
        {
            var image = new Image<Rgb24>(width, height);
            
            // RGBバッファから画像を作成
            image.ProcessPixelRows(accessor => 
            {
                for (int y = 0; y < height; y++)
                {
                    int rowOffset = y * stride;
                    var pixelRowSpan = accessor.GetRowSpan(y);
                    
                    for (int x = 0; x < width; x++)
                    {
                        int pixelOffset = rowOffset + x * 3;
                        pixelRowSpan[x] = new Rgb24(
                            buffer[pixelOffset + 2], // R (BGR24 -> RGB)
                            buffer[pixelOffset + 1], // G
                            buffer[pixelOffset]      // B
                        );
                    }
                }
            });
            
            return new ImageWrapper(image);
        }

        /// <summary>
        /// ビットマップをJPEGとしてファイルに保存します
        /// </summary>
        public void SaveBitmapAsJpeg(object bitmap, string filePath)
        {
            var wrapper = ValidateAndGetImageWrapper(bitmap);
            wrapper.Image.Save(filePath, new JpegEncoder { Quality = 90 });
        }

        /// <summary>
        /// ビットマップをPNGとしてファイルに保存します
        /// </summary>
        public void SaveBitmapAsPng(object bitmap, string filePath)
        {
            var wrapper = ValidateAndGetImageWrapper(bitmap);
            wrapper.Image.Save(filePath, new PngEncoder());
        }

        /// <summary>
        /// ビットマップをJPEGとしてストリームに保存します
        /// </summary>
        public void SaveBitmapAsJpegToStream(object bitmap, Stream stream)
        {
            var wrapper = ValidateAndGetImageWrapper(bitmap);
            wrapper.Image.Save(stream, new JpegEncoder { Quality = 90 });
        }

        /// <summary>
        /// ビットマップをPNGとしてストリームに保存します
        /// </summary>
        public void SaveBitmapAsPngToStream(object bitmap, Stream stream)
        {
            var wrapper = ValidateAndGetImageWrapper(bitmap);
            wrapper.Image.Save(stream, new PngEncoder());
        }

        /// <summary>
        /// 渡されたオブジェクトが正しいImageWrapperかチェックして取得します
        /// </summary>
        private ImageWrapper ValidateAndGetImageWrapper(object bitmap)
        {
            var wrapper = bitmap as ImageWrapper;
            if (wrapper == null)
                throw new ArgumentException("無効なビットマップオブジェクトです", nameof(bitmap));
            
            return wrapper;
        }
    }
} 
