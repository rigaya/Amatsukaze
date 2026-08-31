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
                Payload = new[]
                {
                    new PayloadEntry { Pattern = @"^x264$" },
                    new PayloadEntry { Pattern = @"^x264_.+_x64\.exe$" },
                },
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
                Payload = new[]
                {
                    new PayloadEntry { Pattern = @"^x265$" },
                    new PayloadEntry { Pattern = @"^x265_.+_x64\.exe$" },
                },
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
                VersionPattern = SvtAv1Version.VersionOutputPattern,
                SettingKey = nameof(Setting.SVTAV1Path),
                Payload = new[]
                {
                    new PayloadEntry { Pattern = @"^SvtAv1EncApp$" },
                    new PayloadEntry { Pattern = @"^SvtAv1EncApp_.+\.exe$" },
                },
            },
            CreateRigayaEncoder("QSVEnc", "QSVEnc", "rigaya/QSVEnc", "QSVEncC", "qsvencc", nameof(Setting.QSVEncPath)),
            CreateRigayaEncoder("NVEnc", "NVEnc", "rigaya/NVEnc", "NVEncC", "nvencc", nameof(Setting.NVEncPath)),
            CreateRigayaEncoder("VCEEnc", "VCEEnc", "rigaya/VCEEnc", "VCEEncC", "vceencc", nameof(Setting.VCEEncPath)),
            // tsreplace の deb は libc6 / libgcc-s1 しか要求せず、実行ファイルも
            // libc / libm / libpthread しかリンクしていない。ドライバスタックに依存しないので
            // 実行ファイルを設置するだけで動く（= 新規インストール可）。
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
                Payload = new[]
                {
                    new PayloadEntry { Pattern = @"^(?:usr/bin/)?tsreplace$" },
                    new PayloadEntry { Pattern = @"^tsreplace\.exe$" },
                },
            },
        };

        // ハードウェアエンコーダはいずれも GPU のドライバスタックを必要とする。
        // QSVEnc は intel-media-va-driver / libmfx、VCEEnc は amf-amdgpu-pro を
        // deb の依存として要求し、NVEnc は依存宣言こそ libc6 のみだが
        // 実行ファイルが libcuda.so.1 を直接リンクしている。
        // いずれも実行ファイルの設置だけでは動かないため、新規インストールは許可しない。
        private static UpdateTargetDef CreateRigayaEncoder(string id, string displayName,
            string repository, string windowsName, string linuxName, string settingKey)
        {
            return new UpdateTargetDef
            {
                Id = id,
                DisplayName = displayName,
                Repository = repository,
                RequiresSystemDependencies = true,
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
                Payload = new[]
                {
                    new PayloadEntry { Pattern = $@"^(?:usr/bin/)?{linuxName}$" },
                    new PayloadEntry { Pattern = $@"^{windowsName}64\.exe$" },
                },
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
