using System;
using System.IO;

namespace Amatsukaze.Lib
{
    /// <summary>
    /// ビットマップ操作のためのマネージャークラス
    /// </summary>
    public static class BitmapManager
    {
        private static IBitmapFactory _factory = new DefaultBitmapFactory();

        /// <summary>
        /// ビットマップファクトリーを設定します
        /// </summary>
        /// <param name="factory">使用するファクトリーインスタンス</param>
        public static void SetBitmapFactory(IBitmapFactory factory)
        {
            _factory = factory ?? throw new ArgumentNullException(nameof(factory));
        }

        /// <summary>
        /// 現在のビットマップファクトリーを取得します
        /// </summary>
        public static IBitmapFactory Factory => _factory;

        /// <summary>
        /// バイト配列からビットマップを作成します
        /// </summary>
        public static object CreateBitmapFromByteArray(byte[] buffer)
        {
            return _factory.CreateBitmapFromByteArray(buffer);
        }

        /// <summary>
        /// ストリームからビットマップを作成します
        /// </summary>
        public static object CreateBitmapFromStream(Stream stream)
        {
            return _factory.CreateBitmapFromStream(stream);
        }

        /// <summary>
        /// ファイルからビットマップを作成します
        /// </summary>
        public static object CreateBitmapFromFile(string filePath)
        {
            return _factory.CreateBitmapFromFile(filePath);
        }

        /// <summary>
        /// RGBバッファからビットマップを作成します
        /// </summary>
        public static object CreateBitmapFromRgb(byte[] buffer, int width, int height, int stride)
        {
            return _factory.CreateBitmapFromRgb(buffer, width, height, stride);
        }

        /// <summary>
        /// ビットマップをJPEGとしてファイルに保存します
        /// </summary>
        public static void SaveBitmapAsJpeg(object bitmap, string filePath)
        {
            _factory.SaveBitmapAsJpeg(bitmap, filePath);
        }

        /// <summary>
        /// ビットマップをPNGとしてファイルに保存します
        /// </summary>
        public static void SaveBitmapAsPng(object bitmap, string filePath)
        {
            _factory.SaveBitmapAsPng(bitmap, filePath);
        }

        /// <summary>
        /// ビットマップをJPEGとしてストリームに保存します
        /// </summary>
        public static void SaveBitmapAsJpeg(object bitmap, Stream stream)
        {
            _factory.SaveBitmapAsJpegToStream(bitmap, stream);
        }

        /// <summary>
        /// ビットマップをPNGとしてストリームに保存します
        /// </summary>
        public static void SaveBitmapAsPng(object bitmap, Stream stream)
        {
            _factory.SaveBitmapAsPngToStream(bitmap, stream);
        }
    }
} 