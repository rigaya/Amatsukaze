using Amatsukaze.Lib;
using Xunit;

namespace AmatsukazeServerTest;

public class UserSessionSelectorTests
{
    [Fact]
    public void 対象SIDのコンソールと状態とIDの順で選ぶ()
    {
        var sessions = new[]
        {
            new UserSession { SessionId = 1, Sid = "別ユーザー", State = UserSessionState.Active },
            new UserSession { SessionId = 0, Sid = "対象", State = UserSessionState.Active },
            new UserSession { SessionId = 2, Sid = "対象", State = UserSessionState.Disconnected },
            new UserSession { SessionId = 9, Sid = "対象", State = UserSessionState.Active },
            new UserSession { SessionId = 5, Sid = "対象", State = UserSessionState.Active },
        };

        Assert.Equal(9, UserSessionSelector.Select(sessions, "対象", 9).SessionId);
        Assert.Equal(5, UserSessionSelector.Select(sessions, "対象", uint.MaxValue).SessionId);
        Assert.Equal(5, UserSessionSelector.Select(sessions, "対象", 2).SessionId);
        Assert.Null(UserSessionSelector.Select(sessions, "未ログオン", 9));
    }

    [Fact]
    public void 対話中の候補がなければ切断中を選ぶ()
    {
        var sessions = new[]
        {
            new UserSession { SessionId = 1, Sid = "対象", State = UserSessionState.Other },
            new UserSession { SessionId = 8, Sid = "対象", State = UserSessionState.Disconnected },
            new UserSession { SessionId = 4, Sid = "対象", State = UserSessionState.Disconnected },
        };

        Assert.Equal(4, UserSessionSelector.Select(sessions, "対象", 8).SessionId);
    }
}
