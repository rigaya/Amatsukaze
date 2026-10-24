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
        public bool Build(MakeScriptData? data, out string script, out string error, string target = "local", string type = "sh", string? remote = null, string? subnet = null, string? mac = null, int port = 0, bool windows = false) =>
            MakeScriptBuilder.TryBuild(data!, target, type, remote!, subnet!, mac!, port,
                windows, Root, Root, out script, out error);
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

    [Fact]
    public void リモート向けbatにもサーバー側の起動ユーザーを付与する()
    {
        using var fixture = new Fixture();
        var data = fixture.Data();
        data.LaunchUser = "S-1-5-21-1-2-3-1001";
        Assert.True(fixture.Build(data, out var script, out var error, target: "remote", type: "bat", remote: "192.168.0.10", windows: true), error);
        var command = script.Split('\n').Last();
        Assert.Contains("-ip \"192.168.0.10\"", command);
        Assert.Contains("--launch-user \"S-1-5-21-1-2-3-1001\"", command);
    }

    [Theory]
    [InlineData("bat", "S-1-5-21-1-2-3-1001", true)]
    [InlineData("bat", null, false)]
    [InlineData("sh", "S-1-5-21-1-2-3-1001", false)]
    public void 起動ユーザーは指定したbatだけに付与する(string type, string? sid, bool expected)
    {
        using var fixture = new Fixture();
        var data = fixture.Data();
        data.LaunchUser = sid;
        Assert.True(fixture.Build(data, out var script, out _, type: type, windows: true));
        // 未指定batの案内コメントはコマンドとして数えない。
        string command = script.Split('\n').Last();
        Assert.Equal(expected, command.Contains(" --launch-user "));
        if (expected) Assert.Contains("--launch-user \"" + sid + "\"", command);
        if (type == "bat" && sid == null) Assert.Contains("rem 起動ユーザーを自動指定できませんでした", script);
    }

    [Theory]
    [InlineData("MYPC\\user")]
    [InlineData("S-1-5-21-1&echo x")]
    [InlineData("S-1-5-21-1\r\necho x")]
    public void バッチに埋め込めない起動ユーザーを拒否する(string value)
    {
        using var fixture = new Fixture();
        var data = fixture.Data();
        data.LaunchUser = value;
        Assert.False(fixture.Build(data, out var script, out _, type: "bat", windows: true));
        Assert.Empty(script);
    }

    [Theory]
    [InlineData("S-1-5-21-1-2-3-1002", true)]
    [InlineData("", false)]
    public void 入力欄の起動ユーザーを付与し空欄なら付与しない(string value, bool expected)
    {
        using var fixture = new Fixture();
        var data = fixture.Data();
        data.LaunchUser = value;
        Assert.True(fixture.Build(data, out var script, out _, type: "bat", windows: true));
        var command = script.Split('\n').Last();
        Assert.Equal(expected, command.Contains(" --launch-user "));
        if (expected) Assert.Contains(value, command);
    }

    [Fact]
    public void 起動ユーザーの設定がなければ自動では付与しない()
    {
        using var fixture = new Fixture();
        var data = fixture.Data();
        data.LaunchUser = null;
        Assert.True(fixture.Build(data, out var script, out _, type: "bat", windows: true));
        Assert.DoesNotContain("--launch-user \"", script.Split('\n').Last());
        Assert.Contains("rem 起動ユーザーを自動指定できませんでした", script);
    }

    [Theory]
    [InlineData("bat", false)]
    [InlineData("sh", true)]
    public void Linuxサーバーまたはシェル出力では起動ユーザーを無視する(string type, bool windows)
    {
        using var fixture = new Fixture();
        var data = fixture.Data();
        data.LaunchUser = "別のPCのユーザー";
        Assert.True(fixture.Build(data, out var script, out _, type: type, windows: windows));
        Assert.DoesNotContain("--launch-user", script);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("MYPC\\user")]
    public void 起動ユーザーは設定のコピーと共有DTOに保持される(string? value)
    {
        var source = new MakeScriptData { LaunchUser = value };
        Assert.Equal(value, ServerSupport.DeepCopy(source).LaunchUser);
        var json = System.Text.Json.JsonSerializer.Serialize(source);
        var dto = System.Text.Json.JsonSerializer.Deserialize<Amatsukaze.Shared.MakeScriptData>(json);
        Assert.Equal(value, dto!.LaunchUser);
    }

    [Fact]
    public void 入力済みの起動ユーザーは初期値で上書きしない()
    {
        var data = new MakeScriptData { LaunchUser = "" };
        ServerSupport.SetDefaultLaunchUserForScript(data);
        Assert.Equal("", data.LaunchUser);
        data.LaunchUser = "MYPC\\指定ユーザー";
        ServerSupport.SetDefaultLaunchUserForScript(data);
        Assert.Equal("MYPC\\指定ユーザー", data.LaunchUser);
    }

    [Fact]
    public void 起動ユーザーの初期値はWindowsのサーバー実行ユーザーに限る()
    {
        var data = new MakeScriptData();
        ServerSupport.SetDefaultLaunchUserForScript(data);
        if (OperatingSystem.IsWindows() && !ServerSupport.IsServiceAccount())
        {
            using var identity = System.Security.Principal.WindowsIdentity.GetCurrent();
            Assert.Equal(identity.Name, data.LaunchUser);
        }
        else Assert.Null(data.LaunchUser);
    }

    [Fact]
    public void Windowsでは入力した起動ユーザー名をSIDとして出力する()
    {
        if (!OperatingSystem.IsWindows()) return;
        using var identity = System.Security.Principal.WindowsIdentity.GetCurrent();
        using var fixture = new Fixture();
        var data = fixture.Data();
        data.LaunchUser = identity.Name;
        Assert.True(fixture.Build(data, out var script, out var error, type: "bat", windows: true), error);
        Assert.Contains("--launch-user \"" + identity.User!.Value + "\"", script);
    }

}
