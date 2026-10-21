using Amatsukaze.Lib;
using System.Security.Principal;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class SystemUtilityWindowsTests
{
    [Fact]
    public async Task Windowsで共通ライブラリからメールスロットを通知できる()
    {
        if (!OperatingSystem.IsWindows()) return;
        Assert.Equal("WindowsSystemUtility", SystemUtility.ImplementationName);
        string path = @"\\.\mailslot\AmatsukazeTest_" + Guid.NewGuid().ToString("N");
        Assert.Equal(MailslotRequestStatus.NotFound, SystemUtility.RequestMailslot(path).Status);
        using var receiver = SystemUtility.CreateMailslot(path);
        Assert.Equal(MailslotRequestStatus.Sent, SystemUtility.RequestMailslot(path).Status);
        var buffer = new byte[1];
        Assert.Equal(1, await receiver.ReadAsync(buffer, 0, 1).WaitAsync(TimeSpan.FromSeconds(5)));
    }

    [Fact]
    [System.Runtime.Versioning.SupportedOSPlatform("windows")]
    public void Windowsの修飾名は現在ユーザーのSIDに解決できる()
    {
        if (!OperatingSystem.IsWindows()) return;
        using var identity = WindowsIdentity.GetCurrent();
        Assert.Equal(identity.User!.Value, LaunchUserIdentity.ResolveSid(identity.Name));
        Assert.Equal(identity.User.Value, LaunchUserIdentity.ResolveSid(identity.User.Value));
        Assert.Throws<ArgumentException>(() => LaunchUserIdentity.Validate("修飾のないユーザー"));
    }
}
