using Amatsukaze.Lib;
using System;
using System.Collections.Generic;
using System.Collections.Concurrent;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Threading;
using System.Xml.Linq;

namespace Amatsukaze.Server
{
    // 別局の先行放送と、同じフォルダにあるその録画を探す。
    public sealed class NicoJKReferenceFinder
    {
        private const int HttpTimeoutSeconds = 10;
        private const int RequestIntervalSeconds = 10;
        private const int SearchRangeMinutes = 30;
        private const int TargetToleranceSeconds = 10 * 60;
        private const int ReferenceToleranceSeconds = 5 * 60;
        private const int MaxTsFiles = 78;
        private const int FirstPassTsFiles = 3;
        private const int ProgramCacheDays = 1;
        private const int TitleCacheDays = 30;
        private static readonly HttpClient http = CreateHttpClient();
        private readonly SemaphoreSlim responseLock = new(1, 1);
        private readonly string toolDirectory;
        private readonly string cacheDirectory;
        private readonly Func<string, Task<string>> fetch;
        private readonly Func<DateTime> clock;
        private readonly Func<TimeSpan, Task> delay;
        private DateTime lastRequest = DateTime.MinValue;
        private readonly Func<string, (int[] ServiceIds, DateTime Start)> readTs;
        private readonly ConcurrentDictionary<string, (long Size, DateTime Modified, int[] ServiceIds, DateTime Start)> tsCache = new();

        public NicoJKReferenceFinder(string toolDirectory, string cacheDirectory,
            Func<string, Task<string>> fetch = null,
            Func<string, (int[] ServiceIds, DateTime Start)> readTs = null,
            Func<DateTime> clock = null, Func<TimeSpan, Task> delay = null)
        {
            this.toolDirectory = toolDirectory;
            this.cacheDirectory = cacheDirectory;
            this.fetch = fetch ?? http.GetStringAsync;
            this.readTs = readTs ?? ReadTs;
            this.clock = clock ?? (() => DateTime.UtcNow);
            this.delay = delay ?? Task.Delay;
        }

        private static HttpClient CreateHttpClient()
        {
            var client = new HttpClient { Timeout = TimeSpan.FromSeconds(HttpTimeoutSeconds) };
            client.DefaultRequestHeaders.UserAgent.ParseAdd("Amatsukaze");
            return client;
        }

        private static (int[], DateTime) ReadTs(string path)
        {
            using var context = new AMTContext();
            using var info = new TsInfo(context);
            if (!info.ReadFile(path)) throw new IOException("TS情報を読み込めません: " + path);
            return (info.GetProgramList().Select(p => p.ServiceId).ToArray(), info.GetEITStartTime());
        }

        public static bool HasManualSource(string path)
        {
            var stem = Path.Combine(Path.GetDirectoryName(path) ?? "", Path.GetFileNameWithoutExtension(path));
            foreach (var suffix in new[] { ".nicojk.ini", ".nicojk.xml", ".nicojk.jkl", ".xml", ".jkl" })
                if (File.Exists(stem + suffix)) return true;
            return false;
        }

        public static string NormalizeName(string name)
        {
            return new string(name.Normalize(NormalizationForm.FormKC).ToLowerInvariant()
                .Where(char.IsLetterOrDigit).ToArray());
        }

        private static string[] GetNameKeys(params string[] titles)
        {
            return titles.SelectMany(title => Regex.Split(title.Normalize(NormalizationForm.FormKC).ToLowerInvariant(), @"[^\p{L}\p{Nd}]+"))
                .Where(part => part.Length >= 2 && !part.All(char.IsDigit))
                .Select(part => part.Substring(0, Math.Min(5, part.Length))).Distinct().ToArray();
        }

        public async Task<string> GetArgumentsAsync(QueueItem item, ProfileSetting profile, Action<string> log)
        {
            if (!profile.EnableNicoJK || profile.NicoJKReferenceMode is not (1 or 2) ||
                item.Genre?.Any(g => g.Space == (int)GenreSpace.ARIB && g.Level1 == 7) != true) return "";
            try
            {
                if (HasManualSource(item.SrcPath))
                {
                    log("別局実況: 入力の隣に手動指定があるため自動選択しません");
                    return "";
                }
                var reference = await FindAsync(item.SrcPath, item.ServiceId, item.EITStartTime, log);
                return reference == null ? "" : " --nicojk-ref \"" + reference.Replace("\"", "\\\"") +
                    "\" --nicojk-ref-mode " + (profile.NicoJKReferenceMode == 1 ? "replace" : "merge");
            }
            catch (Exception e)
            {
                log("別局実況: 自動選択に失敗したため対象局のコメントを使います (" + e.Message + ")");
                return "";
            }
        }

        private async Task<XDocument> LoadAsync(string query, bool title, Action<string> log)
        {
            // 同じ応答の読み書きと通信間隔を、この1か所で直列化する。
            await responseLock.WaitAsync();
            try
            {
                var url = "https://cal.syoboi.jp/db.php?Command=" + query;
                var path = Path.Combine(cacheDirectory,
                    Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(url))) + ".xml");
                bool cached = File.Exists(path) && clock() - File.GetLastWriteTimeUtc(path) < TimeSpan.FromDays(title ? TitleCacheDays : ProgramCacheDays);
                string xml;
                if (cached) xml = await File.ReadAllTextAsync(path);
                else
                {
                    var wait = lastRequest.AddSeconds(RequestIntervalSeconds) - clock();
                    if (wait > TimeSpan.Zero)
                    {
                        log("別局実況: しょぼいカレンダーへの問い合わせ間隔を待機");
                        await delay(wait);
                    }
                    lastRequest = clock();
                    xml = await fetch(url);
                }
                var document = XDocument.Parse(xml);
                CheckResult(document);
                if (!cached)
                {
                    Directory.CreateDirectory(cacheDirectory);
                    await File.WriteAllTextAsync(path, xml);
                }
                return document;
            }
            finally
            {
                responseLock.Release();
            }
        }

        private static void CheckResult(XDocument document)
        {
            if ((string)document.Root?.Element("Result")?.Element("Code") != "200")
                throw new IOException("しょぼいカレンダーの応答が正常ではありません");
        }

        // StTimeは変更後の開始時刻なので、StOffsetは加算しない。
        private static DateTime Start(XElement item)
        {
            return DateTime.ParseExact((string)item.Element("StTime"), "yyyy-MM-dd HH:mm:ss",
                CultureInfo.InvariantCulture);
        }

        public async Task<string> FindAsync(string sourcePath, int serviceId, DateTime start, Action<string> log)
        {
            try
            {
                if (start <= new DateTime(2000, 1, 1))
                {
                    log("別局実況: 番組開始時刻がないため検索しません");
                    return null;
                }
                var (channels, commentServices) = ReadChannels();
                int channel = 0;
                foreach (var pair in channels)
                    if (pair.Value.ServiceId == serviceId) { channel = pair.Key; break; }
                if (channel == 0)
                {
                    log("別局実況: 対象局が局対応表にありません");
                    return null;
                }
                var target = await FindBroadcastAsync(channel, start, log);
                if (target == null)
                {
                    log("別局実況: 開始時刻が10分以内の放送がありません");
                    return null;
                }
                int tid = (int)target.Element("TID");
                string count = (string)target.Element("Count");
                if (string.IsNullOrEmpty(count))
                {
                    log("別局実況: 話数がないため検索しません");
                    return null;
                }
                var broadcasts = await LoadAsync("ProgLookup&TID=" + tid + "&Count=" + Uri.EscapeDataString(count), false, log);
                var candidates = SelectCandidates(broadcasts, target, channel, tid, count, channels, commentServices);
                if (candidates.Length == 0)
                {
                    log("別局実況: 実況のある別局の先行放送がありません");
                    return null;
                }
                var titleXml = await LoadAsync("TitleLookup&TID=" + tid, true, log);
                var titleItem = titleXml.Descendants("TitleItem").First(p => (int?)p.Element("TID") == tid);
                string title = (string)titleItem.Element("Title") ?? "";
                var names = GetNameKeys(title, (string)titleItem.Element("ShortTitle") ?? "");
                log("別局実況: " + title + " 第" + count + "話を特定しました");
                return FindRecording(sourcePath, names, candidates, channels, log);
            }
            catch (Exception e)
            {
                log("別局実況: 検索に失敗したため対象局のコメントを使います (" + e.Message + ")");
            }
            return null;
        }

        private (Dictionary<int, (int ServiceId, string Name)> Channels, HashSet<int> CommentServices) ReadChannels()
        {
            var channels = new Dictionary<int, (int ServiceId, string Name)>();
            foreach (var line in File.ReadLines(Path.Combine(toolDirectory, "syoboi_ch.txt")))
            {
                if (string.IsNullOrWhiteSpace(line) || line.StartsWith("#")) continue;
                var fields = line.Split('\t');
                channels.Add(int.Parse(fields[0], CultureInfo.InvariantCulture),
                    (int.Parse(fields[1], CultureInfo.InvariantCulture), fields[2]));
            }
            var commentServices = new HashSet<int>();
            foreach (var line in File.ReadLines(Path.Combine(toolDirectory, "ch_sid.txt")))
            {
                if (string.IsNullOrWhiteSpace(line) || line.StartsWith("#")) continue;
                var fields = line.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
                if (int.Parse(fields[0], CultureInfo.InvariantCulture) > 0)
                    commentServices.Add(int.Parse(fields[2], CultureInfo.InvariantCulture));
            }
            return (channels, commentServices);
        }

        private async Task<XElement> FindBroadcastAsync(int channel, DateTime start, Action<string> log)
        {
            var range = start.AddMinutes(-SearchRangeMinutes).ToString("yyyyMMdd_HHmmss", CultureInfo.InvariantCulture) + "-" +
                start.AddMinutes(SearchRangeMinutes).ToString("yyyyMMdd_HHmmss", CultureInfo.InvariantCulture);
            var nearby = await LoadAsync("ProgLookup&ChID=" + channel + "&Range=" + range, false, log);
            XElement target = null;
            double nearest = double.PositiveInfinity;
            foreach (var item in nearby.Descendants("ProgItem"))
            {
                if ((int?)item.Element("Deleted") == 1 || (int?)item.Element("ChID") != channel) continue;
                double difference = Math.Abs((Start(item) - start).TotalSeconds);
                if (difference <= TargetToleranceSeconds && difference < nearest) { nearest = difference; target = item; }
            }
            return target;
        }

        private static XElement[] SelectCandidates(XDocument broadcasts, XElement target, int channel, int tid, string count,
            Dictionary<int, (int ServiceId, string Name)> channels, HashSet<int> commentServices)
        {
            return broadcasts.Descendants("ProgItem").Where(item =>
                (int?)item.Element("Deleted") != 1 && (int?)item.Element("ChID") != channel &&
                (int?)item.Element("TID") == tid && (string)item.Element("Count") == count &&
                channels.TryGetValue((int)item.Element("ChID"), out var candidate) &&
                commentServices.Contains(candidate.ServiceId) && Start(item) < Start(target))
                .OrderBy(Start).ToArray();
        }

        private string FindRecording(string sourcePath, string[] names, XElement[] candidates,
            Dictionary<int, (int ServiceId, string Name)> channels, Action<string> log)
        {
            var files = Directory.EnumerateFiles(Path.GetDirectoryName(Path.GetFullPath(sourcePath)))
                .Where(p => (string.Equals(Path.GetExtension(p), ".ts", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(Path.GetExtension(p), ".m2ts", StringComparison.OrdinalIgnoreCase)) &&
                    !string.Equals(Path.GetFullPath(p), Path.GetFullPath(sourcePath),
                        OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
                .Where(p => names.Any(key => NormalizeName(Path.GetFileNameWithoutExtension(p)).Contains(key)))
                .OrderBy(p => p, StringComparer.Ordinal).ToArray();
            log("別局実況: 作品名の鍵が一致する録画 " + files.Length + "ファイル");
            int reads = 0;
            foreach (int limit in new[] { FirstPassTsFiles, int.MaxValue })
            {
                foreach (var item in candidates)
                {
                    var candidate = channels[(int)item.Element("ChID")];
                    var candidateStart = Start(item);
                    var candidateEnd = DateTime.ParseExact((string)item.Element("EdTime"), "yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture);
                    log("別局実況: 候補 " + candidate.Name + " " + candidateStart.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture));
                    foreach (var path in files.OrderBy(path => Math.Abs((File.GetLastWriteTime(path) - candidateEnd).TotalSeconds)).Take(limit))
                    {
                        var file = new FileInfo(path);
                        if (!tsCache.TryGetValue(path, out var info) || info.Size != file.Length || info.Modified != file.LastWriteTimeUtc)
                        {
                            if (reads >= MaxTsFiles)
                            {
                                log("別局実況: TS読み込みの上限" + MaxTsFiles + "ファイルに達しました");
                                return null;
                            }
                            ++reads;
                            try
                            {
                                var result = readTs(path);
                                info = (file.Length, file.LastWriteTimeUtc, result.ServiceIds, result.Start);
                                tsCache[path] = info;
                            }
                            catch (Exception e)
                            {
                                log("別局実況: TS情報の読み込みに失敗: " + path + " (" + e.Message + ")");
                                continue;
                            }
                        }
                        if (info.ServiceIds.Contains(candidate.ServiceId) && Math.Abs((info.Start - candidateStart).TotalSeconds) <= ReferenceToleranceSeconds)
                        {
                            log("別局実況: 基準録画 " + path);
                            return path;
                        }
                    }
                    log("別局実況: この候補の録画が見つかりません");
                }
            }
            log("別局実況: 利用できる先行放送の録画がありません");
            return null;
        }
    }
}
