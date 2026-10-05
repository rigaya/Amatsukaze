using Amatsukaze.Server;
using Amatsukaze.Server.Rest;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class MakeScriptBuilderTests
{
    private sealed class Fixture : IDisposable
    {
        public string Root { get; } = Path.Combine(Path.GetTempPath(), "amt-script-test-" + Guid.NewGuid().ToString("N"));
        public Fixture() => Directory.CreateDirectory(Root);
        public MakeScriptData Data() => new() { Profile = "標準", OutDir = Root, Priority = 3, MoveAfter = true };
        public bool Build(MakeScriptData? data, out string script, out string error, string target = "local", string type = "sh", string? remote = null, string? subnet = null, string? mac = null, int port = 0) =>
            MakeScriptBuilder.TryBuild(data!, target, type, remote!, subnet!, mac!, port,
                false, Root, Root, out script, out error);
        public void Dispose() => Directory.Delete(Root, true);
    }

    [Theory]
    [InlineData(0, "MakeScript data is missing.")]
    [InlineData(1, "プロファイルを選択してください")]
    [InlineData(2, "出力先が設定されていません")]
    [InlineData(3, "出力先ディレクトリにアクセスできません")]
    [InlineData(4, "NAS保存先を指定してください。")]
    [InlineData(5, "Wake On Lanに必要な情報が不足しています")]
    public void 必須入力が不足したらスクリプトを生成しない(int scenario, string expected)
    {
        using var fixture = new Fixture();
        var data = fixture.Data();
        if (scenario == 1) data.Profile = " ";
        if (scenario == 2) data.OutDir = "";
        if (scenario == 3) data.OutDir = Path.Combine(fixture.Root, "未作成");
        if (scenario == 4) data.IsNasEnabled = true;
        if (scenario == 5) data.IsWakeOnLan = true;
        Assert.False(fixture.Build(scenario == 0 ? null : data, out var script, out var error));
        Assert.Empty(script);
        Assert.Equal(expected, error);
    }

    [Fact]
    public void リモート接続先が無ければ生成を拒否する()
    {
        using var fixture = new Fixture();
        Assert.False(fixture.Build(fixture.Data(), out var script, out var error, target: "remote"));
        Assert.Empty(script);
        Assert.Equal("接続先ホストが指定されていません", error);
    }

    [Theory]
    [InlineData("sh", "${FilePath}", "# _EDCBX_DIRECT_")]
    [InlineData("BAT", "%FilePath%", "rem _EDCBX_DIRECT_")]
    public void ローカル直接追加はOS別の変数と既定ポートを使う(string type, string token, string comment)
    {
        using var fixture = new Fixture();
        var data = fixture.Data();
        data.IsDirect = true;
        Assert.True(fixture.Build(data, out var script, out var error, type: type));
        Assert.Empty(error);
        Assert.Contains(comment, script);
        Assert.Contains("-f \"" + token + "\" -ip \"127.0.0.1\"", script);
        Assert.Contains("-p " + ServerSupport.DEFAULT_PORT, script);
        if (type == "sh") Assert.StartsWith("#!/bin/bash\n", script);
        else Assert.DoesNotContain("\n", script.Replace("\r\n", ""));
    }

    [Fact]
    public void リモート追加は任意ポートとNASなどの指定をコマンドへ反映する()
    {
        using var fixture = new Fixture();
        var data = fixture.Data();
        data.IsNasEnabled = true;
        data.NasDir = "/保存先";
        data.IsWakeOnLan = true;
        data.MoveAfter = false;
        data.ClearEncoded = true;
        data.WithRelated = true;
        data.AddQueueBat = "追加処理.bat";
        Assert.True(fixture.Build(data, out var script, out _, target: "remote", remote: "192.0.2.1",
            subnet: "255.255.255.0", mac: "00:11:22:33:44:55", port: 40000));
        Assert.Contains("-ip \"192.0.2.1\" -p 40000", script);
        Assert.Contains("-d \"/保存先\"", script);
        Assert.Contains("--no-move --clear-succeeded --with-related", script);
        Assert.Contains("--mac \"00:11:22:33:44:55\"", script);
        Assert.Contains("-b \"追加処理.bat\"", script);
    }
}
