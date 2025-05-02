using System;
using System.IO;
using System.Windows.Media.Imaging;
using Amatsukaze.Lib;

namespace Amatsukaze.Win
{
    /// <summary>
    /// WPFを使用したビットマップファクトリの実装
    /// </summary>
    public class WpfBitmapFactory : IBitmapFactory
    {
        /// <summary>
        /// バイト配列からBitmapSourceを作成します
        /// </summary>
        public object CreateBitmapFromByteArray(byte[] buffer)
        {
            var stream = new MemoryStream(buffer);
            var decoder = BitmapDecoder.Create(stream, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
            return decoder.Frames[0];
        }

        /// <summary>
        /// ストリームからBitmapSourceを作成します
        /// </summary>
        public object CreateBitmapFromStream(Stream stream)
        {
            var decoder = BitmapDecoder.Create(stream, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
            return decoder.Frames[0];
        }

        /// <summary>
        /// ファイルからBitmapSourceを作成します
        /// </summary>
        public object CreateBitmapFromFile(string filePath)
        {
            var uri = new Uri(filePath, UriKind.RelativeOrAbsolute);
            return new BitmapImage(uri);
        }

        public object CreateBitmapFromRgb(byte[] buffer, int width, int height, int stride)
        {
            return BitmapSource.Create(width, height, 96, 96, System.Windows.Media.PixelFormats.Bgr24, null, buffer, stride);
        }

        /// <summary>
        /// BitmapSourceをJPEGとしてファイルに保存します
        /// </summary>
        public void SaveBitmapAsJpeg(object bitmap, string filePath)
        {
            var bitmapSource = bitmap as BitmapSource;
            if (bitmapSource == null)
                throw new ArgumentException("無効なビットマップオブジェクトです", nameof(bitmap));

            var encoder = new JpegBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(bitmapSource));

            using (var stream = new FileStream(filePath, FileMode.Create))
            {
                encoder.Save(stream);
            }
        }

        /// <summary>
        /// BitmapSourceをPNGとしてファイルに保存します
        /// </summary>
        public void SaveBitmapAsPng(object bitmap, string filePath)
        {
            var bitmapSource = bitmap as BitmapSource;
            if (bitmapSource == null)
                throw new ArgumentException("無効なビットマップオブジェクトです", nameof(bitmap));

            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(bitmapSource));

            using (var stream = new FileStream(filePath, FileMode.Create))
            {
                encoder.Save(stream);
            }
        }

        /// <summary>
        /// BitmapSourceをJPEGとしてストリームに保存します
        /// </summary>
        public void SaveBitmapAsJpegToStream(object bitmap, Stream stream)
        {
            var bitmapSource = bitmap as BitmapSource;
            if (bitmapSource == null)
                throw new ArgumentException("無効なビットマップオブジェクトです", nameof(bitmap));

            var encoder = new JpegBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(bitmapSource));
            encoder.Save(stream);
        }

        /// <summary>
        /// BitmapSourceをPNGとしてストリームに保存します
        /// </summary>
        public void SaveBitmapAsPngToStream(object bitmap, Stream stream)
        {
            var bitmapSource = bitmap as BitmapSource;
            if (bitmapSource == null)
                throw new ArgumentException("無効なビットマップオブジェクトです", nameof(bitmap));

            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(bitmapSource));
            encoder.Save(stream);
        }
    }
} 