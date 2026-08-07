using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using Amatsukaze.Server;

namespace AmatsukazeServerPass0Test
{
    internal static class Program
    {
        private static int Main()
        {
            Assert(AutoLogoPass0Validation.IsReadyVersion1("1"));
            Assert(AutoLogoPass0Validation.IsReadyVersion1("1\n"));
            Assert(AutoLogoPass0Validation.IsReadyVersion1("1\r\n"));
            Assert(!AutoLogoPass0Validation.IsReadyVersion1(" 1\n"));
            Assert(!AutoLogoPass0Validation.IsReadyVersion1("1\n\n"));
            Assert(AutoLogoPass0Validation.IsOwnerToken("0123456789abcdef0123456789abcdef"));
            Assert(!AutoLogoPass0Validation.IsOwnerToken("0123456789abcdef0123456789abcdeg"));
            Assert(!AutoLogoPass0Validation.IsOwnerToken("0123"));
            var profile = new ProfileSetting()
            {
                JLSCommandFile = "profile.txt",
                EnableJLSOption = true,
                JLSOption = "-p",
                ChapterExeOption = "--chapter-opt",
            };
            var service = new ServiceSettingElement()
            {
                DisableCMCheck = true,
                JLSCommand = "service.txt",
                JLSOption = "-s",
            };
            var cmPlan = EncodeServer.ResolvePass0CmAnalysisArgumentPlan(profile, service, VideoStreamFormat.H265);
            Assert(cmPlan.JlsCommand == "profile.txt");
            Assert(cmPlan.JlsOption == "-p");
            Assert(cmPlan.ChapterOption == "--chapter-opt");
            Assert(cmPlan.LoadV2);
            profile.JLSCommandFile = null;
            profile.EnableJLSOption = false;
            profile.EnablePmtCut = true;
            profile.PmtCutHeadRate = 50;
            profile.PmtCutTailRate = 25;
            cmPlan = EncodeServer.ResolvePass0CmAnalysisArgumentPlan(profile, service, VideoStreamFormat.H264);
            Assert(cmPlan.JlsCommand == "service.txt");
            Assert(cmPlan.JlsOption == "-s");
            Assert(!cmPlan.LoadV2);
            var args = EncodeServer.BuildAutoLogoPass0Arguments(
                "入力 \"quoted\".ts", 123, profile, cmPlan,
                "drcs map.txt", "chapter exe", "join logo", "JL", "tsreadex",
                "work path", "resume path", "resume path/pass0");
            Assert(Count(args, "--chapter") == 1);
            Assert(Count(args, "--no-logo-in-cm") == 1);
            Assert(Count(args, "--no-delogo") == 1);
            Assert(Count(args, "--auto-logo-detect") == 1 && ValueAfter(args, "--auto-logo-detect") == "0");
            Assert(Count(args, "--no-remove-tmp") == 1);
            Assert(ValueAfter(args, "-i") == "入力 \"quoted\".ts");
            Assert(ValueAfter(args, "--resume-dir") == "resume path");
            Assert(ValueAfter(args, "--logo-pass0-output") == "resume path/pass0");
            Assert(ValueAfter(args, "--pmt-cut") == "0.5:0.25");
            Assert(!args.Contains("--copy-trimavs") && !args.Contains("--logo") && !args.Contains("--pre-enc-bat") && !args.Contains("-o"));
            Assert(args.Contains("--jls-cmd") && ValueAfter(args, "--jls-cmd") == Path.Combine("JL", "service.txt"));
            AssertPass0Orchestration(null, false, false, false, "cleanup,legacy");
            AssertPass0Orchestration("artifact", false, false, false, "pass0,cleanup");
            AssertPass0Orchestration("artifact", true, false, false, "pass0,cleanup,legacy");
            AssertPass0Orchestration("artifact", false, true, false, "pass0,cleanup,fatal");
            AssertPass0Orchestration("artifact", false, false, true, "cleanup,canceled");
            var normalServer = (EncodeServer)RuntimeHelpers.GetUninitializedObject(typeof(EncodeServer));
            var normalSetting = new Setting()
            {
                WorkPath = "work", ChapterExePath = "chapter", JoinLogoScpPath = "jls",
                AutoLogoPendingDisabled = true, AffinitySetting = 0,
            };
            var appData = new EncodeServer.AppData()
            {
                setting = normalSetting,
                services = new ServiceSetting() { ServiceMap = new System.Collections.Generic.Dictionary<int, ServiceSettingElement>() },
            };
            typeof(EncodeServer).GetField("<AppData_>k__BackingField", BindingFlags.Instance | BindingFlags.NonPublic).SetValue(normalServer, appData);
            var normalProfile = new ProfileSetting() { DisableSubs = true, OutputMask = 1 };
            var normalArgs = normalServer.MakeAmatsukazeArgs(ProcMode.CMCheck, normalProfile, normalSetting, false,
                "input.ts", null, null, null, VideoStreamFormat.H264, 123, null, false,
                "service.txt", "-s", "--chapter-opt", null, null, "bat", null, null, 0);
            var expectedNormalArgs = " --mode cm -i \"input.ts\" -s 123 --drcs \"" +
                Path.Combine(Path.GetFullPath("drcs"), "drcs_map.txt") + "\" -w \"work\" --chapter-exe \"chapter\" --jls \"jls\" --cmoutmask 1 --chapter" +
                " --jls-cmd \"" + Path.Combine(Path.GetFullPath("JL"), "service.txt") + "\" --jls-option \"-s\" --chapter-exe-options \"--chapter-opt\" --auto-logo-detect 0";
            Assert(normalArgs == expectedNormalArgs);
            var parent = Path.Combine(Path.GetTempPath(), "amatsukaze-pass0-test-" + Guid.NewGuid().ToString("N"));
            var target = Path.Combine(parent, "logo-pass0-0123456789abcdef0123456789abcdef");
            Assert(AutoLogoPass0Validation.TryCreateDirectoryAtomically(parent));
            try
            {
                Assert(AutoLogoPass0Validation.TryCreateDirectoryAtomically(target));
                Assert(!AutoLogoPass0Validation.TryCreateDirectoryAtomically(target));
                var owner = "0123456789abcdef0123456789abcdef";
                File.WriteAllText(Path.Combine(target, ".logo-pass0-owner"), owner);
                Assert(AutoLogoPass0Validation.IsOwnedJob(target, owner));
                Assert(!AutoLogoPass0Validation.IsOwnedJob(target, "fedcba9876543210fedcba9876543210"));
                Assert(!AutoLogoPass0Validation.TryDeleteOwnedJob(target, "fedcba9876543210fedcba9876543210"));
                Assert(Directory.Exists(target));
                var artifact = Path.Combine(parent, "artifact");
                Directory.CreateDirectory(artifact);
                File.WriteAllText(Path.Combine(artifact, "pass0.ready"), "1\n");
                File.WriteAllText(Path.Combine(artifact, "pass0.amts"), "amts");
                File.WriteAllText(Path.Combine(artifact, "pass0.trim.avs"), "trim");
                Assert(AutoLogoPass0Validation.HasCompleteArtifact(artifact));
                File.WriteAllText(Path.Combine(artifact, "pass0.ready"), "1 \n");
                Assert(!AutoLogoPass0Validation.HasCompleteArtifact(artifact));
                File.WriteAllText(Path.Combine(artifact, "pass0.ready"), "1\n");
                if (OperatingSystem.IsLinux())
                {
                    File.Delete(Path.Combine(artifact, "pass0.amts"));
                    File.CreateSymbolicLink(Path.Combine(artifact, "pass0.amts"), Path.Combine(artifact, "pass0.trim.avs"));
                    Assert(!AutoLogoPass0Validation.HasCompleteArtifact(artifact));
                }
                Assert(AutoLogoPass0Validation.TryDeleteOwnedJob(target, owner));
                Assert(!Directory.Exists(target));
                var oldToken = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
                var oldPath = Path.Combine(parent, "logo-pass0-" + oldToken);
                Assert(AutoLogoPass0Validation.TryCreateDirectoryAtomically(oldPath));
                File.WriteAllText(Path.Combine(oldPath, ".logo-pass0-owner"), oldToken);
                Directory.SetCreationTimeUtc(oldPath, DateTime.UtcNow.AddDays(-2));
                Assert(AutoLogoPass0Validation.CanCollectOwnedJob(oldPath, DateTime.UtcNow));
                var newToken = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
                var newPath = Path.Combine(parent, "logo-pass0-" + newToken);
                Assert(AutoLogoPass0Validation.TryCreateDirectoryAtomically(newPath));
                File.WriteAllText(Path.Combine(newPath, ".logo-pass0-owner"), newToken);
                Assert(!AutoLogoPass0Validation.CanCollectOwnedJob(newPath, DateTime.UtcNow));
                if (OperatingSystem.IsLinux())
                {
                    var link = Path.Combine(parent, "logo-pass0-cccccccccccccccccccccccccccccccc");
                    Directory.CreateSymbolicLink(link, oldPath);
                    Assert(!AutoLogoPass0Validation.CanCollectOwnedJob(link, DateTime.UtcNow));
                }
                var foreignToken = "dddddddddddddddddddddddddddddddd";
                var foreignPath = Path.Combine(parent, "logo-pass0-" + foreignToken);
                Assert(AutoLogoPass0Validation.TryCreateDirectoryAtomically(foreignPath));
                var foreignMarker = Path.Combine(foreignPath, ".logo-pass0-owner");
                File.WriteAllText(foreignMarker, foreignToken);
                var foreignFile = Path.Combine(foreignPath, "external.txt");
                File.WriteAllText(foreignFile, "外部内容");
                AutoLogoPass0Validation.CleanupUnownedCreationCandidate(foreignPath, foreignMarker, foreignToken);
                Assert(Directory.Exists(foreignPath) && File.Exists(foreignFile) && !File.Exists(foreignMarker));
                var emptyPath = Path.Combine(parent, "logo-pass0-eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
                Assert(AutoLogoPass0Validation.TryCreateDirectoryAtomically(emptyPath));
                AutoLogoPass0Validation.CleanupUnownedCreationCandidate(emptyPath, Path.Combine(emptyPath, ".logo-pass0-owner"), "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
                Assert(!Directory.Exists(emptyPath));
            }
            finally
            {
                Directory.Delete(parent, true);
            }
            Console.WriteLine("AutoLogoPass0Validation: OK");
            return 0;
        }

        private static void Assert(bool condition)
        {
            if (!condition)
            {
                throw new InvalidOperationException("pass0検証テストが失敗しました");
            }
        }

        private static int Count(System.Collections.Generic.IReadOnlyList<string> args, string value)
        {
            var count = 0;
            foreach (var arg in args) if (arg == value) ++count;
            return count;
        }

        private static string ValueAfter(System.Collections.Generic.IReadOnlyList<string> args, string key)
        {
            for (var i = 0; i + 1 < args.Count; ++i) if (args[i] == key) return args[i + 1];
            return null;
        }

        private static void AssertPass0Orchestration(string artifact, bool oldSymbol, bool pass0Failure, bool canceled, string expected)
        {
            var calls = new System.Collections.Generic.List<string>();
            try
            {
                AutoLogoPass0Validation.ExecutePass0OrLegacy(
                    artifact,
                    value =>
                    {
                        calls.Add("pass0");
                        if (oldSymbol) throw new EntryPointNotFoundException();
                        if (pass0Failure) throw new InvalidOperationException("native failure");
                        return value;
                    },
                    () => { calls.Add("legacy"); return "legacy"; },
                    () => calls.Add("cleanup"),
                    () => canceled);
            }
            catch (EntryPointNotFoundException) { calls.Add("fatal"); }
            catch (InvalidOperationException) { calls.Add("fatal"); }
            catch (OperationCanceledException) { calls.Add("canceled"); }
            Assert(string.Join(",", calls) == expected);
        }
    }
}
