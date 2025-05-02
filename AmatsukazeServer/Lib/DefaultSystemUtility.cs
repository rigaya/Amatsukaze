using System;
using System.IO;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace Amatsukaze.Lib
{
    /// <summary>
    /// Linuxなど非Windows環境向けのシステムユーティリティのデフォルト実装
    /// </summary>
    public class DefaultSystemUtility : ISystemUtility
    {
        /// <summary>
        /// 最後のユーザー入力からの経過時間を取得します
        /// </summary>
        public TimeSpan GetLastInputTime()
        {
            // Linux環境では実装されていないのでTimeSpan.Zeroを返す
            return TimeSpan.Zero;
        }

        /// <summary>
        /// ウィンドウの配置情報を設定します
        /// </summary>
        public bool SetWindowPlacement(IntPtr hWnd, ref WINDOWPLACEMENT lpwndpl)
        {
            // Linux環境では実装されていないのでfalseを返す
            return false;
        }

        /// <summary>
        /// ウィンドウの配置情報を取得します
        /// </summary>
        public bool GetWindowPlacement(IntPtr hWnd, out WINDOWPLACEMENT lpwndpl)
        {
            // Linux環境では実装されていないので初期値を設定してfalseを返す
            lpwndpl = new WINDOWPLACEMENT();
            return false;
        }

        /// <summary>
        /// メールスロットを作成します
        /// </summary>
        public FileStream CreateMailslot(string path)
        {
            // Linux環境では実装されていないのでnullを返す
            return null;
        }

        /// <summary>
        /// メールスロットをテストします
        /// </summary>
        public bool TestMailslot(string path)
        {
            // Linux環境では実装されていないのでfalseを返す
            return false;
        }

        /// <summary>
        /// ファイルを作成します
        /// </summary>
        public IntPtr CreateFile(string fileName, FileDesiredAccess desiredAccess, FileShareMode shareMode, IntPtr securityAttributes, FileCreationDisposition creationDisposition, int flagsAndAttributes, IntPtr hTemplateFile)
        {
            // Linux環境では実装されていないのでnullを返す
            return IntPtr.Zero;
        }

        /// <summary>
        /// シャットダウン権限のトークン調整を行います
        /// </summary>
        public void AdjustToken()
        {
            // Linux環境では何もしない
        }

        /// <summary>
        /// Windowsをシャットダウン、再起動、ログオフなどの操作を行います
        /// </summary>
        public bool ExitWindowsExNative(ExitWindows uFlags, int dwReason)
        {
            // Linux環境では実装されていないのでfalseを返す
            return false;
        }

        /// <summary>
        /// スリープ/サスペンド/休止状態への移行を実行します
        /// </summary>
        public void SetSuspendState(PowerState state, bool force, bool disableWakeEvent)
        {
            // Linux環境では何もしない
        }

        /// <summary>   
        /// スレッドハンドルを取得します
        /// </summary>
        public IntPtr OpenThreadNative(ThreadAccess dwDesiredAccess, bool bInheritHandle, uint dwThreadId)
        {
            return IntPtr.Zero;
        }

        /// <summary>
        /// スレッドを一時停止します
        /// </summary>
        public uint SuspendThreadNative(IntPtr hThread)
        {
            return 0;
        }

        /// <summary>
        /// スレッドを再開します
        /// </summary>
        public int ResumeThreadNative(IntPtr hThread)
        {
            return 0;
        }

        /// <summary>
        /// ハンドルを閉じます
        /// </summary>
        public void CloseHandleNative(IntPtr hObject)
        {
            // Linux環境では何もしない
        }

        /// <summary>
        /// サスペンド抑止用のコンテキストを作成します
        /// </summary>
        public IDisposable CreatePreventSuspendContext()
        {
            // Linux環境ではダミーのDisposableを返す
            return new DummyPreventSuspendContext();
        }

        /// <summary>
        /// Linux環境向けのダミーのPreventSuspendContextの実装
        /// </summary>
        private class DummyPreventSuspendContext : IDisposable
        {
            public void Dispose()
            {
                // 何もしない
            }
        }
    }
} 