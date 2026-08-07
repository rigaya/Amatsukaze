using System;
using System.IO;
using System.Runtime.InteropServices;

namespace Amatsukaze.Server
{
    // pass0成果物の完了マーカーと所有tokenを、副作用なしで検証する。
    internal static class AutoLogoPass0Validation
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool CreateDirectory(string pathName, IntPtr securityAttributes);

        [DllImport("libc", SetLastError = true)]
        private static extern int mkdir(string pathName, uint mode);

        internal static bool IsReadyVersion1(string text)
        {
            return text == "1" || text == "1\n" || text == "1\r\n";
        }

        internal static bool IsOwnerToken(string token)
        {
            return !string.IsNullOrEmpty(token) && token.Length == 32 && Guid.TryParseExact(token, "N", out _);
        }

        // 既存フォルダを成功扱いにしないOSのatomic mkdir。
        internal static bool TryCreateDirectoryAtomically(string path)
        {
            if (OperatingSystem.IsWindows())
            {
                return CreateDirectory(path, IntPtr.Zero);
            }
            if (OperatingSystem.IsLinux())
            {
                return mkdir(path, 0x1c0) == 0; // 0700
            }
            return false;
        }

        internal static bool IsRegularFile(string path)
        {
            try
            {
                var attributes = File.GetAttributes(path);
                return (attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) == 0;
            }
            catch (IOException)
            {
                return false;
            }
            catch (UnauthorizedAccessException)
            {
                return false;
            }
        }

        internal static bool HasCompleteArtifact(string directoryPath)
        {
            var ready = Path.Combine(directoryPath, "pass0.ready");
            var amts = Path.Combine(directoryPath, "pass0.amts");
            var trim = Path.Combine(directoryPath, "pass0.trim.avs");
            try
            {
                return IsRegularFile(ready) && IsReadyVersion1(File.ReadAllText(ready)) &&
                    IsRegularFile(amts) && IsRegularFile(trim);
            }
            catch (IOException)
            {
                return false;
            }
            catch (UnauthorizedAccessException)
            {
                return false;
            }
        }

        internal static bool IsOwnedJob(string directoryPath, string token)
        {
            if (!IsOwnerToken(token)) return false;
            var marker = Path.Combine(directoryPath, ".logo-pass0-owner");
            try
            {
                return IsRegularFile(marker) && File.ReadAllText(marker) == token;
            }
            catch (IOException)
            {
                return false;
            }
        }

        internal static bool TryDeleteOwnedJob(string directoryPath, string token)
        {
            try
            {
                if ((File.GetAttributes(directoryPath) & FileAttributes.ReparsePoint) != 0 || !IsOwnedJob(directoryPath, token))
                {
                    return false;
                }
                Directory.Delete(directoryPath, true);
                return true;
            }
            catch (IOException)
            {
                return false;
            }
            catch (UnauthorizedAccessException)
            {
                return false;
            }
        }

        // 作成直後に所有を確立できなかった候補だけを、markerと空ディレクトリに限って片付ける。
        internal static void CleanupUnownedCreationCandidate(string directoryPath, string markerPath, string token)
        {
            try
            {
                if (IsRegularFile(markerPath) && File.ReadAllText(markerPath) == token)
                {
                    File.Delete(markerPath);
                }
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
            try
            {
                // 外部内容がある場合は失敗する非再帰削除だけを許可する。
                Directory.Delete(directoryPath, false);
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
        }

        internal static bool CanCollectOwnedJob(string directoryPath, DateTime utcNow)
        {
            try
            {
                var name = Path.GetFileName(directoryPath);
                const string prefix = "logo-pass0-";
                if (!name.StartsWith(prefix, StringComparison.Ordinal) ||
                    (File.GetAttributes(directoryPath) & FileAttributes.ReparsePoint) != 0)
                {
                    return false;
                }
                var token = name.Substring(prefix.Length);
                return IsOwnedJob(directoryPath, token) && Directory.GetCreationTimeUtc(directoryPath) <= utcNow.AddHours(-24);
            }
            catch (IOException)
            {
                return false;
            }
            catch (UnauthorizedAccessException)
            {
                return false;
            }
        }

        // pass0成果物の利用、旧symbolフォールバック、cleanup順序を一箇所へ固定する。
        internal static TResult ExecutePass0OrLegacy<TArtifact, TResult>(TArtifact artifact,
            Func<TArtifact, TResult> executePass0, Func<TResult> executeLegacy, Action cleanup, Func<bool> isCanceled)
            where TArtifact : class
        {
            var cleaned = false;
            void CleanupOnce()
            {
                if (!cleaned)
                {
                    cleaned = true;
                    cleanup();
                }
            }
            try
            {
                if (isCanceled()) throw new OperationCanceledException();
                if (artifact == null)
                {
                    CleanupOnce();
                    if (isCanceled()) throw new OperationCanceledException();
                    return executeLegacy();
                }
                try
                {
                    return executePass0(artifact);
                }
                catch (EntryPointNotFoundException)
                {
                    CleanupOnce();
                    if (isCanceled()) throw new OperationCanceledException();
                    return executeLegacy();
                }
            }
            finally
            {
                CleanupOnce();
            }
        }
    }
}
