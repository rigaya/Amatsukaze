using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Amatsukaze.Lib;

namespace Amatsukaze.Server
{
    /// <summary>
    /// ロゴ設定不足でLogoPendingになったタスクに対して、
    /// バックグラウンドでロゴ自動検出→ロゴ解析→採用までを自動実行する。
    /// 実行は全体で1本に制限し、同一serviceIdの待機・同時実行も行わない。
    ///
    /// 自動生成の成否はタスク単位で管理する。
    /// あるタスクで失敗しても同じserviceIdの別タスクは候補にできるが、
    /// 失敗したタスク自身はAutoLogoResultがFailedになるため自動再試行しない。
    ///
    /// 待機中または実行中に手動ロゴ解析で同じタスクのロゴが採用された場合は、
    /// 自動生成が完了しても結果を保存せず破棄する。
    /// </summary>
    internal class AutoLogoPendingResolver
    {
        private const string MissingLogoReason = "ロゴ設定がありません";

        private readonly EncodeServer server;
        private readonly object sync = new object();
        private readonly Queue<AutoRequest> pendingRequests = new Queue<AutoRequest>();
        private readonly HashSet<int> queuedTaskIds = new HashSet<int>();
        private readonly HashSet<int> queuedServices = new HashSet<int>();
        private readonly HashSet<int> runningServices = new HashSet<int>();
        private readonly HashSet<int> manualAcceptedTaskIds = new HashSet<int>();
        private readonly SemaphoreSlim requestSignal = new SemaphoreSlim(0);

        private int runningTaskId = -1;

        public AutoLogoPendingResolver(EncodeServer server)
        {
            this.server = server;
            _ = Task.Run(WorkerLoop);
        }

        public void TryKick(QueueItem item)
        {
            ScheduleEligiblePendingItems(item);
        }

        public void ScheduleEligiblePendingItems()
        {
            ScheduleEligiblePendingItems(null);
        }

        public void NotifyManualLogoAccepted(int queueItemId)
        {
            if (queueItemId <= 0)
            {
                return;
            }

            lock (sync)
            {
                // 手動採用による破棄対象は、このresolverが管理中のタスクだけに限定する。
                // 通常のロゴ追加や、既に自動処理が終わったタスクの採用履歴を残す必要はない。
                if (queuedTaskIds.Contains(queueItemId) || runningTaskId == queueItemId)
                {
                    manualAcceptedTaskIds.Add(queueItemId);
                }
            }
        }

        public bool ShouldDiscardAutoResult(int queueItemId)
        {
            if (queueItemId <= 0)
            {
                return false;
            }

            lock (sync)
            {
                return manualAcceptedTaskIds.Contains(queueItemId);
            }
        }

        private void ScheduleEligiblePendingItems(QueueItem preferredItem)
        {
            var notifyItems = new List<QueueItem>();
            var shouldSignal = false;

            lock (sync)
            {
                // きっかけになったタスクを先に評価し、その後にキュー全体を登録順で再走査する。
                // これにより、あるタスクの自動生成が失敗しても同じserviceIdの別タスクを次候補にできる。
                if (preferredItem != null)
                {
                    shouldSignal |= TryEnqueueNoLock(preferredItem, notifyItems);
                }

                foreach (var item in server.GetQueueSnapshot().OrderBy(item => item.Order))
                {
                    if (preferredItem != null && item.Id == preferredItem.Id)
                    {
                        continue;
                    }
                    shouldSignal |= TryEnqueueNoLock(item, notifyItems);
                }
            }

            foreach (var item in notifyItems)
            {
                _ = server.NotifyQueueItemUpdate(item);
            }

            if (shouldSignal)
            {
                requestSignal.Release();
            }
        }

        private bool TryEnqueueNoLock(QueueItem item, List<QueueItem> notifyItems)
        {
            if (!IsEligibleNoLock(item))
            {
                return false;
            }

            pendingRequests.Enqueue(new AutoRequest()
            {
                QueueItemId = item.Id,
                ServiceId = item.ServiceId,
                SrcPath = item.SrcPath,
                Item = item
            });
            queuedTaskIds.Add(item.Id);
            queuedServices.Add(item.ServiceId);
            item.AutoLogoQueued = true;
            item.AutoLogoInProgress = false;
            item.AutoLogoLastMessage = "自動ロゴ生成待ち";
            notifyItems.Add(item);
            return true;
        }

        private bool IsEligibleNoLock(QueueItem item)
        {
            if (!IsEnabled())
            {
                return false;
            }
            if (!IsEligibleItem(item))
            {
                return false;
            }
            if (queuedTaskIds.Contains(item.Id))
            {
                return false;
            }
            if (runningTaskId == item.Id)
            {
                return false;
            }
            if (runningServices.Contains(item.ServiceId))
            {
                return false;
            }
            // 同じserviceIdの複数タスクを同時に待機列へ入れない。
            // 先行タスクが失敗または成功した後にキュー全体を再走査し、次の候補を選ぶ。
            if (queuedServices.Contains(item.ServiceId))
            {
                return false;
            }
            return true;
        }

        private bool IsEligibleItem(QueueItem item)
        {
            if (item == null)
            {
                return false;
            }
            if (item.State != QueueState.LogoPending)
            {
                return false;
            }
            // AutoLogoResultはタスク単位の試行結果。
            // Failedのタスク自身は自動再試行しないが、同じserviceIdの別タスクはNoneなら候補にできる。
            if (item.AutoLogoResult != AutoLogoResultState.None)
            {
                return false;
            }
            if (item.ServiceId <= 0 || string.IsNullOrEmpty(item.SrcPath) || !File.Exists(item.SrcPath))
            {
                return false;
            }
            if (item.FailReason != MissingLogoReason)
            {
                return false;
            }
            // 待機列投入時と実行開始直前の両方で確認する。
            // 手動採用や別タスクの自動成功でロゴが追加済みなら、自動解析は不要。
            if (HasUsableLogoSetting(item))
            {
                return false;
            }
            return true;
        }

        private async Task WorkerLoop()
        {
            while (true)
            {
                await requestSignal.WaitAsync().ConfigureAwait(false);

                AutoRequest request = null;
                QueueItem skippedItem = null;
                for (;;)
                {
                    lock (sync)
                    {
                        if (pendingRequests.Count == 0)
                        {
                            request = null;
                            break;
                        }

                        request = pendingRequests.Dequeue();
                        queuedTaskIds.Remove(request.QueueItemId);
                        queuedServices.Remove(request.ServiceId);

                        // 待機中に手動採用や別タスクの自動成功でロゴが追加されることがある。
                        // 実行開始直前にも候補条件を再評価し、既に使えるロゴがあれば何もせず捨てる。
                        if (!IsEligibleItem(request.Item) || runningServices.Contains(request.ServiceId))
                        {
                            request.Item.ClearAutoLogoTransientState();
                            manualAcceptedTaskIds.Remove(request.QueueItemId);
                            skippedItem = request.Item;
                            request = null;
                        }
                        else
                        {
                            runningTaskId = request.QueueItemId;
                            runningServices.Add(request.ServiceId);
                            request.Item.AutoLogoQueued = false;
                            request.Item.AutoLogoInProgress = true;
                            request.Item.AutoLogoLastMessage = "自動ロゴ生成中";
                            skippedItem = null;
                        }
                    }

                    if (skippedItem != null)
                    {
                        await server.NotifyQueueItemUpdate(skippedItem).ConfigureAwait(false);
                        skippedItem = null;
                        continue;
                    }
                    break;
                }

                if (request == null)
                {
                    continue;
                }

                await server.NotifyQueueItemUpdate(request.Item).ConfigureAwait(false);

                var success = false;
                var message = string.Empty;
                try
                {
                    message = RunCore(request);
                    success = true;
                }
                catch (OperationCanceledException)
                {
                    // キューからの取消は通常の解析失敗ではないため、エラー通知を出さない。
                    message = "自動ロゴ生成をキャンセルしました";
                }
                catch (Exception ex)
                {
                    message = ex.Message;
                    _ = server.NotifyError(
                        $"[AutoLogoPending] 失敗: QID={request.QueueItemId}, SID={request.ServiceId}, reason={ex.Message}",
                        true);
                }
                finally
                {
                    lock (sync)
                    {
                        if (runningTaskId == request.QueueItemId)
                        {
                            runningTaskId = -1;
                        }
                        runningServices.Remove(request.ServiceId);
                        manualAcceptedTaskIds.Remove(request.QueueItemId);
                    }
                }

                request.Item.AutoLogoQueued = false;
                request.Item.AutoLogoInProgress = false;
                request.Item.AutoLogoResult = success ? AutoLogoResultState.Success : AutoLogoResultState.Failed;
                request.Item.AutoLogoLastMessage = string.IsNullOrWhiteSpace(message)
                    ? (success ? "自動ロゴ生成に成功" : "自動ロゴ生成に失敗")
                    : message;

                await server.NotifyQueueItemUpdate(request.Item).ConfigureAwait(false);
                // 成否確定後にキュー全体を再走査する。
                // 失敗時は同じserviceIdの別タスク、成功時はまだロゴ未取得の別serviceIdが次候補になる。
                ScheduleEligiblePendingItems();
            }
        }

        private string RunCore(AutoRequest request)
        {
            var setting = server.AppData_ != null ? server.AppData_.setting : null;
            if (setting == null)
            {
                throw new InvalidOperationException("設定が取得できません");
            }

            var workPath = setting.WorkPath;
            if (string.IsNullOrWhiteSpace(workPath))
            {
                workPath = Directory.GetCurrentDirectory();
            }
            Directory.CreateDirectory(workPath);
            CleanupOldPass0Directories(workPath);

            var jobId = Guid.NewGuid().ToString("N");
            var scorePath = Path.Combine(workPath, "logo-auto-score-" + jobId + ".bmp");
            var binaryPath = Path.Combine(workPath, "logo-auto-binary-" + jobId + ".bmp");
            var cclPath = Path.Combine(workPath, "logo-auto-ccl-" + jobId + ".bmp");
            var workfile = Path.Combine(workPath, "logo-auto-work-" + jobId + ".dat");
            var tmppath = Path.Combine(workPath, "logo-auto-" + jobId + ".tmp.lgd");
            var outpath = Path.Combine(workPath, "logo-auto-" + jobId + ".lgd");

            var divX = setting.AutoLogoPendingDivX;
            var divY = setting.AutoLogoPendingDivY;
            var searchFrames = setting.AutoLogoPendingSearchFrames;
            var blockSize = setting.AutoLogoPendingBlockSize;
            var threshold = setting.AutoLogoPendingThreshold;
            var marginX = setting.AutoLogoPendingMarginX;
            var marginY = setting.AutoLogoPendingMarginY;
            var threadN = AutoLogoThreadResolver.Resolve(setting.AutoLogoPendingThreadN);
            var detailedDebug = setting.AutoLogoPendingDetailedDebug;
            var rectX = 0;
            var rectY = 0;
            var rectW = 0;
            var rectH = 0;
            var progressLogger = new LogoAutoDetectProgressLogger("[AutoLogoPending]", request.SrcPath);

            ThrowIfCanceled(request);

            Util.AddLog(
                "[AutoLogoPending] 開始: " +
                "QID=" + request.QueueItemId +
                ", SID=" + request.ServiceId +
                ", file=" + request.SrcPath +
                ", autoDetect={div=" + divX + "x" + divY +
                ", searchFrames=" + searchFrames +
                ", blockSize=" + blockSize +
                ", threshold=" + threshold +
                ", margin=(" + marginX + "," + marginY + ")" +
                ", threadN=" + threadN +
                ", detailedDebug=" + detailedDebug + "}",
                null);
            _ = server.NotifyMessage(
                "[AutoLogoPending] 開始: QID=" + request.QueueItemId + ", SID=" + request.ServiceId + ", file=" + Path.GetFileName(request.SrcPath),
                false);

            using (var ctx = new AMTContext())
            {
                var rect = AutoDetectRectWithPass0OrFallback(
                    request, ctx, workPath,
                    divX, divY, searchFrames, blockSize, threshold, marginX, marginY, threadN,
                    scorePath, binaryPath, cclPath, detailedDebug, progressLogger);
                rectX = rect.X;
                rectY = rect.Y;
                rectW = rect.W;
                rectH = rect.H;
                Util.AddLog(
                    "[AutoLogoPending] ロゴ枠検出完了: " +
                    "QID=" + request.QueueItemId +
                    ", SID=" + request.ServiceId +
                    ", rect=(" + rectX + "," + rectY + "," + rectW + "," + rectH + ")" +
                    ", pass2={entered=" + rect.Pass2Entered +
                    ", prepare=" + rect.Pass2PrepareSucceeded +
                    ", collect=" + rect.Pass2CollectSucceeded +
                    ", fallback=" + rect.Pass2RescueFallbackApplied +
                    ", acceptedFrames=" + rect.Pass2AcceptedFrames +
                    ", skippedFrames=" + rect.Pass2SkippedFrames + "}",
                    null);

                var imgx = (int)Math.Floor(rect.X / 2.0) * 2;
                var imgy = (int)Math.Floor(rect.Y / 2.0) * 2;
                var w = (int)Math.Ceiling(rect.W / 2.0) * 2;
                var h = (int)Math.Ceiling(rect.H / 2.0) * 2;

                Util.AddLog(
                    "[AutoLogoPending] ロゴ生成開始: " +
                    "QID=" + request.QueueItemId +
                    ", SID=" + request.ServiceId +
                    ", rectAligned=(" + imgx + "," + imgy + "," + w + "," + h + ")" +
                    ", threshold=" + threshold +
                    ", maxFrames=" + searchFrames,
                    null);
                ThrowIfCanceled(request);
                try
                {
                    LogoFile.ScanLogo(ctx, request.SrcPath, request.ServiceId, workfile, tmppath, null,
                        imgx, imgy, w, h, threshold, searchFrames,
                        (progress, nread, total, ngather) => !IsCanceled(request),
                        true);
                }
                catch (IOException) when (IsCanceled(request))
                {
                    throw new OperationCanceledException("自動ロゴ生成をキャンセルしました");
                }
                ThrowIfCanceled(request);

                using (var info = new TsInfo(ctx))
                {
                    if (info.ReadFile(request.SrcPath))
                    {
                        using (var logo = new LogoFile(ctx, tmppath))
                        {
                            if (info.HasServiceInfo)
                            {
                                var logoServiceId = logo.ServiceId;
                                var service = info.GetServiceList().FirstOrDefault(s => s.ServiceId == logoServiceId);
                                var date = info.GetTime().ToString("yyyy-MM-dd");
                                logo.Name = (service != null) ? (service.ServiceName + "(" + date + ")") : "情報なし";
                            }
                            else
                            {
                                logo.Name = "情報なし";
                            }
                            logo.Save(outpath);
                        }
                    }
                    else
                    {
                        using (var logo = new LogoFile(ctx, tmppath))
                        {
                            logo.Name = "情報なし";
                            logo.Save(outpath);
                        }
                    }
                }
            }

            int serviceId;
            ThrowIfCanceled(request);
            using (var ctx = new AMTContext())
            using (var logo = new LogoFile(ctx, outpath))
            {
                serviceId = logo.ServiceId;
            }

            if (ShouldDiscardAutoResult(request.QueueItemId))
            {
                var discardMessage = "手動採用済みのため自動ロゴ生成結果を破棄";
                _ = server.NotifyMessage(
                    "[AutoLogoPending] " + discardMessage + ": QID=" + request.QueueItemId + ", SID=" + request.ServiceId,
                    false);
                return discardMessage;
            }

            ThrowIfCanceled(request);
            var data = File.ReadAllBytes(outpath);
            ThrowIfCanceled(request);
            server.SendLogoFile(new LogoFileData()
            {
                ServiceId = serviceId,
                LogoIdx = 1,
                Data = data,
                SourceQueueItemId = request.QueueItemId,
                IsAutoLogoPendingResult = true
            }).GetAwaiter().GetResult();
            server.RequestLogoRescan();
            WaitForLogoRefresh(serviceId);

            var result = "自動ロゴ生成に成功";
            _ = server.NotifyMessage(
                "[AutoLogoPending] 成功: QID=" + request.QueueItemId + ", SID=" + request.ServiceId +
                ", rect=(" + rectX + "," + rectY + "," + rectW + "," + rectH + "), search=" + searchFrames,
                false);
            return result;
        }

        private AutoDetectLogoRectResult AutoDetectRectWithPass0OrFallback(
            AutoRequest request, AMTContext ctx, string workPath,
            int divX, int divY, int searchFrames, int blockSize, int threshold, int marginX, int marginY, int threadN,
            string scorePath, string binaryPath, string cclPath, bool detailedDebug, LogoAutoDetectProgressLogger progressLogger)
        {
            Pass0Job pass0 = null;
            try
                {
                    pass0 = CreatePass0Job(workPath);
                }
                catch (Exception ex)
                {
                    Util.AddLog("[AutoLogoPending] pass0用一時フォルダを作成できないため従来入力へフォールバックします", ex);
                }
                Pass0Artifact artifact = null;
                try
                {
                    if (pass0 != null)
                    {
                        artifact = RunPass0Cli(request, pass0, workPath);
                    }
                }
                catch (OperationCanceledException)
                {
                    DeletePass0Job(pass0);
                    throw;
                }
                catch (Exception ex)
                {
                    // CLI起動、出力読込、成果物検証の失敗は前処理だけの失敗として扱う。
                    Util.AddLog("[AutoLogoPending] pass0 CM解析を実行できないため従来入力へフォールバックします", ex);
                }
                return AutoLogoPass0Validation.ExecutePass0OrLegacy(
                    artifact,
                    pass0Artifact =>
                    {
                        try
                        {
                            var result = LogoFile.AutoDetectLogoRectWithPass0(
                                ctx, request.SrcPath, request.ServiceId, pass0Artifact.AmtSourcePath, pass0Artifact.TrimAvsPath,
                                divX, divY, searchFrames, blockSize, threshold, marginX, marginY, threadN,
                                scorePath, binaryPath, cclPath, null, null, null, null, null, null, null, null, null, null, null,
                                detailedDebug,
                                (stage, stageProgress, progress, nread, total) =>
                                {
                                    progressLogger.Report(stage, stageProgress, progress, nread, total);
                                    return !IsCanceled(request);
                                });
                            ThrowIfCanceled(request);
                            Util.AddLog("[AutoLogoPending] pass0結果: state=" + result.Pass0State +
                                ", accepted=" + result.Pass0AcceptedFrames + ", skippedCM=" + result.Pass0SkippedCmFrames, null);
                            return result;
                        }
                        catch (AutoDetectLogoRectException) when (IsCanceled(request))
                        {
                            throw new OperationCanceledException("自動ロゴ生成のロゴ枠検出をキャンセルしました");
                        }
                    },
                    () =>
                    {
                        Util.AddLog("[AutoLogoPending] pass0成果物を使えないため従来入力でロゴ枠検出を実行します", null);
                        try
                        {
                            return LogoFile.AutoDetectLogoRect(
                                ctx, request.SrcPath, request.ServiceId,
                                divX, divY, searchFrames, blockSize, threshold, marginX, marginY, threadN,
                                scorePath, binaryPath, cclPath, null, null, null, null, null, null, null, null, null, null, null,
                                detailedDebug,
                                (stage, stageProgress, progress, nread, total) =>
                                {
                                    progressLogger.Report(stage, stageProgress, progress, nread, total);
                                    return !IsCanceled(request);
                                });
                        }
                        catch (AutoDetectLogoRectException) when (IsCanceled(request))
                        {
                            throw new OperationCanceledException("自動ロゴ生成のロゴ枠検出をキャンセルしました");
                        }
                    },
                () => DeletePass0Job(pass0),
                () => IsCanceled(request));
        }

        private Pass0Artifact RunPass0Cli(AutoRequest request, Pass0Job pass0, string workPath)
        {
            ThrowIfCanceled(request);
            var args = server.MakeAutoLogoPass0Arguments(request.Item, workPath, pass0.DirectoryPath, pass0.OutputBasePath);
            var setting = server.AppData_.setting;
            var psi = new System.Diagnostics.ProcessStartInfo(setting.AmatsukazePath)
            {
                UseShellExecute = false,
                WorkingDirectory = Directory.GetCurrentDirectory(),
                RedirectStandardError = true,
                RedirectStandardOutput = true,
                RedirectStandardInput = false,
                StandardOutputEncoding = Util.AmatsukazeDefaultEncoding,
                StandardErrorEncoding = Util.AmatsukazeDefaultEncoding,
                CreateNoWindow = true,
            };
            foreach (var arg in args)
            {
                psi.ArgumentList.Add(arg);
            }
            Util.AddLog("[AutoLogoPending] pass0 CM解析開始: " + setting.AmatsukazePath + " " + string.Join(" ", args), null);
            using (var process = new NormalProcess(psi))
            {
                process.OnOutput = (buffer, offset, count) =>
                {
                    var text = Util.AmatsukazeDefaultEncoding.GetString(buffer, offset, count);
                    if (!string.IsNullOrWhiteSpace(text))
                    {
                        Util.AddLog("[AutoLogoPending/pass0] " + text.TrimEnd(), null);
                    }
                    return Task.CompletedTask;
                };
                var waitTask = process.WaitForExitAsync();
                while (!waitTask.Wait(250))
                {
                    if (!IsCanceled(request))
                    {
                        continue;
                    }
                    process.Canel();
                    waitTask.GetAwaiter().GetResult();
                    throw new OperationCanceledException("自動ロゴ生成のpass0 CM解析をキャンセルしました");
                }
                waitTask.GetAwaiter().GetResult();
                ThrowIfCanceled(request);
                if (process.Process.ExitCode != 0)
                {
                    Util.AddLog("[AutoLogoPending] pass0 CM解析が終了コード" + process.Process.ExitCode + "で失敗しました", null);
                    return null;
                }
            }
            if (!TryGetPass0Artifact(pass0, out var artifact))
            {
                Util.AddLog("[AutoLogoPending] pass0成果物が不完全なため従来入力へフォールバックします", null);
                return null;
            }
            return artifact;
        }

        private static Pass0Job CreatePass0Job(string workPath)
        {
            for (var i = 0; i < 16; ++i)
            {
                var token = Guid.NewGuid().ToString("N");
                var path = Path.Combine(workPath, "logo-pass0-" + token);
                try
                {
                    if (!AutoLogoPass0Validation.TryCreateDirectoryAtomically(path))
                    {
                        continue;
                    }
                    var markerPath = Path.Combine(path, ".logo-pass0-owner");
                    using (var marker = new FileStream(markerPath, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                    using (var writer = new StreamWriter(marker, new System.Text.UTF8Encoding(false)))
                    {
                        writer.Write(token);
                    }
                    // marker以外が存在した場合は所有権を証明できないため、そのフォルダには触れない。
                    if (Directory.EnumerateFileSystemEntries(path).Any(entry => !string.Equals(entry, markerPath, StringComparison.Ordinal)))
                    {
                        AutoLogoPass0Validation.CleanupUnownedCreationCandidate(path, markerPath, token);
                        continue;
                    }
                    return new Pass0Job(path, token, markerPath);
                }
                catch (IOException)
                {
                    AutoLogoPass0Validation.CleanupUnownedCreationCandidate(path, Path.Combine(path, ".logo-pass0-owner"), token);
                    // GUID衝突、marker作成失敗、競合時だけ別の名前で再試行する。
                }
                catch (UnauthorizedAccessException)
                {
                    AutoLogoPass0Validation.CleanupUnownedCreationCandidate(path, Path.Combine(path, ".logo-pass0-owner"), token);
                }
            }
            throw new IOException("pass0用一時フォルダを作成できません");
        }

        private static bool TryGetPass0Artifact(Pass0Job job, out Pass0Artifact artifact)
        {
            artifact = null;
            try
            {
                var amts = Path.Combine(job.DirectoryPath, "pass0.amts");
                var trim = Path.Combine(job.DirectoryPath, "pass0.trim.avs");
                if (!AutoLogoPass0Validation.HasCompleteArtifact(job.DirectoryPath))
                {
                    return false;
                }
                artifact = new Pass0Artifact(amts, trim);
                return true;
            }
            catch (Exception ex) when (ex is IOException || ex is UnauthorizedAccessException)
            {
                return false;
            }
        }

        private static void DeletePass0Job(Pass0Job job)
        {
            if (job == null || Interlocked.Exchange(ref job.DeleteStarted, 1) != 0)
            {
                return;
            }
            try
            {
                if (Directory.Exists(job.DirectoryPath) && !AutoLogoPass0Validation.TryDeleteOwnedJob(job.DirectoryPath, job.Token))
                {
                    Util.AddLog("[AutoLogoPending] pass0一時フォルダの所有権を確認できないため削除しません: " + job.DirectoryPath, null);
                }
            }
            catch (Exception ex)
            {
                Util.AddLog("[AutoLogoPending] pass0一時フォルダを削除できません: " + job.DirectoryPath, ex);
            }
        }

        private static void CleanupOldPass0Directories(string workPath)
        {
            try
            {
                foreach (var path in Directory.EnumerateDirectories(workPath, "logo-pass0-*", SearchOption.TopDirectoryOnly))
                {
                    try
                    {
                        if (!AutoLogoPass0Validation.CanCollectOwnedJob(path, DateTime.UtcNow))
                        {
                            continue;
                        }
                        Directory.Delete(path, true);
                    }
                    catch (Exception ex)
                    {
                        Util.AddLog("[AutoLogoPending] 古いpass0一時フォルダを回収できません: " + path, ex);
                    }
                }
            }
            catch (Exception ex)
            {
                Util.AddLog("[AutoLogoPending] pass0一時フォルダの期限回収を開始できません", ex);
            }
        }

        private static bool IsCanceled(AutoRequest request)
        {
            return request.Item.State == QueueState.Canceled;
        }

        private static void ThrowIfCanceled(AutoRequest request)
        {
            if (IsCanceled(request))
            {
                throw new OperationCanceledException("自動ロゴ生成をキャンセルしました");
            }
        }

        private sealed class Pass0Job
        {
            public Pass0Job(string directoryPath, string token, string markerPath)
            {
                DirectoryPath = directoryPath;
                Token = token;
                MarkerPath = markerPath;
                OutputBasePath = Path.Combine(directoryPath, "pass0");
            }
            public string DirectoryPath { get; }
            public string Token { get; }
            public string MarkerPath { get; }
            public string OutputBasePath { get; }
            public int DeleteStarted;
        }

        private sealed class Pass0Artifact
        {
            public Pass0Artifact(string amtSourcePath, string trimAvsPath)
            {
                AmtSourcePath = amtSourcePath;
                TrimAvsPath = trimAvsPath;
            }
            public string AmtSourcePath { get; }
            public string TrimAvsPath { get; }
        }

        private void WaitForLogoRefresh(int serviceId)
        {
            for (int i = 0; i < 25; ++i)
            {
                if (HasAnyExistingLogo(serviceId))
                {
                    return;
                }
                Thread.Sleep(200);
            }
        }

        private bool HasAnyExistingLogo(int serviceId)
        {
            ServiceSettingElement service;
            if (!server.ServiceMap.TryGetValue(serviceId, out service) || service.LogoSettings == null)
            {
                return false;
            }
            return service.LogoSettings.Any(logo => logo.Exists && logo.FileName != LogoSetting.NO_LOGO);
        }

        private bool HasUsableLogoSetting(QueueItem item)
        {
            if (item.Profile != null && item.Profile.NoDelogo &&
                (item.Profile.DisableChapter || item.Profile.NoLogoInCM))
            {
                return true;
            }

            ServiceSettingElement service;
            if (!server.ServiceMap.TryGetValue(item.ServiceId, out service) || service.LogoSettings == null)
            {
                return false;
            }
            return service.LogoSettings.Any(logo => logo.CanUse(item.TsTime));
        }

        private bool IsEnabled()
        {
            return server.AppData_ != null && server.AppData_.setting != null && server.AppData_.setting.AutoLogoPendingDisabled == false;
        }

        private class AutoRequest
        {
            public int QueueItemId { get; set; }
            public int ServiceId { get; set; }
            public string SrcPath { get; set; }
            public QueueItem Item { get; set; }
        }
    }
}
