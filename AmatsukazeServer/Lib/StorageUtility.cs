using System;
using System.IO;
using System.Runtime.InteropServices;

namespace Amatsukaze.Lib
{
    /// <summary>
    /// ストレージ操作に関するユーティリティクラス
    /// </summary>
    public static class StorageUtility
    {
        /// <summary>
        /// 指定したディレクトリが存在するドライブの空き容量を取得します
        /// </summary>
        /// <param name="directoryName">対象のディレクトリパス</param>
        /// <param name="freeBytesAvailable">利用可能な空きバイト数</param>
        /// <param name="totalNumberOfBytes">ディスクの合計サイズ（バイト）</param>
        /// <param name="totalNumberOfFreeBytes">ディスクの空き容量の合計（バイト）</param>
        /// <returns>成功した場合はtrue、失敗した場合はfalse</returns>
        public static bool GetDiskFreeSpace(string directoryName, 
            out ulong freeBytesAvailable, 
            out ulong totalNumberOfBytes, 
            out ulong totalNumberOfFreeBytes)
        {
            // Windowsの場合はWin32 APIを使用
            //if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            //{
            //    return WindowsGetDiskFreeSpace(directoryName, 
            //        out freeBytesAvailable, 
            //        out totalNumberOfBytes, 
            //        out totalNumberOfFreeBytes);
            //}
            
            // Linux/macOSの場合は.NET Coreの機能を使用
            try
            {
                var driveInfo = new DriveInfo(Path.GetPathRoot(directoryName));
                freeBytesAvailable = (ulong)driveInfo.AvailableFreeSpace;
                totalNumberOfBytes = (ulong)driveInfo.TotalSize;
                totalNumberOfFreeBytes = (ulong)driveInfo.TotalFreeSpace;
                return true;
            }
            catch
            {
                freeBytesAvailable = 0;
                totalNumberOfBytes = 0;
                totalNumberOfFreeBytes = 0;
                return false;
            }
        }

        /// <summary>
        /// Windows環境での空き容量取得処理
        /// </summary>
        //[return: MarshalAs(UnmanagedType.Bool)]
        //[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        //private static extern bool WindowsGetDiskFreeSpace(
        //    string lpDirectoryName,
        //    out ulong lpFreeBytesAvailable,
        //    out ulong lpTotalNumberOfBytes,
        //    out ulong lpTotalNumberOfFreeBytes);

        /// <summary>
        /// ファイルを移動します（必要に応じて上書き）
        /// </summary>
        /// <param name="sourceFileName">移動元ファイルパス</param>
        /// <param name="destinationFileName">移動先ファイルパス</param>
        public static void MoveFileWithOverwrite(string sourceFileName, string destinationFileName)
        {
            //// Windows環境ではMoveFileExを使用
            //if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            //{
            //    const int MOVEFILE_REPLACE_EXISTING = 0x1;
            //    WindowsMoveFileEx(sourceFileName, destinationFileName, MOVEFILE_REPLACE_EXISTING);
            //    return;
            //}

            // Linux/macOSでは標準のFile.Moveを使用（上書き処理を追加）
            if (File.Exists(destinationFileName))
            {
                File.Delete(destinationFileName);
            }
            File.Move(sourceFileName, destinationFileName);
        }

        //[return: MarshalAs(UnmanagedType.Bool)]
        //[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        //private static extern bool WindowsMoveFileEx(
        //    string lpExistingFileName,
        //    string lpNewFileName,
        //    int dwFlags);
    }
} 