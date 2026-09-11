using Amatsukaze.DockerConfigFix;

const string DefaultSettingPath = "/app/config/AmatsukazeServer.xml";
const string DefaultExecutableRoot = "/app/exe_files";

if (args.Length > 2)
{
    Console.Error.WriteLine(
        "使用方法: AmatsukazeDockerConfigFix [設定ファイル [実行ファイルディレクトリ]]");
    return 2;
}

var settingPath = args.Length >= 1 ? args[0] : DefaultSettingPath;
var executableRoot = args.Length >= 2 ? args[1] : DefaultExecutableRoot;
try
{
    var repaired = DockerConfigRepair.Repair(settingPath, executableRoot);
    foreach (var item in repaired)
    {
        Console.WriteLine(
            $"Docker再作成後の設定を補正しました: {item.Name}={item.OldValue} -> {item.NewValue}");
    }
    return 0;
}
catch (Exception ex)
{
    Console.Error.WriteLine($"Docker設定の補正に失敗しました: {ex.Message}");
    return 1;
}
