using Amatsukaze.Lib;

namespace Amatsukaze.Win
{
    /// <summary>
    /// Windows環境でのBitmapFactory初期化クラス
    /// </summary>
    public static class BitmapFactoryInitializer
    {
        /// <summary>
        /// Windows用のBitmapFactoryを初期化します
        /// </summary>
        public static void Initialize()
        {
            BitmapManager.SetBitmapFactory(new WpfBitmapFactory());
        }
    }
} 