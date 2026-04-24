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
    /// 実行は全体で1本に制限し、同一serviceIdの同時実行も行わない。
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

        private void ScheduleEligiblePendingItems(QueueItem preferredItem)
        {
            var notifyItems = new List<QueueItem>();
            var shouldSignal = false;

            lock (sync)
            {
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

                        if (!IsEligibleItem(request.Item) || runningServices.Contains(request.ServiceId))
                        {
                            request.Item.ClearAutoLogoTransientState();
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
                    }
                }

                request.Item.AutoLogoQueued = false;
                request.Item.AutoLogoInProgress = false;
                request.Item.AutoLogoResult = success ? AutoLogoResultState.Success : AutoLogoResultState.Failed;
                request.Item.AutoLogoLastMessage = string.IsNullOrWhiteSpace(message)
                    ? (success ? "自動ロゴ生成に成功" : "自動ロゴ生成に失敗")
                    : message;

                await server.NotifyQueueItemUpdate(request.Item).ConfigureAwait(false);
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
                var rect = LogoFile.AutoDetectLogoRect(
                    ctx, request.SrcPath, request.ServiceId,
                    divX, divY, searchFrames, blockSize, threshold,
                    marginX, marginY, threadN,
                    scorePath, binaryPath, cclPath, null, null, null, null, null, null, null, null, null, null, null,
                    detailedDebug,
                    (stage, stageProgress, progress, nread, total) =>
                    {
                        progressLogger.Report(stage, stageProgress, progress, nread, total);
                        return true;
                    });
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
                LogoFile.ScanLogo(ctx, request.SrcPath, request.ServiceId, workfile, tmppath, null,
                    imgx, imgy, w, h, threshold, searchFrames,
                    (progress, nread, total, ngather) => true,
                    true);

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
            using (var ctx = new AMTContext())
            using (var logo = new LogoFile(ctx, outpath))
            {
                serviceId = logo.ServiceId;
            }

            var data = File.ReadAllBytes(outpath);
            server.SendLogoFile(new LogoFileData()
            {
                ServiceId = serviceId,
                LogoIdx = 1,
                Data = data
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
            if (item.Profile != null && item.Profile.DisableChapter && item.Profile.NoDelogo)
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
            return server.AppData_ != null && server.AppData_.setting != null && server.AppData_.setting.AutoLogoPendingEnabled;
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
