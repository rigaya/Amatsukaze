using System;
using System.Collections.Concurrent;
using System.IO;
using System.Linq;
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
        public bool Completed { get; set; }
        public string Error { get; set; }
        public string LogoFilePath { get; set; }
    }

    public class LogoAnalyzeService
    {
        private readonly EncodeServer server;
        private readonly ConcurrentDictionary<string, LogoAnalyzeJob> jobs = new ConcurrentDictionary<string, LogoAnalyzeJob>();

        public LogoAnalyzeService(EncodeServer server)
        {
            this.server = server;
        }

        public LogoAnalyzeStatus Start(LogoAnalyzeRequest request)
        {
            var job = new LogoAnalyzeJob()
            {
                Id = Guid.NewGuid().ToString("N")
            };
            jobs[job.Id] = job;

            Task.Run(() => RunJob(job, request));

            return ToStatus(job);
        }

        public LogoAnalyzeStatus GetStatus(string id)
        {
            if (jobs.TryGetValue(id, out var job))
            {
                return ToStatus(job);
            }
            return null;
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

        private void RunJob(LogoAnalyzeJob job, LogoAnalyzeRequest request)
        {
            try
            {
                if (string.IsNullOrEmpty(request.FilePath) || File.Exists(request.FilePath) == false)
                {
                    job.Error = "入力ファイルが見つかりません";
                    job.Completed = true;
                    return;
                }

                var baseWork = !string.IsNullOrEmpty(request.WorkPath)
                    ? request.WorkPath
                    : server.AppData_?.setting?.WorkPath;
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
                    LogoFile.ScanLogo(ctx, request.FilePath, request.ServiceId, workfile, tmppath,
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
                        if (info.ReadFile(request.FilePath))
                        {
                            using (var logo = new LogoFile(ctx, tmppath))
                            {
                                if (info.HasServiceInfo)
                                {
                                    var serviceId = logo.ServiceId;
                                    var service = info.GetServiceList().FirstOrDefault(s => s.ServiceId == serviceId);
                                    var date = info.GetTime().ToString("yyyy-MM-dd");
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
            return new LogoAnalyzeStatus()
            {
                JobId = job.Id,
                Completed = job.Completed,
                Error = job.Error,
                Progress = job.Progress,
                NumRead = job.NumRead,
                NumTotal = job.NumTotal,
                NumValid = job.NumValid,
                LogoFileName = string.IsNullOrEmpty(job.LogoFilePath) ? null : Path.GetFileName(job.LogoFilePath),
                ImageUrl = job.Completed && !string.IsNullOrEmpty(job.LogoFilePath) ? $"/api/logo/analyze/{job.Id}/image" : null
            };
        }
    }
}
