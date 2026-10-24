using System.ComponentModel;
using Amatsukaze.Server;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class ServerInstanceTests
{
    [Fact]
    public void 不存在と保持中と解放後を区別する()
    {
        string root = Path.Combine(Path.GetTempPath(), "amt-lock-" + Guid.NewGuid().ToString("N"));
        try
        {
            Assert.False(ServerSupport.IsServerProcessRunning(root));
            Directory.CreateDirectory(Path.Combine(root, "data"));
            Assert.False(ServerSupport.IsServerProcessRunning(root));
            using (var owner = ServerSupport.GetLock(ServerInstanceKind.ServerCLI, 32768, root))
            {
                Assert.True(ServerSupport.IsServerProcessRunning(root));
                Assert.Throws<MultipleInstanceException>(() => ServerSupport.GetLock(ServerInstanceKind.ServerCLI, 32768, root));
                if (OperatingSystem.IsWindows())
                {
                    Assert.True(ServerSupport.TryGetRunningServerInfo(root, out var info));
                    Assert.Equal(Environment.ProcessId, info.Pid);
                    Assert.Equal(32768, info.Port);
                }
            }
            Assert.False(ServerSupport.IsServerProcessRunning(root));
            using var next = ServerSupport.GetLock(ServerInstanceKind.ServerCLI, 32769, root);
        }
        finally { Directory.Delete(root, true); }
    }

    [Fact]
    public void 起動時刻が読めない場合はプロセス名で同一と判定する()
    {
        var start = new DateTime(2026, 1, 2, 3, 4, 5, DateTimeKind.Utc);
        var info = new ServerInstanceInfo { ProcessName = "AmatsukazeServerCLI", ProcessStartTimeUtc = start.ToString("o", System.Globalization.CultureInfo.InvariantCulture) };
        Assert.True(ServerSupport.IsSameServerProcess(info, "AmatsukazeServerCLI", () => start));
        Assert.False(ServerSupport.IsSameServerProcess(info, "AmatsukazeServerCLI", () => start.AddSeconds(1)));
        Assert.False(ServerSupport.IsSameServerProcess(info, "notepad", () => start));
        Assert.True(ServerSupport.IsSameServerProcess(info, "amatsukazeservercli", () => throw new Win32Exception(5)));
        Assert.False(ServerSupport.IsSameServerProcess(info, "notepad", () => throw new Win32Exception(5)));
        // アクセス拒否以外の失敗は呼び出し側（情報不明扱い）へ伝える。
        Assert.Throws<Win32Exception>(() => ServerSupport.IsSameServerProcess(info, "AmatsukazeServerCLI", () => throw new Win32Exception(6)));
    }

    [Fact]
    public void ロックの代わりにディレクトリがある障害を起動済みと誤認しない()
    {
        string root = Path.Combine(Path.GetTempPath(), "amt-lock-error-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(root, "data", "Server.lock"));
        try
        {
            Assert.Throws<IOException>(() => ServerSupport.IsServerProcessRunning(root));
            Assert.Throws<IOException>(() => ServerSupport.GetLock(ServerInstanceKind.ServerCLI, 32768, root));
        }
        finally { Directory.Delete(root, true); }
    }
}
