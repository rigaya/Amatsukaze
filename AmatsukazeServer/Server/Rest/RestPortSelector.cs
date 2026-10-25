using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace Amatsukaze.Server.Rest
{
    public enum PortBindResult
    {
        Free,
        InUse,
        AccessDenied,
        Invalid
    }

    // Windowsの除外ポート範囲（netshの開始〜終了、両端を含む）。
    public readonly struct PortRange
    {
        public PortRange(int start, int end)
        {
            if (end < start)
            {
                var swap = start;
                start = end;
                end = swap;
            }
            Start = start;
            End = end;
        }

        public int Start { get; }
        public int End { get; }

        public bool Contains(int port)
        {
            return port >= Start && port <= End;
        }
    }

    // RESTの待ち受けポートを決める。
    // スタンドアロンは除外範囲を飛ばしたあと、使用中と10013を同じように次へ進めて空きを探す。
    // サーバー/CLIは指定ポートだけを使い、除外範囲に入っていれば候補を案内する。
    public static class RestPortSelector
    {
        public const int MaxProbeAttempts = 96;
        public const int MaxStride = 256;
        // この回数までは10013でも1つずつ進む。以降は連続する除外ブロックを飛ばす。
        public const int AccessDeniedStrideThreshold = 8;
        private const int NetshTimeoutMs = 5000;

        private static readonly Regex RangePattern = new Regex(
            @"^\s*(\d+)\s+(\d+)",
            RegexOptions.Multiline | RegexOptions.CultureInvariant | RegexOptions.Compiled);

        public static IReadOnlyList<PortRange> ParseExcludedPortRanges(string netshOutput)
        {
            var ranges = new List<PortRange>();
            if (string.IsNullOrEmpty(netshOutput))
            {
                return ranges;
            }
            foreach (Match match in RangePattern.Matches(netshOutput))
            {
                if (!int.TryParse(match.Groups[1].Value, out var start) ||
                    !int.TryParse(match.Groups[2].Value, out var end))
                {
                    continue;
                }
                if (start <= 0 || end <= 0 || start > 65535 || end > 65535)
                {
                    continue;
                }
                ranges.Add(new PortRange(start, end));
            }
            return ranges;
        }

        // 表示の取得に失敗したときは false。Windows以外は範囲なしで成功とする。
        public static bool TryQueryWindowsTcp(out IReadOnlyList<PortRange> ranges, out string error)
        {
            ranges = Array.Empty<PortRange>();
            error = null;
            if (!OperatingSystem.IsWindows())
            {
                return true;
            }
            try
            {
                var psi = new ProcessStartInfo
                {
                    FileName = "netsh",
                    Arguments = "interface ipv4 show excludedportrange protocol=tcp",
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    // 見出しは環境の文字コードだが、ポート番号はASCIIのまま読める。
                    StandardOutputEncoding = new UTF8Encoding(false, false),
                    StandardErrorEncoding = new UTF8Encoding(false, false)
                };
                using var process = new Process { StartInfo = psi };
                if (!process.Start())
                {
                    error = "netshを起動できませんでした。";
                    return false;
                }
                // UIスレッドのSynchronizationContextで待たないよう、読み取りはスレッドプールで行う。
                var stdoutTask = Task.Run(() => process.StandardOutput.ReadToEnd());
                var stderrTask = Task.Run(() => process.StandardError.ReadToEnd());
                if (!process.WaitForExit(NetshTimeoutMs))
                {
                    try
                    {
                        process.Kill(entireProcessTree: true);
                    }
                    catch
                    {
                    }
                    try
                    {
                        Task.WaitAll(new[] { stdoutTask, stderrTask }, 1000);
                    }
                    catch
                    {
                    }
                    error = "netshが時間内に終了しませんでした。";
                    return false;
                }
                process.WaitForExit();
                Task.WaitAll(stdoutTask, stderrTask);
                ranges = ParseExcludedPortRanges(stdoutTask.Result);
                return true;
            }
            catch (Exception ex)
            {
                error = ex.Message;
                return false;
            }
        }

        public static bool TryGetContainingRange(IReadOnlyList<PortRange> ranges, int port, out PortRange range)
        {
            if (ranges != null)
            {
                for (var i = 0; i < ranges.Count; i++)
                {
                    if (ranges[i].Contains(port))
                    {
                        range = ranges[i];
                        return true;
                    }
                }
            }
            range = default;
            return false;
        }

        // 保存ポートが使えればそれを返し、使えなければ開始ポートから空きを探す。
        // preferredDiscarded は、保存ポートを試して使えなかったときだけ true。
        public static int FindAutoPort(
            int startPort,
            int avoidPort,
            int preferredPort,
            IReadOnlyList<PortRange> ranges,
            Func<int, PortBindResult> probe,
            out bool preferredDiscarded)
        {
            preferredDiscarded = false;
            if (preferredPort > 0)
            {
                if (!IsCandidate(preferredPort, avoidPort) || probe(preferredPort) != PortBindResult.Free)
                {
                    preferredDiscarded = true;
                }
                else
                {
                    return preferredPort;
                }
            }
            return ScanAvailablePort(startPort, avoidPort, ranges, probe);
        }

        public static int ScanAvailablePort(
            int startPort,
            int avoidPort,
            IReadOnlyList<PortRange> ranges,
            Func<int, PortBindResult> probe)
        {
            if (probe == null)
            {
                return 0;
            }
            var port = AdvancePastObstacles(startPort, avoidPort, ranges);
            var attempts = 0;
            var deniedStreak = 0;
            var stride = 1;
            var previous = 0;
            while (attempts < MaxProbeAttempts && port >= 1 && port <= 65535)
            {
                attempts++;
                var result = probe(port);
                if (result == PortBindResult.Free)
                {
                    // 幅を広げて飛んだ場合は、直前の失敗位置の次から最初の空きへ戻す。
                    if (stride > 1 && previous > 0 && previous < port)
                    {
                        return EarliestFree(previous + 1, port, avoidPort, ranges, probe);
                    }
                    return port;
                }
                if (result == PortBindResult.AccessDenied)
                {
                    deniedStreak++;
                    if (deniedStreak >= AccessDeniedStrideThreshold)
                    {
                        stride = Math.Min(Math.Max(stride * 2, 2), MaxStride);
                    }
                }
                else
                {
                    deniedStreak = 0;
                    stride = 1;
                }
                previous = port;
                port = AdvancePastObstacles(port + stride, avoidPort, ranges);
            }
            return 0;
        }

        // 飛んだ区間の中で、最初に空いているポートを返す。見つからなければ found を使う。
        private static int EarliestFree(
            int first,
            int found,
            int avoidPort,
            IReadOnlyList<PortRange> ranges,
            Func<int, PortBindResult> probe)
        {
            var port = AdvancePastObstacles(first, avoidPort, ranges);
            var extra = 0;
            while (port < found && extra < MaxStride)
            {
                extra++;
                if (probe(port) == PortBindResult.Free)
                {
                    return port;
                }
                port = AdvancePastObstacles(port + 1, avoidPort, ranges);
            }
            return found;
        }

        public static string BuildUnavailableMessage(
            int port,
            int serverPort,
            PortBindResult reason,
            IReadOnlyList<PortRange> ranges,
            Func<int, PortBindResult> probe)
        {
            if (TryGetContainingRange(ranges, port, out var range))
            {
                var suggested = ScanAvailablePort(range.End + 1, serverPort, ranges, probe);
                if (suggested > 0)
                {
                    return $"REST APIポート {port} は Windows の除外ポート範囲 {range.Start}-{range.End} に含まれているため待ち受けできません。" +
                        $"{suggested} であれば回避できます。環境変数 AMT_REST_PORT に {suggested} を設定するか、サーバーポートを変更してください。";
                }
                return $"REST APIポート {port} は Windows の除外ポート範囲 {range.Start}-{range.End} に含まれているため待ち受けできません。" +
                    "環境変数 AMT_REST_PORT で除外範囲の外のポートを指定するか、サーバーポートを変更してください。";
            }
            if (reason == PortBindResult.AccessDenied)
            {
                return $"REST APIポート {port} はアクセスが拒否されたため待ち受けできません。" +
                    "Windows の除外ポート範囲に含まれている可能性があります。環境変数 AMT_REST_PORT で別のポートを指定するか、サーバーポートを変更してください。";
            }
            return $"REST APIポート {port} は使用中のため待ち受けできません。" +
                "環境変数 AMT_REST_PORT で別のポートを指定するか、サーバーポートを変更してください。";
        }

        // 本番の待ち受けと同じ 0.0.0.0 で空いているかを調べる。
        public static PortBindResult ProbeTcpPort(int port)
        {
            if (port <= 0 || port > 65535)
            {
                return PortBindResult.Invalid;
            }
            TcpListener listener = null;
            try
            {
                listener = new TcpListener(IPAddress.Any, port);
                listener.Start();
                return PortBindResult.Free;
            }
            catch (Exception ex) when (IsAccessDenied(ex))
            {
                return PortBindResult.AccessDenied;
            }
            catch (SocketException)
            {
                return PortBindResult.InUse;
            }
            catch (Exception)
            {
                return PortBindResult.InUse;
            }
            finally
            {
                try
                {
                    listener?.Stop();
                }
                catch
                {
                }
            }
        }

        public static PortBindResult ClassifyBindFailure(Exception ex)
        {
            if (IsAccessDenied(ex))
            {
                return PortBindResult.AccessDenied;
            }
            if (IsAddressAlreadyInUse(ex))
            {
                return PortBindResult.InUse;
            }
            return PortBindResult.Invalid;
        }

        public static bool IsRetryableBindFailure(Exception ex)
        {
            var result = ClassifyBindFailure(ex);
            return result == PortBindResult.InUse || result == PortBindResult.AccessDenied;
        }

        public static bool IsAccessDenied(Exception ex)
        {
            for (var current = ex; current != null; current = current.InnerException)
            {
                if (current is SocketException socketEx &&
                    (socketEx.SocketErrorCode == SocketError.AccessDenied || socketEx.ErrorCode == 10013))
                {
                    return true;
                }
                if (!string.IsNullOrEmpty(current.Message) &&
                    (current.Message.IndexOf("forbidden by its access permissions", StringComparison.OrdinalIgnoreCase) >= 0 ||
                     current.Message.IndexOf("アクセス許可で禁止", StringComparison.Ordinal) >= 0))
                {
                    return true;
                }
            }
            return false;
        }

        public static bool IsAddressAlreadyInUse(Exception ex)
        {
            for (var current = ex; current != null; current = current.InnerException)
            {
                if (current is SocketException socketEx &&
                    (socketEx.SocketErrorCode == SocketError.AddressAlreadyInUse || socketEx.ErrorCode == 10048))
                {
                    return true;
                }
                if (current.GetType().Name.IndexOf("AddressInUse", StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    return true;
                }
                if (!string.IsNullOrEmpty(current.Message) &&
                    current.Message.IndexOf("address already in use", StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    return true;
                }
            }
            return false;
        }

        private static bool IsCandidate(int port, int avoidPort)
        {
            return port > 0 && port <= 65535 && port != avoidPort;
        }

        // サーバーポート自身と、分かっている除外範囲の終わりまで進める。
        private static int AdvancePastObstacles(int port, int avoidPort, IReadOnlyList<PortRange> ranges)
        {
            var guard = 0;
            while (port >= 1 && port <= 65535 && guard++ < 10000)
            {
                if (avoidPort > 0 && port == avoidPort)
                {
                    port++;
                    continue;
                }
                if (TryGetContainingRange(ranges, port, out var range))
                {
                    port = range.End + 1;
                    continue;
                }
                return port;
            }
            return port;
        }
    }
}
