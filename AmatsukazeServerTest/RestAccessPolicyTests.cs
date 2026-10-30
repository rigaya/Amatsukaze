using System.Net;
using Amatsukaze.Server.Rest;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class RestAccessPolicyTests
{
    [Theory]
    [InlineData("127.0.0.1", true)]
    [InlineData("::1", true)]
    [InlineData("10.1.2.3", true)]
    [InlineData("172.16.0.1", true)]
    [InlineData("172.31.255.255", true)]
    [InlineData("172.15.255.255", false)]
    [InlineData("172.32.0.0", false)]
    [InlineData("192.168.1.1", true)]
    [InlineData("fe80::1", true)]
    [InlineData("fec0::1", true)]
    [InlineData("fc00::1", true)]
    [InlineData("fdff::1", true)]
    [InlineData("2001:db8::1", false)]
    [InlineData("100.64.0.1", false)]
    [InlineData("203.0.113.25", false)]
    [InlineData("::ffff:192.168.1.1", true)]
    [InlineData("::ffff:127.0.0.1", true)]
    public void 未設定でも既存の許可範囲を維持する(string address, bool expected)
    {
        Assert.Equal(expected, RestAccessPolicy.Parse(null, null).IsAllowed(IPAddress.Parse(address)));
        Assert.Equal(expected, RestAccessPolicy.Parse(" , , ", null).IsAllowed(IPAddress.Parse(address)));
    }

    [Theory]
    [InlineData("100.63.255.255", false)]
    [InlineData("100.64.0.0", true)]
    [InlineData("100.127.255.255", true)]
    [InlineData("100.128.0.0", false)]
    [InlineData("203.0.113.24", false)]
    [InlineData("203.0.113.25", true)]
    [InlineData("203.0.113.26", false)]
    [InlineData("2001:db8:1234::", true)]
    [InlineData("2001:db8:1234:ffff:ffff:ffff:ffff:ffff", true)]
    [InlineData("2001:db8:1235::", false)]
    [InlineData("2001:db8:5678::1", true)]
    [InlineData("2001:db8:5678::2", false)]
    [InlineData("::ffff:100.64.0.1", true)]
    [InlineData("192.168.1.1", true)]
    public void 複数の範囲と単独IPを追加する(string address, bool expected)
    {
        var policy = RestAccessPolicy.Parse(" 100.64.1.2/10,203.0.113.25,2001:db8:1234::abcd/48,2001:db8:5678::1, ", null);
        Assert.Equal(expected, policy.IsAllowed(IPAddress.Parse(address)));
    }

    [Theory]
    [InlineData("localhost")]
    [InlineData("203.0.113.*")]
    [InlineData("203.0.113.1-203.0.113.9")]
    [InlineData("203.0.113.1/33")]
    [InlineData("2001:db8::/129")]
    [InlineData("203.0.113.1/-1")]
    [InlineData("203.0.113.1/")]
    [InlineData("203.0.113.1/24/24")]
    [InlineData("127.1")]
    [InlineData("2130706433")]
    [InlineData("[2001:db8::1]")]
    [InlineData("fe80::1%3")]
    public void 不正な項目があれば追加設定全体を無効化する(string invalid)
    {
        var logs = new List<string>();
        var policy = RestAccessPolicy.Parse("203.0.113.25," + invalid, logs.Add);
        Assert.False(policy.IsAllowed(IPAddress.Parse("203.0.113.25")));
        Assert.True(policy.IsAllowed(IPAddress.Parse("192.168.1.1")));
        Assert.Contains("追加設定全体を無効", Assert.Single(logs));
    }

    [Theory]
    [InlineData("0.0.0.0/0", "203.0.113.1", true)]
    [InlineData("0.0.0.0/0", "2001:db8::1", false)]
    [InlineData("::/0", "2001:db8::1", true)]
    [InlineData("::/0", "203.0.113.1", false)]
    [InlineData("::ffff:203.0.113.25/128", "203.0.113.25", true)]
    public void 全範囲と射影アドレスを判定する(string setting, string address, bool expected)
    {
        Assert.Equal(expected, RestAccessPolicy.Parse(setting, null).IsAllowed(IPAddress.Parse(address)));
    }

    [Fact]
    public void 正規化した範囲を起動ログに残す()
    {
        var logs = new List<string>();
        RestAccessPolicy.Parse("100.64.1.2/24,203.0.113.25", logs.Add);
        var log = Assert.Single(logs);
        Assert.Contains("100.64.1.0/24", log);
        Assert.Contains("203.0.113.25/32", log);
    }
}
