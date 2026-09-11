using Amatsukaze.DockerConfigFix;
using System.Xml.Linq;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class DockerConfigRepairTests : IDisposable
{
    private readonly string root = Path.Combine(Path.GetTempPath(),
        "AmatsukazeDockerConfigRepairTests", Guid.NewGuid().ToString("N"));

    [Fact]
    public void 消えた更新版の設定をPATH上のコマンド名へ戻す()
    {
        var executableRoot = Directory.CreateDirectory(
            Path.Combine(root, "app", "exe_files")).FullName;
        var systemBin = Directory.CreateDirectory(Path.Combine(root, "usr", "bin")).FullName;
        foreach (var name in new[] { "qsvencc", "nvencc", "vceencc", "tsreplace" })
        {
            File.WriteAllText(Path.Combine(systemBin, name), string.Empty);
        }
        var settingPath = WriteSetting(
            ("QSVEncPath", Path.Combine(executableRoot, "qsvencc")),
            ("NVEncPath", Path.Combine(executableRoot, "nvencc")),
            ("VCEEncPath", Path.Combine(executableRoot, "vceencc")),
            ("TsReplacePath", Path.Combine(executableRoot, "tsreplace")));

        var repaired = DockerConfigRepair.Repair(settingPath, executableRoot, systemBin);

        Assert.Equal(4, repaired.Count);
        Assert.Equal("qsvencc", ReadValue(settingPath, "QSVEncPath"));
        Assert.Equal("nvencc", ReadValue(settingPath, "NVEncPath"));
        Assert.Equal("vceencc", ReadValue(settingPath, "VCEEncPath"));
        Assert.Equal("tsreplace", ReadValue(settingPath, "TsReplacePath"));
        Assert.Equal("保持", ReadValue(settingPath, "UnknownSetting"));
    }

    [Fact]
    public void 更新版が残っている設定は変更しない()
    {
        var executableRoot = Directory.CreateDirectory(
            Path.Combine(root, "app", "exe_files")).FullName;
        var systemBin = Directory.CreateDirectory(Path.Combine(root, "usr", "bin")).FullName;
        var ephemeralPath = Path.Combine(executableRoot, "qsvencc");
        File.WriteAllText(ephemeralPath, string.Empty);
        File.WriteAllText(Path.Combine(systemBin, "qsvencc"), string.Empty);
        var settingPath = WriteSetting(("QSVEncPath", ephemeralPath));

        var repaired = DockerConfigRepair.Repair(settingPath, executableRoot, systemBin);

        Assert.Empty(repaired);
        Assert.Equal(ephemeralPath, ReadValue(settingPath, "QSVEncPath"));
    }

    [Fact]
    public void カスタムパスと代替コマンドがない設定は変更しない()
    {
        var executableRoot = Directory.CreateDirectory(
            Path.Combine(root, "app", "exe_files")).FullName;
        var emptyBin = Directory.CreateDirectory(Path.Combine(root, "empty-bin")).FullName;
        var customPath = Path.Combine(root, "custom", "qsvencc");
        var missingEphemeralPath = Path.Combine(executableRoot, "nvencc");
        var settingPath = WriteSetting(
            ("QSVEncPath", customPath), ("NVEncPath", missingEphemeralPath));

        var repaired = DockerConfigRepair.Repair(settingPath, executableRoot, emptyBin);

        Assert.Empty(repaired);
        Assert.Equal(customPath, ReadValue(settingPath, "QSVEncPath"));
        Assert.Equal(missingEphemeralPath, ReadValue(settingPath, "NVEncPath"));
    }

    private string WriteSetting(params (string Name, string Value)[] values)
    {
        Directory.CreateDirectory(root);
        XNamespace ns = "http://schemas.datacontract.org/2004/07/Amatsukaze.Server";
        var setting = new XElement(ns + "setting",
            values.Select(value => new XElement(ns + value.Name, value.Value)),
            new XElement(ns + "UnknownSetting", "保持"));
        var document = new XDocument(new XElement(ns + "AppData", setting));
        var path = Path.Combine(root, "AmatsukazeServer.xml");
        document.Save(path);
        return path;
    }

    private static string? ReadValue(string path, string name) =>
        XDocument.Load(path).Descendants().FirstOrDefault(
            element => element.Name.LocalName == name)?.Value;

    public void Dispose()
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}
