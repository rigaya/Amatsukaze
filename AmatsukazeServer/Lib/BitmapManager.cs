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
        private static bool _initialized = false;
        private static readonly object _lock = new object();

        /// <summary>
        /// 静的コンストラクタでBitmapFactoryのセットアップを行う
        /// </summary>
        static BitmapManager()
        {
            // 初期化は初回アクセス時に行われる
        }

        /// <summary>
        /// BitmapFactoryの初期化を行います
        /// </summary>
        private static void Initialize()
        {
            if (_initialized)
                return;

            lock (_lock)
            {
                if (_initialized)
                    return;

                // Windows環境での初期化
                if (Environment.OSVersion.Platform == PlatformID.Win32NT)
                {
                    // Windows専用の実装をセットアップ
                    try
                    {
                        // WpfBitmapFactoryはWindowsでのみ利用可能なクラス
                        // リフレクションで探して、存在すれば初期化する
                        var wpfBitmapFactoryType = Type.GetType("Amatsukaze.Win.WpfBitmapFactory, AmatsukazeServerWin");

                        if (wpfBitmapFactoryType != null)
                        {
                            var bitmapFactory = Activator.CreateInstance(wpfBitmapFactoryType);
                            _factory = (IBitmapFactory)bitmapFactory;
                        }
                        else
                        {
                            _factory = new DefaultBitmapFactory();
                        }
                    }
                    catch (Exception)
                    {
                        // 初期化に失敗したらデフォルト実装を使用
                        _factory = new DefaultBitmapFactory();
                    }
                }
                else
                {
                    // Windows以外の環境ではデフォルト実装を使用
                    _factory = new DefaultBitmapFactory();
                }

                _initialized = true;
            }
        }

        /// <summary>
        /// ビットマップファクトリーを設定します
        /// </summary>
        /// <param name="factory">使用するファクトリーインスタンス</param>
        public static void SetBitmapFactory(IBitmapFactory factory)
        {
            _factory = factory ?? throw new ArgumentNullException(nameof(factory));
            _initialized = true;
        }

        /// <summary>
        /// 現在のビットマップファクトリーを取得します
        /// </summary>
        public static IBitmapFactory Factory
        {
            get
            {
                if (!_initialized)
                {
                    Initialize();
                }
                return _factory;
            }
        }

        /// <summary>
        /// バイト配列からビットマップを作成します
        /// </summary>
        public static object CreateBitmapFromByteArray(byte[] buffer)
        {
            if (!_initialized)
            {
                Initialize();
            }
            return _factory.CreateBitmapFromByteArray(buffer);
        }

        /// <summary>
        /// ストリームからビットマップを作成します
        /// </summary>
        public static object CreateBitmapFromStream(Stream stream)
        {
            if (!_initialized)
            {
                Initialize();
            }
            return _factory.CreateBitmapFromStream(stream);
        }

        /// <summary>
        /// ファイルからビットマップを作成します
        /// </summary>
        public static object CreateBitmapFromFile(string filePath)
        {
            if (!_initialized)
            {
                Initialize();
            }
            return _factory.CreateBitmapFromFile(filePath);
        }

        /// <summary>
        /// RGBバッファからビットマップを作成します
        /// </summary>
        public static object CreateBitmapFromRgb(byte[] buffer, int width, int height, int stride)
        {
            if (!_initialized)
            {
                Initialize();
            }
            return _factory.CreateBitmapFromRgb(buffer, width, height, stride);
        }

        /// <summary>
        /// ビットマップをJPEGとしてファイルに保存します
        /// </summary>
        public static void SaveBitmapAsJpeg(object bitmap, string filePath)
        {
            if (!_initialized)
            {
                Initialize();
            }
            _factory.SaveBitmapAsJpeg(bitmap, filePath);
        }

        /// <summary>
        /// ビットマップをPNGとしてファイルに保存します
        /// </summary>
        public static void SaveBitmapAsPng(object bitmap, string filePath)
        {
            if (!_initialized)
            {
                Initialize();
            }
            _factory.SaveBitmapAsPng(bitmap, filePath);
        }

        /// <summary>
        /// ビットマップをJPEGとしてストリームに保存します
        /// </summary>
        public static void SaveBitmapAsJpeg(object bitmap, Stream stream)
        {
            if (!_initialized)
            {
                Initialize();
            }
            _factory.SaveBitmapAsJpegToStream(bitmap, stream);
        }

        /// <summary>
        /// ビットマップをPNGとしてストリームに保存します
        /// </summary>
        public static void SaveBitmapAsPng(object bitmap, Stream stream)
        {
            if (!_initialized)
            {
                Initialize();
            }
            _factory.SaveBitmapAsPngToStream(bitmap, stream);
        }
    }
} 