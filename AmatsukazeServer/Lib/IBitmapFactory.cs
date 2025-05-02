using System;
using System.IO;

namespace Amatsukaze.Lib
{
    /// <summary>
    /// 画像操作のための抽象インターフェース
    /// </summary>
    public interface IBitmapFactory
    {
        /// <summary>
        /// バイト配列からビットマップを作成します
        /// </summary>
        /// <param name="buffer">画像データのバイト配列</param>
        /// <returns>作成されたビットマップオブジェクト</returns>
        object CreateBitmapFromByteArray(byte[] buffer);

        /// <summary>
        /// ストリームからビットマップを作成します
        /// </summary>
        /// <param name="stream">画像データのストリーム</param>
        /// <returns>作成されたビットマップオブジェクト</returns>
        object CreateBitmapFromStream(Stream stream);

        /// <summary>
        /// ファイルからビットマップを作成します
        /// </summary>
        /// <param name="filePath">画像ファイルのパス</param>
        /// <returns>作成されたビットマップオブジェクト</returns>
        object CreateBitmapFromFile(string filePath);

        /// <summary>
        /// RGBバッファからビットマップを作成します
        /// </summary>
        /// <param name="buffer">RGB画像データのバイト配列</param>
        /// <param name="width">画像の幅</param>
        /// <param name="height">画像の高さ</param>
        /// <param name="stride">1行あたりのバイト数</param>
        /// <returns>作成されたビットマップオブジェクト</returns>
        object CreateBitmapFromRgb(byte[] buffer, int width, int height, int stride);

        /// <summary>
        /// ビットマップをJPEGとしてファイルに保存します
        /// </summary>
        /// <param name="bitmap">保存するビットマップオブジェクト</param>
        /// <param name="filePath">保存先ファイルパス</param>
        void SaveBitmapAsJpeg(object bitmap, string filePath);

        /// <summary>
        /// ビットマップをPNGとしてファイルに保存します
        /// </summary>
        /// <param name="bitmap">保存するビットマップオブジェクト</param>
        /// <param name="filePath">保存先ファイルパス</param>
        void SaveBitmapAsPng(object bitmap, string filePath);

        /// <summary>
        /// ビットマップをJPEGとしてストリームに保存します
        /// </summary>
        /// <param name="bitmap">保存するビットマップオブジェクト</param>
        /// <param name="stream">保存先ストリーム</param>
        void SaveBitmapAsJpegToStream(object bitmap, Stream stream);

        /// <summary>
        /// ビットマップをPNGとしてストリームに保存します
        /// </summary>
        /// <param name="bitmap">保存するビットマップオブジェクト</param>
        /// <param name="stream">保存先ストリーム</param>
        void SaveBitmapAsPngToStream(object bitmap, Stream stream);
    }
} 