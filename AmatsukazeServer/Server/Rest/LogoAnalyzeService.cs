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

    public class LogoAnalyzeService
    {
        private readonly EncodeServer server;
        private readonly RestStateStore state;
        private readonly ConcurrentDictionary<string, LogoAnalyzeJob> jobs = new ConcurrentDictionary<string, LogoAnalyzeJob>();

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
}
}
