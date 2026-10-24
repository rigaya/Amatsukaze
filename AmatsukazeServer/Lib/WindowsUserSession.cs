using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using Amatsukaze.Server;
using Microsoft.Win32.SafeHandles;

namespace Amatsukaze.Lib
{
    public partial class WindowsSystemUtility
    {
        enum WtsConnectionState { Active = 0, Disconnected = 4 }
        enum WtsInfoClass { UserName = 5, DomainName = 7 }
        enum TokenInfoClass { SessionId = 12, Elevation = 20 }
        const uint CreateUnicodeEnvironment = 0x400;
        const int ErrorAccessDenied = 5;
        const int ErrorNoToken = 1008;
        const int ErrorPrivilegeNotHeld = 1314;
        const int ErrorContextWinstationNotFound = 7022;
        const int WtsEnumerationVersion = 1;
        const int MaxSessionSelectionAttempts = 2;

        static bool IsSessionGone(int error)
            => error == ErrorNoToken || error == ErrorContextWinstationNotFound;

        static Win32Exception SessionApiFailure(string api, int error)
        {
            var exception = new Win32Exception(error, api + "に失敗しました。");
            exception.Data["ApiName"] = api;
            return exception;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct WtsSessionInfo
        {
            public int SessionId;
            public IntPtr StationName;
            public WtsConnectionState State;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct StartupInfo
        {
            public int Size;
            public string Reserved;
            public string Desktop;
            public string Title;
            public int X, Y, XSize, YSize, XCountChars, YCountChars, FillAttribute, Flags;
            public short ShowWindow, ReservedSize;
            public IntPtr ReservedBytes, StdInput, StdOutput, StdError;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct ProcessInformation
        {
            public IntPtr Process, Thread;
            public int ProcessId, ThreadId;
        }

        [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool WTSEnumerateSessionsW(IntPtr server, int reserved, int version, out IntPtr sessions, out int count);
        [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool WTSQuerySessionInformationW(IntPtr server, int session, WtsInfoClass infoClass, out IntPtr buffer, out int bytes);
        [DllImport("wtsapi32.dll")]
        static extern void WTSFreeMemory(IntPtr memory);
        [DllImport("kernel32.dll")]
        static extern uint WTSGetActiveConsoleSessionId();
        [DllImport("wtsapi32.dll", SetLastError = true)]
        static extern bool WTSQueryUserToken(uint sessionId, out SafeAccessTokenHandle token);
        [DllImport("advapi32.dll", SetLastError = true)]
        static extern bool GetTokenInformation(SafeAccessTokenHandle token, TokenInfoClass infoClass, IntPtr buffer, int size, out int needed);
        [DllImport("userenv.dll", SetLastError = true)]
        static extern bool CreateEnvironmentBlock(out IntPtr environment, SafeAccessTokenHandle token, bool inherit);
        [DllImport("userenv.dll", SetLastError = true)]
        static extern bool DestroyEnvironmentBlock(IntPtr environment);
        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool CreateProcessAsUserW(SafeAccessTokenHandle token, string application,
            StringBuilder commandLine, IntPtr processAttributes, IntPtr threadAttributes,
            bool inheritHandles, uint flags, IntPtr environment, string currentDirectory,
            ref StartupInfo startupInfo, out ProcessInformation processInformation);

        static string QuerySessionText(int session, WtsInfoClass infoClass)
        {
            if (!WTSQuerySessionInformationW(IntPtr.Zero, session, infoClass, out var buffer, out _))
                throw SessionApiFailure("WTSQuerySessionInformationW", Marshal.GetLastWin32Error());
            try { return Marshal.PtrToStringUni(buffer) ?? ""; }
            finally { WTSFreeMemory(buffer); }
        }

        // セッションのログオンユーザーのSIDを返す。ログオフ済み・セッション不一致はnull。
        static string QuerySessionSid(int sessionId, bool useTokens)
        {
            if (!useTokens)
            {
                string user = QuerySessionText(sessionId, WtsInfoClass.UserName);
                if (user.Length == 0) return null;
                string domain = QuerySessionText(sessionId, WtsInfoClass.DomainName);
                return ((SecurityIdentifier)new NTAccount(domain, user).Translate(typeof(SecurityIdentifier))).Value;
            }
            // 名前解決に依存せず、実際のログオンユーザーをSIDで照合する。
            bool acquired = WTSQueryUserToken((uint)sessionId, out var token);
            int error = acquired ? 0 : Marshal.GetLastWin32Error();
            using (token)
            {
                if (!acquired)
                {
                    if (IsSessionGone(error)) return null;
                    throw SessionApiFailure("WTSQueryUserToken", error);
                }
                using var identity = new WindowsIdentity(token.DangerousGetHandle());
                if (QueryTokenInt(token, TokenInfoClass.SessionId) != sessionId) return null;
                return identity.User?.Value;
            }
        }

        static List<UserSession> EnumerateUserSessions(bool useTokens)
        {
            if (!WTSEnumerateSessionsW(IntPtr.Zero, 0, WtsEnumerationVersion, out var buffer, out int count))
                throw SessionApiFailure("WTSEnumerateSessionsW", Marshal.GetLastWin32Error());
            try
            {
                var sessions = new List<UserSession>();
                for (int i = 0; i < count; i++)
                {
                    var entry = Marshal.PtrToStructure<WtsSessionInfo>(IntPtr.Add(buffer, i * Marshal.SizeOf<WtsSessionInfo>()));
                    if (entry.SessionId <= 0 || (entry.State != WtsConnectionState.Active &&
                            entry.State != WtsConnectionState.Disconnected)) continue;
                    string sid;
                    try
                    {
                        sid = QuerySessionSid(entry.SessionId, useTokens);
                    }
                    catch (SystemException e)
                    {
                        // 1つのセッションの異常で他のセッションへの起動を妨げない。
                        Util.AddLog($"[ユーザー起動] セッション={entry.SessionId}を確認できないため候補から除外します: {e.Message}", null);
                        continue;
                    }
                    if (sid == null) continue;
                    sessions.Add(new UserSession { SessionId = entry.SessionId, Sid = sid,
                        State = entry.State == WtsConnectionState.Active ? UserSessionState.Active : UserSessionState.Disconnected });
                }
                return sessions;
            }
            finally { WTSFreeMemory(buffer); }
        }

        static int QueryTokenInt(SafeAccessTokenHandle token, TokenInfoClass infoClass)
        {
            var buffer = Marshal.AllocHGlobal(sizeof(int));
            try
            {
                if (!GetTokenInformation(token, infoClass, buffer, sizeof(int), out _))
                    throw SessionApiFailure("GetTokenInformation", Marshal.GetLastWin32Error());
                return Marshal.ReadInt32(buffer);
            }
            finally { Marshal.FreeHGlobal(buffer); }
        }

        static UserSessionLaunchResult LaunchFailure(string api, int error, string message)
            => new UserSessionLaunchResult { Status = error == ErrorPrivilegeNotHeld || error == ErrorAccessDenied
                    ? UserSessionLaunchStatus.PrivilegeNotHeld : UserSessionLaunchStatus.Failed,
                ApiName = api, Win32Error = error,
                Message = message + (error == 0 ? "" : $" Win32={error}: {new Win32Exception(error).Message}") };

        public UserSessionLaunchResult LaunchProcessInUserSession(string targetSid, string executablePath, string arguments, string workingDirectory)
        {
            // 名前解決失敗は未ログオンと区別し、呼び出し側で引数エラーにする。
            targetSid = LaunchUserIdentity.ResolveSid(targetSid);
            string api = "WTSEnumerateSessionsW";
            try
            {
                using var current = WindowsIdentity.GetCurrent();
                bool isSystem = current.IsSystem;
                for (int attempt = 0; attempt < MaxSessionSelectionAttempts; attempt++)
                {
                    var selected = UserSessionSelector.Select(EnumerateUserSessions(isSystem), targetSid, WTSGetActiveConsoleSessionId());
                    if (selected == null)
                        return new UserSessionLaunchResult { Status = UserSessionLaunchStatus.UserNotLoggedOn,
                            Message = $"指定SID={targetSid}のログオンセッションがありません。" };

                    if (!isSystem)
                        return LaunchFailure("WTSQueryUserToken", ErrorPrivilegeNotHeld,
                            $"選択セッション={selected.SessionId}。別セッションの起動にはLocalSystemと必要な特権が必要です。");
                    api = "WTSQueryUserToken";
                    bool acquired = WTSQueryUserToken((uint)selected.SessionId, out var token);
                    int error = acquired ? 0 : Marshal.GetLastWin32Error();
                    using (token)
                    {
                        if (!acquired)
                        {
                            // ログオフ競合の場合だけ、一度再列挙する。
                            if (attempt == 0 && IsSessionGone(error)) continue;
                            return LaunchFailure(api, error, $"セッション={selected.SessionId}のユーザートークンを取得できません。");
                        }
                        api = "GetTokenInformation";
                        using var identity = new WindowsIdentity(token.DangerousGetHandle());
                        int tokenSession = QueryTokenInt(token, TokenInfoClass.SessionId);
                        if (identity.User?.Value != targetSid || tokenSession != selected.SessionId)
                        {
                            if (attempt == 0) continue;
                            return LaunchFailure(api, 0, "起動直前のSIDまたはセッションIDが一致しません。ログオン状態が変化しました。");
                        }
                        string accountName;
                        try { accountName = identity.Name; }
                        catch (IdentityNotMappedException) { accountName = targetSid; }
                        int elevated = QueryTokenInt(token, TokenInfoClass.Elevation);
                        Util.AddLog($"[ユーザー起動] SID={targetSid}、アカウント={accountName}、セッション={tokenSession}、昇格={elevated != 0}", null);
                        api = "CreateEnvironmentBlock";
                        if (!CreateEnvironmentBlock(out var environment, token, false))
                            return LaunchFailure(api, Marshal.GetLastWin32Error(), "ユーザー環境を生成できません。");
                        try
                        {
                            executablePath = Path.GetFullPath(executablePath);
                            workingDirectory = Path.GetFullPath(workingDirectory);
                            var startup = new StartupInfo { Size = Marshal.SizeOf<StartupInfo>(), Desktop = @"winsta0\default" };
                            // 実行ファイルは絶対パスで明示し、可変バッファに自身の名前も含める。
                            var commandLine = new StringBuilder("\"" + executablePath + "\" " + arguments);
                            api = "CreateProcessAsUserW";
                            if (!CreateProcessAsUserW(token, executablePath, commandLine, IntPtr.Zero, IntPtr.Zero,
                                    false, CreateUnicodeEnvironment, environment, workingDirectory, ref startup, out var processInfo))
                                return LaunchFailure(api, Marshal.GetLastWin32Error(), "指定ユーザーのプロセス作成に失敗しました。");
                            try
                            {
                                return new UserSessionLaunchResult { Status = UserSessionLaunchStatus.Launched,
                                    ProcessId = processInfo.ProcessId, SessionId = tokenSession,
                                    ProcessHandle = new SafeProcessHandle(processInfo.Process, true),
                                    Message = $"アカウント={accountName}、SID={targetSid}、セッション={tokenSession}、PID={processInfo.ProcessId}" };
                            }
                            finally { CloseHandle(processInfo.Thread); }
                        }
                        finally { DestroyEnvironmentBlock(environment); }
                    }
                }
                return LaunchFailure(api, 0, "ログオン状態が変化したため起動できません。");
            }
            catch (Win32Exception ex) { return LaunchFailure(ex.Data["ApiName"] as string ?? api, ex.NativeErrorCode, ex.Message); }
            catch (IdentityNotMappedException ex) { return LaunchFailure("アカウントSID照合", 0, ex.Message); }
        }
    }
}
