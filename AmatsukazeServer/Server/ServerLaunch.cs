using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Principal;
using Amatsukaze.Lib;

namespace Amatsukaze.Server
{
    public static partial class ServerSupport
    {
        public static string GetAccountType()
        {
            if (!OperatingSystem.IsWindows()) return "通常ユーザー";
            using var identity = WindowsIdentity.GetCurrent();
            var sid = identity.User;
            return sid.IsWellKnown(WellKnownSidType.LocalSystemSid) ? "LocalSystem" :
                sid.IsWellKnown(WellKnownSidType.LocalServiceSid) ? "LocalService" :
                sid.IsWellKnown(WellKnownSidType.NetworkServiceSid) ? "NetworkService" : "通常ユーザー";
        }

        public static bool IsServiceAccount() => OperatingSystem.IsWindows() && GetAccountType() != "通常ユーザー";

        // 初期値の決定はサーバーで行い、クライアントの実行ユーザーは使わない。
        public static void SetDefaultLaunchUserForScript(MakeScriptData data)
        {
            if (data.LaunchUser != null || !OperatingSystem.IsWindows() || IsServiceAccount()) return;
            try
            {
                using var identity = WindowsIdentity.GetCurrent();
                data.LaunchUser = identity.Name;
            }
            catch (Exception e)
            {
                Util.AddLog("バッチの起動ユーザーの初期値を取得できません: " + e.Message, e);
            }
        }

        public static string GetLaunchUserForScript()
        {
            if (!OperatingSystem.IsWindows() || IsNonInteractiveServiceContext(out _)) return null;
            try
            {
                using var identity = WindowsIdentity.GetCurrent();
                return identity.User?.Value;
            }
            catch (Exception e)
            {
                Util.AddLog("バッチの起動ユーザーを取得できません: " + e.Message, e);
                return null;
            }
        }

        public static string GetServiceContextPathHint(params string[] paths)
            => GetServiceContextPathHintForProcess("AmatsukazeServer", paths);

        public static string GetServiceContextPathHintForProcess(string processName, params string[] paths)
        {
            try
            {
                if (!IsNonInteractiveServiceContext(out _) || !paths.Any(IsNetworkPathCandidate)) return null;
                using var process = Process.GetCurrentProcess();
                return $"（{processName}の実行アカウント={Environment.UserDomainName}\\{Environment.UserName}、" +
                    $"セッションID={process.SessionId}。" +
                    "共有先の認証・アクセス権、またはこのログオン環境から見えないドライブが原因の可能性があります。" +
                    "UNCパスでも共有先への認証と権限は必要です。" +
                    (processName == "AmatsukazeAddTask"
                        ? "AddTask自身を必要な権限のあるユーザーで実行するか、共有先の権限を確認してください。--launch-userはコピー権限を変更しません。"
                        : "ログオンユーザーでの起動にはバッチの再生成または--launch-userの指定を確認してください。起動済みサーバーのアカウントは変わりません。") + "）";
            }
            catch (Exception)
            {
                // 診断処理で元の障害を隠さない。
                return null;
            }
        }

        internal static bool IsNetworkPathCandidate(string path)
        {
            if (string.IsNullOrWhiteSpace(path)) return false;
            if (path.StartsWith(@"\\.\", StringComparison.Ordinal)) return false;
            if (path.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase)) return true;
            if (path.StartsWith(@"\\?\", StringComparison.Ordinal)) path = path.Substring(4);
            else if (path.StartsWith(@"\\", StringComparison.Ordinal) || path.StartsWith("//", StringComparison.Ordinal)) return true;
            if (path.Length < 3 || !char.IsLetter(path[0]) || path[1] != ':') return false;
            var drive = new DriveInfo(path.Substring(0, 3));
            return drive.DriveType == DriveType.Network || drive.DriveType == DriveType.NoRootDirectory;
        }
    }
}
