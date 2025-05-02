using System;
using System.IO;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using Amatsukaze.Lib;

namespace Amatsukaze.Win
{
    /// <summary>
    /// Windows環境向けのシステムユーティリティ実装
    /// </summary>
    public class WindowsSystemUtility : ISystemUtility
    {
        #region P/Invoke宣言

        [DllImport("User32.dll")]
        private static extern bool GetLastInputInfo(ref LASTINPUTINFO plii);
        
        [DllImport("Kernel32.dll")]
        private static extern uint GetTickCount();

        [DllImport("user32.dll")]
        private static extern bool SetWindowPlacement(
            IntPtr hWnd,
            [In] ref WINDOWPLACEMENT lpwndpl);

        [DllImport("user32.dll")]
        private static extern bool GetWindowPlacement(
            IntPtr hWnd,
            out WINDOWPLACEMENT lpwndpl);

        [DllImport("kernel32.dll", CharSet = CharSet.Auto, SetLastError = true)]
        private static extern SafeFileHandle CreateMailslot(string mailslotName,
                    uint nMaxMessageSize, int lReadTimeout, IntPtr securityAttributes);

        [DllImport("kernel32.dll", CharSet = CharSet.Auto, SetLastError = true)]
        private static extern SafeFileHandle CreateFile(string fileName,
                               FileDesiredAccess desiredAccess, FileShareMode shareMode,
                               IntPtr securityAttributes,
                               FileCreationDisposition creationDisposition,
                               int flagsAndAttributes, IntPtr hTemplateFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr GetCurrentProcess();

        [DllImport("advapi32.dll", SetLastError = true)]
        private static extern bool OpenProcessToken(IntPtr ProcessHandle,
            uint DesiredAccess,
            out IntPtr TokenHandle);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr hObject);

        [DllImport("advapi32.dll", SetLastError = true,
            CharSet = System.Runtime.InteropServices.CharSet.Auto)]
        private static extern bool LookupPrivilegeValue(string lpSystemName,
            string lpName,
            out long lpLuid);
        

        [StructLayout(LayoutKind.Sequential, Pack = 1)]
        private struct TOKEN_PRIVILEGES
        {
            public int PrivilegeCount;
            public long Luid;
            public int Attributes;
        }

        [DllImport("advapi32.dll", SetLastError = true)]
        private static extern bool AdjustTokenPrivileges(IntPtr TokenHandle,
            bool DisableAllPrivileges,
            ref TOKEN_PRIVILEGES NewState,
            int BufferLength,
            IntPtr PreviousState,
            IntPtr ReturnLength);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool ExitWindowsEx(uint uFlags,
            int dwReason);

        [DllImport("kernel32.dll")]
        private static extern IntPtr OpenThread(ThreadAccess dwDesiredAccess, bool bInheritHandle, uint dwThreadId);
        
        [DllImport("kernel32.dll")]
        private static extern uint SuspendThread(IntPtr hThread);
        
        [DllImport("kernel32.dll")]
        private static extern int ResumeThread(IntPtr hThread);

        [DllImport("powrprof.dll", SetLastError = true)]
        private static extern bool SetSuspendState(bool hibernate, bool forceCritical, bool disableWakeEvent);

        #endregion

        private struct LASTINPUTINFO
        {
            public uint cbSize;
            public uint dwTime;
        }

        /// <summary>
        /// サスペンド抑止コンテキストを作成します
        /// </summary>
        public IDisposable CreatePreventSuspendContext()
        {
            return new PreventSuspendContext();
        }

        /// <summary>
        /// 最後のユーザー入力からの経過時間を取得します
        /// </summary>
        public TimeSpan GetLastInputTime()
        {
            LASTINPUTINFO lastInput = new LASTINPUTINFO();
            lastInput.cbSize = (uint)Marshal.SizeOf(lastInput);
            GetLastInputInfo(ref lastInput);
            return new TimeSpan(0, 0, 0, 0, (int)(GetTickCount() - lastInput.dwTime));
        }

        /// <summary>
        /// ウィンドウの配置情報を設定します
        /// </summary>
        bool ISystemUtility.SetWindowPlacement(IntPtr hWnd, ref WINDOWPLACEMENT placement)
        {
            return SetWindowPlacement(hWnd, ref placement);
        }

        /// <summary>
        /// ウィンドウの配置情報を取得します
        /// </summary>
        bool ISystemUtility.GetWindowPlacement(IntPtr hWnd, out WINDOWPLACEMENT placement)
        {
            return GetWindowPlacement(hWnd, out placement);
        }

        /// <summary>
        /// メールスロットを作成します
        /// </summary>
        public FileStream CreateMailslot(string path)
        {
            // -1: MAILSLOT_WAIT_FOREVER
            var handle = CreateMailslot(path, 0, -1, IntPtr.Zero);
            if(handle.IsInvalid)
            {
                throw new IOException("Failed to create mailslot");
            }
            return new FileStream(handle, FileAccess.Read, 1, true);
        }

        /// <summary>
        /// メールスロットをテストします
        /// </summary>
        public bool TestMailslot(string path)
        {
            // FileStreamの引数にmailslot名を渡すとエラーになってしまうので
            // CreateFileを直接呼び出す
            var handle = CreateFile(path,
                FileDesiredAccess.GenericWrite,
                FileShareMode.FileShareRead | FileShareMode.FileShareWrite,
                IntPtr.Zero, FileCreationDisposition.OpenExisting, 0, IntPtr.Zero);
            if (handle.IsInvalid)
            {
                return false;
            }
            using(var fs = new FileStream(handle, FileAccess.Write))
            {
                byte[] buf = new byte[1] { 0 };
                fs.Write(buf, 0, 1);
            }
            return true;
        }

        /// <summary>
        /// シャットダウン権限のトークン調整を行います
        /// </summary>
        public void AdjustToken()
        {
            const uint TOKEN_ADJUST_PRIVILEGES = 0x20;
            const uint TOKEN_QUERY = 0x8;
            const int SE_PRIVILEGE_ENABLED = 0x2;
            const string SE_SHUTDOWN_NAME = "SeShutdownPrivilege";

            if (Environment.OSVersion.Platform != PlatformID.Win32NT)
                return;

            IntPtr procHandle = GetCurrentProcess();

            //トークンを取得する
            IntPtr tokenHandle;
            OpenProcessToken(procHandle,
                TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, out tokenHandle);
            //LUIDを取得する
            TOKEN_PRIVILEGES tp = new TOKEN_PRIVILEGES();
            tp.Attributes = SE_PRIVILEGE_ENABLED;
            tp.PrivilegeCount = 1;
            LookupPrivilegeValue(null, SE_SHUTDOWN_NAME, out tp.Luid);
            //特権を有効にする
            AdjustTokenPrivileges(
                tokenHandle, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);

            //閉じる
            CloseHandle(tokenHandle);
        }

        /// <summary>
        /// Windowsをシャットダウン、再起動、ログオフなどの操作を行います
        /// </summary>
        public bool ExitWindowsExNative(ExitWindows flags, int reason)
        {
            return ExitWindowsEx((uint)flags, reason);
        }

        /// <summary>
        /// スレッドハンドルを取得します
        /// </summary>
        public IntPtr OpenThreadNative(ThreadAccess dwDesiredAccess, bool bInheritHandle, uint dwThreadId)
        {
            return OpenThread(dwDesiredAccess, bInheritHandle, dwThreadId);
        }

        /// <summary>
        /// スレッドを一時停止します
        /// </summary>
        public uint SuspendThreadNative(uint threadId)
        {
            IntPtr hThread = OpenThread(ThreadAccess.SUSPEND_RESUME, false, threadId);
            if (hThread == IntPtr.Zero)
            {
                return 0;
            }

            try
            {
                return SuspendThread(hThread);
            }
            finally
            {
                CloseHandle(hThread);
            }
        }

        /// <summary>
        /// スレッドを再開します
        /// </summary>
        public int ResumeThreadNative(uint threadId)
        {
            IntPtr hThread = OpenThread(ThreadAccess.SUSPEND_RESUME, false, threadId);
            if (hThread == IntPtr.Zero)
            {
                return 0;
            }

            try
            {
                return ResumeThread(hThread);
            }
            finally
            {
                CloseHandle(hThread);
            }
        }

        /// <summary>
        /// スレッドを一時停止します
        /// </summary>
        public uint SuspendThreadNative(IntPtr hThread)
        {
            return SuspendThread(hThread);
        }

        /// <summary>
        /// スレッドを再開します
        /// </summary>
        public int ResumeThreadNative(IntPtr hThread)
        {
            return ResumeThread(hThread);
        }

        /// <summary>
        /// ハンドルを閉じます
        /// </summary>
        public void CloseHandleNative(IntPtr hObject)
        {
            CloseHandle(hObject);
        }

        /// <summary>
        /// newでサスペンド抑止
        /// サスペンドしてもOKになったらDispose()を呼ぶ
        /// </summary>
        private class PreventSuspendContext : IDisposable
        {
            enum PowerRequestType
            {
                PowerRequestDisplayRequired = 0,
                PowerRequestSystemRequired,
                PowerRequestAwayModeRequired,
                PowerRequestMaximum
            }

            const int POWER_REQUEST_CONTEXT_VERSION = 0;
            const int POWER_REQUEST_CONTEXT_SIMPLE_STRING = 0x1;
            const int POWER_REQUEST_CONTEXT_DETAILED_STRING = 0x2;

            [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
            public struct POWER_REQUEST_CONTEXT
            {
                public UInt32 Version;
                public UInt32 Flags;
                [MarshalAs(UnmanagedType.LPWStr)]
                public string
                    SimpleReasonString;
            }

            [StructLayout(LayoutKind.Sequential)]
            public struct PowerRequestContextDetailedInformation
            {
                public IntPtr LocalizedReasonModule;
                public UInt32 LocalizedReasonId;
                public UInt32 ReasonStringCount;
                [MarshalAs(UnmanagedType.LPWStr)]
                public string[] ReasonStrings;
            }

            [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
            public struct POWER_REQUEST_CONTEXT_DETAILED
            {
                public UInt32 Version;
                public UInt32 Flags;
                public PowerRequestContextDetailedInformation DetailedInformation;
            }

            [DllImport("kernel32.dll")]
            static extern IntPtr PowerCreateRequest(ref POWER_REQUEST_CONTEXT Context);

            [DllImport("kernel32.dll")]
            static extern bool PowerSetRequest(IntPtr PowerRequestHandle, PowerRequestType RequestType);

            [DllImport("kernel32.dll")]
            static extern bool PowerClearRequest(IntPtr PowerRequestHandle, PowerRequestType RequestType);

            [DllImport("kernel32.dll", CharSet = CharSet.Auto, SetLastError = true, ExactSpelling = true)]
            internal static extern int CloseHandle(IntPtr hObject);


            POWER_REQUEST_CONTEXT _PowerRequestContext;
            IntPtr _PowerRequest; //HANDLE

            public PreventSuspendContext()
            {
                _PowerRequestContext.Version = POWER_REQUEST_CONTEXT_VERSION;
                _PowerRequestContext.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
                _PowerRequestContext.SimpleReasonString = "Amatsukazeがエンコード中です。";
                _PowerRequest = PowerCreateRequest(ref _PowerRequestContext);
                PowerSetRequest(_PowerRequest, PowerRequestType.PowerRequestSystemRequired);
                PowerSetRequest(_PowerRequest, PowerRequestType.PowerRequestAwayModeRequired);
            }

            #region IDisposable Support
            private bool disposedValue = false; // 重複する呼び出しを検出するには

            protected virtual void Dispose(bool disposing)
            {
                if (!disposedValue)
                {
                    PowerClearRequest(_PowerRequest, PowerRequestType.PowerRequestAwayModeRequired);
                    PowerClearRequest(_PowerRequest, PowerRequestType.PowerRequestSystemRequired);
                    CloseHandle(_PowerRequest);
                    _PowerRequest = IntPtr.Zero;
                    disposedValue = true;
                }
            }

            ~PreventSuspendContext()
            {
                // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
                Dispose(false);
            }

            // このコードは、破棄可能なパターンを正しく実装できるように追加されました。
            public void Dispose()
            {
                // このコードを変更しないでください。クリーンアップ コードを上の Dispose(bool disposing) に記述します。
                Dispose(true);
                GC.SuppressFinalize(this);
            }
            #endregion
        }

        /// <summary>
        /// スリープ/サスペンド/休止状態への移行を実行します
        /// </summary>
        public void SetSuspendState(PowerState state, bool force, bool disableWakeEvent)
        {
            bool hibernate = state == PowerState.Hibernate;
            SetSuspendState(hibernate, force, disableWakeEvent);
        }
    }
} 