using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Security.Principal;
using Microsoft.Win32.SafeHandles;

namespace Amatsukaze.Lib
{
    public enum UserSessionState { Active, Disconnected, Other }

    public sealed class UserSession
    {
        public int SessionId { get; set; }
        public string Sid { get; set; }
        public UserSessionState State { get; set; }
    }

    public static class UserSessionSelector
    {
        public static UserSession Select(IEnumerable<UserSession> sessions, string targetSid, uint consoleSessionId)
            => sessions.Where(s => s.SessionId > 0 && s.Sid == targetSid &&
                    (s.State == UserSessionState.Active || s.State == UserSessionState.Disconnected))
                .OrderBy(s => s.State == UserSessionState.Disconnected ? 2 :
                    consoleSessionId != uint.MaxValue && (uint)s.SessionId == consoleSessionId ? 0 : 1)
                .ThenBy(s => s.SessionId).FirstOrDefault();
    }

    [SupportedOSPlatform("windows")]
    public static class LaunchUserIdentity
    {
        public static void Validate(string value)
        {
            if (string.IsNullOrWhiteSpace(value))
                throw new ArgumentException("起動ユーザーをSIDまたはドメイン\\ユーザー名で指定してください。");
            if (value.StartsWith("S-", StringComparison.OrdinalIgnoreCase))
            {
                _ = new SecurityIdentifier(value);
                return;
            }
            int separator = value.IndexOf('\\');
            if (separator <= 0 || separator == value.Length - 1 || value.IndexOf('\\', separator + 1) >= 0)
                throw new ArgumentException("起動ユーザーはSIDまたはドメイン\\ユーザー名で指定してください。ローカルユーザーのドメインにはPC名を使ってください。");
        }

        public static string ResolveSid(string value)
        {
            Validate(value);
            return value.StartsWith("S-", StringComparison.OrdinalIgnoreCase)
                ? new SecurityIdentifier(value).Value
                : ((SecurityIdentifier)new NTAccount(value).Translate(typeof(SecurityIdentifier))).Value;
        }
    }

    public enum UserSessionLaunchStatus { Launched, UserNotLoggedOn, PrivilegeNotHeld, NotSupported, Failed }

    public sealed class UserSessionLaunchResult : IDisposable
    {
        public UserSessionLaunchStatus Status { get; set; }
        public int ProcessId { get; set; }
        public int SessionId { get; set; }
        public int Win32Error { get; set; }
        public string ApiName { get; set; }
        public string Message { get; set; }
        public SafeProcessHandle ProcessHandle { get; set; }
        Process process;

        public static UserSessionLaunchResult FromProcess(Process process)
            => new UserSessionLaunchResult { Status = UserSessionLaunchStatus.Launched,
                ProcessId = process.Id, SessionId = process.SessionId, process = process };

        public bool HasExited
        {
            get
            {
                if (process != null) return process.HasExited;
                if (ProcessHandle == null || ProcessHandle.IsInvalid || ProcessHandle.IsClosed)
                    throw new InvalidOperationException("監視できるプロセスハンドルがありません。");
                uint result = WaitForSingleObject(ProcessHandle, 0);
                if (result == uint.MaxValue) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                return result == 0;
            }
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern uint WaitForSingleObject(SafeProcessHandle handle, uint milliseconds);

        public void Dispose()
        {
            ProcessHandle?.Dispose();
            process?.Dispose();
        }
    }

    public enum MailslotRequestStatus { Sent, NotFound, Failed, NotSupported }

    public sealed class MailslotRequestResult
    {
        public MailslotRequestStatus Status { get; set; }
        public int Win32Error { get; set; }
        public string Message { get; set; }
    }
}
