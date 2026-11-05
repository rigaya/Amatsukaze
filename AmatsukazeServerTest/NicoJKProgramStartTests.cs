using Amatsukaze.Server;
using System.Globalization;
using Xunit;

namespace AmatsukazeServerTest;

public class NicoJKProgramStartTests
{
    [Theory]
    [InlineData(DateTimeKind.Unspecified)]
    [InlineData(DateTimeKind.Local)]
    [InlineData(DateTimeKind.Utc)]
    public void 番組開始の年月日時分秒を常にJSTとして渡す(DateTimeKind kind)
    {
        var start = new DateTime(2026, 10, 1, 23, 0, 0, kind);
        var expected = new DateTimeOffset(2026, 10, 1, 23, 0, 0, TimeSpan.FromHours(9)).ToUnixTimeSeconds();
        Assert.Equal(" --nicojk-program-start " + expected.ToString(CultureInfo.InvariantCulture),
            EncodeServer.GetNicoJKProgramStartArgument(true, start));
    }

    [Fact]
    public void 無効な日時と実況無効時は番組開始を渡さない()
    {
        Assert.Empty(EncodeServer.GetNicoJKProgramStartArgument(true, DateTime.MinValue));
        Assert.Empty(EncodeServer.GetNicoJKProgramStartArgument(true, new DateTime(2000, 1, 1)));
        Assert.Empty(EncodeServer.GetNicoJKProgramStartArgument(false, new DateTime(2026, 10, 1)));
        Assert.NotEmpty(EncodeServer.GetNicoJKProgramStartArgument(true, new DateTime(2000, 1, 1, 0, 0, 1)));
    }
}
