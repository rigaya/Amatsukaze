using System;
using System.Collections.Concurrent;
using System.IO;
using System.Linq;
using Amatsukaze.Lib;

namespace Amatsukaze.Server
{
    /// <summary>
    /// ロゴ設定不足でLogoPendingになったタスクに対して、
    /// バックグラウンドでロゴ自動検出→ロゴ解析→採用までを自動実行する。
    /// 
    /// 失敗したサービスIDはラッチし、手動操作（サービス設定更新/ロゴ投入）まで再試行しない。
    /// </summary>
    internal class AutoLogoPendingResolver
    {
        private readonly EncodeServer server;
        private readonly ConcurrentDictionary<int, byte> runningServices = new ConcurrentDictionary<int, byte>();
        private readonly ConcurrentDictionary<int, string> failureLatch = new ConcurrentDictionary<int, string>();

        private const int DefaultDivX = 5;
        private const int DefaultDivY = 5;
        private const int DefaultSearchFrames = 10000;
        private const int DefaultBlockSize = 32;
        private const int DefaultThreshold = 12;
        private const int DefaultMarginX = 6;
        private const int DefaultMarginY = 6;
        private const int DefaultThreadN = 0;
        private const bool DefaultDetailedDebug = false;

        public AutoLogoPendingResolver(EncodeServer server)
        {
            this.server = server;
        }

        public void TryKick(QueueItem item)
        {
            if (item == null)
            {
                return;
            }
            if (!IsEnabled())
            {
                return;
            }
            if (item.ServiceId <= 0 || string.IsNullOrEmpty(item.SrcPath) || !File.Exists(item.SrcPath))
            {
                return;
            }
            if (failureLatch.ContainsKey(item.ServiceId))
            {
                return;
            }
            if (!runningServices.TryAdd(item.ServiceId, 0))
            {
                return;
            }

            var request = new AutoRequest()
            {
                QueueItemId = item.Id,
                ServiceId = item.ServiceId,
                SrcPath = item.SrcPath,
            };

            _ = System.Threading.Tasks.Task.Run(() => RunCore(request));
        }

        public void ClearFailureLatch(int serviceId, string reason)
        {
            if (serviceId <= 0)
            {
                return;
            }
            if (failureLatch.TryRemove(serviceId, out var old))
            {
                _ = server.NotifyMessage($"[AutoLogoPending] 失敗ラッチ解除: SID={serviceId} ({reason}) 以前の理由: {old}", false);
            }
        }

        private void RunCore(AutoRequest request)
        {
            var sid = request.ServiceId;
            try
            {
                var setting = server.AppData_?.setting;
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
                var scorePath = Path.Combine(workPath, $"logo-auto-score-{jobId}.bmp");
                var binaryPath = Path.Combine(workPath, $"logo-auto-binary-{jobId}.bmp");
                var cclPath = Path.Combine(workPath, $"logo-auto-ccl-{jobId}.bmp");
                var workfile = Path.Combine(workPath, $"logo-auto-work-{jobId}.dat");
                var tmppath = Path.Combine(workPath, $"logo-auto-{jobId}.tmp.lgd");
                var outpath = Path.Combine(workPath, $"logo-auto-{jobId}.lgd");

                var divX = DefaultDivX;
                var divY = DefaultDivY;
                var searchFrames = DefaultSearchFrames;
                var blockSize = DefaultBlockSize;
                var threshold = DefaultThreshold;
                var marginX = DefaultMarginX;
                var marginY = DefaultMarginY;
                var threadN = AutoLogoThreadResolver.Resolve(DefaultThreadN);
                var detailedDebug = DefaultDetailedDebug;
                var rectX = 0;
                var rectY = 0;
                var rectW = 0;
                var rectH = 0;

                _ = server.NotifyMessage(
                    $"[AutoLogoPending] 開始: QID={request.QueueItemId}, SID={sid}, file={Path.GetFileName(request.SrcPath)}",
                    false);

                using (var ctx = new AMTContext())
                {
                    var rect = LogoFile.AutoDetectLogoRect(
                        ctx, request.SrcPath, sid,
                        divX, divY, searchFrames, blockSize, threshold,
                        marginX, marginY, threadN,
                        scorePath, binaryPath, cclPath, null, null, null, null, null, null, null, null, null, null, null,
                        detailedDebug,
                        (stage, stageProgress, progress, nread, total) => true);
                    rectX = rect.X;
                    rectY = rect.Y;
                    rectW = rect.W;
                    rectH = rect.H;

                    var imgx = (int)Math.Floor(rect.X / 2.0) * 2;
                    var imgy = (int)Math.Floor(rect.Y / 2.0) * 2;
                    var w = (int)Math.Ceiling(rect.W / 2.0) * 2;
                    var h = (int)Math.Ceiling(rect.H / 2.0) * 2;

                    LogoFile.ScanLogo(ctx, request.SrcPath, sid, workfile, tmppath, null,
                        imgx, imgy, w, h, threshold, searchFrames,
                        (progress, nread, total, ngather) => true);

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

                failureLatch.TryRemove(sid, out _);
                _ = server.NotifyMessage(
                    $"[AutoLogoPending] 成功: QID={request.QueueItemId}, SID={sid}, rect=({rectX},{rectY},{rectW},{rectH}), search={searchFrames}",
                    false);
            }
            catch (Exception ex)
            {
                failureLatch[sid] = ex.Message;
                _ = server.NotifyError(
                    $"[AutoLogoPending] 失敗: QID={request.QueueItemId}, SID={sid}, reason={ex.Message}",
                    true);
            }
            finally
            {
                runningServices.TryRemove(sid, out _);
            }
        }

        private bool IsEnabled()
        {
            return server.AppData_?.setting?.AutoLogoPendingEnabled ?? false;
        }
        private class AutoRequest
        {
            public int QueueItemId { get; set; }
            public int ServiceId { get; set; }
            public string SrcPath { get; set; } = "";
        }
    }
}
