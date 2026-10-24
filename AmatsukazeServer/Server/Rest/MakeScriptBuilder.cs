using System;
using System.IO;
using System.Text;
using Amatsukaze.Lib;

namespace Amatsukaze.Server.Rest
{
    public static class MakeScriptBuilder
    {
        public static bool TryBuild(
            MakeScriptData data,
            string targetHost,
            string scriptType,
            string remoteHost,
            string subnet,
            string mac,
            int serverPort,
            bool isServerWindows,
            string addTaskBaseDir,
            string currentDirectory,
            out string script,
            out string error)
        {
            script = string.Empty;
            error = string.Empty;

            if (data == null)
            {
                error = "MakeScript data is missing.";
                return false;
            }

            var normalizedTargetHost = string.IsNullOrWhiteSpace(targetHost) ? "remote" : targetHost;
            var normalizedScriptType = string.IsNullOrWhiteSpace(scriptType)
                ? (isServerWindows ? "bat" : "sh")
                : scriptType;
            var isBat = string.Equals(normalizedScriptType, "bat", StringComparison.OrdinalIgnoreCase);

            var prof = data.Profile;
            if (string.IsNullOrWhiteSpace(prof))
            {
                error = "プロファイルを選択してください";
                return false;
            }

            var dst = data.OutDir?.TrimEnd(Path.DirectorySeparatorChar);
            if (string.IsNullOrWhiteSpace(dst))
            {
                error = "出力先が設定されていません";
                return false;
            }
            if (!Directory.Exists(dst))
            {
                error = "出力先ディレクトリにアクセスできません";
                return false;
            }

            string nas = null;
            if (data.IsNasEnabled)
            {
                if (string.IsNullOrWhiteSpace(data.NasDir))
                {
                    error = "NAS保存先を指定してください。";
                    return false;
                }
                nas = data.NasDir.TrimEnd(Path.DirectorySeparatorChar);
            }

            var ip = string.Equals(normalizedTargetHost, "local", StringComparison.OrdinalIgnoreCase)
                ? "127.0.0.1"
                : remoteHost;
            if (string.IsNullOrWhiteSpace(ip))
            {
                error = "接続先ホストが指定されていません";
                return false;
            }

            var port = serverPort > 0 ? serverPort : ServerSupport.DEFAULT_PORT;
            var addTaskPath = Path.Combine(addTaskBaseDir, isServerWindows ? "AmatsukazeAddTask.exe" : "AmatsukazeAddTask");
            var lineBreak = isBat ? "\r\n" : "\n";
            var comment = isBat ? "rem" : "#";

            // 起動ユーザーは設定値（初期値はサーバー側で決める）だけを使う。
            // nullは初期値未設定（サービス用アカウントのサーバーなど）、空文字列は指定しない。
            string launchUser = null;
            if (isServerWindows && isBat && data.LaunchUser != null)
            {
                launchUser = data.LaunchUser.Trim();
                if (launchUser.Length > 0 && (OperatingSystem.IsWindows() || !System.Text.RegularExpressions.Regex.IsMatch(launchUser, @"\AS-1-(?:[0-9]+-)+[0-9]+\z")))
                {
                    if (!OperatingSystem.IsWindows())
                    {
                        error = "起動ユーザーの名前解決はWindowsでのみ使用できます";
                        return false;
                    }
                    try { launchUser = LaunchUserIdentity.ResolveSid(launchUser); }
                    catch (SystemException e)
                    {
                        error = "起動ユーザーを確認できません: " + e.Message;
                        return false;
                    }
                }
            }

            // batへ埋め込む値はSIDだけに限定し、コマンド展開文字を受け付けない。
            if (isBat && !string.IsNullOrEmpty(launchUser) &&
                !System.Text.RegularExpressions.Regex.IsMatch(launchUser, @"\AS-1-(?:[0-9]+-)+[0-9]+\z"))
            {
                error = "起動ユーザーのSIDが不正です";
                return false;
            }

            var sb = new StringBuilder();
            if (!isBat)
            {
                sb.Append("#!/bin/bash").Append(lineBreak);
            }
            if (data.IsDirect)
            {
                sb.Append(comment).Append(" _EDCBX_DIRECT_").Append(lineBreak);
            }
            if (isBat && isServerWindows && data.LaunchUser != null && string.IsNullOrEmpty(launchUser))
            {
                sb.Append("rem 起動ユーザーは指定しません。サーバー未起動時はAddTaskと同じアカウントで起動します。").Append(lineBreak);
            }
            else if (isBat && isServerWindows && string.IsNullOrEmpty(launchUser))
            {
                sb.Append("rem 起動ユーザーを自動指定できませんでした。サーバーがサービス用アカウントで動いているか、初期値を取得できませんでした。").Append(lineBreak);
                sb.Append("rem 起動ユーザー欄にPC名\\ユーザー名またはSIDを指定して再生成してください。").Append(lineBreak);
                sb.Append("rem または末尾へ --launch-user \"MYPC\\user\" を手動追加してください。起動済みサーバーのアカウントとAddTaskのコピー権限は変わりません。").Append(lineBreak);
                Util.AddLog("[バッチ生成] 起動ユーザーを自動指定できませんでした。生成したbatの案内に従って再生成または手動指定してください。", null);
            }
            var filePathToken = data.IsDirect
                ? (isBat ? "%FilePath%" : "${FilePath}")
                : "$FilePath$";
            sb.AppendFormat("\"{0}\"", addTaskPath)
                .AppendFormat(" -r \"{0}\"", currentDirectory)
                .AppendFormat(" -f \"{0}\" -ip \"{1}\"", filePathToken, ip)
                .AppendFormat(" -p {0}", port)
                .AppendFormat(" -o \"{0}\"", dst)
                .AppendFormat(" -s \"{0}\"", prof)
                .AppendFormat(" --priority {0}", data.Priority);
            if (isBat && !string.IsNullOrEmpty(launchUser))
            {
                sb.AppendFormat(" --launch-user \"{0}\"", launchUser);
            }
            if (!string.IsNullOrEmpty(nas))
            {
                sb.AppendFormat(" -d \"{0}\"", nas);
            }
            if (data.IsWakeOnLan)
            {
                if (string.IsNullOrWhiteSpace(subnet) || string.IsNullOrWhiteSpace(mac))
                {
                    error = "Wake On Lanに必要な情報が不足しています";
                    return false;
                }
                sb.AppendFormat(" --subnet \"{0}\"", subnet)
                    .AppendFormat(" --mac \"{0}\"", mac);
            }
            if (data.MoveAfter == false)
            {
                sb.Append(" --no-move");
            }
            if (data.ClearEncoded)
            {
                sb.Append(" --clear-succeeded");
            }
            if (data.WithRelated)
            {
                sb.Append(" --with-related");
            }
            if (!string.IsNullOrEmpty(data.AddQueueBat))
            {
                sb.AppendFormat(" -b \"{0}\"", data.AddQueueBat);
            }

            script = sb.ToString();
            return true;
        }
    }
}
