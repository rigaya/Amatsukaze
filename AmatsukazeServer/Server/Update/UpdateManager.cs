using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Amatsukaze.Shared;

namespace Amatsukaze.Server.Update
{
    internal sealed class UpdateManager : IDisposable
    {
        private static readonly TimeSpan MinimumCheckInterval = TimeSpan.FromHours(6);
        private static readonly TimeSpan GuardPollInterval = TimeSpan.FromHours(1);
        private const int MaxRetainedJobs = 10;
        private static readonly Regex DevelopmentVersionRegex = new Regex(
            @"-\d+-g[0-9a-f]+", RegexOptions.Compiled | RegexOptions.CultureInvariant |
            RegexOptions.IgnoreCase, TimeSpan.FromSeconds(1));
        private readonly EncodeServer server;
        private readonly string appRoot;
        private readonly CancellationTokenSource cancellation = new CancellationTokenSource();
        private readonly SemaphoreSlim checkLock = new SemaphoreSlim(1, 1);
        private readonly object stateLock = new object();
        private readonly object jobLock = new object();
        private readonly Dictionary<string, UpdateJobRecord> jobs =
            new Dictionary<string, UpdateJobRecord>(StringComparer.Ordinal);
        private readonly Queue<string> jobOrder = new Queue<string>();
        private IReadOnlyList<UpdateTargetState> states = Array.Empty<UpdateTargetState>();
        private UpdateJobRecord activeJob;
        private ReleaseClient releaseClient;
        private string releaseClientProxy;
        private Task loopTask;
        private bool started;
        private bool disposed;

        public UpdateManager(EncodeServer server)
        {
            this.server = server;
            appRoot = GetApplicationRoot();
            UpdateLog.CleanupOldLogs(appRoot);
        }

        private static string GetApplicationRoot()
        {
            try
            {
                var executableDirectory = new DirectoryInfo(Path.GetFullPath(AppContext.BaseDirectory));
                // 配布物では管理対象の exe_files の親がアプリルートになる。
                if (string.Equals(executableDirectory.Name, "exe_files", StringComparison.OrdinalIgnoreCase) &&
                    executableDirectory.Parent != null)
                {
                    return executableDirectory.Parent.FullName;
                }
                return executableDirectory.FullName;
            }
            catch
            {
                // アプリルートの診断失敗だけでサーバーの生成を止めない。
                return AppContext.BaseDirectory;
            }
        }

        public IReadOnlyList<UpdateTargetState> States
        {
            get
            {
                lock (stateLock)
                {
                    return states.ToArray();
                }
            }
        }

        public void Start()
        {
            if (started || disposed)
            {
                return;
            }
            started = true;
            loopTask = Task.Run(RunPeriodicLoopAsync);
        }

        public Task CheckNowAsync(CancellationToken cancellationToken = default)
        {
            return StartOrJoinCheck(manual: true, cancellationToken).Completion;
        }

        public UpdateJobView StartCheckJob()
        {
            try
            {
                return StartOrJoinCheck(manual: true, cancellation.Token).ToView();
            }
            catch
            {
                return new UpdateJobView
                {
                    JobId = string.Empty,
                    Finished = true,
                    Succeeded = false,
                    RecentLogLines = new List<string> { "更新チェックを開始できませんでした。" },
                };
            }
        }

        public bool TryGetJob(string jobId, out UpdateJobView view)
        {
            view = null;
            if (string.IsNullOrWhiteSpace(jobId))
            {
                return false;
            }
            lock (jobLock)
            {
                if (!jobs.TryGetValue(jobId, out var job))
                {
                    return false;
                }
                view = job.ToView();
                return true;
            }
        }

        public bool TryGetJobLog(string jobId, out string content)
        {
            content = null;
            string path;
            lock (jobLock)
            {
                if (string.IsNullOrWhiteSpace(jobId) || !jobs.TryGetValue(jobId, out var job))
                {
                    return false;
                }
                path = job.GetLogFilePath();
            }
            if (string.IsNullOrEmpty(path))
            {
                return false;
            }
            try
            {
                using var stream = new FileStream(path, FileMode.Open, FileAccess.Read,
                    FileShare.ReadWrite);
                using var reader = new StreamReader(stream);
                content = reader.ReadToEnd();
                return true;
            }
            catch
            {
                return false;
            }
        }

        public UpdateStatusView GetStatusView()
        {
            try
            {
                var setting = server.AppData_?.setting;
                var checkEnabled = setting?.UpdateCheckEnabled != false;
                var environment = UpdateRuntimeEnvironment.Detect();
                var supported = OperatingSystem.IsWindows() || OperatingSystem.IsLinux();
                var lastCheckedAt = server.LastUpdateCheckedAt?.ToUniversalTime();
                DateTime? nextCheckAt = null;
                if (checkEnabled && lastCheckedAt.HasValue)
                {
                    var hours = Math.Max(6, setting?.UpdateCheckIntervalHours > 0
                        ? setting.UpdateCheckIntervalHours : 24);
                    nextCheckAt = lastCheckedAt.Value.AddHours(hours);
                }
                IReadOnlyList<UpdateTargetState> stateSnapshot;
                lock (stateLock)
                {
                    stateSnapshot = states.ToArray();
                }
                var disabledTargets = new HashSet<string>(setting?.UpdateDisabledTargets ??
                    new List<string>(), StringComparer.OrdinalIgnoreCase);
                var items = new List<UpdateItemView>();
                foreach (var target in UpdateCatalog.Targets)
                {
                    var state = stateSnapshot.FirstOrDefault(item =>
                        string.Equals(item.Id, target.Id, StringComparison.OrdinalIgnoreCase));
                    var initialStatus = disabledTargets.Contains(target.Id)
                        ? UpdateTargetStatus.Disabled
                        : environment.IsDocker && target.IsApplication
                            ? UpdateTargetStatus.Unsupported : UpdateTargetStatus.Unknown;
                    items.Add(new UpdateItemView
                    {
                        Id = target.Id,
                        DisplayName = target.DisplayName,
                        InstalledVersion = state?.CurrentVersion,
                        LatestVersion = state?.LatestVersion,
                        State = (state?.Status ?? initialStatus).ToString(),
                        StateReason = state?.Reason ?? (initialStatus == UpdateTargetStatus.Disabled
                            ? "disabled_by_setting" : initialStatus == UpdateTargetStatus.Unsupported
                                ? "docker_self_update_unsupported" : "not_checked"),
                        RequiresRestart = target.RequiresRestart,
                        DownloadSizeBytes = state?.SelectedAsset?.Size ?? 0,
                        ReleaseUrl = state?.ReleaseUrl,
                        AssetUrl = state?.SelectedAsset?.BrowserDownloadUrl,
                    });
                }
                var exeFilesMounted = UpdateDiagnostics.GetAppExeFilesMountState();
                return new UpdateStatusView
                {
                    CheckEnabled = checkEnabled,
                    Supported = supported,
                    UnsupportedReason = supported ? null : "unsupported_operating_system",
                    EnvironmentWarning = environment.IsDocker && exeFilesMounted != "yes"
                        ? "Docker コンテナ内の exe 更新はコンテナを再作成すると失われます。" : null,
                    LastCheckedAt = lastCheckedAt,
                    NextCheckAt = nextCheckAt,
                    HasUpdate = items.Any(item =>
                        item.State == UpdateTargetStatus.UpdateAvailable.ToString() ||
                        item.StateReason == "docker_self_update_available"),
                    HasPendingSelfUpdate = false,
                    Items = items,
                };
            }
            catch
            {
                return new UpdateStatusView
                {
                    CheckEnabled = true,
                    Supported = false,
                    UnsupportedReason = "status_view_failed",
                    Items = new List<UpdateItemView>(),
                };
            }
        }

        private async Task RunPeriodicLoopAsync()
        {
            try
            {
                await Task.Yield();
                await TryRunPeriodicCheckAsync(startup: true, cancellation.Token).ConfigureAwait(false);
                using var timer = new PeriodicTimer(GuardPollInterval);
                while (await timer.WaitForNextTickAsync(cancellation.Token).ConfigureAwait(false))
                {
                    await TryRunPeriodicCheckAsync(startup: false, cancellation.Token).ConfigureAwait(false);
                }
            }
            catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
            {
            }
            catch (Exception ex)
            {
                Util.AddLog($"[Update] 定期チェックループを停止せずに例外を処理しました: {ex.GetType().Name}: {ex.Message}", ex);
            }
        }

        private async Task TryRunPeriodicCheckAsync(bool startup, CancellationToken cancellationToken)
        {
            try
            {
                var setting = server.AppData_?.setting;
                if (setting == null || setting.UpdateCheckEnabled == false)
                {
                    return;
                }
                var lastCheckedAt = server.LastUpdateCheckedAt;
                var now = DateTime.UtcNow;
                var configuredHours = Math.Max(6, setting.UpdateCheckIntervalHours <= 0
                    ? 24 : setting.UpdateCheckIntervalHours);
                var interval = startup ? MinimumCheckInterval : TimeSpan.FromHours(
                    Math.Min(configuredHours, TimeSpan.MaxValue.TotalHours - 1));
                if (lastCheckedAt.HasValue && now - lastCheckedAt.Value.ToUniversalTime() < interval)
                {
                    return;
                }
                await StartOrJoinCheck(manual: false, cancellationToken).Completion
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
            }
            catch (Exception ex)
            {
                Util.AddLog($"[Update] 定期チェックの事前判定で例外を処理しました: {ex.GetType().Name}: {ex.Message}", ex);
            }
        }

        private UpdateJobRecord StartOrJoinCheck(bool manual, CancellationToken cancellationToken)
        {
            lock (jobLock)
            {
                if (activeJob != null && !activeJob.IsFinished)
                {
                    return activeJob;
                }
                var job = new UpdateJobRecord(Guid.NewGuid().ToString("N"));
                jobs[job.JobId] = job;
                jobOrder.Enqueue(job.JobId);
                activeJob = job;
                TrimJobsLocked();
                _ = Task.Run(() => RunCheckSafelyAsync(manual, cancellationToken, job));
                return job;
            }
        }

        private async Task RunCheckSafelyAsync(bool manual, CancellationToken cancellationToken,
            UpdateJobRecord job)
        {
            var succeeded = false;
            try
            {
                await checkLock.WaitAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                CompleteJob(job, false);
                return;
            }
            try
            {
                succeeded = await CheckCoreAsync(manual, cancellationToken, job).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
            }
            catch (Exception ex)
            {
                Util.AddLog($"[Update] 更新チェックの例外をサーバー本体へ伝播させずに処理しました: {ex.GetType().Name}: {ex.Message}", ex);
            }
            finally
            {
                checkLock.Release();
                CompleteJob(job, succeeded);
            }
        }

        private async Task<bool> CheckCoreAsync(bool manual, CancellationToken cancellationToken,
            UpdateJobRecord job)
        {
            var setting = server.AppData_?.setting;
            if (setting == null)
            {
                return false;
            }
            using var log = new UpdateLog(appRoot, job.ObserveLine);
            job.AttachLog(log.TransactionId, log.FilePath);
            await UpdateDiagnostics.LogEnvironmentAsync(log, appRoot).ConfigureAwait(false);
            var checkedAt = DateTime.UtcNow;

            if (!UpdateCatalog.TryInitialize(out var catalogError))
            {
                var invalidStates = new List<UpdateTargetState>();
                foreach (var target in UpdateCatalog.Targets)
                {
                    log.Write(target.Id, "S01_PRECHECK", "NG",
                        ("code", "INVALID_CATALOG"),
                        ("error", catalogError));
                    log.Write(target.Id, "S99_SUMMARY", "NG",
                        ("code", "INVALID_CATALOG"),
                        ("failed_stage", "S01_PRECHECK"),
                        ("current", "Unknown"),
                        ("latest", "Unknown"));
                    invalidStates.Add(CreateState(target, null, null, UpdateTargetStatus.Unknown,
                        "invalid_catalog", checkedAt, null));
                }
                SetStates(invalidStates);
                log.Write("-", "S99_SUMMARY", "NG",
                    ("code", "INVALID_CATALOG"),
                    ("targets", invalidStates.Count));
                return false;
            }

            var environment = UpdateRuntimeEnvironment.Detect();
            var disabledTargets = new HashSet<string>(setting.UpdateDisabledTargets ?? new List<string>(),
                StringComparer.OrdinalIgnoreCase);
            var client = GetReleaseClient(setting.UpdateProxy);
            if (manual)
            {
                client.ClearCache();
            }
            var extractor = FindExtractor(appRoot);
            var newStates = new List<UpdateTargetState>();
            var successfulReleaseQueries = 0;
            foreach (var target in UpdateCatalog.Targets)
            {
                var targetTimer = System.Diagnostics.Stopwatch.StartNew();
                cancellationToken.ThrowIfCancellationRequested();
                if (disabledTargets.Contains(target.Id))
                {
                    log.Write(target.Id, "S01_PRECHECK", "SKIP",
                        ("enabled", setting.UpdateCheckEnabled ?? true),
                        ("target_enabled", "no"),
                        ("reason", "disabled_by_setting"));
                    newStates.Add(CreateState(target, null, null, UpdateTargetStatus.Disabled,
                        "disabled_by_setting", checkedAt, null));
                    WriteTargetSummary(log, target.Id, "SKIP", null, null,
                        UpdateTargetStatus.Disabled, "disabled_by_setting", targetTimer,
                        null, null);
                    continue;
                }

                var dockerApplication = environment.IsDocker && target.IsApplication;
                var applicableRules = target.AssetRules.Where(rule => rule.AppliesTo(environment)).ToArray();
                log.Write(target.Id, "S01_PRECHECK", dockerApplication ? "SKIP" : "OK",
                    ("enabled", setting.UpdateCheckEnabled ?? true),
                    ("target_enabled", "yes"),
                    ("encoding", server.NowEncoding ? "yes" : "no"),
                    ("extractor", extractor),
                    ("os", environment.OS),
                    ("arch", environment.Architecture),
                    ("layout", target.GetInstallLayout(environment.OS)),
                    ("reason", dockerApplication ? "docker_self_update_unsupported_check_only" : "ready"));

                var local = await VersionProbe.ProbeAsync(target, server, setting, log, cancellationToken)
                    .ConfigureAwait(false);
                var releases = await client.GetReleasesAsync(target.Repository, target.Id,
                    target.ReleaseSelect, log, cancellationToken)
                    .ConfigureAwait(false);
                if (releases == null)
                {
                    newStates.Add(CreateState(target, local.Version, null,
                        dockerApplication ? UpdateTargetStatus.Unsupported : UpdateTargetStatus.Unknown,
                        "release_query_failed", checkedAt, null));
                    WriteTargetSummary(log, target.Id, "NG", local.Version, null,
                        dockerApplication ? UpdateTargetStatus.Unsupported : UpdateTargetStatus.Unknown,
                        "release_query_failed", targetTimer, "RELEASE_QUERY_FAILED", "S03_CONNECT");
                    continue;
                }
                successfulReleaseQueries++;
                var scanned = 0;
                ReleaseInfo release = null;
                foreach (var candidate in releases)
                {
                    scanned++;
                    if (target.ReleaseSelect == ReleaseSelectMode.Latest || applicableRules.Length == 0 ||
                        candidate.Assets.Any(asset => applicableRules.Any(rule => rule.Match(asset.Name).Success)))
                    {
                        release = candidate;
                        break;
                    }
                }
                if (release == null)
                {
                    var rules = string.Join("|", applicableRules.Select(rule => rule.Pattern));
                    var candidateNames = releases.SelectMany(item => item.Assets)
                        .Select(asset => asset.Name).ToArray();
                    log.Write(target.Id, "S04_LATEST", "OK",
                        ("tag", "(none)"),
                        ("published", "?"),
                        ("assets", candidateNames.Length),
                        ("scanned", scanned));
                    log.Write(target.Id, "S05_SELECT_ASSET", "NG",
                        ("code", "ASSET_NOT_FOUND"),
                        ("rule", rules),
                        ("candidates", string.Join(",", candidateNames)),
                        ("selected", "(none)"));
                    UpdateDiagnostics.LogAssetNotFound(log, target.Id, rules, candidateNames);
                    newStates.Add(CreateState(target, local.Version, null, UpdateTargetStatus.Unknown,
                        "asset_not_found", checkedAt, null));
                    WriteTargetSummary(log, target.Id, "NG", local.Version, null,
                        UpdateTargetStatus.Unknown, "asset_not_found", targetTimer,
                        "ASSET_NOT_FOUND", "S05_SELECT_ASSET");
                    continue;
                }
                log.Write(target.Id, "S04_LATEST", "OK",
                    ("tag", release.TagName),
                    ("published", release.PublishedAt),
                    ("assets", release.Assets.Count),
                    ("scanned", scanned));

                if (applicableRules.Length == 0)
                {
                    log.Write(target.Id, "S05_SELECT_ASSET", "SKIP",
                        ("rule", "(none)"),
                        ("candidates", string.Join(",", release.Assets.Select(asset => asset.Name))),
                        ("selected", "(none)"),
                        ("reason", "unsupported_os_or_arch"));
                    newStates.Add(CreateState(target, local.Version, null, UpdateTargetStatus.Unsupported,
                        "unsupported_os_or_arch", checkedAt, null));
                    WriteTargetSummary(log, target.Id, "SKIP", local.Version, null,
                        UpdateTargetStatus.Unsupported, "unsupported_os_or_arch", targetTimer,
                        null, null);
                    continue;
                }

                ReleaseAssetInfo selectedAsset = null;
                string latestVersion = null;
                AssetRule selectedRule = null;
                foreach (var rule in applicableRules)
                {
                    foreach (var asset in release.Assets)
                    {
                        var match = rule.Match(asset.Name);
                        if (match.Success)
                        {
                            selectedAsset = asset;
                            selectedRule = rule;
                            latestVersion = match.Groups["ver"].Value;
                            break;
                        }
                    }
                    if (selectedAsset != null)
                    {
                        break;
                    }
                }
                if (selectedAsset == null)
                {
                    var rules = string.Join("|", applicableRules.Select(rule => rule.Pattern));
                    log.Write(target.Id, "S05_SELECT_ASSET", "NG",
                        ("code", "ASSET_NOT_FOUND"),
                        ("rule", rules),
                        ("candidates", string.Join(",", release.Assets.Select(asset => asset.Name))),
                        ("selected", "(none)"));
                    UpdateDiagnostics.LogAssetNotFound(log, target.Id, rules,
                        release.Assets.Select(asset => asset.Name));
                    newStates.Add(CreateState(target, local.Version, null, UpdateTargetStatus.Unknown,
                        "asset_not_found", checkedAt, null));
                    WriteTargetSummary(log, target.Id, "NG", local.Version, null,
                        UpdateTargetStatus.Unknown, "asset_not_found", targetTimer,
                        "ASSET_NOT_FOUND", "S05_SELECT_ASSET");
                    continue;
                }
                log.Write(target.Id, "S05_SELECT_ASSET", "OK",
                    ("rule", selectedRule.Pattern),
                    ("candidates", string.Join(",", release.Assets.Select(asset => asset.Name))),
                    ("asset", selectedAsset.Name),
                    ("url", selectedAsset.BrowserDownloadUrl),
                    ("size", selectedAsset.Size),
                    ("digest", selectedAsset.Digest),
                    ("version", latestVersion));

                var status = DetermineStatus(target, local.Version, latestVersion, dockerApplication,
                    out var reason);
                newStates.Add(CreateState(target, local.Version, latestVersion, status, reason,
                    checkedAt, selectedAsset, release.HtmlUrl));
                var result = status == UpdateTargetStatus.Unknown ? "NG" :
                    status == UpdateTargetStatus.Unsupported ? "SKIP" : "OK";
                WriteTargetSummary(log, target.Id, result, local.Version, latestVersion, status,
                    reason, targetTimer,
                    status == UpdateTargetStatus.Unknown ? "LOCAL_VERSION_UNKNOWN" : null,
                    status == UpdateTargetStatus.Unknown ? "S02_LOCAL_VERSION" : null);
            }
            SetStates(newStates);
            if (successfulReleaseQueries > 0)
            {
                server.SetLastUpdateCheckedAt(checkedAt);
            }
            log.Write("-", "S99_SUMMARY", "OK",
                ("targets", newStates.Count),
                ("updates", newStates.Count(state => state.Status == UpdateTargetStatus.UpdateAvailable)),
                ("unknown", newStates.Count(state => state.Status == UpdateTargetStatus.Unknown)),
                ("release_success", successfulReleaseQueries),
                ("manual", manual ? "yes" : "no"));
            return true;
        }

        private static void WriteTargetSummary(UpdateLog log, string target, string result,
            string current, string latest, UpdateTargetStatus status, string reason,
            System.Diagnostics.Stopwatch timer, string code, string failedStage)
        {
            var values = new List<(string Key, object Value)>
            {
                ("current", current ?? "Unknown"),
                ("latest", latest ?? "Unknown"),
                ("status", status),
                ("reason", reason),
                ("elapsed", timer.ElapsedMilliseconds + "ms"),
            };
            if (result == "NG")
            {
                values.Insert(0, ("failed_stage", failedStage ?? "?"));
                values.Insert(0, ("code", code ?? "UNKNOWN"));
            }
            log.Write(target, "S99_SUMMARY", result, values.ToArray());
        }

        private ReleaseClient GetReleaseClient(string proxy)
        {
            proxy ??= string.Empty;
            if (releaseClient == null || !string.Equals(releaseClientProxy, proxy, StringComparison.Ordinal))
            {
                releaseClient?.Dispose();
                releaseClient = new ReleaseClient(proxy);
                releaseClientProxy = proxy;
            }
            return releaseClient;
        }

        private static UpdateTargetStatus DetermineStatus(UpdateTargetDef target, string current,
            string latest, bool dockerApplication, out string reason)
        {
            if (string.IsNullOrWhiteSpace(current) || string.IsNullOrWhiteSpace(latest))
            {
                return ApplyDockerStatus(UpdateTargetStatus.Unknown, "version_unknown",
                    dockerApplication, out reason);
            }
            if (target.IsApplication && DevelopmentVersionRegex.IsMatch(current))
            {
                return ApplyDockerStatus(UpdateTargetStatus.UpToDate, "development_build",
                    dockerApplication, out reason);
            }
            if (!TryParseVersion(current, out var currentParts) ||
                !TryParseVersion(latest, out var latestParts))
            {
                return ApplyDockerStatus(UpdateTargetStatus.Unknown, "version_parse_failed",
                    dockerApplication, out reason);
            }
            var length = Math.Max(currentParts.Length, latestParts.Length);
            for (var index = 0; index < length; index++)
            {
                var currentPart = index < currentParts.Length ? currentParts[index] : 0;
                var latestPart = index < latestParts.Length ? latestParts[index] : 0;
                if (latestPart > currentPart)
                {
                    return ApplyDockerStatus(UpdateTargetStatus.UpdateAvailable, "newer_release",
                        dockerApplication, out reason);
                }
                if (latestPart < currentPart)
                {
                    return ApplyDockerStatus(UpdateTargetStatus.UpToDate, "local_is_newer",
                        dockerApplication, out reason);
                }
            }
            return ApplyDockerStatus(UpdateTargetStatus.UpToDate, "same_version",
                dockerApplication, out reason);
        }

        private static UpdateTargetStatus ApplyDockerStatus(UpdateTargetStatus status, string statusReason,
            bool dockerApplication, out string reason)
        {
            if (dockerApplication)
            {
                reason = status == UpdateTargetStatus.UpdateAvailable
                    ? "docker_self_update_available" : "docker_self_update_unsupported";
                return UpdateTargetStatus.Unsupported;
            }
            reason = statusReason;
            return status;
        }

        private static bool TryParseVersion(string version, out int[] parts)
        {
            parts = null;
            if (string.IsNullOrWhiteSpace(version))
            {
                return false;
            }
            var normalized = version.Trim().TrimStart('v', 'V');
            var tokens = Regex.Split(normalized, @"[.+-]");
            var parsed = new List<int>();
            foreach (var token in tokens)
            {
                if (!int.TryParse(token, NumberStyles.None, CultureInfo.InvariantCulture, out var value))
                {
                    break;
                }
                parsed.Add(value);
            }
            if (parsed.Count == 0)
            {
                return false;
            }
            parts = parsed.ToArray();
            return true;
        }

        private static UpdateTargetState CreateState(UpdateTargetDef target, string current,
            string latest, UpdateTargetStatus status, string reason, DateTime checkedAt,
            ReleaseAssetInfo selectedAsset, string releaseUrl = null)
        {
            return new UpdateTargetState
            {
                Id = target.Id,
                DisplayName = target.DisplayName,
                CurrentVersion = current,
                LatestVersion = latest,
                Status = status,
                Reason = reason,
                SelectedAsset = selectedAsset,
                ReleaseUrl = releaseUrl,
                CheckedAtUtc = checkedAt,
            };
        }

        private void SetStates(IReadOnlyList<UpdateTargetState> newStates)
        {
            lock (stateLock)
            {
                states = newStates.ToArray();
            }
        }

        private static string FindExtractor(string root)
        {
            var bundled = OperatingSystem.IsWindows()
                ? Path.Combine(root, "exe_files", "7z", "7za.exe")
                : Path.Combine(root, "exe_files", "7z", "7zzs");
            if (File.Exists(bundled))
            {
                return bundled;
            }
            var names = OperatingSystem.IsWindows()
                ? new[] { "7z.exe", "7za.exe", "7zr.exe" }
                : new[] { "7z", "7zz", "7za", "7zr" };
            var path = Environment.GetEnvironmentVariable("PATH") ?? string.Empty;
            foreach (var directory in path.Split(Path.PathSeparator,
                StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
            {
                foreach (var name in names)
                {
                    var candidate = Path.Combine(directory, name);
                    if (File.Exists(candidate))
                    {
                        return candidate;
                    }
                }
            }
            return "(none)";
        }

        private void CompleteJob(UpdateJobRecord job, bool succeeded)
        {
            job.Complete(succeeded);
            lock (jobLock)
            {
                if (ReferenceEquals(activeJob, job))
                {
                    activeJob = null;
                }
                TrimJobsLocked();
            }
        }

        private void TrimJobsLocked()
        {
            while (jobOrder.Count > MaxRetainedJobs)
            {
                var oldestId = jobOrder.Peek();
                if (jobs.TryGetValue(oldestId, out var oldest) && !oldest.IsFinished)
                {
                    break;
                }
                jobOrder.Dequeue();
                jobs.Remove(oldestId);
            }
        }

        private sealed class UpdateJobRecord
        {
            private const int MaxRecentLogLines = 50;
            private readonly object sync = new object();
            private readonly Queue<string> recentLogLines = new Queue<string>();
            private readonly TaskCompletionSource<bool> completion =
                new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            private string txId;
            private string currentTargetId;
            private string currentStage;
            private string logFilePath;
            private bool finished;
            private bool succeeded;

            public UpdateJobRecord(string jobId)
            {
                JobId = jobId;
            }

            public string JobId { get; }
            public Task Completion => completion.Task;

            public bool IsFinished
            {
                get
                {
                    lock (sync)
                    {
                        return finished;
                    }
                }
            }

            public void AttachLog(string transactionId, string filePath)
            {
                lock (sync)
                {
                    txId = transactionId;
                    logFilePath = filePath;
                }
            }

            public void ObserveLine(string line)
            {
                lock (sync)
                {
                    recentLogLines.Enqueue(line);
                    while (recentLogLines.Count > MaxRecentLogLines)
                    {
                        recentLogLines.Dequeue();
                    }
                    var parts = line?.Split(']', 5);
                    if (parts?.Length >= 4)
                    {
                        currentTargetId = parts[2].TrimStart('[');
                        currentStage = parts[3].TrimStart('[');
                    }
                }
            }

            public void Complete(bool result)
            {
                lock (sync)
                {
                    finished = true;
                    succeeded = result;
                }
                completion.TrySetResult(result);
            }

            public string GetLogFilePath()
            {
                lock (sync)
                {
                    return logFilePath;
                }
            }

            public UpdateJobView ToView()
            {
                lock (sync)
                {
                    return new UpdateJobView
                    {
                        JobId = JobId,
                        TxId = txId,
                        CurrentTargetId = currentTargetId,
                        CurrentStage = currentStage,
                        ReceivedBytes = 0,
                        TotalBytes = 0,
                        SpeedBytesPerSec = 0,
                        Finished = finished,
                        Succeeded = succeeded,
                        RecentLogLines = recentLogLines.ToList(),
                    };
                }
            }
        }

        public void Dispose()
        {
            if (disposed)
            {
                return;
            }
            disposed = true;
            try
            {
                cancellation.Cancel();
            }
            catch
            {
                // 更新チェックの停止失敗をサーバー終了処理へ伝播させない。
            }
            try
            {
                loopTask?.Wait(TimeSpan.FromSeconds(2));
            }
            catch
            {
            }
            try
            {
                releaseClient?.Dispose();
            }
            catch
            {
                // HTTP クライアントの破棄失敗をサーバー終了処理へ伝播させない。
            }
            // 手動チェックが並行していても破棄済み同期オブジェクトへ触れないよう、
            // プロセス終了時に解放される小さな同期オブジェクトはここでは破棄しない。
        }
    }
}
