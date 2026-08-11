using System.Collections.Generic;

namespace Amatsukaze.Server.Update
{
    internal static class UpdateCatalog
    {
        private const string RigayaVersionPattern = @"^\S+ \(x(?:64|86)\) (?<ver>\d+\.\d+) \(r(?<rev>\d+)\)";

        public static IReadOnlyList<UpdateTargetDef> Targets { get; } = new[]
        {
            new UpdateTargetDef
            {
                Id = "Amatsukaze",
                DisplayName = "Amatsukaze 本体",
                Repository = "rigaya/Amatsukaze",
                ReleaseSelect = ReleaseSelectMode.Latest,
                AssetRules = new[]
                {
                    new AssetRule(UpdateOSKind.Windows, UpdateArchitecture.X64,
                        @"^Amatsukaze_(?<ver>[0-9]+(?:\.[0-9]+)+)\.7z$"),
                    new AssetRule(UpdateOSKind.Linux, UpdateArchitecture.X64,
                        @"^Amatsukaze_Ubuntu22\.04_(?<ver>[0-9]+(?:\.[0-9]+)+)\.tar\.xz$", "22.04"),
                    new AssetRule(UpdateOSKind.Linux, UpdateArchitecture.X64,
                        @"^Amatsukaze_Ubuntu24\.04_(?<ver>[0-9]+(?:\.[0-9]+)+)\.tar\.xz$", "24.04"),
                },
                WindowsLayout = InstallLayout.AppRootPartial,
                LinuxLayout = InstallLayout.AppRootPartial,
                RequiresRestart = true,
                IsApplication = true,
            },
            new UpdateTargetDef
            {
                Id = "x264",
                DisplayName = "x264",
                Repository = "rigaya/AutoBuildForAviUtlPlugins",
                ReleaseSelect = ReleaseSelectMode.LatestWithAsset,
                AssetRules = new[]
                {
                    new AssetRule(UpdateOSKind.Windows, UpdateArchitecture.X64,
                        @"^x264_(?<ver>\d+)_x64\.zip$"),
                    new AssetRule(UpdateOSKind.Linux, UpdateArchitecture.X64,
                        @"^x264_(?<ver>\d+)_amd64_linux\.tar\.xz$"),
                },
                WindowsLayout = InstallLayout.ExeFilesFlat,
                LinuxLayout = InstallLayout.ExeFilesFlat,
                VersionArgument = "--version",
                VersionPattern = @"^x264 \d+\.\d+\.(?<ver>\d+)",
                SettingKey = nameof(Setting.X264Path),
            },
            new UpdateTargetDef
            {
                Id = "x265",
                DisplayName = "x265",
                Repository = "rigaya/AutoBuildForAviUtlPlugins",
                ReleaseSelect = ReleaseSelectMode.LatestWithAsset,
                AssetRules = new[]
                {
                    new AssetRule(UpdateOSKind.Windows, UpdateArchitecture.X64,
                        @"^x265_(?<ver>[0-9]+\.[0-9]+\+[0-9]+)_x64\.zip$"),
                    new AssetRule(UpdateOSKind.Linux, UpdateArchitecture.X64,
                        @"^x265_(?<ver>[0-9]+\.[0-9]+\+[0-9]+)_amd64_linux\.tar\.xz$"),
                },
                WindowsLayout = InstallLayout.ExeFilesFlat,
                LinuxLayout = InstallLayout.ExeFilesFlat,
                VersionArgument = "--version",
                VersionPattern = @"HEVC encoder version (?<ver>[0-9]+\.[0-9]+\+[0-9]+)",
                SettingKey = nameof(Setting.X265Path),
            },
            new UpdateTargetDef
            {
                Id = "SVT-AV1",
                DisplayName = "SVT-AV1",
                Repository = "rigaya/AutoBuildForAviUtlPlugins",
                ReleaseSelect = ReleaseSelectMode.LatestWithAsset,
                AssetRules = new[]
                {
                    new AssetRule(UpdateOSKind.Windows, UpdateArchitecture.X64,
                        @"^SvtAv1EncApp_(?<ver>[0-9]+\.[0-9]+\.[0-9]+-[0-9]+)_x64_clang\.zip$"),
                    new AssetRule(UpdateOSKind.Linux, UpdateArchitecture.X64,
                        @"^SvtAv1EncApp_(?<ver>[0-9]+\.[0-9]+\.[0-9]+-[0-9]+)_amd64_linux_clang\.tar\.xz$"),
                },
                WindowsLayout = InstallLayout.ExeFilesFlat,
                LinuxLayout = InstallLayout.ExeFilesFlat,
                VersionArgument = "--version",
                VersionPattern = @"^SVT-AV1 v(?<ver>[0-9]+\.[0-9]+\.[0-9]+-[0-9]+)",
                SettingKey = nameof(Setting.SVTAV1Path),
            },
            CreateRigayaEncoder("QSVEnc", "QSVEnc", "rigaya/QSVEnc", "QSVEncC", "qsvencc", nameof(Setting.QSVEncPath)),
            CreateRigayaEncoder("NVEnc", "NVEnc", "rigaya/NVEnc", "NVEncC", "nvencc", nameof(Setting.NVEncPath)),
            CreateRigayaEncoder("VCEEnc", "VCEEnc", "rigaya/VCEEnc", "VCEEncC", "vceencc", nameof(Setting.VCEEncPath)),
            new UpdateTargetDef
            {
                Id = "tsreplace",
                DisplayName = "tsreplace",
                Repository = "rigaya/tsreplace",
                ReleaseSelect = ReleaseSelectMode.Latest,
                AssetRules = new[]
                {
                    new AssetRule(UpdateOSKind.Windows, UpdateArchitecture.X64,
                        @"^tsreplace_(?<ver>\d+\.\d+)_x64\.7z$"),
                    new AssetRule(UpdateOSKind.Linux, UpdateArchitecture.X64,
                        @"^tsreplace_(?<ver>\d+\.\d+)_amd64\.deb$"),
                    new AssetRule(UpdateOSKind.Linux, UpdateArchitecture.Arm64,
                        @"^tsreplace_(?<ver>\d+\.\d+)_arm64\.deb$"),
                },
                WindowsLayout = InstallLayout.ExeFilesSubDir,
                LinuxLayout = InstallLayout.ExeFilesFlat,
                VersionArgument = "--version",
                VersionPattern = RigayaVersionPattern,
                SettingKey = nameof(Setting.TsReplacePath),
            },
        };

        private static UpdateTargetDef CreateRigayaEncoder(string id, string displayName,
            string repository, string windowsName, string linuxName, string settingKey)
        {
            return new UpdateTargetDef
            {
                Id = id,
                DisplayName = displayName,
                Repository = repository,
                ReleaseSelect = ReleaseSelectMode.Latest,
                AssetRules = new[]
                {
                    new AssetRule(UpdateOSKind.Windows, UpdateArchitecture.X64,
                        $@"^{windowsName}_(?<ver>\d+\.\d+)_x64\.7z$"),
                    new AssetRule(UpdateOSKind.Linux, UpdateArchitecture.X64,
                        $@"^{linuxName}_(?<ver>\d+\.\d+)_amd64\.deb$"),
                },
                WindowsLayout = InstallLayout.ExeFilesSubDir,
                LinuxLayout = InstallLayout.ExeFilesFlat,
                VersionArgument = "--version",
                VersionPattern = RigayaVersionPattern,
                SettingKey = settingKey,
            };
        }

        public static bool TryInitialize(out string error)
        {
            foreach (var target in Targets)
            {
                if (!target.TryCompileRegexes(out error))
                {
                    return false;
                }
            }
            error = null;
            return true;
        }
    }
}
