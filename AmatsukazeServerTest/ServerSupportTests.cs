using System;
using System.IO;
using Amatsukaze.Lib;
using Amatsukaze.Server;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class ServerSupportTempDirTests : IDisposable
{
    private readonly string root;

    public ServerSupportTempDirTests()
    {
        root = Path.Combine(Path.GetTempPath(), "amt-tempdir-log-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
    }

    public void Dispose()
    {
        try
        {
            Directory.Delete(root, true);
        }
        catch (IOException)
        {
        }
    }

    private string WriteLog(string name, string content)
    {
        var path = Path.Combine(root, name + ".txt");
        File.WriteAllBytes(path, Util.AmatsukazeDefaultEncoding.GetBytes(content));
        return path;
    }

    [Fact]
    public void 一時フォルダ行からパスを抽出できる()
    {
        var tempDir = Path.Combine(root, "amt12345678");
        Directory.CreateDirectory(tempDir);
        var log = WriteLog("ok", "[設定]\r\n入力: Y:/ts/test.ts\r\n一時フォルダ: " + tempDir + "\r\n[本エンコーダ設定]\r\n");
        Assert.Equal(tempDir, ServerSupport.ExtractTempDirFromLog(log));
    }

    [Fact]
    public void 全角コロンと引用符付きでも抽出できる()
    {
        var tempDir = Path.Combine(root, "amt87654321");
        Directory.CreateDirectory(tempDir);
        var log = WriteLog("zenkaku", "一時フォルダ： \"" + tempDir + "\"\r\n");
        Assert.Equal(tempDir, ServerSupport.ExtractTempDirFromLog(log));
    }

    [Fact]
    public void 一時フォルダが消えていても絶対パスならフォールバックで返す()
    {
        var gone = Path.Combine(root, "amt99999999");
        var log = WriteLog("gone", "一時フォルダ: " + gone + "\r\n");
        Assert.Equal(gone, ServerSupport.ExtractTempDirFromLog(log));
    }

    [Fact]
    public void 複数行あるうち存在する方を優先する()
    {
        var existing = Path.Combine(root, "amt11111111");
        Directory.CreateDirectory(existing);
        var gone = Path.Combine(root, "amt22222222");
        var log = WriteLog("multi", "一時フォルダ: " + gone + "\r\n一時フォルダ: " + existing + "\r\n");
        Assert.Equal(existing, ServerSupport.ExtractTempDirFromLog(log));
    }

    [Fact]
    public void 一時フォルダ行がなければnull()
    {
        var log = WriteLog("none", "[設定]\r\n[本エンコーダ設定]\r\n");
        Assert.Null(ServerSupport.ExtractTempDirFromLog(log));
    }

    [Fact]
    public void 空の一時フォルダ行は無視する()
    {
        var log = WriteLog("empty", "一時フォルダ: \r\n");
        Assert.Null(ServerSupport.ExtractTempDirFromLog(log));
    }

    [Fact]
    public void ログファイルがなければnull()
    {
        Assert.Null(ServerSupport.ExtractTempDirFromLog(Path.Combine(root, "notexist.txt")));
    }
}
