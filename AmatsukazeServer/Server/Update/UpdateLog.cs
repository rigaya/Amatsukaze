using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;

namespace Amatsukaze.Server.Update
{
    internal sealed class UpdateLog : IDisposable
    {
        internal const string TimestampFormat = "yyyyMMdd-HHmmss";
        internal const string UnknownTransactionId = "00000000";
        private const int MaxLogFiles = 50;
        private const long MaxTotalBytes = 50L * 1024 * 1024;
        private readonly object sync = new object();
        private readonly Action<string> lineObserver;
        private StreamWriter writer;

        public string TransactionId { get; }
        public string FilePath { get; }

        public UpdateLog(string appRoot, Action<string> lineObserver = null,
            string transactionId = null)
        {
            this.lineObserver = lineObserver;
            TransactionId = string.IsNullOrEmpty(transactionId)
                ? CreateTransactionId() : transactionId;
            try
            {
                var directory = Path.Combine(appRoot, "log", "update");
                Directory.CreateDirectory(directory);
                FilePath = Path.Combine(directory,
                    DateTime.Now.ToString(TimestampFormat, CultureInfo.InvariantCulture) +
                    "_" + TransactionId + ".log");
                writer = new StreamWriter(new FileStream(FilePath, FileMode.CreateNew,
                    FileAccess.Write, FileShare.Read), new UTF8Encoding(false))
                {
                    AutoFlush = true,
                };
            }
            catch (Exception ex)
            {
                SafeAddLog($"[Update][{TransactionId}][-][S00_ENV] NG code=LOG_FILE_CREATE_FAILED type={FormatValue(ex.GetType().Name)} msg={FormatValue(ex.Message)}");
            }
        }

        public void Write(string target, string stage, string result,
            params (string Key, object Value)[] values)
        {
            var line = BuildLine(target, stage, result, values);
            try
            {
                SafeAddLog(line);
            }
            catch
            {
                // 更新ログの出力失敗でサーバー本体を停止させない
            }
            try
            {
                lineObserver?.Invoke(line);
            }
            catch
            {
                // 進捗通知の失敗を更新チェックへ波及させない
            }
            lock (sync)
            {
                try
                {
                    writer?.WriteLine(line);
                }
                catch
                {
                    // 独立ログの書き込み失敗は既存サーバーログの動作へ波及させない
                }
            }
        }

        public void WriteDiagnostic(string target, string stage,
            params (string Key, object Value)[] values)
        {
            Write(target, stage, "DIAG", values);
        }

        // updater が既に整形した S20 系ログを、文字列を変えずに取り込む。
        internal void WriteRaw(string line)
        {
            if (line == null) return;
            try { SafeAddLog(line); }
            catch { }
            try { lineObserver?.Invoke(line); }
            catch { }
            lock (sync)
            {
                try { writer?.WriteLine(line); }
                catch { }
            }
        }

        private string BuildLine(string target, string stage, string result,
            IReadOnlyList<(string Key, object Value)> values)
        {
            var builder = new StringBuilder();
            builder.Append("[Update][").Append(TransactionId).Append("][")
                .Append(string.IsNullOrEmpty(target) ? "-" : target).Append("][")
                .Append(stage).Append("] ").Append(result);
            foreach (var value in values)
            {
                builder.Append(' ').Append(value.Key).Append('=').Append(FormatValue(value.Value));
            }
            return builder.ToString();
        }

        internal static string FormatValue(object value)
        {
            if (value == null)
            {
                return "?";
            }
            var text = value switch
            {
                DateTime dateTime => dateTime.ToUniversalTime().ToString("O", CultureInfo.InvariantCulture),
                DateTimeOffset dateTimeOffset => dateTimeOffset.ToUniversalTime().ToString("O", CultureInfo.InvariantCulture),
                IFormattable formattable => formattable.ToString(null, CultureInfo.InvariantCulture),
                _ => value.ToString(),
            } ?? "?";
            if (text.Length > 0 && !text.Any(character =>
                    char.IsWhiteSpace(character) || character == '"' || character == '\\'))
            {
                return text;
            }
            return "\"" + text.Replace("\\", "\\\\", StringComparison.Ordinal)
                .Replace("\"", "\\\"", StringComparison.Ordinal)
                .Replace("\r", "\\r", StringComparison.Ordinal)
                .Replace("\n", "\\n", StringComparison.Ordinal)
                .Replace("\t", "\\t", StringComparison.Ordinal) + "\"";
        }

        // 独立ログを作れない後始末処理の失敗を既存ログに残す。
        internal static void WriteFallbackError(string stage, string code, Exception exception)
        {
            try
            {
                Util.AddLog($"[Update][{UnknownTransactionId}][-][{stage}] NG " +
                    $"code={code} error={FormatValue(exception.GetType().Name)} " +
                    $"message={FormatValue(exception.Message)}", exception);
            }
            catch
            {
                // 後始末ログ自体の障害は本来の失敗理由へ波及させない。
            }
        }

        private static string CreateTransactionId()
        {
            Span<byte> bytes = stackalloc byte[4];
            RandomNumberGenerator.Fill(bytes);
            return Convert.ToHexString(bytes).ToLowerInvariant();
        }

        public static void CleanupOldLogs(string appRoot)
        {
            var cleanupTransactionId = CreateTransactionId();
            try
            {
                var directory = Path.Combine(appRoot, "log", "update");
                if (!Directory.Exists(directory))
                {
                    return;
                }
                var files = new DirectoryInfo(directory).GetFiles("*.log")
                    .OrderByDescending(file => file.LastWriteTimeUtc)
                    .ThenByDescending(file => file.Name, StringComparer.Ordinal)
                    .ToArray();
                long totalBytes = 0;
                for (var index = 0; index < files.Length; index++)
                {
                    var keep = index < MaxLogFiles && totalBytes + files[index].Length <= MaxTotalBytes;
                    if (keep)
                    {
                        totalBytes += files[index].Length;
                    }
                    else
                    {
                        try
                        {
                            files[index].Delete();
                        }
                        catch (Exception ex)
                        {
                            SafeAddLog($"[Update][{cleanupTransactionId}][-][S00_ENV] NG code=LOG_RETENTION_DELETE_FAILED path={FormatValue(files[index].FullName)} type={FormatValue(ex.GetType().Name)}");
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                SafeAddLog($"[Update][{cleanupTransactionId}][-][S00_ENV] NG code=LOG_RETENTION_FAILED type={FormatValue(ex.GetType().Name)} msg={FormatValue(ex.Message)}");
            }
        }

        private static void SafeAddLog(string line)
        {
            try
            {
                Util.AddLog(line, null);
            }
            catch
            {
                // 更新ログの障害を既存サーバーログへ波及させない
            }
        }

        public void Dispose()
        {
            lock (sync)
            {
                try
                {
                    writer?.Dispose();
                }
                catch
                {
                    // 終了時のログ破棄失敗は無視する
                }
                writer = null;
            }
        }
    }
}
