using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;
using System.Runtime.InteropServices;
using Amatsukaze.Shared;
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
        private readonly LogoPreviewService logoPreview;
        private IHost host;

        private class MakeScriptGenerateRequestInternal
        {
            public MakeScriptData MakeScriptData { get; set; }
            public string TargetHost { get; set; }
            public string ScriptType { get; set; }
            public string RemoteHost { get; set; }
            public string Subnet { get; set; }
            public string Mac { get; set; }
        }

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
            logoAnalyze = new LogoAnalyzeService(server, state);
            logoPreview = new LogoPreviewService(state);
        }

        public async Task StartAsync(CancellationToken cancellationToken = default)
        {
            if (port <= 0)
            {
                return;
            }
            var baseDir = AppContext.BaseDirectory;
            var webRoot = Path.Combine(baseDir, "wwwroot");
            var builder = WebApplication.CreateBuilder(new WebApplicationOptions
            {
                ContentRootPath = baseDir,
                WebRootPath = webRoot,
            });
            builder.Services.Configure<JsonOptions>(options =>
            {
                options.SerializerOptions.PropertyNamingPolicy = JsonNamingPolicy.CamelCase;
                options.SerializerOptions.IncludeFields = true;
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
                            if (!Uri.TryCreate(origin, UriKind.Absolute, out var uri))
                                return false;
                            if (!uri.Scheme.Equals("http", StringComparison.OrdinalIgnoreCase) &&
                                !uri.Scheme.Equals("https", StringComparison.OrdinalIgnoreCase))
                                return false;
                            if (uri.Host.Equals("localhost", StringComparison.OrdinalIgnoreCase))
                                return true;
                            if (IPAddress.TryParse(uri.Host, out var originAddress))
                                return IsAllowedRemote(originAddress);
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
            app.Urls.Add($"http://0.0.0.0:{port}");

            app.Use(async (context, next) =>
            {
                var remote = context.Connection.RemoteIpAddress;
                if (remote != null && !IsAllowedRemote(remote))
                {
                    context.Response.StatusCode = StatusCodes.Status403Forbidden;
                    return;
                }
                await next();
            });
            app.UseCors("LocalCors");

            app.UseBlazorFrameworkFiles();
            app.UseDefaultFiles();
            app.UseStaticFiles();

            MapEndpoints(app);
            app.MapFallbackToFile("index.html");

            host = app;
            await host.StartAsync(cancellationToken).ConfigureAwait(false);
            await state.RequestInitialSync(server).ConfigureAwait(false);
        }

        public async Task StopAsync(CancellationToken cancellationToken = default)
        {
            if (host != null)
            {
                var start = DateTime.Now;
                var stopTask = host.StopAsync(cancellationToken);
                var timeoutTask = Task.Delay(TimeSpan.FromSeconds(10), cancellationToken);
                var completed = await Task.WhenAny(stopTask, timeoutTask).ConfigureAwait(false);
                if (completed == stopTask)
                {
                    try
                    {
                        await stopTask.ConfigureAwait(false);
                    }
                    catch (Exception ex)
                    {
                        Util.AddLog($"[REST] StopAsync failed: {ex.GetType().Name}: {ex.Message}", ex);
                    }
                }
                else
                {
                    Util.AddLog("[REST] StopAsync timeout -> Dispose", null);
                }

                // StopAsyncが戻らないケースがあるので、必ずDisposeして解放する
                try
                {
                    host.Dispose();
                }
                catch (Exception ex)
                {
                    Util.AddLog($"[REST] Dispose failed: {ex.GetType().Name}: {ex.Message}", ex);
                }
                finally
                {
                    host = null;
                }
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
            app.MapGet("/api/info/latest-release", async () =>
            {
                var info = await GetLatestReleaseInfoAsync();
                return Results.Json(info ?? new LatestReleaseInfo());
            });
            app.MapGet("/api/ui-state", () => Results.Json(state.GetUiStateView()));
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
                var filter = new Amatsukaze.Shared.QueueFilter();
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

            app.MapGet("/api/queue/changes", (HttpRequest request) =>
            {
                if (request.Query.TryGetValue("since", out var sinceRaw) &&
                    long.TryParse(sinceRaw.ToString(), out var since))
                {
                    return Results.Json(state.GetQueueChanges(since));
                }
                return Results.BadRequest(new { error = "since is required." });
            });

            app.MapGet("/api/messages/changes", (HttpRequest request) =>
            {
                var since = GetQueryLong(request, "since", 0);
                var page = request.Query["page"].ToString();
                var requestId = request.Query["requestId"].ToString();
                var levelsStr = request.Query["levels"].ToString();
                var max = GetQueryInt(request, "max", 50);
                HashSet<string> levels = null;
                if (!string.IsNullOrEmpty(levelsStr))
                {
                    levels = new HashSet<string>(
                        levelsStr.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                                 .Select(s => s.ToLowerInvariant()));
                }
                return Results.Json(state.GetMessageChanges(since, page, requestId, levels, max));
            });

            app.MapPost("/api/queue/add", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<Amatsukaze.Shared.AddQueueRequest>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                if (string.IsNullOrEmpty(data.RequestId))
                {
                    data.RequestId = Guid.NewGuid().ToString("N");
                }

                if (data.Targets != null && data.Targets.Count > 0)
                {
                    foreach (var target in data.Targets)
                    {
                        if (string.IsNullOrWhiteSpace(target.Path) || !File.Exists(target.Path))
                        {
                            return Results.BadRequest(new { error = "Target file not found." });
                        }
                    }
                }

                var serverReq = new Amatsukaze.Server.AddQueueRequest
                {
                    DirPath = data.DirPath,
                    Mode = (Amatsukaze.Server.ProcMode)data.Mode,
                    RequestId = data.RequestId,
                    AddQueueBat = data.AddQueueBat,
                    Targets = data.Targets != null
                        ? data.Targets.Select(t => new Amatsukaze.Server.AddQueueItem
                        {
                            Path = t.Path,
                            Hash = t.Hash
                        }).ToList()
                        : new List<Amatsukaze.Server.AddQueueItem>(),
                    Outputs = data.Outputs != null
                        ? data.Outputs.Select(o => new Amatsukaze.Server.OutputInfo
                        {
                            DstPath = o.DstPath,
                            Profile = o.Profile,
                            Priority = o.Priority
                        }).ToList()
                        : new List<Amatsukaze.Server.OutputInfo>()
                };

                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "queue",
                    Action = "add",
                    RequestId = data.RequestId,
                    Source = "rest"
                });

                await server.AddQueue(serverReq);

                state.TryGetMessageForRequestId(data.RequestId, out var message);
                return Results.Json(new
                {
                    requestId = data.RequestId,
                    message = message?.Message,
                    level = message?.Level
                });
            });

            app.MapPost("/api/queue/change", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<ChangeItemData>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                if (string.IsNullOrEmpty(data.RequestId))
                {
                    data.RequestId = Guid.NewGuid().ToString("N");
                }

                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "queue",
                    Action = GetQueueAction(data.ChangeType),
                    RequestId = data.RequestId,
                    Source = "rest"
                });

                await server.ChangeItem(data);

                state.TryGetMessageForRequestId(data.RequestId, out var message);
                return Results.Json(new
                {
                    ok = true,
                    requestId = data.RequestId,
                    message = message?.Message,
                    level = message?.Level
                });
            });

            app.MapPost("/api/queue/pause", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<PauseRequest>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "queue",
                    Action = "pause",
                    RequestId = Guid.NewGuid().ToString("N"),
                    Source = "rest"
                });
                await server.PauseEncode(data);
                return Results.Json(new { ok = true });
            });

            app.MapPost("/api/queue/cancel-add", async () =>
            {
                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "queue",
                    Action = "cancel-add",
                    RequestId = Guid.NewGuid().ToString("N"),
                    Source = "rest"
                });
                await server.CancelAddQueue();
                return Results.Json(new { ok = true });
            });

            app.MapGet("/api/logs/encode", () => Results.Json(state.GetEncodeLogs()));
            app.MapGet("/api/logs/encode/page", (HttpRequest request) =>
            {
                var offset = Math.Max(0, GetQueryInt(request, "offset", 0));
                var limit = Clamp(GetQueryInt(request, "limit", 50), 1, 200);
                var items = state.GetEncodeLogs();
                return Results.Json(BuildPaged(items, offset, limit));
            });
            app.MapGet("/api/logs/check", () => Results.Json(state.GetCheckLogs()));
            app.MapGet("/api/logs/check/page", (HttpRequest request) =>
            {
                var offset = Math.Max(0, GetQueryInt(request, "offset", 0));
                var limit = Clamp(GetQueryInt(request, "limit", 50), 1, 200);
                var items = state.GetCheckLogs();
                return Results.Json(BuildPaged(items, offset, limit));
            });

            app.MapGet("/api/path/suggest", (HttpRequest request) =>
            {
                var input = request.Query["input"].ToString();
                var extensions = request.Query["ext"].ToString();
                var allowFiles = GetQueryBool(request, "allowFiles", true);
                var allowDirs = GetQueryBool(request, "allowDirs", true);
                var checkAccess = GetQueryBool(request, "checkAccess", false);
                var maxDirs = GetQueryInt(request, "maxDirs", 10);
                var maxFiles = GetQueryInt(request, "maxFiles", 10);
                var dirOffset = GetQueryInt(request, "dirOffset", 0);
                var fileOffset = GetQueryInt(request, "fileOffset", 0);

                try
                {
                    var response = BuildPathSuggestResponse(input, extensions, allowFiles, allowDirs, maxDirs, maxFiles, checkAccess, dirOffset, fileOffset);
                    return Results.Json(response);
                }
                catch (Exception ex)
                {
                    Debug.Print("[REST] path suggest error: " + ex);
                    return Results.Json(new PathSuggestResponse
                    {
                        Input = input ?? string.Empty,
                        BaseDir = string.Empty,
                        Dirs = new List<PathCandidate>(),
                        Files = new List<PathCandidate>()
                    });
                }
            });

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
            app.MapGet("/api/profile-options", () =>
            {
                var isLinux = RuntimeInformation.IsOSPlatform(OSPlatform.Linux);
                var audioEncoderList = isLinux
                    ? new List<string> { "----", "----", "fdkaac", "opusenc" }
                    : ProfileSettingExtensions.AudioEncoderList.ToList();
                var outputOptions = new List<OutputOptionItem>
                {
                    new OutputOptionItem { Name = "通常", Mask = 1 },
                    new OutputOptionItem { Name = "CMをカット", Mask = 2 },
                    new OutputOptionItem { Name = "本編とCMを分離", Mask = 6 },
                    new OutputOptionItem { Name = "CMのみ", Mask = 4 },
                    new OutputOptionItem { Name = "前後のCMのみカット", Mask = 8 }
                };
                var filterOptions = new List<FilterOptionItem>
                {
                    new FilterOptionItem { Id = (int)FilterOption.None, Name = "フィルタなし" },
                    new FilterOptionItem { Id = (int)FilterOption.Setting, Name = "標準フィルタ" },
                    new FilterOptionItem { Id = (int)FilterOption.Custom, Name = "カスタムフィルタ" }
                };
                var options = new ProfileOptions
                {
                    EncoderList = ProfileSettingExtensions.EncoderList.ToList(),
                    EncoderParallelList = new List<int> { 1, 2, 3, 4, 5, 6, 7, 8 },
                    SvtAv1BitDepthList = new List<string> { "自動", "8", "10" },
                    DeinterlaceAlgorithmList = ProfileSettingExtensions.DeinterlaceAlgorithmNames.ToList(),
                    DeblockStrengthList = ProfileSettingExtensions.DeblockStrengthList.ToList(),
                    DeblockQualityList = ProfileSettingExtensions.DeblockQualityList.ToList(),
                    DeblockQualityValues = ProfileSettingExtensions.DeblockQualityListData.ToList(),
                    D3dvpGpuList = ProfileSettingExtensions.D3DVPGPUList.ToList(),
                    QtgmcPresetList = ProfileSettingExtensions.QTGMCPresetList.ToList(),
                    FilterFpsList = ProfileSettingExtensions.FilterFPSList.ToList(),
                    VfrFpsList = ProfileSettingExtensions.VFRFpsList.ToList(),
                    JlsCommandFiles = state.GetJlsCommandFiles(),
                    Mpeg2DecoderList = ProfileSettingExtensions.Mpeg2DecoderList.ToList(),
                    H264DecoderList = ProfileSettingExtensions.H264DecoderList.ToList(),
                    HevcDecoderList = ProfileSettingExtensions.HEVCDecoderList.ToList(),
                    FormatList = ProfileSettingExtensions.FormatList.ToList(),
                    OutputOptionList = outputOptions,
                    TsreplaceOutputMasks = new List<int> { 1, 8 },
                    PreBatFiles = state.GetPreBatFiles(),
                    PreEncodeBatFiles = state.GetPreEncodeBatFiles(),
                    PostBatFiles = state.GetPostBatFiles(),
                    FilterOptions = filterOptions,
                    MainScriptFiles = state.GetMainScriptFiles(),
                    PostScriptFiles = state.GetPostScriptFiles(),
                    SubtitleModeList = ProfileSettingExtensions.SubtitleModeList.ToList(),
                    WhisperModelList = ProfileSettingExtensions.WhisperModelList.ToList(),
                    AudioEncoderList = audioEncoderList,
                    IsServerLinux = isLinux
                };
                return Results.Json(options);
            });
            app.MapMethods("/api/profiles", new[] { "OPTIONS" }, () => Results.Ok());
            app.MapMethods("/api/profiles/{name}", new[] { "OPTIONS" }, () => Results.Ok());
            app.MapPost("/api/profiles", async (HttpRequest request) =>
            {
                var body = await new StreamReader(request.Body).ReadToEndAsync();
                if (string.IsNullOrWhiteSpace(body))
                {
                    return Results.BadRequest("Empty body");
                }
                var node = System.Text.Json.Nodes.JsonNode.Parse(body);
                RemoveExtensionData(node);
                var normalized = node?.ToJsonString();
                if (string.IsNullOrWhiteSpace(normalized))
                {
                    return Results.BadRequest("Invalid JSON");
                }
                var data = JsonSerializer.Deserialize<ProfileSetting>(normalized, CreateRestJsonOptions());
                if (data == null)
                {
                    return Results.BadRequest("Invalid JSON");
                }
                var requestId = Guid.NewGuid().ToString("N");
                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "profiles",
                    Action = "add",
                    RequestId = requestId,
                    Source = "rest"
                });
                await server.SetProfile(new ProfileUpdate() { Type = UpdateType.Add, Profile = data });
                return Results.Json(new { ok = true, requestId });
            });
            app.MapPut("/api/profiles/{name}", async (HttpRequest request, string name) =>
            {
                var body = await new StreamReader(request.Body).ReadToEndAsync();
                if (string.IsNullOrWhiteSpace(body))
                {
                    return Results.BadRequest("Empty body");
                }
                var node = System.Text.Json.Nodes.JsonNode.Parse(body);
                RemoveExtensionData(node);
                var normalized = node?.ToJsonString();
                if (string.IsNullOrWhiteSpace(normalized))
                {
                    return Results.BadRequest("Invalid JSON");
                }
                var data = JsonSerializer.Deserialize<ProfileSetting>(normalized, CreateRestJsonOptions());
                if (data == null)
                {
                    return Results.BadRequest("Invalid JSON");
                }
                var requestId = Guid.NewGuid().ToString("N");
                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "profiles",
                    Action = "update",
                    RequestId = requestId,
                    Source = "rest"
                });
                await server.SetProfile(new ProfileUpdate() { Type = UpdateType.Update, Profile = data, NewName = name != data.Name ? name : null });
                return Results.Json(new { ok = true, requestId });
            });
            app.MapDelete("/api/profiles/{name}", async (string name) =>
            {
                var requestId = Guid.NewGuid().ToString("N");
                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "profiles",
                    Action = "remove",
                    RequestId = requestId,
                    Source = "rest"
                });
                await server.SetProfile(new ProfileUpdate() { Type = UpdateType.Remove, Profile = new ProfileSetting() { Name = name } });
                return Results.Json(new { ok = true, requestId });
            });

            app.MapGet("/api/autoselect", () => Results.Json(state.GetAutoSelects()));
            app.MapGet("/api/autoselect/options", () => Results.Json(BuildAutoSelectOptionsView()));
            app.MapMethods("/api/autoselect", new[] { "OPTIONS" }, () => Results.Ok());
            app.MapMethods("/api/autoselect/options", new[] { "OPTIONS" }, () => Results.Ok());
            app.MapMethods("/api/autoselect/{name}", new[] { "OPTIONS" }, () => Results.Ok());
            app.MapPost("/api/autoselect", async (HttpRequest request) =>
            {
                var body = await new StreamReader(request.Body).ReadToEndAsync();
                if (string.IsNullOrWhiteSpace(body))
                {
                    return Results.BadRequest("Empty body");
                }
                var node = System.Text.Json.Nodes.JsonNode.Parse(body);
                RemoveExtensionData(node);
                var normalized = node?.ToJsonString();
                if (string.IsNullOrWhiteSpace(normalized))
                {
                    return Results.BadRequest("Invalid JSON");
                }
                var data = JsonSerializer.Deserialize<AutoSelectProfile>(normalized, CreateRestJsonOptions());
                if (data == null)
                {
                    return Results.BadRequest("Invalid JSON");
                }
                var requestId = Guid.NewGuid().ToString("N");
                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "profiles",
                    Action = "autoselect-add",
                    RequestId = requestId,
                    Source = "rest"
                });
                await server.SetAutoSelect(new AutoSelectUpdate() { Type = UpdateType.Add, Profile = data });
                return Results.Json(new { ok = true, requestId });
            });
            app.MapPut("/api/autoselect/{name}", async (HttpRequest request, string name) =>
            {
                var body = await new StreamReader(request.Body).ReadToEndAsync();
                if (string.IsNullOrWhiteSpace(body))
                {
                    return Results.BadRequest("Empty body");
                }
                var node = System.Text.Json.Nodes.JsonNode.Parse(body);
                RemoveExtensionData(node);
                var normalized = node?.ToJsonString();
                if (string.IsNullOrWhiteSpace(normalized))
                {
                    return Results.BadRequest("Invalid JSON");
                }
                var data = JsonSerializer.Deserialize<AutoSelectProfile>(normalized, CreateRestJsonOptions());
                if (data == null)
                {
                    return Results.BadRequest("Invalid JSON");
                }
                var requestId = Guid.NewGuid().ToString("N");
                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "profiles",
                    Action = "autoselect-update",
                    RequestId = requestId,
                    Source = "rest"
                });
                await server.SetAutoSelect(new AutoSelectUpdate() { Type = UpdateType.Update, Profile = data, NewName = name != data.Name ? name : null });
                return Results.Json(new { ok = true, requestId });
            });
            app.MapDelete("/api/autoselect/{name}", async (string name) =>
            {
                var requestId = Guid.NewGuid().ToString("N");
                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "profiles",
                    Action = "autoselect-remove",
                    RequestId = requestId,
                    Source = "rest"
                });
                await server.SetAutoSelect(new AutoSelectUpdate() { Type = UpdateType.Remove, Profile = new AutoSelectProfile() { Name = name } });
                return Results.Json(new { ok = true, requestId });
            });

            app.MapGet("/api/services", () => Results.Json(state.GetServiceViews()));
            app.MapGet("/api/service-settings", () => Results.Json(state.GetServiceSettingViews()));
            app.MapGet("/api/service-options", () => Results.Json(new ServiceOptions()
            {
                JlsCommandFiles = state.GetJlsCommandFiles()
            }));
            app.MapPost("/api/services/update", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<ServiceSettingUpdate>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                var requestId = Guid.NewGuid().ToString("N");
                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "services",
                    Action = "update",
                    RequestId = requestId,
                    Source = "rest"
                });
                await server.SetServiceSetting(data);
                return Results.Json(new { ok = true, requestId });
            });

            app.MapPut("/api/service-settings/{serviceId}/logos/period", async (HttpRequest request, int serviceId) =>
            {
                var data = await request.ReadFromJsonAsync<LogoPeriodUpdateRequest>(CreateRestJsonOptions());
                if (data == null || string.IsNullOrWhiteSpace(data.FileName))
                {
                    return Results.BadRequest();
                }
                if (!state.TryGetServiceSetting(serviceId, out var service) || service == null)
                {
                    return Results.NotFound();
                }
                var logo = service.LogoSettings?.FirstOrDefault(l => l.FileName == data.FileName);
                if (logo == null)
                {
                    return Results.NotFound();
                }
                if (data.From.HasValue)
                {
                    logo.From = data.From.Value;
                }
                if (data.To.HasValue)
                {
                    logo.To = data.To.Value;
                }
                var requestId = Guid.NewGuid().ToString("N");
                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "services",
                    Action = "logo-period",
                    RequestId = requestId,
                    Source = "rest"
                });
                await server.SetServiceSetting(new ServiceSettingUpdate()
                {
                    Type = ServiceSettingUpdateType.Update,
                    ServiceId = serviceId,
                    Data = service
                });
                return Results.Json(new { ok = true, requestId });
            });

            app.MapPut("/api/service-settings/{serviceId}/logos/enabled", async (HttpRequest request, int serviceId) =>
            {
                var data = await request.ReadFromJsonAsync<LogoEnabledUpdateRequest>(CreateRestJsonOptions());
                if (data == null || string.IsNullOrWhiteSpace(data.FileName))
                {
                    return Results.BadRequest();
                }
                if (!state.TryGetServiceSetting(serviceId, out var service) || service == null)
                {
                    return Results.NotFound();
                }
                var logo = service.LogoSettings?.FirstOrDefault(l => l.FileName == data.FileName);
                if (logo == null)
                {
                    return Results.NotFound();
                }
                logo.Enabled = data.Enabled;
                var requestId = Guid.NewGuid().ToString("N");
                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "services",
                    Action = "logo-enabled",
                    RequestId = requestId,
                    Source = "rest"
                });
                await server.SetServiceSetting(new ServiceSettingUpdate()
                {
                    Type = ServiceSettingUpdateType.Update,
                    ServiceId = serviceId,
                    Data = service
                });
                return Results.Json(new { ok = true, requestId });
            });

            app.MapDelete("/api/service-settings/{serviceId}/logos", async (HttpRequest request, int serviceId) =>
            {
                var data = await request.ReadFromJsonAsync<LogoFileNameRequest>(CreateRestJsonOptions());
                if (data == null || string.IsNullOrWhiteSpace(data.FileName))
                {
                    return Results.BadRequest();
                }
                if (!state.TryGetServiceSetting(serviceId, out var service) || service == null)
                {
                    return Results.NotFound();
                }
                var idx = -1;
                if (service.LogoSettings != null)
                {
                    for (int i = 0; i < service.LogoSettings.Count; i++)
                    {
                        if (service.LogoSettings[i]?.FileName == data.FileName)
                        {
                            idx = i;
                            break;
                        }
                    }
                }
                if (idx < 0)
                {
                    return Results.NotFound();
                }
                var requestId = Guid.NewGuid().ToString("N");
                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "services",
                    Action = "logo-remove",
                    RequestId = requestId,
                    Source = "rest"
                });
                await server.SetServiceSetting(new ServiceSettingUpdate()
                {
                    Type = ServiceSettingUpdateType.RemoveLogo,
                    ServiceId = serviceId,
                    RemoveLogoIndex = idx
                });
                return Results.Json(new { ok = true, requestId });
            });

            app.MapPost("/api/service-settings/{serviceId}/logos/no-logo", async (int serviceId) =>
            {
                var requestId = Guid.NewGuid().ToString("N");
                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "services",
                    Action = "no-logo-add",
                    RequestId = requestId,
                    Source = "rest"
                });
                await server.SetServiceSetting(new ServiceSettingUpdate()
                {
                    Type = ServiceSettingUpdateType.AddNoLogo,
                    ServiceId = serviceId
                });
                return Results.Json(new { ok = true, requestId });
            });

            app.MapDelete("/api/service-settings/{serviceId}/logos/no-logo", async (int serviceId) =>
            {
                if (!state.TryGetServiceSetting(serviceId, out var service) || service == null)
                {
                    return Results.NotFound();
                }
                var idx = -1;
                if (service.LogoSettings != null)
                {
                    for (int i = 0; i < service.LogoSettings.Count; i++)
                    {
                        if (service.LogoSettings[i]?.FileName == LogoSetting.NO_LOGO)
                        {
                            idx = i;
                            break;
                        }
                    }
                }
                if (idx < 0)
                {
                    return Results.NotFound();
                }
                var requestId = Guid.NewGuid().ToString("N");
                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "services",
                    Action = "no-logo-remove",
                    RequestId = requestId,
                    Source = "rest"
                });
                await server.SetServiceSetting(new ServiceSettingUpdate()
                {
                    Type = ServiceSettingUpdateType.RemoveLogo,
                    ServiceId = serviceId,
                    RemoveLogoIndex = idx
                });
                return Results.Json(new { ok = true, requestId });
            });

            app.MapPost("/api/services/logo", async (HttpRequest request) =>
            {
                if (!request.HasFormContentType)
                {
                    return Results.BadRequest();
                }
                var form = await request.ReadFormAsync();
                int? serviceId = null;
                if (int.TryParse(form["serviceId"], out var parsedServiceId))
                {
                    serviceId = parsedServiceId;
                }
                int? imgw = null;
                int? imgh = null;
                if (int.TryParse(form["imgw"], out var parsedImgw))
                {
                    imgw = parsedImgw;
                }
                if (int.TryParse(form["imgh"], out var parsedImgh))
                {
                    imgh = parsedImgh;
                }
                var file = form.Files.GetFile("image");
                if (file == null)
                {
                    return Results.BadRequest();
                }
                byte[] uploadData;
                using (var ms = new MemoryStream())
                {
                    await file.CopyToAsync(ms);
                    uploadData = ms.ToArray();
                }
                if (uploadData.Length == 0)
                {
                    return Results.BadRequest("Empty file.");
                }

                var requestId = Guid.NewGuid().ToString("N");
                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "services",
                    Action = "logo-upload",
                    RequestId = requestId,
                    Source = "rest"
                });

                using var ctx = new AMTContext();
                var tmpBase = Path.Combine(Path.GetTempPath(), "amatsukaze-logo-" + Guid.NewGuid().ToString("N"));
                var srcPath = tmpBase + ".lgd";
                var tmpConverted = tmpBase + "-converted.lgd";
                try
                {
                    await File.WriteAllBytesAsync(srcPath, uploadData);
                    int? extendedSid = null;
                    string extendedServiceName = null;
                    try
                    {
                        using (var logo = new LogoFile(ctx, srcPath))
                        {
                            extendedSid = logo.ServiceId;
                            extendedServiceName = logo.Name;
                        }
                    }
                    catch (IOException)
                    {
                        extendedSid = null;
                    }

                    if (extendedSid.HasValue)
                    {
                        var fileSid = extendedSid.Value;
                        try
                        {
                            using var verify = new LogoFile(ctx, srcPath);
                        }
                        catch (Exception ex)
                        {
                            return Results.BadRequest("Invalid lgd file: " + ex.Message);
                        }
                        if (!state.TryGetServiceSetting(fileSid, out var existing) || existing == null)
                        {
                            string NormalizeServiceName(string name)
                            {
                                if (string.IsNullOrWhiteSpace(name))
                                {
                                    return name ?? string.Empty;
                                }
                                var s = name.Trim();
                                if (s.EndsWith(")", StringComparison.Ordinal))
                                {
                                    var idx = s.LastIndexOf('(');
                                    if (idx >= 0 && idx < s.Length - 1)
                                    {
                                        s = s.Substring(0, idx).TrimEnd();
                                    }
                                }
                                return s;
                            }

                            var element = new ServiceSettingElement()
                            {
                                ServiceId = fileSid,
                                ServiceName = string.IsNullOrWhiteSpace(extendedServiceName)
                                    ? $"SID{fileSid}"
                                    : NormalizeServiceName(extendedServiceName),
                                DisableCMCheck = false,
                                JLSCommand = state.GetJlsCommandFiles().FirstOrDefault(),
                                JLSOption = "",
                                LogoSettings = new List<LogoSetting>(),
                            };
                            await server.SetServiceSetting(new ServiceSettingUpdate()
                            {
                                Type = ServiceSettingUpdateType.Update,
                                ServiceId = element.ServiceId,
                                Data = element
                            });
                        }
                        await server.SendLogoFile(new LogoFileData()
                        {
                            Data = uploadData,
                            ServiceId = fileSid,
                            LogoIdx = 1
                        });
                    }
                    else
                    {
                        if (!serviceId.HasValue)
                        {
                            return Results.BadRequest("ServiceId is required for AviUtl lgd.");
                        }
                        if (!imgw.HasValue || !imgh.HasValue || imgw.Value <= 0 || imgh.Value <= 0)
                        {
                            return Results.BadRequest("imgw/imgh is required for AviUtl lgd.");
                        }
                        LogoFile.ConvertAviUtlToExtended(ctx, srcPath, tmpConverted, serviceId.Value, imgw.Value, imgh.Value);
                        try
                        {
                            using var verify = new LogoFile(ctx, tmpConverted);
                        }
                        catch (Exception ex)
                        {
                            return Results.BadRequest("Invalid converted lgd file: " + ex.Message);
                        }
                        var convertedData = await File.ReadAllBytesAsync(tmpConverted);
                        await server.SendLogoFile(new LogoFileData()
                        {
                            Data = convertedData,
                            ServiceId = serviceId.Value,
                            LogoIdx = 1
                        });
                    }
                }
                finally
                {
                    try
                    {
                        if (File.Exists(srcPath))
                        {
                            File.Delete(srcPath);
                        }
                        if (File.Exists(tmpConverted))
                        {
                            File.Delete(tmpConverted);
                        }
                    }
                    catch { }
                }
                return Results.Json(new { ok = true, requestId });
            });

            app.MapPost("/api/services/logo/probe", async (HttpRequest request) =>
            {
                if (!request.HasFormContentType)
                {
                    return Results.BadRequest("Invalid form.");
                }
                var form = await request.ReadFormAsync();
                var file = form.Files.GetFile("image");
                if (file == null)
                {
                    return Results.BadRequest("File is required.");
                }
                byte[] uploadData;
                using (var ms = new MemoryStream())
                {
                    await file.CopyToAsync(ms);
                    uploadData = ms.ToArray();
                }
                if (uploadData.Length == 0)
                {
                    return Results.BadRequest("Empty file.");
                }

                using var ctx = new AMTContext();
                var tmpBase = Path.Combine(Path.GetTempPath(), "amatsukaze-logo-probe-" + Guid.NewGuid().ToString("N"));
                var srcPath = tmpBase + ".lgd";
                try
                {
                    await File.WriteAllBytesAsync(srcPath, uploadData);
                    try
                    {
                        using var logo = new LogoFile(ctx, srcPath);
                        var resp = new LogoProbeResponse()
                        {
                            IsExtended = true,
                            ServiceId = logo.ServiceId,
                            ServiceName = logo.Name,
                            ImageWidth = logo.ImageWidth,
                            ImageHeight = logo.ImageHeight
                        };
                        return Results.Json(resp);
                    }
                    catch (IOException)
                    {
                        return Results.Json(new LogoProbeResponse() { IsExtended = false });
                    }
                    catch (Exception)
                    {
                        return Results.Json(new LogoProbeResponse() { IsExtended = false });
                    }
                }
                finally
                {
                    try
                    {
                        if (File.Exists(srcPath))
                        {
                            File.Delete(srcPath);
                        }
                    }
                    catch { }
                }
            });

            app.MapPost("/api/services/logo/rescan", () =>
            {
                var requestId = Guid.NewGuid().ToString("N");
                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "services",
                    Action = "logo-rescan",
                    RequestId = requestId,
                    Source = "rest"
                });
                server.RequestLogoRescan();
                return Results.Json(new { ok = true, requestId });
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
            app.MapPut("/api/drcs/map", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<DrcsMapUpdateRequest>();
                if (data == null || string.IsNullOrWhiteSpace(data.Md5))
                {
                    return Results.BadRequest();
                }
                await server.AddDrcsMap(new DrcsImage
                {
                    MD5 = data.Md5,
                    MapStr = data.MapStr
                });
                return Results.Json(new { ok = true });
            });
            app.MapDelete("/api/drcs/map/{md5}", async (string md5) =>
            {
                if (string.IsNullOrWhiteSpace(md5))
                {
                    return Results.BadRequest();
                }
                await server.AddDrcsMap(new DrcsImage
                {
                    MD5 = md5,
                    MapStr = null
                });
                return Results.Json(new { ok = true });
            });
            app.MapGet("/api/drcs/appearance/{md5}", (string md5) =>
            {
                state.TryGetDrcsAppearance(md5, out var items);
                return Results.Json(new DrcsAppearanceResponse
                {
                    Md5 = md5,
                    Items = items ?? new List<string>()
                });
            });

            app.MapGet("/api/console", () => Results.Json(state.GetConsoleView()));

            app.MapGet("/api/settings", () => Results.Json(state.GetSetting()));
            app.MapMethods("/api/settings", new[] { "OPTIONS" }, () => Results.Ok());
            app.MapPut("/api/settings", async (HttpRequest request) =>
            {
                try
                {
                    var origin = request.Headers["Origin"].ToString();
                    var body = await new StreamReader(request.Body).ReadToEndAsync();
                    Console.WriteLine($"[REST] PUT /api/settings Origin='{origin}' Length={body?.Length ?? 0}");
                    if (string.IsNullOrWhiteSpace(body))
                    {
                        return Results.BadRequest("Empty body");
                    }

                    var node = System.Text.Json.Nodes.JsonNode.Parse(body);
                    RemoveExtensionData(node);
                    var normalized = node?.ToJsonString();
                    if (string.IsNullOrWhiteSpace(normalized))
                    {
                        return Results.BadRequest("Invalid JSON");
                    }

                    var data = JsonSerializer.Deserialize<Setting>(normalized, CreateRestJsonOptions());
                    if (data == null)
                    {
                        return Results.BadRequest("Invalid JSON");
                    }
                    var requestId = Guid.NewGuid().ToString("N");
                    using var scope = OperationContextScope.Use(new OperationContext
                    {
                        Page = "settings",
                        Action = "update",
                        RequestId = requestId,
                        Source = "rest"
                    });
                    await server.SetCommonData(new CommonData() { Setting = data });
                    return Results.Json(new { ok = true, requestId });
                }
                catch (Exception ex)
                {
                    Console.WriteLine($"[REST] PUT /api/settings Error: {ex}");
                    return Results.Problem("Failed to update settings");
                }
            });

            app.MapGet("/api/makescript", () => Results.Json(state.GetMakeScriptData()));
            app.MapGet("/api/batfiles/addqueue", () => Results.Json(state.GetAddQueueBatFiles()));
            app.MapGet("/api/batfiles/queuefinish", () => Results.Json(state.GetQueueFinishBatFiles()));
            app.MapPut("/api/makescript", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<MakeScriptData>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                var requestId = Guid.NewGuid().ToString("N");
                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "makescript",
                    Action = "update",
                    RequestId = requestId,
                    Source = "rest"
                });
                await server.SetCommonData(new CommonData() { MakeScriptData = data });
                return Results.Json(new { ok = true, requestId });
            });

            app.MapPut("/api/finish-setting", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<FinishSetting>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                var requestId = Guid.NewGuid().ToString("N");
                using var scope = OperationContextScope.Use(new OperationContext
                {
                    Page = "settings",
                    Action = "finish-setting",
                    RequestId = requestId,
                    Source = "rest"
                });
                await server.SetCommonData(new CommonData() { FinishSetting = data });
                return Results.Json(new { ok = true, requestId });
            });

            app.MapGet("/api/makescript/preview", () =>
            {
                var req = new MakeScriptGenerateRequestInternal
                {
                    MakeScriptData = state.GetMakeScriptData(),
                    TargetHost = "local",
                    ScriptType = Util.IsServerWindows() ? "bat" : "sh"
                };
                if (!TryBuildMakeScript(req, server.ServerPortForRest, out var script, out var error))
                {
                    return Results.Problem(error ?? "Failed to build script.");
                }
                return Results.Json(new MakeScriptPreview()
                {
                    CommandLine = script
                });
            });
            app.MapPost("/api/makescript/preview", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<MakeScriptGenerateRequestInternal>(CreateRestJsonOptions());
                if (data == null)
                {
                    return Results.BadRequest("Invalid request");
                }
                if (!TryBuildMakeScript(data, server.ServerPortForRest, out var script, out var error))
                {
                    return Results.BadRequest(error ?? "Failed to build script");
                }
                return Results.Json(new MakeScriptPreview()
                {
                    CommandLine = script
                });
            });
            app.MapPost("/api/makescript/file", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<MakeScriptGenerateRequestInternal>(CreateRestJsonOptions());
                if (data == null)
                {
                    return Results.BadRequest("Invalid request");
                }
                if (!TryBuildMakeScript(data, server.ServerPortForRest, out var script, out var error))
                {
                    return Results.BadRequest(error ?? "Failed to build script");
                }
                var isBat = string.Equals(data.ScriptType, "bat", StringComparison.OrdinalIgnoreCase)
                    || (string.IsNullOrWhiteSpace(data.ScriptType) && Util.IsServerWindows());
                var fileName = isBat ? "AmatsukazeAddTask.bat" : "AmatsukazeAddTask.sh";
                var bytes = Encoding.UTF8.GetBytes(script);
                return Results.File(bytes, "text/plain", fileName);
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
                var data = await request.ReadFromJsonAsync<LogoAnalyzeStartRequest>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                if (!logoAnalyze.TryStart(data, out var status, out var error))
                {
                    return Results.BadRequest(new { message = error ?? "ロゴ解析を開始できませんでした" });
                }
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

            app.MapPost("/api/logo/analyze/{jobId}/apply", (string jobId) =>
            {
                if (logoAnalyze.TryApply(jobId, out var error))
                {
                    return Results.Ok();
                }
                return Results.BadRequest(new { message = error ?? "ロゴの採用に失敗しました" });
            });

            app.MapPost("/api/logo/analyze/{jobId}/discard", (string jobId) =>
            {
                if (logoAnalyze.TryDiscard(jobId, out var error))
                {
                    return Results.Ok();
                }
                return Results.BadRequest(new { message = error ?? "ロゴの破棄に失敗しました" });
            });

            app.MapPost("/api/logo/preview/sessions", async (HttpRequest request) =>
            {
                var data = await request.ReadFromJsonAsync<LogoPreviewSessionRequest>();
                if (data == null)
                {
                    return Results.BadRequest();
                }
                if (!logoPreview.TryCreateSession(data, out var response, out var error))
                {
                    return Results.BadRequest(new { message = error ?? "プレビューセッションを作成できませんでした" });
                }
                return Results.Json(response);
            });

            app.MapGet("/api/logo/preview/sessions/{sessionId}/frame", (HttpContext context, string sessionId, string pos) =>
            {
                var session = logoPreview.GetSession(sessionId);
                if (session == null)
                {
                    return Results.NotFound();
                }
                if (!float.TryParse(pos, NumberStyles.Float, CultureInfo.InvariantCulture, out var posValue))
                {
                    return Results.BadRequest(new { message = "posの形式が不正です" });
                }
                if (posValue < 0f || posValue > 1f)
                {
                    return Results.BadRequest(new { message = "posは0〜1の範囲で指定してください" });
                }
                var bitmap = session.GetFrame(posValue);
                if (bitmap == null)
                {
                    return Results.NotFound();
                }
                using var ms = new MemoryStream();
                BitmapManager.SaveBitmapAsPng(bitmap, ms);
                var bytes = ms.ToArray();
                context.Response.Headers.CacheControl = "no-store, no-cache, must-revalidate, max-age=0";
                return Results.File(bytes, "image/png");
            });

            app.MapDelete("/api/logo/preview/sessions/{sessionId}", (string sessionId) =>
            {
                if (logoPreview.RemoveSession(sessionId))
                {
                    return Results.Ok();
                }
                return Results.NotFound();
            });
        }

        private static bool IsAllowedRemote(IPAddress address)
        {
            if (IPAddress.IsLoopback(address))
            {
                return true;
            }

            if (address.IsIPv4MappedToIPv6)
            {
                address = address.MapToIPv4();
            }

            if (address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork)
            {
                var bytes = address.GetAddressBytes();
                if (bytes[0] == 10)
                {
                    return true;
                }
                if (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31)
                {
                    return true;
                }
                if (bytes[0] == 192 && bytes[1] == 168)
                {
                    return true;
                }
                return false;
            }

            if (address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetworkV6)
            {
                if (address.IsIPv6LinkLocal || address.IsIPv6SiteLocal)
                {
                    return true;
                }
                var bytes = address.GetAddressBytes();
                return (bytes[0] & 0xFE) == 0xFC; // Unique local (fc00::/7)
            }

            return false;
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

        private static bool TryBuildMakeScript(MakeScriptGenerateRequestInternal request, int serverPort, out string script, out string error)
        {
            script = string.Empty;
            error = string.Empty;
            if (request == null || request.MakeScriptData == null)
            {
                error = "MakeScript data is missing.";
                return false;
            }

            var data = request.MakeScriptData;
            var targetHost = string.IsNullOrWhiteSpace(request.TargetHost) ? "remote" : request.TargetHost;
            var scriptType = string.IsNullOrWhiteSpace(request.ScriptType)
                ? (Util.IsServerWindows() ? "bat" : "sh")
                : request.ScriptType;
            var isBat = string.Equals(scriptType, "bat", StringComparison.OrdinalIgnoreCase);
            var isWindows = Util.IsServerWindows();

            if (string.Equals(targetHost, "local", StringComparison.OrdinalIgnoreCase))
            {
                if (isWindows && !isBat)
                {
                    error = "Windowsサーバーではバッチのみ生成できます。";
                    return false;
                }
                if (!isWindows && isBat)
                {
                    error = "Linuxサーバーではシェルのみ生成できます。";
                    return false;
                }
            }

            var prof = data.Profile;
            if (string.IsNullOrWhiteSpace(prof))
            {
                error = "プロファイルを選択してください";
                return false;
            }

            var dst = data.OutDir?.TrimEnd(Path.DirectorySeparatorChar);
            if (string.IsNullOrWhiteSpace(dst))
            {
                error = "出力先が設定されていません";
                return false;
            }
            if (!Directory.Exists(dst))
            {
                error = "出力先ディレクトリにアクセスできません";
                return false;
            }

            string nas = null;
            if (data.IsNasEnabled)
            {
                if (string.IsNullOrWhiteSpace(data.NasDir))
                {
                    error = "NAS保存先を指定してください。";
                    return false;
                }
                nas = data.NasDir.TrimEnd(Path.DirectorySeparatorChar);
            }

            var remoteHost = request.RemoteHost;
            var ip = string.Equals(targetHost, "local", StringComparison.OrdinalIgnoreCase)
                ? "127.0.0.1"
                : remoteHost;
            if (string.IsNullOrWhiteSpace(ip))
            {
                error = "接続先ホストが指定されていません";
                return false;
            }

            var port = serverPort > 0 ? serverPort : ServerSupport.DEFAULT_PORT;
            var direct = data.IsDirect;
            var addTaskPath = Path.Combine(AppContext.BaseDirectory, Util.IsServerWindows() ? "AmatsukazeAddTask.exe" : "AmatsukazeAddTask");
            var lineBreak = isBat ? "\r\n" : "\n";
            var comment = isBat ? "rem" : "#";

            var sb = new StringBuilder();
            if (!isBat)
            {
                sb.Append("#!/bin/bash").Append(lineBreak);
            }
            if (direct)
            {
                sb.Append(comment).Append(" _EDCBX_DIRECT_").Append(lineBreak);
            }
            var filePathToken = direct
                ? "$FilePath$"
                : isBat ? "%FilePath%" : "${FilePath}";
            sb.AppendFormat("\"{0}\"", addTaskPath)
                .AppendFormat(" -r \"{0}\"", Directory.GetCurrentDirectory())
                .AppendFormat(" -f \"{0}\" -ip \"{1}\"", filePathToken, ip)
                .AppendFormat(" -p {0}", port)
                .AppendFormat(" -o \"{0}\"", dst)
                .AppendFormat(" -s \"{0}\"", prof)
                .AppendFormat(" --priority {0}", data.Priority);
            if (!string.IsNullOrEmpty(nas))
            {
                sb.AppendFormat(" -d \"{0}\"", nas);
            }
            if (data.IsWakeOnLan)
            {
                if (string.IsNullOrWhiteSpace(request.Subnet) || string.IsNullOrWhiteSpace(request.Mac))
                {
                    error = "Wake On Lanに必要な情報が不足しています";
                    return false;
                }
                sb.AppendFormat(" --subnet \"{0}\"", request.Subnet)
                    .AppendFormat(" --mac \"{0}\"", request.Mac);
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
            if (!string.IsNullOrEmpty(data.AddQueueBat))
            {
                sb.AppendFormat(" -b \"{0}\"", data.AddQueueBat);
            }

            script = sb.ToString();
            return true;
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

        private static void RemoveExtensionData(System.Text.Json.Nodes.JsonNode node)
        {
            if (node == null)
            {
                return;
            }
            if (node is System.Text.Json.Nodes.JsonObject obj)
            {
                var keys = obj.Select(kv => kv.Key).ToList();
                foreach (var key in keys)
                {
                    if (string.Equals(key, "ExtensionData", StringComparison.OrdinalIgnoreCase))
                    {
                        obj.Remove(key);
                        continue;
                    }
                    RemoveExtensionData(obj[key]);
                }
                return;
            }
            if (node is System.Text.Json.Nodes.JsonArray arr)
            {
                foreach (var item in arr)
                {
                    RemoveExtensionData(item);
                }
            }
        }

        private static bool GetQueryBool(HttpRequest request, string key, bool fallback)
        {
            if (!request.Query.TryGetValue(key, out var value))
            {
                return fallback;
            }
            if (bool.TryParse(value.ToString(), out var parsed))
            {
                return parsed;
            }
            return fallback;
        }

        private static string GetQueueAction(ChangeItemType type)
        {
            return type switch
            {
                ChangeItemType.ResetState => "retry",
                ChangeItemType.UpdateProfile => "reapply",
                ChangeItemType.Duplicate => "duplicate",
                ChangeItemType.Cancel => "cancel",
                ChangeItemType.Priority => "priority",
                ChangeItemType.Profile => "profile",
                ChangeItemType.RemoveItem => "delete",
                ChangeItemType.RemoveCompleted => "delete-completed",
                ChangeItemType.ForceStart => "force",
                ChangeItemType.RemoveSourceFile => "delete-source",
                ChangeItemType.Move => "move",
                _ => "change"
            };
        }

        private static int GetQueryInt(HttpRequest request, string key, int fallback)
        {
            if (!request.Query.TryGetValue(key, out var value))
            {
                return fallback;
            }
            if (int.TryParse(value.ToString(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed))
            {
                return parsed;
            }
            return fallback;
        }

        private static int Clamp(int value, int min, int max)
        {
            if (value < min) return min;
            if (value > max) return max;
            return value;
        }

        private static PagedResult<T> BuildPaged<T>(List<T> items, int offset, int limit)
        {
            if (offset > items.Count)
            {
                offset = items.Count;
            }
            var page = items.Skip(offset).Take(limit).ToList();
            return new PagedResult<T>
            {
                Total = items.Count,
                Items = page
            };
        }

        private static long GetQueryLong(HttpRequest request, string key, long fallback)
        {
            if (!request.Query.TryGetValue(key, out var value))
            {
                return fallback;
            }
            if (long.TryParse(value.ToString(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed))
            {
                return parsed;
            }
            return fallback;
        }

        private static PathSuggestResponse BuildPathSuggestResponse(string input, string extensions, bool allowFiles, bool allowDirs, int maxDirs, int maxFiles, bool checkAccess, int dirOffset, int fileOffset)
        {
            var response = new PathSuggestResponse
            {
                Input = input ?? string.Empty,
                BaseDir = string.Empty,
                HasMoreDirs = false,
                HasMoreFiles = false,
                NextDirOffset = 0,
                NextFileOffset = 0
            };

            if (string.IsNullOrWhiteSpace(input))
            {
                return response;
            }

            var normalized = NormalizePath(input.Trim());
            var (baseDir, needle) = SplitInput(normalized);
            response.BaseDir = baseDir;

            if (string.IsNullOrWhiteSpace(baseDir) || !Directory.Exists(baseDir))
            {
                return response;
            }

            var extSet = ParseExtensions(extensions);

            if (allowDirs && maxDirs > 0)
            {
                var dirs = new List<PathCandidate>();
                var seen = 0;
                foreach (var dir in SafeEnumerateDirectories(baseDir))
                {
                    if (checkAccess && !CanReadDirectory(dir))
                    {
                        continue;
                    }
                    var name = GetName(dir);
                    if (!TryMatch(name, needle, out var startsWith, out var matchIndex))
                    {
                        continue;
                    }
                    if (seen < dirOffset)
                    {
                        seen++;
                        continue;
                    }
                    if (dirs.Count < maxDirs)
                    {
                        dirs.Add(new PathCandidate
                        {
                            Name = name,
                            FullPath = dir,
                            StartsWith = startsWith,
                            MatchIndex = matchIndex
                        });
                        seen++;
                    }
                    else
                    {
                        response.HasMoreDirs = true;
                        break;
                    }
                }
                response.Dirs = OrderCandidates(dirs).ToList();
                response.NextDirOffset = dirOffset + response.Dirs.Count;
            }

            if (allowFiles && maxFiles > 0)
            {
                var files = new List<PathCandidate>();
                var seen = 0;
                foreach (var file in SafeEnumerateFiles(baseDir))
                {
                    if (checkAccess && !CanReadFile(file))
                    {
                        continue;
                    }
                    if (extSet.Count > 0)
                    {
                        var ext = Path.GetExtension(file);
                        if (string.IsNullOrEmpty(ext) || !extSet.Contains(ext.ToLowerInvariant()))
                        {
                            continue;
                        }
                    }
                    var name = GetName(file);
                    if (!TryMatch(name, needle, out var startsWith, out var matchIndex))
                    {
                        continue;
                    }
                    if (seen < fileOffset)
                    {
                        seen++;
                        continue;
                    }
                    if (files.Count < maxFiles)
                    {
                        files.Add(new PathCandidate
                        {
                            Name = name,
                            FullPath = file,
                            StartsWith = startsWith,
                            MatchIndex = matchIndex
                        });
                        seen++;
                    }
                    else
                    {
                        response.HasMoreFiles = true;
                        break;
                    }
                }
                response.Files = OrderCandidates(files).ToList();
                response.NextFileOffset = fileOffset + response.Files.Count;
            }

            return response;
        }

        private static string NormalizePath(string input)
        {
            if (string.IsNullOrEmpty(input))
            {
                return string.Empty;
            }
            var normalized = input.Replace('\\', Path.DirectorySeparatorChar);
            return normalized.Replace(Path.AltDirectorySeparatorChar, Path.DirectorySeparatorChar);
        }

#nullable enable
        private static readonly HttpClient GithubClient = CreateGithubClient();
        private static readonly object LatestReleaseLock = new();
        private static LatestReleaseInfo? LatestReleaseCache;
        private static DateTime LatestReleaseCacheAt = DateTime.MinValue;

        private static HttpClient CreateGithubClient()
        {
            var client = new HttpClient();
            client.DefaultRequestHeaders.UserAgent.ParseAdd("AmatsukazeServer/1.0");
            client.Timeout = TimeSpan.FromSeconds(5);
            return client;
        }

        private static async Task<LatestReleaseInfo?> GetLatestReleaseInfoAsync()
        {
            var now = DateTime.UtcNow;
            lock (LatestReleaseLock)
            {
                if (LatestReleaseCache != null && (now - LatestReleaseCacheAt) < TimeSpan.FromMinutes(30))
                {
                    return LatestReleaseCache;
                }
            }

            try
            {
                using var req = new HttpRequestMessage(HttpMethod.Get, "https://api.github.com/repos/rigaya/Amatsukaze/releases/latest");
                using var res = await GithubClient.SendAsync(req);
                if (!res.IsSuccessStatusCode)
                {
                    return LatestReleaseCache;
                }
                var json = await res.Content.ReadAsStringAsync();
                using var doc = JsonDocument.Parse(json);
                var root = doc.RootElement;
                var tag = root.TryGetProperty("tag_name", out var tagProp) ? tagProp.GetString() : null;
                var url = root.TryGetProperty("html_url", out var urlProp) ? urlProp.GetString() : null;
                DateTime? publishedAt = null;
                if (root.TryGetProperty("published_at", out var pubProp))
                {
                    if (DateTime.TryParse(pubProp.GetString(), CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal, out var parsed))
                    {
                        publishedAt = parsed;
                    }
                }

                var info = new LatestReleaseInfo
                {
                    Tag = tag ?? string.Empty,
                    Url = url ?? string.Empty,
                    PublishedAt = publishedAt
                };

                lock (LatestReleaseLock)
                {
                    LatestReleaseCache = info;
                    LatestReleaseCacheAt = now;
                }
                return info;
            }
            catch
            {
                return LatestReleaseCache;
            }
        }
#nullable restore

        private static (string baseDir, string needle) SplitInput(string input)
        {
            if (string.IsNullOrEmpty(input))
            {
                return (string.Empty, string.Empty);
            }

            if (input.EndsWith(Path.DirectorySeparatorChar) || input.EndsWith(Path.AltDirectorySeparatorChar))
            {
                return (input, string.Empty);
            }

            var dir = Path.GetDirectoryName(input);
            if (string.IsNullOrEmpty(dir))
            {
                return (string.Empty, input);
            }

            var baseDir = dir.EndsWith(Path.DirectorySeparatorChar) || dir.EndsWith(Path.AltDirectorySeparatorChar)
                ? dir
                : dir + Path.DirectorySeparatorChar;
            var needle = input.Length >= baseDir.Length ? input.Substring(baseDir.Length) : string.Empty;
            return (baseDir, needle);
        }

        private static HashSet<string> ParseExtensions(string extensions)
        {
            var result = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            if (string.IsNullOrWhiteSpace(extensions))
            {
                return result;
            }
            var parts = extensions.Split(new[] { ';', ',', '|' }, StringSplitOptions.RemoveEmptyEntries);
            foreach (var part in parts)
            {
                var trimmed = part.Trim();
                if (string.IsNullOrEmpty(trimmed))
                {
                    continue;
                }
                var ext = trimmed.StartsWith(".") ? trimmed : "." + trimmed;
                result.Add(ext.ToLowerInvariant());
            }
            return result;
        }

        private static IEnumerable<PathCandidate> OrderCandidates(List<PathCandidate> items)
        {
            return items
                .OrderByDescending(item => item.StartsWith)
                .ThenBy(item => item.MatchIndex < 0 ? int.MaxValue : item.MatchIndex)
                .ThenBy(item => item.Name, StringComparer.OrdinalIgnoreCase);
        }

        private static bool TryMatch(string name, string needle, out bool startsWith, out int matchIndex)
        {
            if (string.IsNullOrEmpty(needle))
            {
                startsWith = true;
                matchIndex = 0;
                return true;
            }
            startsWith = name.StartsWith(needle, StringComparison.OrdinalIgnoreCase);
            matchIndex = name.IndexOf(needle, StringComparison.OrdinalIgnoreCase);
            return matchIndex >= 0;
        }

        private static IEnumerable<string> SafeEnumerateDirectories(string baseDir)
        {
            var options = new EnumerationOptions
            {
                IgnoreInaccessible = true,
                RecurseSubdirectories = false
            };
            IEnumerable<string> enumerable;
            try
            {
                enumerable = Directory.EnumerateDirectories(baseDir, "*", options);
            }
            catch
            {
                yield break;
            }

            using var enumerator = enumerable.GetEnumerator();
            var errorCount = 0;
            while (true)
            {
                string current = null;
                try
                {
                    if (!enumerator.MoveNext())
                    {
                        yield break;
                    }
                    current = enumerator.Current;
                }
                catch (Exception ex) when (ex is IOException || ex is UnauthorizedAccessException || ex is PathTooLongException)
                {
                    errorCount++;
                    if (errorCount >= 3)
                    {
                        yield break;
                    }
                    continue;
                }
                errorCount = 0;
                if (!string.IsNullOrEmpty(current))
                {
                    yield return current;
                }
            }
        }

        private static IEnumerable<string> SafeEnumerateFiles(string baseDir)
        {
            var options = new EnumerationOptions
            {
                IgnoreInaccessible = true,
                RecurseSubdirectories = false
            };
            IEnumerable<string> enumerable;
            try
            {
                enumerable = Directory.EnumerateFiles(baseDir, "*", options);
            }
            catch
            {
                yield break;
            }

            using var enumerator = enumerable.GetEnumerator();
            var errorCount = 0;
            while (true)
            {
                string current = null;
                try
                {
                    if (!enumerator.MoveNext())
                    {
                        yield break;
                    }
                    current = enumerator.Current;
                }
                catch (Exception ex) when (ex is IOException || ex is UnauthorizedAccessException || ex is PathTooLongException)
                {
                    errorCount++;
                    if (errorCount >= 3)
                    {
                        yield break;
                    }
                    continue;
                }
                errorCount = 0;
                if (!string.IsNullOrEmpty(current))
                {
                    yield return current;
                }
            }
        }

        private static bool CanReadDirectory(string path)
        {
            try
            {
                using var enumerator = Directory.EnumerateFileSystemEntries(path).GetEnumerator();
                return true;
            }
            catch
            {
                return false;
            }
        }

        private static bool CanReadFile(string path)
        {
            try
            {
                using var stream = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                return true;
            }
            catch
            {
                return false;
            }
        }

        private static string GetName(string path)
        {
            var trimmed = path.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            return Path.GetFileName(trimmed);
        }

        private static readonly Dictionary<VideoSizeCondition, string> VideoSizeLabels = new()
        {
            { VideoSizeCondition.FullHD, "1920x1080" },
            { VideoSizeCondition.HD1440, "1440x1080" },
            { VideoSizeCondition.SD, "720x480" },
            { VideoSizeCondition.OneSeg, "320x240" },
        };

        private AutoSelectOptionsView BuildAutoSelectOptionsView()
        {
            var profiles = state.GetProfiles().Select(p => p.Name).Where(n => !string.IsNullOrWhiteSpace(n)).ToList();
            var services = state.GetServiceViews().Select(s => new ServiceOptionView
            {
                ServiceId = s.ServiceId,
                Name = s.Name
            }).ToList();
            var genres = BuildGenreTree();
            var videoSizes = VideoSizeLabels.Select(pair => new VideoSizeOptionView
            {
                Name = pair.Value,
                Value = pair.Key.ToString()
            }).ToList();
            return new AutoSelectOptionsView
            {
                ProfileNames = profiles,
                PriorityList = new List<int> { 1, 2, 3, 4, 5 },
                Genres = genres,
                Services = services,
                VideoSizes = videoSizes
            };
        }

        private static List<GenreNodeView> BuildGenreTree()
        {
            var result = new List<GenreNodeView>();
            foreach (var spaceEntry in SubGenre.GENRE_TABLE.OrderBy(p => p.Key))
            {
                foreach (var mainEntry in spaceEntry.Value.MainGenres.OrderBy(p => p.Key))
                {
                    var main = mainEntry.Value;
                    var mainItem = main.Item;
                    var mainNode = new GenreNodeView
                    {
                        Id = BuildGenreId(mainItem),
                        Space = mainItem.Space,
                        Level1 = mainItem.Level1,
                        Level2 = mainItem.Level2,
                        Name = main.Name,
                        Children = new List<GenreNodeView>()
                    };
                    foreach (var subEntry in main.SubGenres.OrderBy(p => p.Key))
                    {
                        var sub = subEntry.Value;
                        var subItem = sub.Item;
                        mainNode.Children.Add(new GenreNodeView
                        {
                            Id = BuildGenreId(subItem),
                            Space = subItem.Space,
                            Level1 = subItem.Level1,
                            Level2 = subItem.Level2,
                            Name = sub.Name,
                            Children = new List<GenreNodeView>()
                        });
                    }
                    result.Add(mainNode);
                }
            }
            return result;
        }

        private static string BuildGenreId(GenreItem item)
            => $"{item.Space}:{item.Level1}:{item.Level2}";

        private static JsonSerializerOptions CreateRestJsonOptions()
        {
            var options = new JsonSerializerOptions
            {
                PropertyNameCaseInsensitive = true,
                IncludeFields = true
            };
            options.Converters.Add(new JsonStringEnumConverter());
            return options;
        }
    }
}
