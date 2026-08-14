using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;

namespace Amatsukaze.Server.Update
{
    internal enum UpdateOSKind
    {
        Windows,
        Linux,
    }

    internal enum UpdateArchitecture
    {
        X64,
        Arm64,
        Other,
    }

    internal enum ReleaseSelectMode
    {
        Latest,
        LatestWithAsset,
    }

    internal enum InstallLayout
    {
        ExeFilesFlat,
        ExeFilesSubDir,
        AppRootPartial,
    }

    internal enum UpdateTargetStatus
    {
        Unknown,
        UpToDate,
        UpdateAvailable,
        NotInstalled,
        Unsupported,
        Disabled,
    }

    internal sealed class UpdateRuntimeEnvironment
    {
        public UpdateOSKind OS { get; init; }
        public UpdateArchitecture Architecture { get; init; }
        public string DistributionVersion { get; init; }
        public bool IsDocker { get; init; }

        public static UpdateRuntimeEnvironment Detect()
        {
            var os = OperatingSystem.IsWindows() ? UpdateOSKind.Windows : UpdateOSKind.Linux;
            var architecture = RuntimeInformation.OSArchitecture switch
            {
                System.Runtime.InteropServices.Architecture.X64 => UpdateArchitecture.X64,
                System.Runtime.InteropServices.Architecture.Arm64 => UpdateArchitecture.Arm64,
                _ => UpdateArchitecture.Other,
            };
            return new UpdateRuntimeEnvironment
            {
                OS = os,
                Architecture = architecture,
                DistributionVersion = UpdateDiagnostics.GetLinuxDistributionVersion(),
                IsDocker = UpdateDiagnostics.IsDockerEnvironment(),
            };
        }
    }

    internal sealed class AssetRule
    {
        private Regex regex;

        public UpdateOSKind OS { get; }
        public UpdateArchitecture Architecture { get; }
        public string DistributionVersion { get; }
        public string Pattern { get; }

        public AssetRule(UpdateOSKind os, UpdateArchitecture architecture, string pattern,
            string distributionVersion = null)
        {
            OS = os;
            Architecture = architecture;
            Pattern = pattern;
            DistributionVersion = distributionVersion;
        }

        public bool TryCompile(out string error)
        {
            error = null;
            if (regex != null)
            {
                return true;
            }
            try
            {
                var compiled = new Regex(Pattern,
                    RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.Multiline,
                    TimeSpan.FromSeconds(1));
                if (!compiled.GetGroupNames().Contains("ver"))
                {
                    error = "asset_regex_missing_ver_group";
                    return false;
                }
                regex = compiled;
                return true;
            }
            catch (Exception ex)
            {
                error = ex.GetType().Name + ":" + ex.Message;
                return false;
            }
        }

        public bool AppliesTo(UpdateRuntimeEnvironment environment)
        {
            return OS == environment.OS && Architecture == environment.Architecture &&
                (string.IsNullOrEmpty(DistributionVersion) ||
                 string.Equals(DistributionVersion, environment.DistributionVersion,
                     StringComparison.OrdinalIgnoreCase));
        }

        public Match Match(string assetName)
        {
            return regex?.Match(assetName ?? string.Empty) ?? System.Text.RegularExpressions.Match.Empty;
        }
    }

    // アーカイブからステージング対象として取り出すファイルを宣言する。
    internal sealed class PayloadEntry
    {
        private Regex regex;

        // '/' 区切りへ正規化したアーカイブ内相対パスに対する正規表現。
        public string Pattern { get; init; }
        // 配置後の名前。null の場合はアーカイブ内のファイル名を維持する。
        public string DestName { get; init; }

        public bool TryCompile(out string error)
        {
            error = null;
            if (regex != null)
            {
                return true;
            }
            try
            {
                regex = new Regex(Pattern,
                    RegexOptions.Compiled | RegexOptions.CultureInvariant,
                    TimeSpan.FromSeconds(1));
                return true;
            }
            catch (Exception ex)
            {
                error = ex.GetType().Name + ":" + ex.Message;
                return false;
            }
        }

        public bool IsMatch(string relativePath)
        {
            if (regex == null)
            {
                throw new InvalidOperationException("Payload の正規表現がコンパイルされていません");
            }
            return regex.IsMatch(relativePath ?? string.Empty);
        }
    }

    internal sealed class UpdateTargetDef
    {
        private Regex versionRegex;

        public string Id { get; init; }
        public string DisplayName { get; init; }
        public string Repository { get; init; }
        public ReleaseSelectMode ReleaseSelect { get; init; }
        public IReadOnlyList<AssetRule> AssetRules { get; init; }
        public InstallLayout WindowsLayout { get; init; }
        public InstallLayout LinuxLayout { get; init; }
        public string VersionArgument { get; init; }
        public string VersionPattern { get; init; }
        public string SettingKey { get; init; }
        public bool RequiresRestart { get; init; }
        public bool IsApplication { get; init; }
        // Linux 版がシステムのドライバスタック（パッケージマネージャでしか導入できないもの）を
        // 必要とする対象。deb から実行ファイルだけを取り出す本経路では依存が揃わないため、
        // 新規インストールは許可しない。依存が揃っている既存環境の更新は可能。
        public bool RequiresSystemDependencies { get; init; }
        public PayloadEntry[] Payload { get; init; }

        public Regex VersionRegex => versionRegex;

        public InstallLayout GetInstallLayout(UpdateOSKind os)
        {
            return os == UpdateOSKind.Windows ? WindowsLayout : LinuxLayout;
        }

        public bool TryCompileRegexes(out string error)
        {
            error = null;
            foreach (var rule in AssetRules)
            {
                if (!rule.TryCompile(out error))
                {
                    error = $"target={Id} regex={rule.Pattern} error={error}";
                    return false;
                }
            }
            if (Payload != null)
            {
                foreach (var entry in Payload)
                {
                    if (entry == null || !entry.TryCompile(out error))
                    {
                        error = $"target={Id} payload_regex={entry?.Pattern ?? "?"} error={error ?? "null_entry"}";
                        return false;
                    }
                }
            }
            if (IsApplication)
            {
                return true;
            }
            try
            {
                var compiled = new Regex(VersionPattern,
                    RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.Multiline,
                    TimeSpan.FromSeconds(1));
                if (!compiled.GetGroupNames().Contains("ver"))
                {
                    error = $"target={Id} version_regex_missing_ver_group";
                    return false;
                }
                versionRegex = compiled;
                return true;
            }
            catch (Exception ex)
            {
                error = $"target={Id} regex={VersionPattern} error={ex.GetType().Name}:{ex.Message}";
                return false;
            }
        }

        public string GetExecutablePath(Setting setting)
        {
            return SettingKey switch
            {
                nameof(Setting.X264Path) => setting.X264Path,
                nameof(Setting.X265Path) => setting.X265Path,
                nameof(Setting.SVTAV1Path) => setting.SVTAV1Path,
                nameof(Setting.QSVEncPath) => setting.QSVEncPath,
                nameof(Setting.NVEncPath) => setting.NVEncPath,
                nameof(Setting.VCEEncPath) => setting.VCEEncPath,
                nameof(Setting.TsReplacePath) => setting.TsReplacePath,
                _ => null,
            };
        }

        public bool SetExecutablePath(Setting setting, string path)
        {
            switch (SettingKey)
            {
                case nameof(Setting.X264Path): setting.X264Path = path; return true;
                case nameof(Setting.X265Path): setting.X265Path = path; return true;
                case nameof(Setting.SVTAV1Path): setting.SVTAV1Path = path; return true;
                case nameof(Setting.QSVEncPath): setting.QSVEncPath = path; return true;
                case nameof(Setting.NVEncPath): setting.NVEncPath = path; return true;
                case nameof(Setting.VCEEncPath): setting.VCEEncPath = path; return true;
                case nameof(Setting.TsReplacePath): setting.TsReplacePath = path; return true;
                default: return false;
            }
        }
    }

    internal sealed class UpdateTargetState
    {
        public string Id { get; init; }
        public string DisplayName { get; init; }
        public string CurrentVersion { get; init; }
        public string LatestVersion { get; init; }
        public UpdateTargetStatus Status { get; init; }
        public string Reason { get; init; }
        public ReleaseAssetInfo SelectedAsset { get; init; }
        public string ReleaseUrl { get; init; }
        public DateTime CheckedAtUtc { get; init; }
    }
}
