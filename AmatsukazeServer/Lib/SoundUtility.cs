using System;
using System.Runtime.InteropServices;

namespace Amatsukaze.Lib
{
    /// <summary>
    /// サウンド再生のためのユーティリティクラス
    /// </summary>
    public static class SoundUtility
    {
        /// <summary>
        /// WAVファイルを再生します
        /// </summary>
        /// <param name="filePath">再生するWAVファイルのパス</param>
        public static void PlaySound(string filePath)
        {
            // Windowsの場合はWin32 APIを使用
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                try
                {
                    // Windows上ではSystem.Media.SoundPlayerを動的に使用
                    var assembly = typeof(string).Assembly.GetType("System.Media.SoundPlayer");
                    if (assembly != null)
                    {
                        var player = Activator.CreateInstance(assembly, filePath);
                        var method = assembly.GetMethod("Play");
                        method.Invoke(player, null);
                    }
                }
                catch
                {
                    // 再生に失敗しても処理は続行
                }
            }
            else
            {
                // Linux/macOSの場合は何もしない
                // 将来的にLinux向けサウンド再生を実装する場合はここに追加
            }
        }
    }
} 