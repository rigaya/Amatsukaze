using System.Net;
using System.Net.Sockets;
using Amatsukaze.Server.Rest;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class RestPortSelectorTests
{
    private const string JapaneseNetsh = """
        プロトコル tcp ポート除外範囲

        開始ポート    終了ポート
        ----------    --------
              5357        5357
             32768       32867
             50000       50059     *

        """;

    [Fact]
    public void 日本語のnetsh出力から除外範囲を読む()
    {
        var ranges = RestPortSelector.ParseExcludedPortRanges(JapaneseNetsh);
        Assert.Equal(3, ranges.Count);
        Assert.Equal(5357, ranges[0].Start);
        Assert.Equal(5357, ranges[0].End);
        Assert.Equal(32768, ranges[1].Start);
        Assert.Equal(32867, ranges[1].End);
        Assert.Equal(50000, ranges[2].Start);
        Assert.Equal(50059, ranges[2].End);
    }

    [Fact]
    public void 除外範囲の直後かつサーバーポート以外を選ぶ()
    {
        var ranges = new[] { new PortRange(32768, 32867), new PortRange(32860, 32900) };
        var probed = new List<int>();
        var selected = RestPortSelector.ScanAvailablePort(32769, 32901, ranges, port =>
        {
            probed.Add(port);
            return PortBindResult.Free;
        });
        Assert.Equal(32902, selected);
        Assert.Equal(new[] { 32902 }, probed);
    }

    [Fact]
    public void 使用中も10013も次のポートへ進む()
    {
        var selected = RestPortSelector.ScanAvailablePort(40000, 0, Array.Empty<PortRange>(), port =>
        {
            if (port == 40000)
            {
                return PortBindResult.InUse;
            }
            if (port == 40001)
            {
                return PortBindResult.AccessDenied;
            }
            return PortBindResult.Free;
        });
        Assert.Equal(40002, selected);
    }

    [Fact]
    public void 連続する10013は幅を広げて空きまで進む()
    {
        const int firstFree = 41000;
        var selected = RestPortSelector.ScanAvailablePort(40000, 0, Array.Empty<PortRange>(), port =>
            port < firstFree ? PortBindResult.AccessDenied : PortBindResult.Free);
        Assert.Equal(firstFree, selected);
    }

    [Fact]
    public void 保存ポートが空いていればそれを使い失敗したら破棄して探索する()
    {
        var kept = RestPortSelector.FindAutoPort(32769, 32768, 34000, Array.Empty<PortRange>(), port =>
            port == 34000 ? PortBindResult.Free : PortBindResult.InUse, out var discarded);
        Assert.Equal(34000, kept);
        Assert.False(discarded);

        var again = RestPortSelector.FindAutoPort(32769, 32768, 34000, Array.Empty<PortRange>(), port =>
            port == 32769 ? PortBindResult.Free : PortBindResult.AccessDenied, out discarded);
        Assert.Equal(32769, again);
        Assert.True(discarded);
    }

    [Fact]
    public void 保存ポートがサーバーポートと同じときは破棄する()
    {
        var selected = RestPortSelector.FindAutoPort(32769, 32768, 32768, Array.Empty<PortRange>(), port =>
            port == 32769 ? PortBindResult.Free : PortBindResult.InUse, out var discarded);
        Assert.Equal(32769, selected);
        Assert.True(discarded);
    }

    [Fact]
    public void 除外範囲に入っているポートには候補を案内する()
    {
        var ranges = RestPortSelector.ParseExcludedPortRanges(JapaneseNetsh);
        var message = RestPortSelector.BuildUnavailableMessage(
            32769, 32768, PortBindResult.AccessDenied, ranges, port =>
                port == 32868 ? PortBindResult.Free : PortBindResult.InUse);
        Assert.Contains("32768-32867", message, StringComparison.Ordinal);
        Assert.Contains("32868", message, StringComparison.Ordinal);
        Assert.Contains("AMT_REST_PORT", message, StringComparison.Ordinal);
    }

    [Fact]
    public void 使用中で除外範囲でなければ候補番号は出さない()
    {
        var message = RestPortSelector.BuildUnavailableMessage(
            32769, 32768, PortBindResult.InUse, Array.Empty<PortRange>(), _ => PortBindResult.Free);
        Assert.Contains("使用中", message, StringComparison.Ordinal);
        Assert.DoesNotContain("であれば回避できます", message, StringComparison.Ordinal);
    }

    [Fact]
    public void バインド失敗の10013は次のポートを試す対象になる()
    {
        var denied = new SocketException(10013);
        Assert.Equal(PortBindResult.AccessDenied, RestPortSelector.ClassifyBindFailure(denied));
        Assert.True(RestPortSelector.IsRetryableBindFailure(new IOException("Failed to bind", denied)));
        Assert.Equal(PortBindResult.InUse, RestPortSelector.ClassifyBindFailure(new SocketException(10048)));
    }

    [Fact]
    public void 実際のTCP待ち受けと同じ条件で使用中を判定する()
    {
        var listener = new TcpListener(IPAddress.Any, 0);
        listener.Start();
        try
        {
            var port = ((IPEndPoint)listener.LocalEndpoint).Port;
            Assert.Equal(PortBindResult.InUse, RestPortSelector.ProbeTcpPort(port));
        }
        finally
        {
            listener.Stop();
        }
    }
}
