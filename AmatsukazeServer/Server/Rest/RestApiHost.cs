using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;
using Amatsukaze.Lib;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Http.Json;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.DependencyInjection;
using SixLabors.ImageSharp;
using SixLabors.ImageSharp.Formats.Png;
using SixLabors.ImageSharp.PixelFormats;

namespace Amatsukaze.Server.Rest
{
    public class RestApiHost : IDisposable
    {
        private readonly EncodeServer server;
        private readonly RestStateStore state;
        private readonly int port;
        private readonly LogoAnalyzeService logoAnalyze;
        private IHost host;

        public static int GetEnabledPort(int serverPort)
        {
            var disabled = Environment.GetEnvironmentVariable("AMT_REST_DISABLED");
            if (!string.IsNullOrEmpty(disabled) && (disabled == "1" || disabled.Equals("true", StringComparison.OrdinalIgnoreCase)))
            {
                return 0;
            }
            var envPort = Environment.GetEnvironmentVariable("AMT_REST_PORT");
            if (!string.IsNullOrEmpty(envPort) && int.TryParse(envPort, out var parsed) && parsed > 0)
            {
                return parsed;
            }
            if (serverPort > 0)
            {
                return serverPort + 1;
            }
            return 0;
        }

        public RestApiHost(EncodeServer server, RestStateStore state, int port)
        {
            this.server = server;
            this.state = state;
            this.port = port;
            logoAnalyze = new LogoAnalyzeService(server);
        }

        public async Task StartAsync(CancellationToken cancellationToken = default)
        {
            if (port <= 0)
            {
                return;
            }

            var builder = WebApplication.CreateBuilder();
            builder.Services.Configure<JsonOptions>(options =>
            {
                options.SerializerOptions.PropertyNamingPolicy = JsonNamingPolicy.CamelCase;
                options.SerializerOptions.Converters.Add(new JsonStringEnumConverter());
            });
            builder.Services.AddCors(options =>
            {
                options.AddPolicy("LocalCors", policy =>
                {
                    policy
                        .SetIsOriginAllowed(origin =>
                        {
                            if (string.IsNullOrEmpty(origin))
                                return false;
                            return origin.StartsWith("http://127.0.0.1", StringComparison.OrdinalIgnoreCase) ||
                                   origin.StartsWith("https://127.0.0.1", StringComparison.OrdinalIgnoreCase) ||
                                   origin.StartsWith("http://localhost", StringComparison.OrdinalIgnoreCase) ||
                                   origin.StartsWith("https://localhost", StringComparison.OrdinalIgnoreCase) ||
                                   origin.StartsWith("http://[::1]", StringComparison.OrdinalIgnoreCase) ||
                                   origin.StartsWith("https://[::1]", StringComparison.OrdinalIgnoreCase);
                        })
                        .AllowAnyHeader()
                        .AllowAnyMethod();
                });
            });

            var app = builder.Build();
            app.Urls.Add($"http://127.0.0.1:{port}");

            app.Use(async (context, next) =>
            {
                var remote = context.Connection.RemoteIpAddress;
                if (remote != null && !IPAddress.IsLoopback(remote))
                {
                    context.Response.StatusCode = StatusCodes.Status403Forbidden;
                    return;
                }
                await next();
            });
            app.UseCors("LocalCors");

            MapEndpoints(app);

            host = app;
            await host.StartAsync(cancellationToken);
            await state.RequestInitialSync(server);
        }

        public async Task StopAsync(CancellationToken cancellationToken = default)
        {
            if (host != null)
            {
                await host.StopAsync(cancellationToken);
                host.Dispose();
                host = null;
            }
        }

        public void Dispose()
        {
            if (host != null)
            {
                host.Dispose();
                host = null;
            }
        }

        private void MapEndpoints(WebApplication app)
        {
            app.MapGet("/api/health", () => Results.Json(new { ok = true }));

            app.MapGet("/api/snapshot", () => Results.Json(state.GetSnapshot()));
            app.MapGet("/api/system", () => Results.Json(state.GetSystemSnapshot()));
            app.MapPost("/api/system/end", async () =>
            {
                await server.EndServer();
                return Results.Json(new { ok = true });
            });
            app.MapPost("/api/system/cancel-sleep", async () =>
            {
                await server.CancelSleep();
                return Results.Json(new { ok = true });
            });

            app.MapGet("/api/queue", (HttpRequest request) =>
            {
                var filter = new QueueFilter();
                if (request.Query.TryGetValue("state", out var states))
                {
                    filter.States.AddRange(states);
                }
                if (request.Query.TryGetValue("search", out var search))
                {
                    filter.Search = search.ToString();
                }
                if (request.Query.TryGetValue("searchTargets", out var targets))
                {
                    filter.SearchTargets.AddRange(targets.SelectMany(t => t.Split(',')));
                }
                if (request.Query.TryGetValue("dateFrom", out var dateFrom) &&
                    DateTime.TryParse(dateFrom.ToString(), CultureInfo.InvariantCulture, DateTimeStyles.AssumeLocal, out var from))
                {
                    filter.DateFrom = from;
                }
                if (request.Query.TryGetValue("dateTo", out var dateTo) &&
                    DateTime.TryParse(dateTo.ToString(), CultureInfo.InvariantCulture, DateTimeStyles.AssumeLocal, out var to))
                {
                    filter.DateTo = to;
                }
                if (request.Query.TryGetValue("hideOneSeg", out var hide) &&
                    bool.TryParse(hide.ToString(), out var hideOneSeg))
                {
                    filter.HideOneSeg = hideOneSeg;
                }
                return Results.Json(state.GetQueueView(filter));
            });

            app.MapPost("/api/queue/add", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<AddQueueRequest>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                if (string.IsNullOrEmpty(data.RequestId))
                {
                    data.RequestId = Guid.NewGuid().ToString("N");
                }
                await server.AddQueue(data);
                return Results.Json(new { requestId = data.RequestId });
            });

            app.MapPost("/api/queue/change", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<ChangeItemData>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                await server.ChangeItem(data);
                return Results.Json(new { ok = true });
            });

            app.MapPost("/api/queue/pause", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<PauseRequest>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                await server.PauseEncode(data);
                return Results.Json(new { ok = true });
            });

            app.MapPost("/api/queue/cancel-add", async () =>
            {
                await server.CancelAddQueue();
                return Results.Json(new { ok = true });
            });

            app.MapGet("/api/logs/encode", () => Results.Json(state.GetEncodeLogs()));
            app.MapGet("/api/logs/check", () => Results.Json(state.GetCheckLogs()));

            app.MapGet("/api/logs/file", (HttpRequest request) =>
            {
                if (request.Query.TryGetValue("encodeStart", out var encodeStart) &&
                    DateTime.TryParse(encodeStart.ToString(), CultureInfo.InvariantCulture, DateTimeStyles.AssumeLocal, out var encDate))
                {
                    return Results.Json(GetLogFileContent(server.GetLogFileBase(encDate) + ".txt"));
                }
                if (request.Query.TryGetValue("checkStart", out var checkStart) &&
                    DateTime.TryParse(checkStart.ToString(), CultureInfo.InvariantCulture, DateTimeStyles.AssumeLocal, out var chkDate))
                {
                    return Results.Json(GetLogFileContent(server.GetCheckLogFileBase(chkDate) + ".txt"));
                }
                return Results.BadRequest();
            });

            app.MapGet("/api/logs/encode.csv", () => Results.Text(BuildEncodeCsv(state.GetEncodeLogs()), "text/csv", Encoding.UTF8));
            app.MapGet("/api/logs/check.csv", () => Results.Text(BuildCheckCsv(state.GetCheckLogs()), "text/csv", Encoding.UTF8));

            app.MapGet("/api/profiles", () => Results.Json(state.GetProfiles()));
            app.MapPost("/api/profiles", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<ProfileSetting>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                await server.SetProfile(new ProfileUpdate() { Type = UpdateType.Add, Profile = data });
                return Results.Json(new { ok = true });
            });
            app.MapPut("/api/profiles/{name}", async (HttpRequest request, string name) =>
            {
                var data = await request.ReadFromJsonAsync<ProfileSetting>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                await server.SetProfile(new ProfileUpdate() { Type = UpdateType.Update, Profile = data, NewName = name != data.Name ? name : null });
                return Results.Json(new { ok = true });
            });
            app.MapDelete("/api/profiles/{name}", async (string name) =>
            {
                await server.SetProfile(new ProfileUpdate() { Type = UpdateType.Remove, Profile = new ProfileSetting() { Name = name } });
                return Results.Json(new { ok = true });
            });

            app.MapGet("/api/autoselect", () => Results.Json(state.GetAutoSelects()));
            app.MapPost("/api/autoselect", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<AutoSelectProfile>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                await server.SetAutoSelect(new AutoSelectUpdate() { Type = UpdateType.Add, Profile = data });
                return Results.Json(new { ok = true });
            });
            app.MapPut("/api/autoselect/{name}", async (HttpRequest request, string name) =>
            {
                var data = await request.ReadFromJsonAsync<AutoSelectProfile>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                await server.SetAutoSelect(new AutoSelectUpdate() { Type = UpdateType.Update, Profile = data, NewName = name != data.Name ? name : null });
                return Results.Json(new { ok = true });
            });
            app.MapDelete("/api/autoselect/{name}", async (string name) =>
            {
                await server.SetAutoSelect(new AutoSelectUpdate() { Type = UpdateType.Remove, Profile = new AutoSelectProfile() { Name = name } });
                return Results.Json(new { ok = true });
            });

            app.MapGet("/api/services", () => Results.Json(state.GetServiceViews()));
            app.MapPost("/api/services/update", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<ServiceSettingUpdate>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                await server.SetServiceSetting(data);
                return Results.Json(new { ok = true });
            });

            app.MapPost("/api/services/logo", async (HttpRequest request) =>
            {
                if (!request.HasFormContentType)
                {
                    return Results.BadRequest();
                }
                var form = await request.ReadFormAsync();
                if (!int.TryParse(form["serviceId"], out var serviceId))
                {
                    return Results.BadRequest();
                }
                if (!int.TryParse(form["logoIdx"], out var logoIdx))
                {
                    return Results.BadRequest();
                }
                var file = form.Files.GetFile("image");
                if (file == null)
                {
                    return Results.BadRequest();
                }
                using (var ms = new MemoryStream())
                {
                    await file.CopyToAsync(ms);
                    await server.SendLogoFile(new LogoFileData()
                    {
                        Data = ms.ToArray(),
                        ServiceId = serviceId,
                        LogoIdx = logoIdx
                    });
                }
                return Results.Json(new { ok = true });
            });

            app.MapGet("/api/drcs", () => Results.Json(state.GetDrcsViews()));
            app.MapPost("/api/drcs", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<DrcsImage>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                await server.AddDrcsMap(data);
                return Results.Json(new { ok = true });
            });

            app.MapGet("/api/console", () => Results.Json(state.GetConsoleView()));

            app.MapGet("/api/settings", () => Results.Json(state.GetSetting()));
            app.MapPut("/api/settings", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<Setting>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                await server.SetCommonData(new CommonData() { Setting = data });
                return Results.Json(new { ok = true });
            });

            app.MapGet("/api/makescript", () => Results.Json(state.GetMakeScriptData()));
            app.MapPut("/api/makescript", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<MakeScriptData>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                await server.SetCommonData(new CommonData() { MakeScriptData = data });
                return Results.Json(new { ok = true });
            });

            app.MapPut("/api/finish-setting", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<FinishSetting>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                await server.SetCommonData(new CommonData() { FinishSetting = data });
                return Results.Json(new { ok = true });
            });

            app.MapGet("/api/makescript/preview", () =>
            {
                return Results.Json(new MakeScriptPreview()
                {
                    CommandLine = BuildMakeScriptPreview(state.GetMakeScriptData(), server.ServerPortForRest)
                });
            });

            app.MapGet("/api/assets/logo/{serviceId:int}/{logoIdx:int}", (int serviceId, int logoIdx) =>
            {
                var service = state.GetServiceViews().FirstOrDefault(s => s.ServiceId == serviceId);
                if (service == null || logoIdx < 0 || logoIdx >= service.LogoList.Count)
                {
                    return Results.NotFound();
                }
                var fileName = service.LogoList[logoIdx].FileName;
                if (string.IsNullOrEmpty(fileName) || fileName == LogoSetting.NO_LOGO)
                {
                    return Results.NotFound();
                }
                try
                {
                    var bytes = server.ReadLogoImagePng(fileName, 0);
                    if (bytes == null || bytes.Length == 0)
                    {
                        return Results.NotFound();
                    }
                    return Results.File(bytes, "image/png");
                }
                catch (Exception)
                {
                    return Results.NotFound();
                }
            });

            app.MapGet("/api/assets/drcs/{md5}", (string md5) =>
            {
                var path = server.GetDRCSImagePath(md5);
                if (string.IsNullOrEmpty(path) || File.Exists(path) == false)
                {
                    return Results.NotFound();
                }
                try
                {
                    var bitmap = BitmapManager.CreateBitmapFromFile(path);
                    using var ms = new MemoryStream();
                    BitmapManager.SaveBitmapAsPng(bitmap, ms);
                    return Results.File(ms.ToArray(), "image/png");
                }
                catch (Exception)
                {
                    try
                    {
                        var bytes = File.ReadAllBytes(path);
                        return Results.File(bytes, "image/bmp");
                    }
                    catch (Exception)
                    {
                        return Results.NotFound();
                    }
                }
            });

            app.MapPost("/api/logo/analyze", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<LogoAnalyzeRequest>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                var status = logoAnalyze.Start(data);
                return Results.Json(status);
            });

            app.MapGet("/api/logo/analyze/{jobId}", (string jobId) =>
            {
                var status = logoAnalyze.GetStatus(jobId);
                if (status == null)
                {
                    return Results.NotFound();
                }
                return Results.Json(status);
            });

            app.MapGet("/api/logo/analyze/{jobId}/file", (string jobId) =>
            {
                var bytes = logoAnalyze.GetLogoFile(jobId);
                if (bytes == null)
                {
                    return Results.NotFound();
                }
                return Results.File(bytes, "application/octet-stream", "logo.lgd");
            });

            app.MapGet("/api/logo/analyze/{jobId}/image", (string jobId) =>
            {
                var bytes = logoAnalyze.GetLogoImagePng(jobId);
                if (bytes == null)
                {
                    return Results.NotFound();
                }
                return Results.File(bytes, "image/png");
            });
        }

        private static LogFileContent GetLogFileContent(string path)
        {
            if (!File.Exists(path))
            {
                return new LogFileContent()
                {
                    Content = "ログファイルが見つかりません。パス: " + path,
                    Meta = new LogFileMeta() { Size = 0, TooLarge = false, Encoding = "cp932" }
                };
            }
            var bytes = File.ReadAllBytes(path);
            var content = Util.AmatsukazeDefaultEncoding.GetString(bytes);
            return new LogFileContent()
            {
                Content = content,
                Meta = new LogFileMeta()
                {
                    Size = bytes.LongLength,
                    TooLarge = bytes.LongLength > 100 * 1000,
                    Encoding = "cp932"
                }
            };
        }

        private static string BuildEncodeCsv(List<LogItem> logs)
        {
            var sb = new StringBuilder();
            var header = new string[] {
                "結果","メッセージ","入力ファイル","出力ファイル","出力ファイル数",
                "エンコード開始","エンコード終了","エンコード時間（秒）",
                "入力ファイル時間（秒）","出力ファイル時間（秒）",
                "インシデント数","入力ファイルサイズ","中間ファイルサイズ","出力ファイルサイズ",
                "圧縮率（％）","入力音声フレーム","出力音声フレーム","ユニーク出力音声フレーム",
                "未出力音声割合(%)","平均音ズレ(ms)","最大音ズレ(ms)","最大音ズレ位置(ms)"
            };
            sb.AppendLine(string.Join(",", header));
            foreach (var item in logs.AsEnumerable().Reverse())
            {
                var row = new string[] {
                    item.Success ? ((item.Incident > 0) ? "△" : "〇") : "×",
                    item.Reason,
                    item.SrcPath,
                    (item.OutPath != null) ? string.Join(":", item.OutPath) : "-",
                    (item.OutPath?.Count ?? 0).ToString(),
                    item.DisplayEncodeStart,
                    item.DisplayEncodeFinish,
                    (item.EncodeFinishDate - item.EncodeStartDate).TotalSeconds.ToString(),
                    item.SrcVideoDuration.TotalSeconds.ToString(),
                    item.OutVideoDuration.TotalSeconds.ToString(),
                    item.Incident.ToString(),
                    item.SrcFileSize.ToString(),
                    item.IntVideoFileSize.ToString(),
                    item.OutFileSize.ToString(),
                    item.DisplayCompressionRate,
                    (item.AudioDiff?.TotalSrcFrames ?? 0).ToString(),
                    (item.AudioDiff?.TotalOutFrames ?? 0).ToString(),
                    (item.AudioDiff?.TotalOutUniqueFrames ?? 0).ToString(),
                    (item.AudioDiff?.NotIncludedPer ?? 0).ToString(),
                    (item.AudioDiff?.AvgDiff ?? 0).ToString(),
                    (item.AudioDiff?.MaxDiff ?? 0).ToString(),
                    (item.AudioDiff?.MaxDiffPos ?? 0).ToString()
                };
                sb.AppendLine(string.Join(",", row));
            }
            return sb.ToString();
        }

        private static string BuildCheckCsv(List<CheckLogItem> logs)
        {
            var sb = new StringBuilder();
            var header = new string[] {
                "種別","結果","入力ファイル","開始","終了","理由"
            };
            sb.AppendLine(string.Join(",", header));
            foreach (var item in logs.AsEnumerable().Reverse())
            {
                var row = new string[] {
                    item.DisplayType,
                    item.DisplayResult,
                    item.SrcPath,
                    item.DisplayEncodeStart,
                    item.DisplayEncodeFinish,
                    item.Reason
                };
                sb.AppendLine(string.Join(",", row));
            }
            return sb.ToString();
        }

        private static string BuildMakeScriptPreview(MakeScriptData data, int serverPort)
        {
            if (data == null)
            {
                return "";
            }

            var cur = Directory.GetCurrentDirectory();
            var exe = AppContext.BaseDirectory;
            var dst = data.OutDir?.TrimEnd(Path.DirectorySeparatorChar);
            var prof = data.Profile;
            var bat = data.AddQueueBat;
            var nas = data.IsNasEnabled ? data.NasDir?.TrimEnd(Path.DirectorySeparatorChar) : null;
            var ip = "localhost";
            var port = serverPort > 0 ? serverPort : ServerSupport.DEFAULT_PORT;
            var direct = data.IsDirect;

            var sb = new StringBuilder();
            if (direct)
            {
                sb.Append("rem _EDCBX_DIRECT_\r\n");
            }
            var addTaskPath = Path.Combine(exe, Util.IsServerWindows() ? "AmatsukazeAddTask.exe" : "AmatsukazeAddTask");
            sb.AppendFormat("\"{0}\"", addTaskPath)
                .AppendFormat(" -r \"{0}\"", cur)
                .AppendFormat(" -f \"{0}FilePath{0}\" -ip \"{1}\"", direct ? "%" : "$", ip)
                .AppendFormat(" -p {0}", port)
                .AppendFormat(" -o \"{0}\"", dst ?? "")
                .AppendFormat(" -s \"{0}\"", prof ?? "")
                .AppendFormat(" --priority {0}", data.Priority);
            if (!string.IsNullOrEmpty(nas))
            {
                sb.AppendFormat(" -d \"{0}\"", nas);
            }
            if (!string.IsNullOrEmpty(bat))
            {
                sb.AppendFormat(" -b \"{0}\"", bat);
            }
            if (data.MoveAfter == false)
            {
                sb.Append(" --no-move");
            }
            if (data.ClearEncoded)
            {
                sb.Append(" --clear-succeeded");
            }
            if (data.WithRelated)
            {
                sb.Append(" --with-related");
            }
            return sb.ToString();
        }

        private static byte[] ReadImageAsPng(string path)
        {
            using (var image = Image.Load<Rgba32>(path))
            using (var ms = new MemoryStream())
            {
                image.Save(ms, new PngEncoder());
                return ms.ToArray();
            }
        }
    }
}
