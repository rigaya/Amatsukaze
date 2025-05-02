using System;
using System.IO;

namespace Amatsukaze.Lib
{
    /// <summary>
    /// 基本的なビットマップファクトリの実装
    /// 実際の機能はプラットフォーム固有の実装が担当
    /// </summary>
    public class DefaultBitmapFactory : IBitmapFactory
    {
        /// <summary>
        /// バイト配列からビットマップを作成します
        /// </summary>
        public object CreateBitmapFromByteArray(byte[] buffer)
        {
            // この実装は単にバイト配列を保持するだけ
            return buffer;
        }

        /// <summary>
        /// ストリームからビットマップを作成します
        /// </summary>
        public object CreateBitmapFromStream(Stream stream)
        {
            using (var memoryStream = new MemoryStream())
            {
                stream.CopyTo(memoryStream);
                return memoryStream.ToArray();
            }
        }

        /// <summary>
        /// ファイルからビットマップを作成します
        /// </summary>
        public object CreateBitmapFromFile(string filePath)
        {
            return File.ReadAllBytes(filePath);
        }

        /// <summary>
        /// RGBバッファからビットマップを作成します
        /// </summary>
        public object CreateBitmapFromRgb(byte[] buffer, int width, int height, int stride)
        {
            // 単純にバッファを返す
            return buffer;
        }

        /// <summary>
        /// ビットマップをJPEGとしてファイルに保存します
        /// </summary>
        public void SaveBitmapAsJpeg(object bitmap, string filePath)
        {
            var buffer = bitmap as byte[];
            if (buffer == null)
                throw new ArgumentException("無効なビットマップオブジェクトです", nameof(bitmap));

            File.WriteAllBytes(filePath, buffer);
        }

        /// <summary>
        /// ビットマップをPNGとしてファイルに保存します
        /// </summary>
        public void SaveBitmapAsPng(object bitmap, string filePath)
        {
            var buffer = bitmap as byte[];
            if (buffer == null)
                throw new ArgumentException("無効なビットマップオブジェクトです", nameof(bitmap));

            File.WriteAllBytes(filePath, buffer);
        }

        /// <summary>
        /// ビットマップをJPEGとしてストリームに保存します
        /// </summary>
        public void SaveBitmapAsJpegToStream(object bitmap, Stream stream)
        {
            var buffer = bitmap as byte[];
            if (buffer == null)
                throw new ArgumentException("無効なビットマップオブジェクトです", nameof(bitmap));

            stream.Write(buffer, 0, buffer.Length);
        }

        /// <summary>
        /// ビットマップをPNGとしてストリームに保存します
        /// </summary>
        public void SaveBitmapAsPngToStream(object bitmap, Stream stream)
        {
            var buffer = bitmap as byte[];
            if (buffer == null)
                throw new ArgumentException("無効なビットマップオブジェクトです", nameof(bitmap));

            stream.Write(buffer, 0, buffer.Length);
        }
    }
} 