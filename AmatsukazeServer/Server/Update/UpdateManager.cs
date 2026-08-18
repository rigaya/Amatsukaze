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
    // 更新機能のローカル検証だけで使う注入口。環境変数が未設定なら通常動作のままになる。
    internal static class UpdateDebugOptions
    {
        internal const string ApiBaseUrlEnvironmentVariable = "AMT_UPDATE_API_BASE_URL";
        internal const string AllowDevelopmentBuildEnvironmentVariable =
            "AMT_UPDATE_ALLOW_DEV_BUILD";
        private const string EnabledNumericValue = "1";
        private const string EnabledTextValue = "true";

        internal static readonly string ApiBaseUrl =
            Environment.GetEnvironmentVariable(ApiBaseUrlEnvironmentVariable);
        internal static readonly bool AllowDevelopmentBuild = IsEnabled(
            Environment.GetEnvironmentVariable(AllowDevelopmentBuildEnvironmentVariable));

        private static bool IsEnabled(string value) =>
            string.Equals(value, EnabledNumericValue, StringComparison.Ordinal) ||
            string.Equals(value, EnabledTextValue, StringComparison.OrdinalIgnoreCase);
    }

    internal enum UpdateCancelResult
    {
        Accepted,
        NotFound,
        AlreadyFinished,
    }

    internal sealed class UpdateManager : IDisposable
    {
        private static readonly TimeSpan MinimumCheckInterval = TimeSpan.FromHours(6);
        // 起動時チェックの下限間隔。再起動直後に状態表示が空のままになるのを防ぐ。
        private static readonly TimeSpan StartupCheckMinimumInterval = TimeSpan.FromMinutes(15);
        // サーバー起動処理と競合しないよう、起動時チェックは少し待ってから実行する。
        private static readonly TimeSpan StartupCheckDelay = TimeSpan.FromSeconds(10);
        private static readonly TimeSpan GuardPollInterval = TimeSpan.FromHours(1);
        private const int MaxRetainedJobs = 10;
        // 本経路は deb から実行ファイルを取り出すだけで dpkg を実行しないため、依存は導入されない。
        // 既存インストールなら依存は解決済みなので更新は成立するが、新規インストールでは揃わない。
        // 実際に依存が問題になるかは対象ごとに異なるので UpdateTargetDef.RequiresSystemDependencies で持つ。
        private const string DebAssetExtension = ".deb";
        private static readonly Regex DevelopmentVersionRegex = new Regex(
            @"-\d+-g[0-9a-f]+", RegexOptions.Compiled | RegexOptions.CultureInvariant |
            RegexOptions.IgnoreCase, TimeSpan.FromSeconds(1));
        private readonly EncodeServer server;
        private readonly string appRoot;
        private readonly CancellationTokenSource cancellation = new CancellationTokenSource();
        private readonly SemaphoreSlim checkLock = new SemaphoreSlim(1, 1);
        private readonly SemaphoreSlim applyLock = new SemaphoreSlim(1, 1);
        private readonly object stateLock = new object();
        private readonly object jobLock = new object();
        private readonly Dictionary<string, UpdateJobRecord> jobs =
            new Dictionary<string, UpdateJobRecord>(StringComparer.Ordinal);
        private readonly Queue<string> jobOrder = new Queue<string>();
        private IReadOnlyList<UpdateTargetState> states = Array.Empty<UpdateTargetState>();
        private UpdateJobRecord activeCheckJob;
        private UpdateJobRecord activeApplyJob;
        private ReleaseClient releaseClient;
        private string releaseClientProxy;
        private Task loopTask;
        private bool started;
        private bool disposed;
        private readonly SelfUpdatePendingState selfUpdatePendingState;

        internal bool HasPendingSelfUpdate => selfUpdatePendingState.Value;
        internal SelfUpdateResult LastSelfUpdateResult { get; }

        public UpdateManager(EncodeServer server)
        {
            this.server = server;
            appRoot = GetApplicationRoot();
            UpdateLog.CleanupOldLogs(appRoot);
            UpdateTransaction.CleanupStale(appRoot);
            UpdateInstaller.CleanupStartupResidues(appRoot);
            // テスターが更新を適用した後の再起動では、GUI・サービス・手動起動の違いにより
            // 環境変数が引き継がれる保証がない。ここをゲートすると .amatsukaze_update と
            // updater_ready が残り続けるため、起動時リカバリは機能ゲートの外に置く。
            var recovery = SelfUpdateRecovery.RunStartupRecovery(appRoot);
            selfUpdatePendingState = new SelfUpdatePendingState(recovery.HasPending);
            LastSelfUpdateResult = recovery.LastResult;
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

        public bool TryStartApplyJob(IReadOnlyList<string> targetIds,
            out UpdateJobView view, out string error, out bool conflict)
        {
            view = null;
            error = null;
            conflict = false;
            var requested = (targetIds ?? Array.Empty<string>())
                .Where(id => !string.IsNullOrWhiteSpace(id))
                .Select(id => id.Trim()).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
            if (requested.Length == 0)
            {
                error = "更新対象が選択されていません";
                return false;
            }

            lock (jobLock)
            {
                if (activeApplyJob != null && !activeApplyJob.IsFinished)
                {
                    conflict = true;
                    error = "別の更新適用が実行中です";
                    return false;
                }
                if (activeCheckJob != null && !activeCheckJob.IsFinished)
                {
                    conflict = true;
                    error = "更新チェックの完了後に実行してください";
                    return false;
                }

                var environment = UpdateRuntimeEnvironment.Detect();
                var targets = new List<UpdateApplyTarget>();
                lock (stateLock)
                {
                    foreach (var id in requested)
                    {
                        var target = UpdateFeatureFlags.FindTarget(UpdateCatalog.Targets, id,
                            UpdateFeatureFlags.SelfUpdateEnabled);
                        if (target == null)
                        {
                            error = $"不明な更新対象です: {id}";
                            return false;
                        }
                        var cannotApplyReason = GetCannotApplyReason(target, environment.OS,
                            server.AppData_?.setting, appRoot);
                        if (cannotApplyReason == "layout_not_supported_yet")
                        {
                            error = $"まだ適用できない配置形式です: {id} " +
                                "(layout_not_supported_yet)";
                            return false;
                        }
                        if (cannotApplyReason == "payload_not_defined_yet")
                        {
                            error = $"配置対象がまだ宣言されていません: {id} " +
                                "(payload_not_defined_yet)";
                            return false;
                        }
                        if (cannotApplyReason == "setting_path_outside_exe_files")
                        {
                            error = $"設定のパスが exe_files の外を指しているため自動更新できません: {id} " +
                                "(setting_path_outside_exe_files)";
                            return false;
                        }
                        var state = states.FirstOrDefault(item =>
                            string.Equals(item.Id, id, StringComparison.OrdinalIgnoreCase));
                        if ((state?.Status != UpdateTargetStatus.UpdateAvailable &&
                             state?.Status != UpdateTargetStatus.NotInstalled) ||
                            state.SelectedAsset == null || string.IsNullOrWhiteSpace(state.LatestVersion))
                        {
                            error = $"適用可能な更新情報がありません: {id}";
                            return false;
                        }
                        if (GetStateCannotApplyReason(target, state) ==
                            "fresh_install_requires_dependencies")
                        {
                            error = $"新規インストールは依存パッケージの導入が必要なため対応していません: {id} " +
                                "(fresh_install_requires_dependencies)";
                            return false;
                        }
                        targets.Add(new UpdateApplyTarget(target, state.SelectedAsset,
                            state.LatestVersion));
                    }
                }

                var job = new UpdateJobRecord(Guid.NewGuid().ToString("N"), cancellation.Token,
                    isApply: true);
                jobs[job.JobId] = job;
                jobOrder.Enqueue(job.JobId);
                activeApplyJob = job;
                TrimJobsLocked();
                _ = Task.Run(() => RunApplySafelyAsync(targets, job));
                view = job.ToView();
                return true;
            }
        }

        public bool TryDiscardPendingSelfUpdate(out string error)
        {
            error = null;
            lock (jobLock)
            {
                if (activeApplyJob != null && !activeApplyJob.IsFinished)
                {
                    error = "更新適用中のため保留中の本体更新を破棄できません";
                    return false;
                }

                try
                {
                    SelfUpdateRecovery.DiscardPendingStaging(appRoot);
                    selfUpdatePendingState.Clear();
                }
                catch (Exception ex)
                {
                    error = "保留中の本体更新を破棄できませんでした: " + ex.Message;
                    UpdateLog.WriteFallbackError("S01_PRECHECK",
                        "SELF_UPDATE_DISCARD_FAILED", ex);
                    return false;
                }

                try
                {
                    using var log = new UpdateLog(appRoot);
                    log.Write("Amatsukaze", "S01_PRECHECK", "OK",
                        ("action", "discard_pending_staging"));
                }
                catch (Exception ex)
                {
                    // 破棄自体が成功している場合、診断ログの失敗で結果を失敗へ戻さない。
                    UpdateLog.WriteFallbackError("S01_PRECHECK",
                        "SELF_UPDATE_DISCARD_LOG_FAILED", ex);
                }
                return true;
            }
        }

        internal UpdateCancelResult CancelApplyJob(string jobId)
        {
            lock (jobLock)
            {
                if (string.IsNullOrWhiteSpace(jobId) ||
                    !jobs.TryGetValue(jobId, out var job) || !job.IsApply)
                {
                    return UpdateCancelResult.NotFound;
                }
                return job.Cancel() ? UpdateCancelResult.Accepted :
                    UpdateCancelResult.AlreadyFinished;
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
                    var hours = Math.Max(MinimumCheckInterval.TotalHours,
                        setting?.UpdateCheckIntervalHours > 0
                        ? setting.UpdateCheckIntervalHours : 24);
                    nextCheckAt = lastCheckedAt.Value.AddHours(hours);
                }
                IReadOnlyList<UpdateTargetState> stateSnapshot;
                lock (stateLock)
                {
                    stateSnapshot = states.ToArray();
                }
                string activeApplyJobId;
                lock (jobLock)
                {
                    activeApplyJobId = activeApplyJob != null && !activeApplyJob.IsFinished
                        ? activeApplyJob.JobId : null;
                }
                var disabledTargets = new HashSet<string>(setting?.UpdateDisabledTargets ??
                    new List<string>(), StringComparer.OrdinalIgnoreCase);
                var items = new List<UpdateItemView>();
                foreach (var target in UpdateFeatureFlags.FilterTargets(UpdateCatalog.Targets,
                    UpdateFeatureFlags.SelfUpdateEnabled))
                {
                    var state = stateSnapshot.FirstOrDefault(item =>
                        string.Equals(item.Id, target.Id, StringComparison.OrdinalIgnoreCase));
                    var cannotApplyReason = GetCannotApplyReason(target, environment.OS,
                        setting, appRoot);
                    cannotApplyReason ??= GetStateCannotApplyReason(target, state);
                    var initialStatus = UpdateTargetStatus.Unknown;
                    var initialReason = "not_checked";
                    if (disabledTargets.Contains(target.Id))
                    {
                        initialStatus = UpdateTargetStatus.Disabled;
                        initialReason = "disabled_by_setting";
                    }
                    else if (environment.IsDocker && target.IsApplication)
                    {
                        initialStatus = UpdateTargetStatus.Unsupported;
                        initialReason = "docker_self_update_unsupported";
                    }
                    else if (cannotApplyReason == "setting_path_outside_exe_files")
                    {
                        initialStatus = UpdateTargetStatus.Unsupported;
                        initialReason = cannotApplyReason;
                    }
                    items.Add(new UpdateItemView
                    {
                        Id = target.Id,
                        DisplayName = target.DisplayName,
                        InstalledVersion = state?.CurrentVersion,
                        LatestVersion = state?.LatestVersion,
                        State = (state?.Status ?? initialStatus).ToString(),
                        StateReason = state?.Reason ?? initialReason,
                        RequiresRestart = target.RequiresRestart,
                        DownloadSizeBytes = state?.SelectedAsset?.Size ?? 0,
                        ReleaseUrl = state?.ReleaseUrl,
                        AssetUrl = state?.SelectedAsset?.BrowserDownloadUrl,
                        CanApply = cannotApplyReason == null,
                        CannotApplyReason = cannotApplyReason,
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
                    // 未インストールは利用しないハードウェア向けの場合もあるため、更新ありには数えない。
                    HasUpdate = items.Any(item =>
                        item.State == UpdateTargetStatus.UpdateAvailable.ToString() ||
                        item.StateReason == "docker_self_update_available"),
                    HasPendingSelfUpdate = HasPendingSelfUpdate,
                    LastSelfUpdateResult = LastSelfUpdateResult == null ? null :
                        new SelfUpdateResultView
                        {
                            Status = LastSelfUpdateResult.Status,
                            Version = LastSelfUpdateResult.Version,
                            ErrorCode = string.IsNullOrEmpty(LastSelfUpdateResult.ErrorCode)
                                ? null : LastSelfUpdateResult.ErrorCode,
                        },
                    ActiveApplyJobId = activeApplyJobId,
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
                await Task.Delay(StartupCheckDelay, cancellation.Token).ConfigureAwait(false);
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
                if (IsApplyInProgress())
                {
                    using var log = new UpdateLog(appRoot);
                    log.Write("-", "S01_PRECHECK", "SKIP",
                        ("reason", "apply_in_progress"));
                    return;
                }
                var lastCheckedAt = server.LastUpdateCheckedAt;
                var now = DateTime.UtcNow;
                var configuredHours = Math.Max(MinimumCheckInterval.TotalHours,
                    setting.UpdateCheckIntervalHours <= 0 ? 24 : setting.UpdateCheckIntervalHours);
                var interval = startup ? StartupCheckMinimumInterval : TimeSpan.FromHours(
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

        private bool IsApplyInProgress()
        {
            lock (jobLock)
            {
                return activeApplyJob != null && !activeApplyJob.IsFinished;
            }
        }

        internal static string GetCannotApplyReason(UpdateTargetDef target, UpdateOSKind os,
            Setting setting = null, string root = null)
        {
            if (target.IsApplication)
            {
                return null;
            }
            if (target.GetInstallLayout(os) == InstallLayout.AppRootPartial)
            {
                return "layout_not_supported_yet";
            }
            // Payload を宣言する変更は、対象の展開・設置経路が揃ってから入れること。
            if (target.Payload == null || target.Payload.Length == 0)
            {
                return "payload_not_defined_yet";
            }
            if (setting == null || string.IsNullOrWhiteSpace(root))
            {
                return null;
            }
            var settingPath = target.GetExecutablePath(setting);
            if (string.IsNullOrWhiteSpace(settingPath))
            {
                return null;
            }
            var pathKind = ClassifySettingPath(target, settingPath, root, os);
            return pathKind == SettingPathKind.BarePayload ||
                pathKind == SettingPathKind.InstalledPayload
                ? null : "setting_path_outside_exe_files";
        }

        internal static string GetStateCannotApplyReason(UpdateTargetDef target,
            UpdateTargetState state)
        {
            if (state?.Status != UpdateTargetStatus.NotInstalled ||
                target?.RequiresSystemDependencies != true)
            {
                return null;
            }
            // deb 経路のときだけ制限する。Windows の書庫は依存の導入を伴わないため対象外。
            return state.SelectedAsset?.Name?.EndsWith(DebAssetExtension,
                StringComparison.OrdinalIgnoreCase) == true
                ? "fresh_install_requires_dependencies" : null;
        }

        private UpdateJobRecord StartOrJoinCheck(bool manual, CancellationToken cancellationToken)
        {
            lock (jobLock)
            {
                if (activeCheckJob != null && !activeCheckJob.IsFinished)
                {
                    return activeCheckJob;
                }
                if (activeApplyJob != null && !activeApplyJob.IsFinished)
                {
                    throw new UpdateInstallException("UPDATE_BUSY", "S01_PRECHECK",
                        "更新の適用中は再チェックできません");
                }
                var job = new UpdateJobRecord(Guid.NewGuid().ToString("N"), cancellation.Token);
                jobs[job.JobId] = job;
                jobOrder.Enqueue(job.JobId);
                activeCheckJob = job;
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
                CompleteCheckJob(job, false);
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
                CompleteCheckJob(job, succeeded);
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
                foreach (var target in UpdateFeatureFlags.FilterTargets(UpdateCatalog.Targets,
                    UpdateFeatureFlags.SelfUpdateEnabled))
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
            foreach (var target in UpdateFeatureFlags.FilterTargets(UpdateCatalog.Targets,
                UpdateFeatureFlags.SelfUpdateEnabled))
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

                var cannotApplyReason = GetCannotApplyReason(target, environment.OS,
                    setting, appRoot);
                if (cannotApplyReason == "setting_path_outside_exe_files")
                {
                    var settingPath = target.GetExecutablePath(setting);
                    log.Write(target.Id, "S01_PRECHECK", "SKIP",
                        ("enabled", setting.UpdateCheckEnabled ?? true),
                        ("target_enabled", "yes"),
                        ("reason", cannotApplyReason),
                        ("key", target.SettingKey),
                        ("path", settingPath));
                    newStates.Add(CreateState(target, null, null, UpdateTargetStatus.Unsupported,
                        cannotApplyReason, checkedAt, null));
                    WriteTargetSummary(log, target.Id, "SKIP", null, null,
                        UpdateTargetStatus.Unsupported, cannotApplyReason, targetTimer,
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

                var status = DetermineStatus(target, local.Version, latestVersion,
                    local.NotInstalled, dockerApplication, out var reason);
                newStates.Add(CreateState(target, local.Version, latestVersion, status, reason,
                    checkedAt, selectedAsset, release.HtmlUrl));
                var result = status == UpdateTargetStatus.Unknown ? "NG" :
                    status == UpdateTargetStatus.Unsupported ||
                    status == UpdateTargetStatus.NotInstalled ? "SKIP" : "OK";
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
                ("not_installed", newStates.Count(state => state.Status == UpdateTargetStatus.NotInstalled)),
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
                releaseClient = new ReleaseClient(proxy, UpdateDebugOptions.ApiBaseUrl);
                releaseClientProxy = proxy;
            }
            return releaseClient;
        }

        private static UpdateTargetStatus DetermineStatus(UpdateTargetDef target, string current,
            string latest, bool notInstalled, bool dockerApplication, out string reason)
        {
            if (notInstalled && !string.IsNullOrWhiteSpace(latest))
            {
                reason = "not_installed";
                return UpdateTargetStatus.NotInstalled;
            }
            if (string.IsNullOrWhiteSpace(current) || string.IsNullOrWhiteSpace(latest))
            {
                return ApplyDockerStatus(UpdateTargetStatus.Unknown, "version_unknown",
                    dockerApplication, out reason);
            }
            if (target.IsApplication && DevelopmentVersionRegex.IsMatch(current) &&
                !UpdateDebugOptions.AllowDevelopmentBuild)
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

        internal async Task<PreparedUpdate> PrepareTargetAsync(UpdateTargetDef target,
            UpdateTransaction transaction, ReleaseAssetInfo asset, string expectedVersion,
            UpdateLog log, UpdateJobRecord job, CancellationToken cancellationToken)
        {
            if (transaction == null || log == null ||
                !string.Equals(transaction.TxId, log.TransactionId, StringComparison.Ordinal))
            {
                throw new UpdatePreparationException("INVALID_TRANSACTION_ID", "S01_PRECHECK",
                    "ログと一時ディレクトリのトランザクションIDが一致しません");
            }
            if (target == null || target.Payload == null || target.Payload.Length == 0)
            {
                throw new UpdatePreparationException("INVALID_CATALOG", "S01_PRECHECK",
                    "更新対象の Payload 宣言がありません");
            }
            if (!target.TryCompileRegexes(out var catalogError))
            {
                throw new UpdatePreparationException("INVALID_CATALOG", "S01_PRECHECK",
                    "更新対象の定義が不正です: " + catalogError);
            }
            var setting = server.AppData_?.setting;
            using var downloader = new UpdateDownloader(setting?.UpdateProxy);
            var progress = job == null ? null : new Progress<DownloadProgress>(job.ReportProgress);
            var download = await downloader.DownloadAsync(asset,
                transaction.GetTargetDownloadDirectory(target.Id), target.Id, log, progress,
                cancellationToken).ConfigureAwait(false);
            var extractor = new ArchiveExtractor(FindExtractor(appRoot));
            var extraction = await extractor.ExtractAsync(download,
                transaction.GetTargetExtractDirectory(target.Id), target.Id, log,
                cancellationToken, target.Payload).ConfigureAwait(false);
            return await new UpdateStaging().PrepareAsync(target, extraction, expectedVersion,
                log, cancellationToken).ConfigureAwait(false);
        }

        internal Task<PreparedSelfUpdate> PrepareSelfUpdateAsync(ReleaseAssetInfo asset,
            string expectedVersion, UpdateLog log, UpdateJobRecord job,
            CancellationToken cancellationToken)
        {
            SelfUpdateWorkspace.EnsureNoPendingUpdate(appRoot);
            var environment = UpdateRuntimeEnvironment.Detect();
            var setting = server.AppData_?.setting;
            var progress = job == null ? null : new Progress<DownloadProgress>(job.ReportProgress);
            return new SelfUpdatePreparation(appRoot, setting?.UpdateProxy,
                FindExtractor(appRoot)).PrepareAsync(asset, expectedVersion, environment.OS,
                log, progress, cancellationToken);
        }

        private async Task RunApplySafelyAsync(IReadOnlyList<UpdateApplyTarget> targets,
            UpdateJobRecord job)
        {
            var succeeded = false;
            UpdateLog log = null;
            try
            {
                log = new UpdateLog(appRoot, job.ObserveLine);
                job.AttachLog(log.TransactionId, log.FilePath);
                await UpdateDiagnostics.LogEnvironmentAsync(log, appRoot).ConfigureAwait(false);
                await ApplyTargetsAsync(targets, log, job, job.Token).ConfigureAwait(false);
                var snapshot = job.ToView();
                succeeded = snapshot.TargetResults.Count == targets.Count &&
                    snapshot.TargetResults.All(result => result.Succeeded);
                log.Write("-", "S99_SUMMARY", succeeded ? "OK" : "NG",
                    ("targets", targets.Count),
                    ("succeeded", snapshot.TargetResults.Count(result => result.Succeeded)),
                    ("failed", snapshot.TargetResults.Count(result => !result.Succeeded)));
            }
            catch (OperationCanceledException) when (job.Token.IsCancellationRequested)
            {
                AddMissingTargetResults(targets, job, "CANCELED", "更新は中止されました");
                log?.Write("-", "S99_SUMMARY", "NG", ("code", "CANCELED"),
                    ("failed_stage", job.ToView().CurrentStage ?? "S01_PRECHECK"),
                    ("targets", targets.Count));
            }
            catch (Exception ex)
            {
                var code = GetApplyErrorCode(ex);
                AddMissingTargetResults(targets, job, code, ex.Message);
                if (log != null)
                {
                    log.Write("-", "S99_SUMMARY", "NG", ("code", code),
                        ("failed_stage", GetApplyErrorStage(ex)),
                        ("error", ex.GetType().Name), ("message", ex.Message));
                }
                else
                {
                    Util.AddLog($"[Update] 更新適用ログの開始に失敗しました: " +
                        $"{ex.GetType().Name}: {ex.Message}", ex);
                }
            }
            finally
            {
                if (targets.Any(target => target.Target.IsApplication))
                {
                    ReevaluatePendingSelfUpdate();
                }
                log?.Dispose();
                CompleteApplyJob(job, succeeded);
            }
        }

        // updater 起動失敗など、サーバーが生存したまま staging が残る経路を反映する。
        internal void ReevaluatePendingSelfUpdate()
        {
            selfUpdatePendingState.Refresh(appRoot);
        }

        internal async Task ApplyTargetsAsync(IReadOnlyList<UpdateApplyTarget> targets,
            UpdateLog log, UpdateJobRecord job, CancellationToken cancellationToken)
        {
            using var writer = await UpdateWriterLease.AcquireAsync(applyLock,
                cancellationToken).ConfigureAwait(false);
            using var lease = await UpdateMaintenanceLease.AcquireAsync(server,
                cancellationToken).ConfigureAwait(false);
            var selfUpdateTargets = targets.Where(item => item.Target.IsApplication).ToArray();
            if (selfUpdateTargets.Length > 0)
            {
                ValidateSelfUpdateSelection(targets, log);
                await ApplySelfUpdateAsync(selfUpdateTargets[0], log, job, lease,
                    cancellationToken).ConfigureAwait(false);
                return;
            }

            EnsureWritableUpdateDirectories(appRoot, log);
            using var transaction = UpdateTransaction.Create(appRoot, log.TransactionId);
            foreach (var applyTarget in targets)
            {
                cancellationToken.ThrowIfCancellationRequested();
                job.ResetProgress(applyTarget.Target.Id);
                try
                {
                    await ApplyTargetCoreAsync(applyTarget, transaction, log, job,
                        cancellationToken).ConfigureAwait(false);
                    MarkTargetApplied(applyTarget);
                    job.AddTargetResult(applyTarget.Target.Id, true, null,
                        "更新を適用しました");
                    log.Write(applyTarget.Target.Id, "S99_SUMMARY", "OK",
                        ("current", applyTarget.ExpectedVersion),
                        ("latest", applyTarget.ExpectedVersion));
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    job.AddTargetResult(applyTarget.Target.Id, false, "CANCELED",
                        "更新は中止されました");
                    throw;
                }
                catch (Exception ex)
                {
                    var code = GetApplyErrorCode(ex);
                    var stage = GetApplyErrorStage(ex);
                    job.AddTargetResult(applyTarget.Target.Id, false, code, ex.Message);
                    log.Write(applyTarget.Target.Id, "S99_SUMMARY", "NG", ("code", code),
                        ("failed_stage", stage), ("message", ex.Message));
                }
            }
        }

        internal static void ValidateSelfUpdateSelection(
            IReadOnlyList<UpdateApplyTarget> targets, UpdateLog log)
        {
            if (targets.Count == 1 && targets[0].Target.IsApplication) return;
            log?.Write("Amatsukaze", "S01_PRECHECK", "NG",
                ("code", "SELF_UPDATE_NOT_ALONE"), ("targets", targets.Count));
            throw new UpdateInstallException("SELF_UPDATE_NOT_ALONE", "S01_PRECHECK",
                "本体更新はほかの更新対象と同時に適用できません");
        }

        private async Task ApplySelfUpdateAsync(UpdateApplyTarget applyTarget, UpdateLog log,
            UpdateJobRecord job, UpdateMaintenanceLease lease,
            CancellationToken cancellationToken)
        {
            job.ResetProgress(applyTarget.Target.Id);
            var prepared = await PrepareSelfUpdateAsync(applyTarget.Asset,
                applyTarget.ExpectedVersion, log, job, cancellationToken).ConfigureAwait(false);
            var environment = UpdateRuntimeEnvironment.Detect();
            var generated = SelfUpdaterScriptGenerator.Generate(appRoot, Environment.ProcessId,
                prepared, log.TransactionId, environment.OS, DateTime.UtcNow);

            // pause 取得後に開始した worker との競合を、終了処理へ入る直前にも確認する。
            if (server.NowEncoding)
            {
                log.Write(applyTarget.Target.Id, "S01_PRECHECK", "NG",
                    ("code", "ENCODING_ACTIVE"),
                    ("message", "エンコード実行中のため本体更新を開始しません"));
                throw new UpdateInstallException("ENCODING_ACTIVE", "S01_PRECHECK",
                    "エンコード実行中のため本体更新を適用できません");
            }

            try
            {
                await SelfUpdaterLauncher.StartAndWaitReadyAsync(generated, environment.OS,
                    cancellationToken).ConfigureAwait(false);
            }
            catch (UpdateInstallException ex) when (ex.Code == "UPDATER_START_FAILED")
            {
                log.Write(applyTarget.Target.Id, "S01_PRECHECK", "NG",
                    ("code", ex.Code), ("message", ex.Message),
                    ("staging", prepared.StagingDirectory));
                throw;
            }
            // ready 確認後は updater がサーバー終了を待つ。終了までキュー停止を維持する。
            lease.KeepPaused();
            job.AddTargetResult(applyTarget.Target.Id, true, null,
                "本体 updater を起動しました。サーバーを再起動します");
            log.Write(applyTarget.Target.Id, "S99_SUMMARY", "OK",
                ("current", applyTarget.ExpectedVersion),
                ("latest", applyTarget.ExpectedVersion), ("action", "restart"));
            await server.EndServer().ConfigureAwait(false);
        }

        internal static void EnsureWritableUpdateDirectories(string appRoot, UpdateLog log)
        {
            var executableRoot = Path.GetFullPath(Path.Combine(appRoot, "exe_files"));
            var temporaryRoot = Path.Combine(executableRoot, ".update_tmp");
            // Kind はログ用の識別子、Label は利用者に見えるメッセージ用の表記
            foreach (var item in new[]
            {
                (Kind: "install", Label: "配置先", Path: executableRoot, Create: false),
                (Kind: "temporary", Label: "一時領域", Path: temporaryRoot, Create: true),
            })
            {
                var writable = UpdateDiagnostics.CheckDirectoryWritable(item.Path, item.Create);
                if (writable == false)
                {
                    log.Write("-", "S01_PRECHECK", "NG",
                        ("code", "WRITE_ACCESS_DENIED"), ("area", item.Kind),
                        ("path", item.Path));
                    throw new UpdatePreparationException("WRITE_ACCESS_DENIED", "S01_PRECHECK",
                        $"更新用の{item.Label}へ書き込めません: {item.Path}");
                }
                if (!writable.HasValue)
                {
                    log.WriteDiagnostic("-", "S01_PRECHECK",
                        ("code", "WRITE_CHECK_UNAVAILABLE"), ("area", item.Kind),
                        ("path", item.Path), ("action", "continue"));
                }
            }
        }

        private void MarkTargetApplied(UpdateApplyTarget applied)
        {
            lock (stateLock)
            {
                states = states.Select(state => string.Equals(state.Id, applied.Target.Id,
                    StringComparison.OrdinalIgnoreCase) ? CreateState(applied.Target,
                        applied.ExpectedVersion, applied.ExpectedVersion,
                        UpdateTargetStatus.UpToDate, "same_version", DateTime.UtcNow,
                        state.SelectedAsset, state.ReleaseUrl) : state).ToArray();
            }
        }

        private static string GetApplyErrorCode(Exception exception) => exception switch
        {
            UpdateInstallException install => install.Code,
            UpdatePreparationException preparation => preparation.Code,
            _ => "UPDATE_FAILED",
        };

        private static string GetApplyErrorStage(Exception exception) => exception switch
        {
            UpdateInstallException install => install.Stage,
            UpdatePreparationException preparation => preparation.Stage,
            _ => "S01_PRECHECK",
        };

        private static void AddMissingTargetResults(IReadOnlyList<UpdateApplyTarget> targets,
            UpdateJobRecord job, string code, string message)
        {
            var completed = new HashSet<string>(job.ToView().TargetResults.Select(
                result => result.TargetId), StringComparer.OrdinalIgnoreCase);
            foreach (var target in targets.Where(item => !completed.Contains(item.Target.Id)))
            {
                job.AddTargetResult(target.Target.Id, false, code, message);
            }
        }

        // lease と一時ディレクトリは呼び出し側がジョブ全体で保持する。
        private async Task<InstalledUpdate> ApplyTargetCoreAsync(UpdateApplyTarget applyTarget,
            UpdateTransaction transaction, UpdateLog log, UpdateJobRecord job,
            CancellationToken cancellationToken)
        {
            var target = applyTarget.Target;
            var asset = applyTarget.Asset;
            var expectedVersion = applyTarget.ExpectedVersion;
            PreparedUpdate prepared;
            try
            {
                prepared = await PrepareTargetAsync(target, transaction, asset,
                    expectedVersion, log, job, cancellationToken).ConfigureAwait(false);
            }
            catch
            {
                log.Write(target?.Id ?? "-", "S13_ROLLBACK", "SKIP",
                    ("reason", "no_files_modified"));
                throw;
            }

            // pause の取得直前に開始した worker との競合を、既存ファイルに触る直前にも確認する。
            if (server.NowEncoding)
            {
                log.Write(target.Id, "S10_INSTALL", "NG",
                    ("code", "ENCODING_ACTIVE"),
                    ("message", "エンコード実行中のため設置を開始しません"));
                log.Write(target.Id, "S13_ROLLBACK", "SKIP",
                    ("reason", "no_files_modified"));
                throw new UpdateInstallException("ENCODING_ACTIVE", "S10_INSTALL",
                    "エンコード実行中のため更新を適用できません");
            }

            var installer = new UpdateInstaller(appRoot);
            InstalledUpdate installed;
            try
            {
                installed = await installer.InstallAsync(target, prepared, expectedVersion,
                    log, cancellationToken).ConfigureAwait(false);
            }
            catch (UpdateInstallException ex) when (ex.Stage == "S10_INSTALL")
            {
                log.Write(target.Id, "S13_ROLLBACK", "SKIP",
                    ("reason", "no_files_modified"));
                throw;
            }

            try
            {
                await UpdateInstalledPathAsync(target, installed, log)
                    .ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                log.Write(target.Id, "S12_SETTINGS", "NG",
                    ("code", "SETTINGS_UPDATE_FAILED"),
                    ("key", target.SettingKey ?? "(none)"),
                    ("error", ex.GetType().Name), ("message", ex.Message));
                await installer.RollbackInstalledAsync(installed, log).ConfigureAwait(false);
                throw new UpdateInstallException("SETTINGS_UPDATE_FAILED", "S12_SETTINGS",
                    "設定パスの反映に失敗したため旧版へ戻しました", ex);
            }

            log.Write(target.Id, "S13_ROLLBACK", "SKIP",
                ("reason", "installation_succeeded"));
            return installed;
        }

        private async Task UpdateInstalledPathAsync(UpdateTargetDef target,
            InstalledUpdate installed, UpdateLog log)
        {
            var current = server.AppData_?.setting;
            var oldPath = current == null ? null : target.GetExecutablePath(current);
            var os = OperatingSystem.IsWindows() ? UpdateOSKind.Windows : UpdateOSKind.Linux;
            if (string.IsNullOrWhiteSpace(oldPath))
            {
                if (!IsInstallDestination(target, installed.DestinationPath, appRoot, os))
                {
                    log.Write(target.Id, "S12_SETTINGS", "SKIP",
                        ("reason", "empty_default_path"),
                        ("key", target.SettingKey ?? "(none)"),
                        ("old", "(empty)"));
                    return;
                }
            }
            else if (os == UpdateOSKind.Linux &&
                AreSamePath(oldPath, installed.DestinationPath, StringComparison.Ordinal))
            {
                log.Write(target.Id, "S12_SETTINGS", "SKIP",
                    ("reason", "unchanged_flat_linux"),
                    ("key", target.SettingKey ?? "(none)"),
                    ("old", oldPath));
                return;
            }
            else if (!ShouldUpdateSettingPath(target, oldPath,
                installed.DestinationPath, appRoot, os))
            {
                log.Write(target.Id, "S12_SETTINGS", "SKIP",
                    ("reason", "custom_or_unrelated_path"),
                    ("key", target.SettingKey ?? "(none)"), ("old", oldPath));
                return;
            }

            var updated = ServerSupport.DeepCopy(current);
            if (!target.SetExecutablePath(updated, installed.DestinationPath))
            {
                throw new InvalidOperationException("設定パスのキーがカタログに定義されていません");
            }
            try
            {
                await server.SetCommonData(new CommonData { Setting = updated }).ConfigureAwait(false);
            }
            catch (Exception applyException)
            {
                // 通知だけが失敗しても設定本体は置換済みの可能性があるため、旧値へ戻す。
                Exception restoreException = null;
                try
                {
                    await server.SetCommonData(new CommonData { Setting = current })
                        .ConfigureAwait(false);
                }
                catch (Exception ex)
                {
                    restoreException = ex;
                }
                var restoredSetting = server.AppData_?.setting;
                var restoredPath = restoredSetting == null ? null :
                    target.GetExecutablePath(restoredSetting);
                if (!string.Equals(restoredPath, oldPath, StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidOperationException(
                        $"設定パスの旧値への復元にも失敗しました。old={oldPath} current={restoredPath ?? "(null)"}",
                        restoreException ?? applyException);
                }
                throw;
            }
            log.Write(target.Id, "S12_SETTINGS", "OK", ("key", target.SettingKey),
                ("old", string.IsNullOrWhiteSpace(oldPath) ? "(empty)" : oldPath),
                ("new", installed.DestinationPath),
                ("backup", "server_managed"));
        }

        internal static bool ShouldUpdateSettingPath(UpdateTargetDef target,
            string oldPath, string newPath, string root, UpdateOSKind os)
        {
            if (target?.Payload == null || string.IsNullOrWhiteSpace(oldPath) ||
                string.IsNullOrWhiteSpace(newPath) || string.IsNullOrWhiteSpace(root))
            {
                return false;
            }
            if (os == UpdateOSKind.Windows &&
                (!oldPath.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ||
                 !newPath.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)))
            {
                return false;
            }
            try
            {
                // 設置先は既定の場所でなければならない。設定の現在値と違い、ここは緩めない。
                if (!IsInstallDestination(target, newPath, root, os))
                {
                    return false;
                }
                var oldKind = ClassifySettingPath(target, oldPath, root, os);
                if (os == UpdateOSKind.Windows)
                {
                    return oldKind == SettingPathKind.InstalledPayload;
                }
                if (oldKind == SettingPathKind.BarePayload)
                {
                    return true;
                }
                return oldKind == SettingPathKind.InstalledPayload &&
                    !AreSamePath(oldPath, newPath, StringComparison.Ordinal);
            }
            catch
            {
                return false;
            }
        }

        // 更新機能が実際に設置する場所（exe_files 直下、または exe_files/<対象ID>）かどうか。
        // 設置先は UpdateInstaller が固定しているので、それ以外は設置先として認めない。
        private static bool IsInstallDestination(UpdateTargetDef target, string path,
            string root, UpdateOSKind os)
        {
            if (target?.Payload == null || string.IsNullOrWhiteSpace(path) ||
                string.IsNullOrWhiteSpace(root) || !Path.IsPathFullyQualified(path))
            {
                return false;
            }
            try
            {
                var executableRoot = Path.GetFullPath(Path.Combine(root, "exe_files"));
                var fullPath = Path.GetFullPath(path);
                var comparison = os == UpdateOSKind.Windows
                    ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
                var parent = Path.GetDirectoryName(fullPath);
                var subDirectory = target.GetInstallLayout(os) == InstallLayout.ExeFilesSubDir
                    ? Path.Combine(executableRoot, target.Id) : null;
                return (string.Equals(parent, executableRoot, comparison) ||
                    (!string.IsNullOrEmpty(subDirectory) &&
                     string.Equals(parent, subDirectory, comparison))) &&
                    target.Payload.Any(entry => entry.IsMatch(Path.GetFileName(fullPath)));
            }
            catch
            {
                return false;
            }
        }

        private static SettingPathKind ClassifySettingPath(UpdateTargetDef target,
            string path, string root, UpdateOSKind os)
        {
            if (string.IsNullOrWhiteSpace(path)) return SettingPathKind.Empty;
            if (target?.Payload == null || string.IsNullOrWhiteSpace(root) ||
                !target.TryCompileRegexes(out _))
            {
                return SettingPathKind.Other;
            }
            if (os == UpdateOSKind.Linux &&
                Path.GetFileName(path).EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
            {
                return SettingPathKind.Other;
            }
            if (os == UpdateOSKind.Linux && IsBareExecutableName(path))
            {
                return target.Payload.Any(entry => entry.IsMatch(path))
                    ? SettingPathKind.BarePayload : SettingPathKind.Other;
            }
            if (!Path.IsPathFullyQualified(path)) return SettingPathKind.Other;
            try
            {
                var executableRoot = Path.GetFullPath(Path.Combine(root, "exe_files"));
                var fullPath = Path.GetFullPath(path);
                var comparison = os == UpdateOSKind.Windows
                    ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
                // exe_files 配下なら深さを問わず更新機能の管理領域とみなす。
                // 既定の設置場所（exe_files 直下 / exe_files/<対象ID>）以外に置かれていても、
                // 更新時は既定の場所へ設置して設定をそちらへ移す。元の場所は利用者が
                // 置いたものなので削除しない。
                var rootPrefix = executableRoot.EndsWith(Path.DirectorySeparatorChar)
                    ? executableRoot : executableRoot + Path.DirectorySeparatorChar;
                return fullPath.StartsWith(rootPrefix, comparison) &&
                    target.Payload.Any(entry => entry.IsMatch(Path.GetFileName(fullPath)))
                    ? SettingPathKind.InstalledPayload : SettingPathKind.Other;
            }
            catch
            {
                return SettingPathKind.Other;
            }
        }

        private enum SettingPathKind
        {
            Empty,
            BarePayload,
            InstalledPayload,
            Other,
        }

        private static bool IsBareExecutableName(string path) =>
            !string.IsNullOrWhiteSpace(path) && !Path.IsPathFullyQualified(path) &&
            path.IndexOf(Path.DirectorySeparatorChar) < 0 &&
            path.IndexOf(Path.AltDirectorySeparatorChar) < 0 &&
            path.IndexOf('/') < 0 && path.IndexOf('\\') < 0;

        private static bool AreSamePath(string first, string second, StringComparison comparison)
        {
            try
            {
                return Path.IsPathFullyQualified(first) && Path.IsPathFullyQualified(second) &&
                    string.Equals(Path.GetFullPath(first), Path.GetFullPath(second), comparison);
            }
            catch
            {
                return false;
            }
        }

        internal static string FindExtractor(string root)
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

        private void CompleteCheckJob(UpdateJobRecord job, bool succeeded)
        {
            job.Complete(succeeded);
            lock (jobLock)
            {
                if (ReferenceEquals(activeCheckJob, job))
                {
                    activeCheckJob = null;
                }
                TrimJobsLocked();
            }
        }

        private void CompleteApplyJob(UpdateJobRecord job, bool succeeded)
        {
            job.Complete(succeeded);
            lock (jobLock)
            {
                if (ReferenceEquals(activeApplyJob, job))
                {
                    activeApplyJob = null;
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
                if (jobs.Remove(oldestId, out var removed))
                {
                    removed.Dispose();
                }
            }
        }

        internal sealed record UpdateApplyTarget(UpdateTargetDef Target, ReleaseAssetInfo Asset,
            string ExpectedVersion);

        internal sealed class UpdateJobRecord
        {
            private const int MaxRecentLogLines = 50;
            private readonly object sync = new object();
            private readonly Queue<string> recentLogLines = new Queue<string>();
            private readonly TaskCompletionSource<bool> completion =
                new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            private readonly CancellationTokenSource jobCancellation;
            private readonly List<UpdateTargetResultView> targetResults =
                new List<UpdateTargetResultView>();
            private string txId;
            private string currentTargetId;
            private string currentStage;
            private string logFilePath;
            private bool finished;
            private bool succeeded;
            private long receivedBytes;
            private long totalBytes;
            private double speedBytesPerSec;

            public UpdateJobRecord(string jobId, CancellationToken managerCancellation,
                bool isApply = false)
            {
                JobId = jobId;
                IsApply = isApply;
                jobCancellation = CancellationTokenSource.CreateLinkedTokenSource(
                    managerCancellation);
            }

            public string JobId { get; }
            public bool IsApply { get; }
            public Task Completion => completion.Task;
            public CancellationToken Token => jobCancellation.Token;

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

            public void ReportProgress(DownloadProgress progress)
            {
                lock (sync)
                {
                    receivedBytes = progress.ReceivedBytes;
                    totalBytes = progress.TotalBytes;
                    speedBytesPerSec = progress.SpeedBytesPerSec;
                }
            }

            public void ResetProgress(string targetId)
            {
                lock (sync)
                {
                    currentTargetId = targetId;
                    currentStage = "S01_PRECHECK";
                    receivedBytes = 0;
                    totalBytes = 0;
                    speedBytesPerSec = 0;
                }
            }

            public void AddTargetResult(string targetId, bool result, string errorCode,
                string message)
            {
                lock (sync)
                {
                    targetResults.Add(new UpdateTargetResultView
                    {
                        TargetId = targetId,
                        Succeeded = result,
                        ErrorCode = errorCode,
                        Message = message,
                    });
                }
            }

            public bool Cancel()
            {
                lock (sync)
                {
                    if (finished || jobCancellation.IsCancellationRequested)
                    {
                        return false;
                    }
                }
                jobCancellation.Cancel();
                return true;
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
                        ReceivedBytes = receivedBytes,
                        TotalBytes = totalBytes,
                        SpeedBytesPerSec = speedBytesPerSec,
                        Finished = finished,
                        Succeeded = succeeded,
                        RecentLogLines = recentLogLines.ToList(),
                        TargetResults = targetResults.Select(result => new UpdateTargetResultView
                        {
                            TargetId = result.TargetId,
                            Succeeded = result.Succeeded,
                            ErrorCode = result.ErrorCode,
                            Message = result.Message,
                        }).ToList(),
                    };
                }
            }

            public void Dispose()
            {
                jobCancellation.Dispose();
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
