using System.Xml.Linq;

namespace Amatsukaze.DockerConfigFix;

public sealed record RepairedSetting(string Name, string OldValue, string NewValue);

public static class DockerConfigRepair
{
    private sealed record RepairTarget(string SettingName, string ExecutableName);

    private static readonly RepairTarget[] Targets =
    {
        new("QSVEncPath", "qsvencc"),
        new("NVEncPath", "nvencc"),
        new("VCEEncPath", "vceencc"),
        new("TsReplacePath", "tsreplace"),
    };

    public static IReadOnlyList<RepairedSetting> Repair(
        string settingPath, string executableRoot, string? searchPath = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(settingPath);
        ArgumentException.ThrowIfNullOrWhiteSpace(executableRoot);

        var fullSettingPath = Path.GetFullPath(settingPath);
        if (!File.Exists(fullSettingPath))
        {
            return Array.Empty<RepairedSetting>();
        }

        var fullExecutableRoot = Path.GetFullPath(executableRoot);
        var document = XDocument.Load(fullSettingPath, LoadOptions.PreserveWhitespace);
        var setting = document.Descendants().FirstOrDefault(
            element => element.Name.LocalName == "setting");
        if (setting == null)
        {
            return Array.Empty<RepairedSetting>();
        }

        var repaired = new List<RepairedSetting>();
        foreach (var target in Targets)
        {
            var element = setting.Elements().FirstOrDefault(
                child => child.Name.LocalName == target.SettingName);
            if (element == null)
            {
                continue;
            }

            var ephemeralPath = Path.Combine(fullExecutableRoot, target.ExecutableName);
            if (!string.Equals(element.Value, ephemeralPath, StringComparison.Ordinal) ||
                File.Exists(ephemeralPath) ||
                !ExistsOnPath(target.ExecutableName, searchPath))
            {
                continue;
            }

            var oldValue = element.Value;
            element.Value = target.ExecutableName;
            repaired.Add(new RepairedSetting(target.SettingName, oldValue,
                target.ExecutableName));
        }

        if (repaired.Count != 0)
        {
            SaveAtomically(document, fullSettingPath);
        }
        return repaired;
    }

    private static bool ExistsOnPath(string executableName, string? searchPath)
    {
        searchPath ??= Environment.GetEnvironmentVariable("PATH");
        if (string.IsNullOrWhiteSpace(searchPath))
        {
            return false;
        }
        foreach (var directory in searchPath.Split(Path.PathSeparator,
            StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            try
            {
                if (File.Exists(Path.Combine(directory, executableName)))
                {
                    return true;
                }
            }
            catch
            {
                // 不正なPATH要素は無視して、残りを検索する。
            }
        }
        return false;
    }

    private static void SaveAtomically(XDocument document, string settingPath)
    {
        var temporaryPath = settingPath + ".tmp." + Guid.NewGuid().ToString("N");
        try
        {
            using (var stream = new FileStream(temporaryPath, FileMode.CreateNew,
                FileAccess.Write, FileShare.None))
            {
                document.Save(stream, SaveOptions.DisableFormatting);
                stream.Flush(flushToDisk: true);
            }
            PreserveUnixFileMode(settingPath, temporaryPath);
            File.Move(temporaryPath, settingPath, overwrite: true);
        }
        finally
        {
            try
            {
                File.Delete(temporaryPath);
            }
            catch
            {
                // 一時ファイルの後始末失敗は、補正結果に影響しないため無視する。
            }
        }
    }

    private static void PreserveUnixFileMode(string sourcePath, string destinationPath)
    {
        if (OperatingSystem.IsWindows())
        {
            return;
        }
        try
        {
            File.SetUnixFileMode(destinationPath, File.GetUnixFileMode(sourcePath));
        }
        catch
        {
            // マウント先がモード変更を扱えない場合は、作成時のモードを使用する。
        }
    }
}
