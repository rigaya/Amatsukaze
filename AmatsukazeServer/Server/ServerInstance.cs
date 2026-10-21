using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Amatsukaze.Server
{
    public enum ServerInstanceKind { ServerGUI, ServerCLI, Standalone }

    // 接続を案内するための補助情報。排他制御には必ずServer.lockを使う。
    public sealed class ServerInstanceInfo
    {
        public int FormatVersion { get; set; } = 1;
        public int Pid { get; set; }
        public string ProcessName { get; set; }
        [JsonConverter(typeof(JsonStringEnumConverter))]
        public ServerInstanceKind Kind { get; set; }
        public int Port { get; set; }
        public string Account { get; set; }
        public string AccountType { get; set; }
        public int SessionId { get; set; }
        public bool IsServiceContext { get; set; }
        public string ProcessStartTimeUtc { get; set; }
        public string Version { get; set; }
    }

    public sealed class ServerLock : IDisposable
    {
        private FileStream stream;
        private readonly string infoPath;
        private ServerInstanceInfo info;

        internal ServerLock(FileStream stream, string rootDir, ServerInstanceKind kind, int port)
        {
            this.stream = stream;
            if (!OperatingSystem.IsWindows()) return;
            infoPath = Path.Combine(rootDir, "data", "ServerInfo.json");
            string temporaryPath = infoPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                // 古い情報はロックを保持している間に無効化する。
                File.Delete(infoPath);
                using var process = Process.GetCurrentProcess();
                info = new ServerInstanceInfo
                {
                    Pid = process.Id,
                    ProcessName = process.ProcessName,
                    Kind = kind,
                    Port = port,
                    Account = Environment.UserDomainName + "\\" + Environment.UserName,
                    AccountType = ServerSupport.GetAccountType(),
                    SessionId = process.SessionId,
                    IsServiceContext = ServerSupport.IsNonInteractiveServiceContext(out _),
                    ProcessStartTimeUtc = process.StartTime.ToUniversalTime().ToString("o", CultureInfo.InvariantCulture),
                    Version = typeof(ServerSupport).Assembly.GetName().Version?.ToString()
                };
                File.WriteAllText(temporaryPath, JsonSerializer.Serialize(info));
                File.Move(temporaryPath, infoPath, true);
            }
            catch (Exception e)
            {
                Util.AddLog("サーバー情報を書き込めません: " + e.Message, e);
            }
            finally
            {
                try { File.Delete(temporaryPath); }
                catch (Exception e) { Util.AddLog("サーバー情報の一時ファイルを削除できません: " + e.Message, e); }
            }
        }

        public void Dispose()
        {
            if (stream == null) return;
            try
            {
                // 解放後に削除すると、次のインスタンスの情報を消してしまう。
                if (infoPath != null && info != null)
                {
                    var current = JsonSerializer.Deserialize<ServerInstanceInfo>(File.ReadAllText(infoPath));
                    if (current?.Pid == info.Pid && current.ProcessStartTimeUtc == info.ProcessStartTimeUtc)
                        File.Delete(infoPath);
                }
            }
            catch (FileNotFoundException) { }
            catch (Exception e) { Util.AddLog("サーバー情報を削除できません: " + e.Message, e); }
            finally
            {
                stream.Dispose();
                stream = null;
            }
        }
    }

    public static partial class ServerSupport
    {
        // 不存在、排他競合、その他の障害を区別する。権限不足を起動中と扱わない。
        internal static bool IsLockConflict(IOException error)
        {
            const int sharingViolation = 32, lockViolation = 33;
            const int unixTryAgain = 11, unixWouldBlock = 35;
            int code = error.HResult & 0xffff;
            return code == sharingViolation || code == lockViolation ||
                (!OperatingSystem.IsWindows() && (code == unixTryAgain || code == unixWouldBlock));
        }

        public static bool TryGetRunningServerInfo(string rootDir, out ServerInstanceInfo info)
        {
            info = null;
            if (!OperatingSystem.IsWindows()) return false;
            try
            {
                if (!IsServerProcessRunning(rootDir)) return false;
                var value = JsonSerializer.Deserialize<ServerInstanceInfo>(
                    File.ReadAllText(Path.Combine(rootDir, "data", "ServerInfo.json")));
                if (value == null || value.FormatVersion != 1 || value.Pid <= 0 ||
                    !Enum.IsDefined(value.Kind) || value.Port < 1 || value.Port > 65535) return false;
                using var process = Process.GetProcessById(value.Pid);
                var start = DateTime.ParseExact(value.ProcessStartTimeUtc, "o", CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind);
                if (process.StartTime.ToUniversalTime() != start.ToUniversalTime() || !IsServerProcessRunning(rootDir)) return false;
                info = value;
                return true;
            }
            catch (Exception)
            {
                // 古い版、権限不足、途中の終了や置換は情報不明として扱う。
                return false;
            }
        }

        public static string GetRunningServerDescription(string rootDir)
        {
            return TryGetRunningServerInfo(rootDir, out var info)
                ? $"種類={info.Kind}、PID={info.Pid}、アカウント={info.Account}、ポート={info.Port}"
                : "起動中のインスタンスの情報を取得できませんでした。";
        }
    }
}
