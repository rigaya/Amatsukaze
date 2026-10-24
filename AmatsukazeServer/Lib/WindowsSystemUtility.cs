using System;
using System.IO;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using Amatsukaze.Server;
using System.ComponentModel;
using System.Runtime.Versioning;

namespace Amatsukaze.Lib
{
    /// <summary>
    /// Windows環境向けのシステムユーティリティ実装
    /// </summary>
    [SupportedOSPlatform("windows")]
    public partial class WindowsSystemUtility : ISystemUtility
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
        [return: MarshalAs(UnmanagedType.U1)]
        private static extern bool SetSuspendState(
            [MarshalAs(UnmanagedType.U1)] bool hibernate,
            [MarshalAs(UnmanagedType.U1)] bool forceCritical,
            [MarshalAs(UnmanagedType.U1)] bool disableWakeEvent);

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
                int error = Marshal.GetLastWin32Error();
                handle.Dispose();
                throw new Win32Exception(error, "CreateMailslotに失敗しました。");
            }
            return new FileStream(handle, FileAccess.Read, 1, true);
        }

        /// <summary>
        /// メールスロットをテストします
        /// </summary>
        public bool TestMailslot(string path) => RequestMailslot(path).Status == MailslotRequestStatus.Sent;

        public MailslotRequestResult RequestMailslot(string path)
        {
            using var handle = CreateFile(path, FileDesiredAccess.GenericWrite,
                FileShareMode.FileShareRead | FileShareMode.FileShareWrite,
                IntPtr.Zero, FileCreationDisposition.OpenExisting, 0, IntPtr.Zero);
            if (handle.IsInvalid)
            {
                int error = Marshal.GetLastWin32Error();
                return new MailslotRequestResult {
                    Status = error == 2 || error == 3 ? MailslotRequestStatus.NotFound : MailslotRequestStatus.Failed,
                    Win32Error = error, Message = "CreateFile: " + new Win32Exception(error).Message };
            }
            try
            {
                using var stream = new FileStream(handle, FileAccess.Write);
                stream.WriteByte(0);
                return new MailslotRequestResult { Status = MailslotRequestStatus.Sent };
            }
            catch (IOException ex)
            {
                return new MailslotRequestResult { Status = MailslotRequestStatus.Failed,
                    Win32Error = ex.HResult & 0xffff, Message = ex.Message };
            }
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

            // 特権の調整に失敗しても、従来どおり電源操作自体は試みる。
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, out var tokenHandle))
            {
                LogPowerFailure("OpenProcessToken", Marshal.GetLastWin32Error());
                return;
            }
            try
            {
                var privileges = new TOKEN_PRIVILEGES { Attributes = SE_PRIVILEGE_ENABLED, PrivilegeCount = 1 };
                if (!LookupPrivilegeValue(null, SE_SHUTDOWN_NAME, out privileges.Luid))
                {
                    LogPowerFailure("LookupPrivilegeValue", Marshal.GetLastWin32Error());
                    return;
                }
                bool adjusted = AdjustTokenPrivileges(tokenHandle, false, ref privileges, 0, IntPtr.Zero, IntPtr.Zero);
                int error = Marshal.GetLastWin32Error();
                if (!adjusted || error != 0)
                    LogPowerFailure("AdjustTokenPrivileges", error);
            }
            finally { CloseHandle(tokenHandle); }
        }

        // 電源操作の失敗は呼び出し元へ伝えずにログへ残す（完了後動作は待ち受けのないTaskで実行されるため）。
        private static void LogPowerFailure(string api, int error)
            => Util.AddLog($"[電源操作] {api}失敗。Win32={error}: {new Win32Exception(error).Message}", null);

        /// <summary>
        /// Windowsをシャットダウン、再起動、ログオフなどの操作を行います
        /// </summary>
        public bool ExitWindowsExNative(ExitWindows flags, int reason)
        {
            bool result = ExitWindowsEx((uint)flags, reason);
            if (!result)
            {
                int error = Marshal.GetLastWin32Error();
                LogPowerFailure("ExitWindowsEx", error);
            }
            return result;
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
        private sealed class PreventSuspendContext : IDisposable
        {
            enum PowerRequestType { DisplayRequired, SystemRequired, AwayModeRequired }

            [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
            struct PowerRequestContext
            {
                public uint Version;
                public uint Flags;
                [MarshalAs(UnmanagedType.LPWStr)] public string Reason;
                // REASON_CONTEXTの共用体全体の領域を確保する。
                public uint Reserved1, Reserved2;
                public IntPtr Reserved3;
            }

            [DllImport("kernel32.dll", SetLastError = true)]
            static extern SafeFileHandle PowerCreateRequest(ref PowerRequestContext context);
            [DllImport("kernel32.dll", SetLastError = true)]
            static extern bool PowerSetRequest(SafeFileHandle handle, PowerRequestType type);
            [DllImport("kernel32.dll", SetLastError = true)]
            static extern bool PowerClearRequest(SafeFileHandle handle, PowerRequestType type);

            readonly SafeFileHandle handle;
            readonly bool systemRequired;
            readonly bool awayModeRequired;
            bool disposed;

            public PreventSuspendContext()
            {
                var context = new PowerRequestContext { Flags = 1, Reason = "Amatsukazeがエンコード中です。" };
                handle = PowerCreateRequest(ref context);
                if (handle.IsInvalid)
                {
                    Warn("PowerCreateRequest", "SystemRequired/AwayModeRequired", Marshal.GetLastWin32Error());
                    handle.Dispose();
                    return;
                }
                systemRequired = PowerSetRequest(handle, PowerRequestType.SystemRequired);
                if (!systemRequired) Warn("PowerSetRequest", "SystemRequired", Marshal.GetLastWin32Error());
                awayModeRequired = PowerSetRequest(handle, PowerRequestType.AwayModeRequired);
                if (!awayModeRequired) Warn("PowerSetRequest", "AwayModeRequired", Marshal.GetLastWin32Error());
            }

            static void Warn(string api, string type, int error)
                => Util.AddLog($"[電源要求] 警告: {api}({type})失敗。Win32={error}: {new Win32Exception(error).Message}", null);

            public void Dispose()
            {
                if (disposed) return;
                disposed = true;
                try
                {
                    if (awayModeRequired && !PowerClearRequest(handle, PowerRequestType.AwayModeRequired))
                        Warn("PowerClearRequest", "AwayModeRequired", Marshal.GetLastWin32Error());
                    if (systemRequired && !PowerClearRequest(handle, PowerRequestType.SystemRequired))
                        Warn("PowerClearRequest", "SystemRequired", Marshal.GetLastWin32Error());
                }
                finally { handle.Dispose(); }
            }
        }

        /// <summary>
        /// スリープ/サスペンド/休止状態への移行を実行します
        /// </summary>
        public void SetSuspendState(PowerState state, bool force, bool disableWakeEvent)
        {
            bool hibernate = state == PowerState.Hibernate;
            if (!SetSuspendState(hibernate, force, disableWakeEvent))
                LogPowerFailure("SetSuspendState", Marshal.GetLastWin32Error());
        }
    }
} 