using System;
using System.IO;
using Amatsukaze.Server;

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
        private static bool _initialized = false;
        private static readonly object _lock = new object();

        /// <summary>
        /// 静的コンストラクタでシステム機能初期化のセットアップを行う
        /// </summary>
        static SystemUtility()
        {
            // 初期化は初回アクセス時に行われる
        }

        /// <summary>
        /// システム機能の初期化を行います
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
                if (Util.IsServerWindows())
                {
                    // Windows専用の実装をセットアップ
                    try
                    {
                        // WindowsSystemUtilityはWindowsでのみ利用可能なクラス
                        // リフレクションで探して、存在すれば初期化する
                        var windowsSystemUtilityType = Type.GetType("Amatsukaze.Win.WindowsSystemUtility, AmatsukazeServerWin");

                        if (windowsSystemUtilityType != null)
                        {
                            var systemUtility = Activator.CreateInstance(windowsSystemUtilityType);
                            _instance = (ISystemUtility)systemUtility;
                        }
                        else
                        {
                            _instance = new DefaultSystemUtility();
                        }
                    }
                    catch (Exception)
                    {
                        // 初期化に失敗したらデフォルト実装を使用
                        _instance = new DefaultSystemUtility();
                    }
                }
                else
                {
                    // Windows以外の環境ではデフォルト実装を使用
                    _instance = new DefaultSystemUtility();
                }

                _initialized = true;
            }
        }

        /// <summary>
        /// プラットフォーム依存のシステム機能の実装を設定します
        /// </summary>
        /// <param name="implementation">システム機能の実装</param>
        public static void SetImplementation(ISystemUtility implementation)
        {
            _instance = implementation ?? throw new ArgumentNullException(nameof(implementation));
            _initialized = true;
        }

        /// <summary>
        /// 現在のインスタンスを取得します
        /// </summary>
        /// <returns>システム機能の実装</returns>
        /// <exception cref="InvalidOperationException">実装が設定されていない場合</exception>
        private static ISystemUtility GetInstance()
        {
            if (!_initialized)
            {
                Initialize();
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
        public static bool ExitWindowsExNative(ExitWindows uFlags, int dwReason)
        {
            return GetInstance().ExitWindowsExNative(uFlags, dwReason);
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
        public static IntPtr OpenThreadNative(ThreadAccess dwDesiredAccess, bool bInheritHandle, uint dwThreadId)
        {
            return GetInstance().OpenThreadNative(dwDesiredAccess, bInheritHandle, dwThreadId);
        }

        /// <summary>
        /// スレッドを一時停止します
        /// </summary>
        public static uint SuspendThreadNative(IntPtr hThread)
        {
            return GetInstance().SuspendThreadNative(hThread);
        }

        /// <summary>
        /// スレッドを再開します
        /// </summary>
        public static int ResumeThreadNative(IntPtr hThread)
        {
            return GetInstance().ResumeThreadNative(hThread);
        }

        /// <summary>
        /// ハンドルを閉じます
        /// </summary>
        public static void CloseHandleNative(IntPtr hObject)
        {
            GetInstance().CloseHandleNative(hObject);
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
        bool ExitWindowsExNative(ExitWindows uFlags, int dwReason);

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
        IntPtr OpenThreadNative(ThreadAccess dwDesiredAccess, bool bInheritHandle, uint dwThreadId);

        /// <summary>
        /// スレッドを一時停止します
        /// </summary>
        uint SuspendThreadNative(IntPtr hThread);

        /// <summary>
        /// スレッドを再開します
        /// </summary>
        int ResumeThreadNative(IntPtr hThread);

        /// <summary>
        /// スレッドハンドルを閉じます
        /// </summary>
        void CloseHandleNative(IntPtr hObject);
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