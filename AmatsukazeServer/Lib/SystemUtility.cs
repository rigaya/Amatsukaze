using System;
using System.IO;

namespace Amatsukaze.Lib
{
    /// <summary>
    /// プラットフォーム依存のシステム機能を提供するインターフェース
    /// Linux環境ではダミーの実装を使用します (DefaultSystemUtility)
    /// Windows環境ではWindowsSystemUtilityを使用します
    /// </summary>
    public static class SystemUtility
    {
        private static ISystemUtility _instance;

        /// <summary>
        /// プラットフォーム依存のシステム機能の実装を設定します
        /// </summary>
        /// <param name="implementation">システム機能の実装</param>
        public static void SetImplementation(ISystemUtility implementation)
        {
            _instance = implementation ?? throw new ArgumentNullException(nameof(implementation));
        }

        /// <summary>
        /// 現在のインスタンスを取得します
        /// </summary>
        /// <returns>システム機能の実装</returns>
        /// <exception cref="InvalidOperationException">実装が設定されていない場合</exception>
        private static ISystemUtility GetInstance()
        {
            if (_instance == null)
            {
                throw new InvalidOperationException("SystemUtility implementation has not been set");
            }
            return _instance;
        }

        /// <summary>
        /// 最後のユーザー入力からの経過時間を取得します
        /// </summary>
        public static TimeSpan GetLastInputTime()
        {
            return GetInstance().GetLastInputTime();
        }

        /// <summary>
        /// ウィンドウの配置情報を設定します
        /// </summary>
        public static bool SetWindowPlacement(IntPtr hWnd, ref WINDOWPLACEMENT lpwndpl)
        {
            return GetInstance().SetWindowPlacement(hWnd, ref lpwndpl);
        }

        /// <summary>
        /// ウィンドウの配置情報を取得します
        /// </summary>
        public static bool GetWindowPlacement(IntPtr hWnd, out WINDOWPLACEMENT lpwndpl)
        {
            return GetInstance().GetWindowPlacement(hWnd, out lpwndpl);
        }

        /// <summary>
        /// メールスロットを作成します
        /// </summary>
        public static FileStream CreateMailslot(string path)
        {
            return GetInstance().CreateMailslot(path);
        }

        /// <summary>
        /// メールスロットをテストします
        /// </summary>
        public static bool TestMailslot(string path)
        {
            return GetInstance().TestMailslot(path);
        }

        /// <summary>
        /// シャットダウン権限のトークン調整を行います
        /// </summary>
        public static void AdjustToken()
        {
            GetInstance().AdjustToken();
        }

        /// <summary>
        /// Windowsをシャットダウン、再起動、ログオフなどの操作を行います
        /// </summary>
        public static bool ExitWindowsEx(ExitWindows uFlags, int dwReason)
        {
            return GetInstance().ExitWindowsEx(uFlags, dwReason);
        }

        /// <summary>
        /// スリープ/サスペンド/休止状態への移行を実行します
        /// </summary>
        public static void SetSuspendState(PowerState state, bool force, bool disableWakeEvent)
        {
            GetInstance().SetSuspendState(state, force, disableWakeEvent);
        }

        /// <summary>
        /// サスペンド抑止用のコンテキストを作成します
        /// </summary>
        public static IDisposable CreatePreventSuspendContext()
        {
            return GetInstance().CreatePreventSuspendContext();
        }

        /// <summary>
        /// スレッドハンドルを取得します
        /// </summary>
        public static IntPtr OpenThread(ThreadAccess dwDesiredAccess, bool bInheritHandle, uint dwThreadId)
        {
            return GetInstance().OpenThread(dwDesiredAccess, bInheritHandle, dwThreadId);
        }

        /// <summary>
        /// スレッドを一時停止します
        /// </summary>
        public static uint SuspendThread(IntPtr hThread)
        {
            return GetInstance().SuspendThread(hThread);
        }

        /// <summary>
        /// スレッドを再開します
        /// </summary>
        public static int ResumeThread(IntPtr hThread)
        {
            return GetInstance().ResumeThread(hThread);
        }

        /// <summary>
        /// ハンドルを閉じます
        /// </summary>
        public static void CloseHandle(IntPtr hObject)
        {
            GetInstance().CloseHandle(hObject);
        }
    }

    /// <summary>
    /// プラットフォーム依存のシステム機能を提供するインターフェース
    /// </summary>
    public interface ISystemUtility
    {
        /// <summary>
        /// 最後のユーザー入力からの経過時間を取得します
        /// </summary>
        TimeSpan GetLastInputTime();

        /// <summary>
        /// ウィンドウの配置情報を設定します
        /// </summary>
        bool SetWindowPlacement(IntPtr hWnd, ref WINDOWPLACEMENT lpwndpl);

        /// <summary>
        /// ウィンドウの配置情報を取得します
        /// </summary>
        bool GetWindowPlacement(IntPtr hWnd, out WINDOWPLACEMENT lpwndpl);

        /// <summary>
        /// メールスロットを作成します
        /// </summary>
        FileStream CreateMailslot(string path);

        /// <summary>
        /// メールスロットをテストします
        /// </summary>
        bool TestMailslot(string path);

        /// <summary>
        /// シャットダウン権限のトークン調整を行います
        /// </summary>
        void AdjustToken();

        /// <summary>
        /// Windowsをシャットダウン、再起動、ログオフなどの操作を行います
        /// </summary>
        bool ExitWindowsEx(ExitWindows uFlags, int dwReason);

        /// <summary>
        /// スリープ/サスペンド/休止状態への移行を実行します
        /// </summary>
        void SetSuspendState(PowerState state, bool force, bool disableWakeEvent);

        /// <summary>
        /// サスペンド抑止用のコンテキストを作成します
        /// </summary>
        IDisposable CreatePreventSuspendContext();

        /// <summary>
        /// スレッドハンドルを取得します
        /// </summary>
        IntPtr OpenThread(ThreadAccess dwDesiredAccess, bool bInheritHandle, uint dwThreadId);

        /// <summary>
        /// スレッドを一時停止します
        /// </summary>
        uint SuspendThread(IntPtr hThread);

        /// <summary>
        /// スレッドを再開します
        /// </summary>
        int ResumeThread(IntPtr hThread);

        /// <summary>
        /// スレッドハンドルを閉じます
        /// </summary>
        void CloseHandle(IntPtr hObject);
    }

    /// <summary>
    /// 電源状態を表す列挙型
    /// </summary>
    public enum PowerState
    {
        /// <summary>
        /// スリープ/サスペンド状態
        /// </summary>
        Suspend,

        /// <summary>
        /// 休止状態
        /// </summary>
        Hibernate
    }
} 