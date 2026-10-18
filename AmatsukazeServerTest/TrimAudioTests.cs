using System.Text;
using Amatsukaze.Server.Rest;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using Xunit;

namespace AmatsukazeServerTest;

[Collection("REST一時保存先")]
public sealed class TrimAudioTests
{
    [Fact]
    public void WAVはPCMの形式と長さと内容を保持する()
    {
        var pcm = new byte[] { 0x01, 0x02, 0x03, 0x04, 0xfe, 0xff, 0xfc, 0xff };
        var wav = TrimAdjustSession.BuildAudioWav(pcm, 48000, 2);
        using var reader = new BinaryReader(new MemoryStream(wav));
        Assert.Equal("RIFF", Encoding.ASCII.GetString(reader.ReadBytes(4)));
        Assert.Equal(wav.Length - 8, reader.ReadInt32());
        Assert.Equal("WAVEfmt ", Encoding.ASCII.GetString(reader.ReadBytes(8)));
        Assert.Equal(16, reader.ReadInt32());
        Assert.Equal(1, reader.ReadInt16());
        Assert.Equal(2, reader.ReadInt16());
        Assert.Equal(48000, reader.ReadInt32());
        Assert.Equal(192000, reader.ReadInt32());
        Assert.Equal(4, reader.ReadInt16());
        Assert.Equal(16, reader.ReadInt16());
        Assert.Equal("data", Encoding.ASCII.GetString(reader.ReadBytes(4)));
        Assert.Equal(pcm.Length, reader.ReadInt32());
        Assert.Equal(pcm, reader.ReadBytes(pcm.Length));
    }

    [Theory]
    [InlineData(null, false)]
    [InlineData("", false)]
    [InlineData("true", false)]
    [InlineData("0", false)]
    [InlineData("1", true)]
    public async Task 音声は環境変数が1の場合だけ有効(string? value, bool enabled)
    {
        var previous = Environment.GetEnvironmentVariable("AMT_TRIM_AUDIO_PLAYBACK");
        try
        {
            Environment.SetEnvironmentVariable("AMT_TRIM_AUDIO_PLAYBACK", value);
            Assert.Equal(enabled, TrimAdjustService.AudioPlaybackEnabled);
            await using var fixture = new RestEndpointTestFixture();
            var result = await fixture.Send("GET", "/api/trim/sessions/{sessionId}/audio", "?start=0&end=0",
                routeValues: new { sessionId = "存在しない" });
            Assert.Equal(404, result.Status);
            var invalid = await fixture.Send("GET", "/api/trim/sessions/{sessionId}/audio", "?start=不正",
                routeValues: new { sessionId = "存在しない" });
            Assert.Equal(enabled ? 400 : 404, invalid.Status);
        }
        finally { Environment.SetEnvironmentVariable("AMT_TRIM_AUDIO_PLAYBACK", previous); }
    }

    [Theory]
    [InlineData(null, 200)]
    [InlineData("bytes=44-47", 206)]
    [InlineData("bytes=1000-", 416)]
    public async Task 音声応答はWAVとRangeを返す(string? range, int status)
    {
        var wav = TrimAdjustSession.BuildAudioWav(new byte[] { 1, 2, 3, 4 }, 48000, 2);
        using var services = new ServiceCollection().AddLogging().BuildServiceProvider();
        var context = new DefaultHttpContext { RequestServices = services };
        context.Request.Method = "GET";
        if (range != null) context.Request.Headers.Range = range;
        using var output = new MemoryStream();
        context.Response.Body = output;
        await RestApiHost.BuildTrimAudioResult(context, () => wav).ExecuteAsync(context);
        Assert.Equal(status, context.Response.StatusCode);
        Assert.Equal("no-store", context.Response.Headers.CacheControl);
        if (status == 200) Assert.Equal(wav, output.ToArray());
        if (status == 206)
        {
            Assert.Equal("audio/wav", context.Response.ContentType);
            Assert.Equal("bytes 44-47/48", context.Response.Headers.ContentRange);
            Assert.Equal(new byte[] { 1, 2, 3, 4 }, output.ToArray());
        }
    }

    [Theory]
    [InlineData(false, 204)]
    [InlineData(true, 400)]
    public async Task 音声なしと範囲不正を区別する(bool invalid, int status)
    {
        using var services = new ServiceCollection().AddLogging().BuildServiceProvider();
        var context = new DefaultHttpContext { RequestServices = services };
        context.Response.Body = new MemoryStream();
        await RestApiHost.BuildTrimAudioResult(context, () => invalid
            ? throw new ArgumentException("範囲が不正です") : null!).ExecuteAsync(context);
        Assert.Equal(status, context.Response.StatusCode);
        Assert.Equal("no-store", context.Response.Headers.CacheControl);
    }
}
