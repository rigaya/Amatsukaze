using System;
using System.Collections.Concurrent;
using System.IO;
using System.Linq;
using System.Globalization;
using System.Threading.Tasks;
using Amatsukaze.Lib;

namespace Amatsukaze.Server.Rest
{
    internal class LogoAnalyzeJob
    {
        public string Id { get; set; }
        public float Progress { get; set; }
        public int NumRead { get; set; }
        public int NumTotal { get; set; }
        public int NumValid { get; set; }
        public int MaxFrames { get; set; }
        public bool Completed { get; set; }
        public string Error { get; set; }
        public string LogoFilePath { get; set; }
    }

    internal class LogoAutoDetectJob
    {
        public string Id { get; set; }
        public float Progress { get; set; }
        public int Stage { get; set; }
        public float StageProgress { get; set; }
        public int NumRead { get; set; }
        public int NumTotal { get; set; }
        public bool Completed { get; set; }
        public string Error { get; set; }
        public LogoRect DetectedRect { get; set; }
        public string ScoreImagePath { get; set; }
        public string BinaryImagePath { get; set; }
        public string CclImagePath { get; set; }
        public string CountImagePath { get; set; }
        public string AImagePath { get; set; }
        public string BImagePath { get; set; }
        public string AlphaImagePath { get; set; }
        public string LogoYImagePath { get; set; }
        public string ConsistencyImagePath { get; set; }
        public string FgVarImagePath { get; set; }
        public string BgVarImagePath { get; set; }
        public string AcceptedImagePath { get; set; }
        public bool DetailedDebug { get; set; }
    }

    public class LogoAnalyzeService
    {
        private readonly EncodeServer server;
        private readonly RestStateStore state;
        private readonly ConcurrentDictionary<string, LogoAnalyzeJob> jobs = new ConcurrentDictionary<string, LogoAnalyzeJob>();
        private readonly ConcurrentDictionary<string, LogoAutoDetectJob> autoJobs = new ConcurrentDictionary<string, LogoAutoDetectJob>();

        public LogoAnalyzeService(EncodeServer server, RestStateStore state)
        {
            this.server = server;
            this.state = state;
        }

        public bool TryStart(LogoAnalyzeStartRequest request, out LogoAnalyzeStatus status, out string error)
        {
            status = null;
            error = null;

            if (request == null || request.QueueItemId <= 0)
            {
                error = "QueueItemIdが不正です";
                return false;
            }
            if (!state.TryGetQueueItem(request.QueueItemId, out var item) || string.IsNullOrEmpty(item.SrcPath))
            {
                error = "キューの入力ファイルが見つかりません";
                return false;
            }
            if (!ServerSupport.TryResolveInputFilePath(item.SrcPath, out var filePath))
            {
                error = "入力ファイルが存在しません";
                return false;
            }
            if (item.ServiceId <= 0)
            {
                error = "サービスIDが取得できません";
                return false;
            }

            var job = new LogoAnalyzeJob()
            {
                Id = Guid.NewGuid().ToString("N"),
                MaxFrames = request.MaxFrames
            };
            jobs[job.Id] = job;

            Task.Run(() => RunJob(job, request, filePath, item.ServiceId));

            status = ToStatus(job);
            return true;
        }

        public LogoAnalyzeStatus GetStatus(string id)
        {
            if (jobs.TryGetValue(id, out var job))
            {
                return ToStatus(job);
            }
            return null;
        }

        public bool TryStartAutoDetect(LogoAutoDetectStartRequest request, out LogoAutoDetectStatus status, out string error)
        {
            status = null;
            error = null;
            if (request == null || request.QueueItemId <= 0)
            {
                error = "QueueItemIdが不正です";
                return false;
            }
            if (!state.TryGetQueueItem(request.QueueItemId, out var item) || string.IsNullOrEmpty(item.SrcPath))
            {
                error = "キューの入力ファイルが見つかりません";
                return false;
            }
            if (!File.Exists(item.SrcPath))
            {
                error = "入力ファイルが存在しません";
                return false;
            }
            if (item.ServiceId <= 0)
            {
                error = "サービスIDが取得できません";
                return false;
            }
            if (request.DivX <= 0 || request.DivY <= 0)
            {
                error = "div_x/div_yは1以上で指定してください";
                return false;
            }
            if (request.SearchFrames < 100)
            {
                error = "search_frameは100以上で指定してください";
                return false;
            }
            if (request.BlockSize < 4)
            {
                error = "block_sizeは4以上で指定してください";
                return false;
            }
            if (request.Threshold < 1)
            {
                error = "thresholdは1以上で指定してください";
                return false;
            }
            if (request.ThreadN < 1)
            {
                error = "thread_nは1以上で指定してください";
                return false;
            }

            var job = new LogoAutoDetectJob()
            {
                Id = Guid.NewGuid().ToString("N"),
                Stage = 1,
                StageProgress = 0,
                Progress = 0,
                DetailedDebug = request.DetailedDebug
            };
            autoJobs[job.Id] = job;
            Task.Run(() => RunAutoDetectJob(job, request, item.SrcPath, item.ServiceId));

            status = ToAutoStatus(job);
            return true;
        }

        public LogoAutoDetectStatus GetAutoDetectStatus(string id)
        {
            if (autoJobs.TryGetValue(id, out var job))
            {
                return ToAutoStatus(job);
            }
            return null;
        }

        public byte[] GetAutoDetectDebugImagePng(string id, string kind)
        {
            if (!autoJobs.TryGetValue(id, out var job))
            {
                return null;
            }

            string path = null;
            if (string.Equals(kind, "score", StringComparison.OrdinalIgnoreCase))
            {
                path = job.ScoreImagePath;
            }
            else if (string.Equals(kind, "binary", StringComparison.OrdinalIgnoreCase))
            {
                path = job.BinaryImagePath;
            }
            else if (string.Equals(kind, "ccl", StringComparison.OrdinalIgnoreCase))
            {
                path = job.CclImagePath;
            }
            else if (string.Equals(kind, "count", StringComparison.OrdinalIgnoreCase))
            {
                path = job.CountImagePath;
            }
            else if (string.Equals(kind, "a", StringComparison.OrdinalIgnoreCase))
            {
                path = job.AImagePath;
            }
            else if (string.Equals(kind, "b", StringComparison.OrdinalIgnoreCase))
            {
                path = job.BImagePath;
            }
            else if (string.Equals(kind, "alpha", StringComparison.OrdinalIgnoreCase))
            {
                path = job.AlphaImagePath;
            }
            else if (string.Equals(kind, "logoy", StringComparison.OrdinalIgnoreCase))
            {
                path = job.LogoYImagePath;
            }
            else if (string.Equals(kind, "consistency", StringComparison.OrdinalIgnoreCase))
            {
                path = job.ConsistencyImagePath;
            }
            else if (string.Equals(kind, "fgvar", StringComparison.OrdinalIgnoreCase))
            {
                path = job.FgVarImagePath;
            }
            else if (string.Equals(kind, "bgvar", StringComparison.OrdinalIgnoreCase))
            {
                path = job.BgVarImagePath;
            }
            else if (string.Equals(kind, "accepted", StringComparison.OrdinalIgnoreCase))
            {
                path = job.AcceptedImagePath;
            }

            if (string.IsNullOrEmpty(path) || !File.Exists(path))
            {
                return null;
            }
            try
            {
                var bitmap = BitmapManager.CreateBitmapFromFile(path);
                using var ms = new MemoryStream();
                BitmapManager.SaveBitmapAsPng(bitmap, ms);
                return ms.ToArray();
            }
            catch
            {
                return File.ReadAllBytes(path);
            }
        }

        public bool TryApply(string id, out string error)
        {
            error = null;
            if (!jobs.TryGetValue(id, out var job))
            {
                error = "ジョブが見つかりません";
                return false;
            }
            if (string.IsNullOrEmpty(job.LogoFilePath) || !File.Exists(job.LogoFilePath))
            {
                error = "ロゴファイルが見つかりません";
                return false;
            }
            try
            {
                int serviceId;
                using (var ctx = new AMTContext())
                using (var logo = new LogoFile(ctx, job.LogoFilePath))
                {
                    serviceId = logo.ServiceId;
                }
                var data = File.ReadAllBytes(job.LogoFilePath);
                server.SendLogoFile(new LogoFileData()
                {
                    ServiceId = serviceId,
                    LogoIdx = 1,
                    Data = data
                }).GetAwaiter().GetResult();
                server.RequestLogoRescan();
                return true;
            }
            catch (Exception ex)
            {
                error = ex.Message;
                return false;
            }
        }

        public bool TryDiscard(string id, out string error)
        {
            error = null;
            if (!jobs.TryRemove(id, out var job))
            {
                error = "ジョブが見つかりません";
                return false;
            }
            try
            {
                if (!string.IsNullOrEmpty(job.LogoFilePath) && File.Exists(job.LogoFilePath))
                {
                    File.Delete(job.LogoFilePath);
                }
            }
            catch (Exception ex)
            {
                error = ex.Message;
                return false;
            }
            return true;
        }

        public byte[] GetLogoFile(string id)
        {
            if (jobs.TryGetValue(id, out var job))
            {
                if (!string.IsNullOrEmpty(job.LogoFilePath) && File.Exists(job.LogoFilePath))
                {
                    return File.ReadAllBytes(job.LogoFilePath);
                }
            }
            return null;
        }

        public byte[] GetLogoImagePng(string id)
        {
            if (jobs.TryGetValue(id, out var job))
            {
                if (!string.IsNullOrEmpty(job.LogoFilePath) && File.Exists(job.LogoFilePath))
                {
                    using (var ctx = new AMTContext())
                    using (var logo = new LogoFile(ctx, job.LogoFilePath))
                    using (var ms = new MemoryStream())
                    {
                        var image = logo.GetImage(0);
                        BitmapManager.SaveBitmapAsPng(image, ms);
                        return ms.ToArray();
                    }
                }
            }
            return null;
        }

        private static void SetAutoProgress(LogoAutoDetectJob job, int stage, float stageProgress, float progress)
        {
            job.Stage = stage;
            job.StageProgress = stageProgress;
            job.Progress = progress;
        }

        private void RunAutoDetectJob(LogoAutoDetectJob job, LogoAutoDetectStartRequest request, string filePath, int serviceId)
        {
            try
            {
                var baseWork = server.AppData_?.setting?.WorkPath;
                if (string.IsNullOrEmpty(baseWork))
                {
                    baseWork = Directory.GetCurrentDirectory();
                }
                Directory.CreateDirectory(baseWork);

                using (var ctx = new AMTContext())
                {
                    var scorePath = Path.Combine(baseWork, $"logo-auto-score-{job.Id}.bmp");
                    var binaryPath = Path.Combine(baseWork, $"logo-auto-binary-{job.Id}.bmp");
                    var cclPath = Path.Combine(baseWork, $"logo-auto-ccl-{job.Id}.bmp");
                    var countPath = request.DetailedDebug ? Path.Combine(baseWork, $"logo-auto-count-{job.Id}.bmp") : null;
                    var aPath = request.DetailedDebug ? Path.Combine(baseWork, $"logo-auto-a-{job.Id}.bmp") : null;
                    var bPath = request.DetailedDebug ? Path.Combine(baseWork, $"logo-auto-b-{job.Id}.bmp") : null;
                    var alphaPath = request.DetailedDebug ? Path.Combine(baseWork, $"logo-auto-alpha-{job.Id}.bmp") : null;
                    var logoYPath = request.DetailedDebug ? Path.Combine(baseWork, $"logo-auto-logoy-{job.Id}.bmp") : null;
                    var consistencyPath = request.DetailedDebug ? Path.Combine(baseWork, $"logo-auto-consistency-{job.Id}.bmp") : null;
                    var fgVarPath = request.DetailedDebug ? Path.Combine(baseWork, $"logo-auto-fgvar-{job.Id}.bmp") : null;
                    var bgVarPath = request.DetailedDebug ? Path.Combine(baseWork, $"logo-auto-bgvar-{job.Id}.bmp") : null;
                    var acceptedPath = request.DetailedDebug ? Path.Combine(baseWork, $"logo-auto-accepted-{job.Id}.bmp") : null;
                    job.ScoreImagePath = scorePath;
                    job.BinaryImagePath = binaryPath;
                    job.CclImagePath = cclPath;
                    job.CountImagePath = countPath;
                    job.AImagePath = aPath;
                    job.BImagePath = bPath;
                    job.AlphaImagePath = alphaPath;
                    job.LogoYImagePath = logoYPath;
                    job.ConsistencyImagePath = consistencyPath;
                    job.FgVarImagePath = fgVarPath;
                    job.BgVarImagePath = bgVarPath;
                    job.AcceptedImagePath = acceptedPath;

                    int x = 0;
                    int y = 0;
                    int w = 0;
                    int h = 0;
                    var result = LogoFile.AutoDetectLogoRect(
                        ctx, filePath, serviceId,
                        request.DivX, request.DivY, request.SearchFrames, request.BlockSize, request.Threshold,
                        request.MarginX, request.MarginY, request.ThreadN,
                        scorePath, binaryPath, cclPath, countPath, aPath, bPath, alphaPath, logoYPath, consistencyPath, fgVarPath, bgVarPath, acceptedPath,
                        request.DetailedDebug,
                        (stage, stageProgress, progress, nread, total) =>
                        {
                            job.Stage = stage;
                            job.StageProgress = stageProgress;
                            job.Progress = progress;
                            job.NumRead = nread;
                            job.NumTotal = total;
                            return true;
                        });
                    x = result.X;
                    y = result.Y;
                    w = result.W;
                    h = result.H;

                    job.DetectedRect = new LogoRect
                    {
                        X = x,
                        Y = y,
                        Width = w,
                        Height = h
                    };
                    SetAutoProgress(job, 4, 1.0f, 1.0f);
                }

                job.Completed = true;
            }
            catch (Exception ex)
            {
                job.Error = ex.Message;
                job.Completed = true;
            }
        }

        private void RunJob(LogoAnalyzeJob job, LogoAnalyzeStartRequest request, string filePath, int serviceId)
        {
            try
            {
                var baseWork = server.AppData_?.setting?.WorkPath;
                if (string.IsNullOrEmpty(baseWork))
                {
                    baseWork = Directory.GetCurrentDirectory();
                }
                Directory.CreateDirectory(baseWork);

                var workfile = Path.Combine(baseWork, "logotmp-" + job.Id + ".dat");
                var tmppath = Path.Combine(baseWork, "logotmp-" + job.Id + ".lgd");
                var outpath = Path.Combine(baseWork, "logo-" + job.Id + ".lgd");

                int imgx = (int)Math.Floor(request.X / 2.0) * 2;
                int imgy = (int)Math.Floor(request.Y / 2.0) * 2;
                int w = (int)Math.Ceiling(request.Width / 2.0) * 2;
                int h = (int)Math.Ceiling(request.Height / 2.0) * 2;

                using (var ctx = new AMTContext())
                {
                    LogoFile.ScanLogo(ctx, filePath, serviceId, workfile, tmppath,
                        imgx, imgy, w, h, request.Threshold, request.MaxFrames, (progress, nread, total, ngather) =>
                        {
                            job.Progress = progress;
                            job.NumRead = nread;
                            job.NumTotal = total;
                            job.NumValid = ngather;
                            return true;
                        });

                    using (var info = new TsInfo(ctx))
                    {
                        if (info.ReadFile(filePath))
                        {
                            using (var logo = new LogoFile(ctx, tmppath))
                            {
                                if (info.HasServiceInfo)
                                {
                                    var logoServiceId = logo.ServiceId;
                                    var service = info.GetServiceList().FirstOrDefault(s => s.ServiceId == logoServiceId);
                                    var date = info.GetTime().ToString("yyyy-MM-dd", CultureInfo.InvariantCulture);
                                    if (service != null)
                                    {
                                        logo.Name = service.ServiceName + "(" + date + ")";
                                    }
                                    else
                                    {
                                        logo.Name = "情報なし";
                                    }
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

                job.LogoFilePath = outpath;
                job.Completed = true;
            }
            catch (Exception ex)
            {
                job.Error = ex.Message;
                job.Completed = true;
            }
        }

        private static LogoAnalyzeStatus ToStatus(LogoAnalyzeJob job)
        {
            var pass = 1;
            if (job.Completed)
            {
                pass = 3;
            }
            else if (job.Progress >= 75.0f)
            {
                pass = 3;
            }
            else if (job.Progress >= 50.0f)
            {
                pass = 2;
            }
            return new LogoAnalyzeStatus()
            {
                JobId = job.Id,
                Completed = job.Completed,
                Error = job.Error,
                Progress = job.Progress,
                NumRead = job.NumRead,
                NumTotal = job.NumTotal,
                NumValid = job.NumValid,
                Pass = pass,
                LogoFileName = string.IsNullOrEmpty(job.LogoFilePath) ? null : Path.GetFileName(job.LogoFilePath),
                ImageUrl = job.Completed && !string.IsNullOrEmpty(job.LogoFilePath) ? $"/api/logo/analyze/{job.Id}/image" : null
            };
        }

        private static LogoAutoDetectStatus ToAutoStatus(LogoAutoDetectJob job)
        {
            var stageName = job.Stage switch
            {
                1 => "フレーム収集",
                2 => "ロゴ輝度/α推定",
                3 => "2値化・投影・CCL",
                4 => "後処理・確定",
                _ => "待機中"
            };
            var debug = new LogoAutoDetectDebugImages();
            if (!string.IsNullOrEmpty(job.ScoreImagePath))
            {
                debug.ScoreUrl = $"/api/logo/analyze/auto/{job.Id}/debug/score";
            }
            if (!string.IsNullOrEmpty(job.BinaryImagePath))
            {
                debug.BinaryUrl = $"/api/logo/analyze/auto/{job.Id}/debug/binary";
            }
            if (!string.IsNullOrEmpty(job.CclImagePath))
            {
                debug.CclUrl = $"/api/logo/analyze/auto/{job.Id}/debug/ccl";
            }
            if (!string.IsNullOrEmpty(job.CountImagePath))
            {
                debug.CountUrl = $"/api/logo/analyze/auto/{job.Id}/debug/count";
            }
            if (!string.IsNullOrEmpty(job.AImagePath))
            {
                debug.AUrl = $"/api/logo/analyze/auto/{job.Id}/debug/a";
            }
            if (!string.IsNullOrEmpty(job.BImagePath))
            {
                debug.BUrl = $"/api/logo/analyze/auto/{job.Id}/debug/b";
            }
            if (!string.IsNullOrEmpty(job.AlphaImagePath))
            {
                debug.AlphaUrl = $"/api/logo/analyze/auto/{job.Id}/debug/alpha";
            }
            if (!string.IsNullOrEmpty(job.LogoYImagePath))
            {
                debug.LogoYUrl = $"/api/logo/analyze/auto/{job.Id}/debug/logoy";
            }
            if (!string.IsNullOrEmpty(job.ConsistencyImagePath))
            {
                debug.ConsistencyUrl = $"/api/logo/analyze/auto/{job.Id}/debug/consistency";
            }
            if (!string.IsNullOrEmpty(job.FgVarImagePath))
            {
                debug.FgVarUrl = $"/api/logo/analyze/auto/{job.Id}/debug/fgvar";
            }
            if (!string.IsNullOrEmpty(job.BgVarImagePath))
            {
                debug.BgVarUrl = $"/api/logo/analyze/auto/{job.Id}/debug/bgvar";
            }
            if (!string.IsNullOrEmpty(job.AcceptedImagePath))
            {
                debug.AcceptedUrl = $"/api/logo/analyze/auto/{job.Id}/debug/accepted";
            }

            return new LogoAutoDetectStatus()
            {
                JobId = job.Id,
                Completed = job.Completed,
                Error = job.Error,
                Progress = job.Progress,
                Stage = job.Stage,
                StageName = stageName,
                StageProgress = job.StageProgress,
                NumRead = job.NumRead,
                NumTotal = job.NumTotal,
                DetectedRect = job.DetectedRect,
                DebugImages = debug
            };
        }
    }
}
