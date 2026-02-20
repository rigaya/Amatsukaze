using System;
using System.IO;
using System.Text;

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

            if (string.Equals(normalizedTargetHost, "local", StringComparison.OrdinalIgnoreCase))
            {
                if (isServerWindows && !isBat)
                {
                    error = "Windowsサーバーではバッチのみ生成できます。";
                    return false;
                }
                if (!isServerWindows && isBat)
                {
                    error = "Linuxサーバーではシェルのみ生成できます。";
                    return false;
                }
            }

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

            var sb = new StringBuilder();
            if (!isBat)
            {
                sb.Append("#!/bin/bash").Append(lineBreak);
            }
            if (data.IsDirect)
            {
                sb.Append(comment).Append(" _EDCBX_DIRECT_").Append(lineBreak);
            }
            var filePathToken = data.IsDirect
                ? "$FilePath$"
                : isBat ? "%FilePath%" : "${FilePath}";
            sb.AppendFormat("\"{0}\"", addTaskPath)
                .AppendFormat(" -r \"{0}\"", currentDirectory)
                .AppendFormat(" -f \"{0}\" -ip \"{1}\"", filePathToken, ip)
                .AppendFormat(" -p {0}", port)
                .AppendFormat(" -o \"{0}\"", dst)
                .AppendFormat(" -s \"{0}\"", prof)
                .AppendFormat(" --priority {0}", data.Priority);
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
