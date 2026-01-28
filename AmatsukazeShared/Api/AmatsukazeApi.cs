using System;
using System.Collections.Generic;
using System.Globalization;
using System.Net.Http;
using System.Net.Http.Json;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;

namespace Amatsukaze.Shared
{
    public sealed class AmatsukazeApi : IAmatsukazeApi
    {
        private readonly HttpClient http;
        private readonly JsonSerializerOptions jsonOptions;

        public AmatsukazeApi(HttpClient http, JsonSerializerOptions? jsonOptions = null)
        {
            this.http = http ?? throw new ArgumentNullException(nameof(http));
            this.jsonOptions = jsonOptions ?? new JsonSerializerOptions
            {
                PropertyNameCaseInsensitive = true
            };
            this.jsonOptions.Converters.Add(new JsonStringEnumConverter());
        }

        public Task<ApiResult<Snapshot>> GetSnapshotAsync()
            => GetJsonAsync<Snapshot>("/api/snapshot");

        public Task<ApiResult<SystemSnapshot>> GetSystemAsync()
            => GetJsonAsync<SystemSnapshot>("/api/system");

        public Task<ApiResult<LatestReleaseInfo>> GetLatestReleaseAsync()
            => GetJsonAsync<LatestReleaseInfo>("/api/info/latest-release");

        public Task<ApiResult<UiStateView>> GetUiStateAsync()
            => GetJsonAsync<UiStateView>("/api/ui-state");

        public Task<ApiResult<bool>> EndServerAsync()
            => PostJsonAsync("/api/system/end", new { }, _ => true);

        public Task<ApiResult<bool>> CancelSleepAsync()
            => PostJsonAsync("/api/system/cancel-sleep", new { }, _ => true);

        public Task<ApiResult<QueueView>> GetQueueAsync(QueueFilter? filter = null)
        {
            var query = new List<string>();
            if (filter != null)
            {
                if (filter.States != null)
                {
                    foreach (var state in filter.States)
                    {
                        if (!string.IsNullOrWhiteSpace(state))
                        {
                            query.Add($"state={Uri.EscapeDataString(state)}");
                        }
                    }
                }
                if (!string.IsNullOrWhiteSpace(filter.Search))
                {
                    query.Add($"search={Uri.EscapeDataString(filter.Search)}");
                }
                if (filter.SearchTargets != null && filter.SearchTargets.Count > 0)
                {
                    query.Add($"searchTargets={Uri.EscapeDataString(string.Join(",", filter.SearchTargets))}");
                }
                if (filter.DateFrom.HasValue)
                {
                    query.Add($"dateFrom={Uri.EscapeDataString(filter.DateFrom.Value.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture))}");
                }
                if (filter.DateTo.HasValue)
                {
                    query.Add($"dateTo={Uri.EscapeDataString(filter.DateTo.Value.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture))}");
                }
                if (filter.HideOneSeg)
                {
                    query.Add("hideOneSeg=true");
                }
            }
            var url = "/api/queue" + (query.Count > 0 ? "?" + string.Join("&", query) : "");
            return GetJsonAsync<QueueView>(url);
        }

        public Task<ApiResult<QueueChangesView>> GetQueueChangesAsync(long sinceVersion)
            => GetJsonAsync<QueueChangesView>($"/api/queue/changes?since={sinceVersion}");

        public Task<ApiResult<MessageChangesView>> GetMessageChangesAsync(long sinceId, string? page = null, string? requestId = null, string? levels = null, int max = 50)
        {
            var query = new List<string> { $"since={sinceId}" };
            if (!string.IsNullOrEmpty(page))
            {
                query.Add($"page={Uri.EscapeDataString(page)}");
            }
            if (!string.IsNullOrEmpty(requestId))
            {
                query.Add($"requestId={Uri.EscapeDataString(requestId)}");
            }
            if (!string.IsNullOrEmpty(levels))
            {
                query.Add($"levels={Uri.EscapeDataString(levels)}");
            }
            if (max > 0)
            {
                query.Add($"max={max}");
            }
            var url = "/api/messages/changes" + (query.Count > 0 ? "?" + string.Join("&", query) : "");
            return GetJsonAsync<MessageChangesView>(url);
        }

        public Task<ApiResult<string>> AddQueueAsync(AddQueueRequest req)
            => PostJsonAsync("/api/queue/add", req, result => result.GetProperty("requestId").GetString() ?? "");

        public Task<ApiResult<bool>> ChangeQueueAsync(ChangeItemData req)
            => PostJsonAsync("/api/queue/change", req, _ => true);

        public Task<ApiResult<bool>> PauseQueueAsync(PauseRequest req)
            => PostJsonAsync("/api/queue/pause", req, _ => true);

        public Task<ApiResult<bool>> CancelAddQueueAsync()
            => PostJsonAsync("/api/queue/cancel-add", new { }, _ => true);

        public Task<ApiResult<List<LogItemView>>> GetEncodeLogsAsync()
            => GetJsonAsync<List<LogItemView>>("/api/logs/encode");

        public Task<ApiResult<List<CheckLogItemView>>> GetCheckLogsAsync()
            => GetJsonAsync<List<CheckLogItemView>>("/api/logs/check");

        public Task<ApiResult<PagedResult<LogItemView>>> GetEncodeLogsPageAsync(int offset, int limit)
            => GetJsonAsync<PagedResult<LogItemView>>($"/api/logs/encode/page?offset={offset}&limit={limit}");

        public Task<ApiResult<PagedResult<CheckLogItemView>>> GetCheckLogsPageAsync(int offset, int limit)
            => GetJsonAsync<PagedResult<CheckLogItemView>>($"/api/logs/check/page?offset={offset}&limit={limit}");

        public Task<ApiResult<LogFileContent>> GetLogFileAsync(DateTime? encodeStart, DateTime? checkStart)
        {
            var query = new List<string>();
            if (encodeStart.HasValue)
            {
                query.Add($"encodeStart={Uri.EscapeDataString(encodeStart.Value.ToString("yyyy-MM-ddTHH:mm:ss.fff", CultureInfo.InvariantCulture))}");
            }
            if (checkStart.HasValue)
            {
                query.Add($"checkStart={Uri.EscapeDataString(checkStart.Value.ToString("yyyy-MM-ddTHH:mm:ss.fff", CultureInfo.InvariantCulture))}");
            }
            var url = "/api/logs/file" + (query.Count > 0 ? "?" + string.Join("&", query) : "");
            return GetJsonAsync<LogFileContent>(url);
        }

        public Task<ApiResult<string>> GetEncodeCsvAsync()
            => GetTextAsync("/api/logs/encode.csv");

        public Task<ApiResult<string>> GetCheckCsvAsync()
            => GetTextAsync("/api/logs/check.csv");

        public Task<ApiResult<List<JsonElement>>> GetProfilesAsync()
            => GetJsonAsync<List<JsonElement>>("/api/profiles");

        public Task<ApiResult<ProfileOptions>> GetProfileOptionsAsync()
            => GetJsonAsync<ProfileOptions>("/api/profile-options");

        public Task<ApiResult<bool>> AddProfileAsync(JsonElement profile)
            => PostJsonAsync("/api/profiles", profile, _ => true);

        public Task<ApiResult<bool>> UpdateProfileAsync(JsonElement profile, string? newName = null)
        {
            var name = !string.IsNullOrWhiteSpace(newName) ? newName : (profile.TryGetProperty("name", out var n) ? n.GetString() : "");
            if (string.IsNullOrEmpty(name))
            {
                return Task.FromResult(ApiResult<bool>.Fail(0, "Profile name is missing"));
            }
            return PutJsonAsync($"/api/profiles/{Uri.EscapeDataString(name)}", profile, _ => true);
        }

        public Task<ApiResult<bool>> RemoveProfileAsync(string name)
            => DeleteAsync($"/api/profiles/{Uri.EscapeDataString(name)}");

        public Task<ApiResult<List<JsonElement>>> GetAutoSelectsAsync()
            => GetJsonAsync<List<JsonElement>>("/api/autoselect");

        public Task<ApiResult<AutoSelectOptionsView>> GetAutoSelectOptionsAsync()
            => GetJsonAsync<AutoSelectOptionsView>("/api/autoselect/options");

        public Task<ApiResult<bool>> AddAutoSelectAsync(JsonElement profile)
            => PostJsonAsync("/api/autoselect", profile, _ => true);

        public Task<ApiResult<bool>> UpdateAutoSelectAsync(JsonElement profile, string? newName = null)
        {
            var name = !string.IsNullOrWhiteSpace(newName) ? newName : (profile.TryGetProperty("name", out var n) ? n.GetString() : "");
            if (string.IsNullOrEmpty(name))
            {
                return Task.FromResult(ApiResult<bool>.Fail(0, "AutoSelect name is missing"));
            }
            return PutJsonAsync($"/api/autoselect/{Uri.EscapeDataString(name)}", profile, _ => true);
        }

        public Task<ApiResult<bool>> RemoveAutoSelectAsync(string name)
            => DeleteAsync($"/api/autoselect/{Uri.EscapeDataString(name)}");

        public Task<ApiResult<List<ServiceView>>> GetServicesAsync()
            => GetJsonAsync<List<ServiceView>>("/api/services");

        public Task<ApiResult<bool>> UpdateServiceAsync(JsonElement update)
            => PostJsonAsync("/api/services/update", update, _ => true);

        public async Task<ApiResult<bool>> UploadLogoAsync(byte[] lgdBytes, int? serviceId, int? imgWidth, int? imgHeight)
        {
            try
            {
                using var content = new MultipartFormDataContent();
                if (serviceId.HasValue)
                {
                    content.Add(new StringContent(serviceId.Value.ToString(CultureInfo.InvariantCulture)), "serviceId");
                }
                if (imgWidth.HasValue)
                {
                    content.Add(new StringContent(imgWidth.Value.ToString(CultureInfo.InvariantCulture)), "imgw");
                }
                if (imgHeight.HasValue)
                {
                    content.Add(new StringContent(imgHeight.Value.ToString(CultureInfo.InvariantCulture)), "imgh");
                }
                content.Add(new ByteArrayContent(lgdBytes), "image", "logo.lgd");
                using var res = await http.PostAsync("/api/services/logo", content);
                if (!res.IsSuccessStatusCode)
                {
                    return ApiResult<bool>.Fail((int)res.StatusCode, await res.Content.ReadAsStringAsync());
                }
                return ApiResult<bool>.Success(true, (int)res.StatusCode);
            }
            catch (Exception ex)
            {
                return ApiResult<bool>.Fail(0, ex.Message);
            }
        }

        public async Task<ApiResult<LogoProbeResponse>> ProbeLogoAsync(byte[] lgdBytes)
        {
            try
            {
                using var content = new MultipartFormDataContent();
                content.Add(new ByteArrayContent(lgdBytes), "image", "logo.lgd");
                using var res = await http.PostAsync("/api/services/logo/probe", content);
                if (!res.IsSuccessStatusCode)
                {
                    return ApiResult<LogoProbeResponse>.Fail((int)res.StatusCode, await res.Content.ReadAsStringAsync());
                }
                var body = await res.Content.ReadAsStringAsync();
                var data = JsonSerializer.Deserialize<LogoProbeResponse>(body, jsonOptions);
                if (data == null)
                {
                    return ApiResult<LogoProbeResponse>.Fail((int)res.StatusCode, "Invalid response");
                }
                return ApiResult<LogoProbeResponse>.Success(data, (int)res.StatusCode);
            }
            catch (Exception ex)
            {
                return ApiResult<LogoProbeResponse>.Fail(0, ex.Message);
            }
        }

        public Task<ApiResult<List<ServiceSettingView>>> GetServiceSettingsAsync()
            => GetJsonAsync<List<ServiceSettingView>>("/api/service-settings");

        public Task<ApiResult<ServiceOptions>> GetServiceOptionsAsync()
            => GetJsonAsync<ServiceOptions>("/api/service-options");

        public Task<ApiResult<bool>> UpdateServiceLogoPeriodAsync(int serviceId, LogoPeriodUpdateRequest request)
            => PutJsonAsync($"/api/service-settings/{serviceId}/logos/period", request, _ => true);

        public Task<ApiResult<bool>> UpdateServiceLogoEnabledAsync(int serviceId, LogoEnabledUpdateRequest request)
            => PutJsonAsync($"/api/service-settings/{serviceId}/logos/enabled", request, _ => true);

        public Task<ApiResult<bool>> RemoveServiceLogoAsync(int serviceId, LogoFileNameRequest request)
            => DeleteJsonAsync($"/api/service-settings/{serviceId}/logos", request);

        public Task<ApiResult<bool>> AddNoLogoAsync(int serviceId)
            => PostJsonAsync($"/api/service-settings/{serviceId}/logos/no-logo", new { }, _ => true);

        public Task<ApiResult<bool>> RemoveNoLogoAsync(int serviceId)
            => DeleteAsync($"/api/service-settings/{serviceId}/logos/no-logo");

        public Task<ApiResult<bool>> RequestLogoRescanAsync()
            => PostJsonAsync("/api/services/logo/rescan", new { }, _ => true);

        public Task<ApiResult<List<DrcsView>>> GetDrcsAsync()
            => GetJsonAsync<List<DrcsView>>("/api/drcs");

        public Task<ApiResult<bool>> AddDrcsAsync(JsonElement image)
            => PostJsonAsync("/api/drcs", image, _ => true);

        public Task<ApiResult<bool>> UpdateDrcsMapAsync(string md5, string? mapStr)
            => PutJsonAsync("/api/drcs/map", new DrcsMapUpdateRequest { Md5 = md5, MapStr = mapStr }, _ => true);

        public Task<ApiResult<bool>> DeleteDrcsMapAsync(string md5)
            => DeleteAsync($"/api/drcs/map/{Uri.EscapeDataString(md5)}");

        public Task<ApiResult<DrcsAppearanceResponse>> GetDrcsAppearanceAsync(string md5)
            => GetJsonAsync<DrcsAppearanceResponse>($"/api/drcs/appearance/{Uri.EscapeDataString(md5)}");

        public Task<ApiResult<JsonElement>> GetSettingAsync()
            => GetJsonAsync<JsonElement>("/api/settings");

        public Task<ApiResult<bool>> UpdateSettingAsync(JsonElement setting)
            => PutJsonAsync("/api/settings", setting, _ => true);

        public Task<ApiResult<MakeScriptData>> GetMakeScriptAsync()
            => GetJsonAsync<MakeScriptData>("/api/makescript");

        public Task<ApiResult<List<string>>> GetAddQueueBatFilesAsync()
            => GetJsonAsync<List<string>>("/api/batfiles/addqueue");

        public Task<ApiResult<List<string>>> GetQueueFinishBatFilesAsync()
            => GetJsonAsync<List<string>>("/api/batfiles/queuefinish");

        public Task<ApiResult<bool>> UpdateMakeScriptAsync(MakeScriptData data)
            => PutJsonAsync("/api/makescript", data, _ => true);

        public Task<ApiResult<MakeScriptPreview>> GetMakeScriptPreviewAsync()
            => GetJsonAsync<MakeScriptPreview>("/api/makescript/preview");

        public Task<ApiResult<MakeScriptPreview>> GetMakeScriptPreviewAsync(MakeScriptGenerateRequest request)
            => PostJsonAsync("/api/makescript/preview", request,
                element => element.Deserialize<MakeScriptPreview>(jsonOptions) ?? new MakeScriptPreview());

        public async Task<ApiResult<string>> GetMakeScriptFileAsync(MakeScriptGenerateRequest request)
        {
            try
            {
                using var res = await http.PostAsJsonAsync("/api/makescript/file", request, jsonOptions);
                if (!res.IsSuccessStatusCode)
                {
                    return ApiResult<string>.Fail((int)res.StatusCode, await res.Content.ReadAsStringAsync());
                }
                return ApiResult<string>.Success(await res.Content.ReadAsStringAsync(), (int)res.StatusCode);
            }
            catch (Exception ex)
            {
                return ApiResult<string>.Fail(0, ex.Message);
            }
        }

        public Task<ApiResult<bool>> UpdateFinishSettingAsync(FinishSetting setting)
            => PutJsonAsync("/api/finish-setting", setting, _ => true);

        public Task<ApiResult<ConsoleView>> GetConsoleAsync()
            => GetJsonAsync<ConsoleView>("/api/console");

        public Task<ApiResult<LogoAnalyzeStatus>> StartLogoAnalyzeAsync(LogoAnalyzeStartRequest req)
            => PostJsonAsync("/api/logo/analyze", req, element => element.Deserialize<LogoAnalyzeStatus>(jsonOptions) ?? new LogoAnalyzeStatus());

        public Task<ApiResult<LogoAnalyzeStatus>> GetLogoAnalyzeStatusAsync(string jobId)
            => GetJsonAsync<LogoAnalyzeStatus>($"/api/logo/analyze/{Uri.EscapeDataString(jobId)}");

        public Task<ApiResult<byte[]>> GetLogoAnalyzeImageAsync(string jobId)
            => GetBytesAsync($"/api/logo/analyze/{Uri.EscapeDataString(jobId)}/image");

        public Task<ApiResult<byte[]>> GetLogoAnalyzeFileAsync(string jobId)
            => GetBytesAsync($"/api/logo/analyze/{Uri.EscapeDataString(jobId)}/file");

        public async Task<ApiResult<bool>> ApplyLogoAnalyzeAsync(string jobId)
        {
            try
            {
                var res = await http.PostAsync($"/api/logo/analyze/{Uri.EscapeDataString(jobId)}/apply", null);
                if (res.IsSuccessStatusCode)
                {
                    return ApiResult<bool>.Success(true, (int)res.StatusCode);
                }
                return ApiResult<bool>.Fail((int)res.StatusCode, await res.Content.ReadAsStringAsync());
            }
            catch (Exception ex)
            {
                return ApiResult<bool>.Fail(0, ex.Message);
            }
        }

        public async Task<ApiResult<bool>> DiscardLogoAnalyzeAsync(string jobId)
        {
            try
            {
                var res = await http.PostAsync($"/api/logo/analyze/{Uri.EscapeDataString(jobId)}/discard", null);
                if (res.IsSuccessStatusCode)
                {
                    return ApiResult<bool>.Success(true, (int)res.StatusCode);
                }
                return ApiResult<bool>.Fail((int)res.StatusCode, await res.Content.ReadAsStringAsync());
            }
            catch (Exception ex)
            {
                return ApiResult<bool>.Fail(0, ex.Message);
            }
        }

        public Task<ApiResult<LogoPreviewSessionResponse>> CreateLogoPreviewSessionAsync(LogoPreviewSessionRequest req)
            => PostJsonAsync("/api/logo/preview/sessions", req, element => element.Deserialize<LogoPreviewSessionResponse>(jsonOptions) ?? new LogoPreviewSessionResponse());

        public async Task<ApiResult<bool>> DeleteLogoPreviewSessionAsync(string sessionId)
        {
            try
            {
                var response = await http.DeleteAsync($"/api/logo/preview/sessions/{Uri.EscapeDataString(sessionId)}");
                if (response.IsSuccessStatusCode)
                {
                    return ApiResult<bool>.Success(true, (int)response.StatusCode);
                }
                return ApiResult<bool>.Fail((int)response.StatusCode, await response.Content.ReadAsStringAsync());
            }
            catch (Exception ex)
            {
                return ApiResult<bool>.Fail(0, ex.Message);
            }
        }

        public Task<ApiResult<PathSuggestResponse>> GetPathSuggestAsync(PathSuggestRequest req)
        {
            var query = new List<string>();
            if (!string.IsNullOrWhiteSpace(req.Input))
            {
                query.Add($"input={Uri.EscapeDataString(req.Input)}");
            }
            if (!string.IsNullOrWhiteSpace(req.Extensions))
            {
                query.Add($"ext={Uri.EscapeDataString(req.Extensions)}");
            }
            if (req.MaxDirs > 0)
            {
                query.Add($"maxDirs={req.MaxDirs}");
            }
            if (req.MaxFiles > 0)
            {
                query.Add($"maxFiles={req.MaxFiles}");
            }
            if (req.DirOffset > 0)
            {
                query.Add($"dirOffset={req.DirOffset}");
            }
            if (req.FileOffset > 0)
            {
                query.Add($"fileOffset={req.FileOffset}");
            }
            if (!req.AllowFiles)
            {
                query.Add("allowFiles=false");
            }
            if (!req.AllowDirs)
            {
                query.Add("allowDirs=false");
            }
            if (req.CheckAccess)
            {
                query.Add("checkAccess=true");
            }
            var url = "/api/path/suggest" + (query.Count > 0 ? "?" + string.Join("&", query) : "");
            return GetJsonAsync<PathSuggestResponse>(url);
        }

        private async Task<ApiResult<T>> GetJsonAsync<T>(string url)
        {
            try
            {
                using var res = await http.GetAsync(url);
                if (!res.IsSuccessStatusCode)
                {
                    return ApiResult<T>.Fail((int)res.StatusCode, await res.Content.ReadAsStringAsync());
                }
                var data = await res.Content.ReadFromJsonAsync<T>(jsonOptions);
                if (data == null)
                {
                    return ApiResult<T>.Fail((int)res.StatusCode, "Empty response");
                }
                return ApiResult<T>.Success(data, (int)res.StatusCode);
            }
            catch (Exception ex)
            {
                return ApiResult<T>.Fail(0, ex.Message);
            }
        }

        private async Task<ApiResult<string>> GetTextAsync(string url)
        {
            try
            {
                using var res = await http.GetAsync(url);
                if (!res.IsSuccessStatusCode)
                {
                    return ApiResult<string>.Fail((int)res.StatusCode, await res.Content.ReadAsStringAsync());
                }
                var text = await res.Content.ReadAsStringAsync();
                return ApiResult<string>.Success(text, (int)res.StatusCode);
            }
            catch (Exception ex)
            {
                return ApiResult<string>.Fail(0, ex.Message);
            }
        }

        private async Task<ApiResult<byte[]>> GetBytesAsync(string url)
        {
            try
            {
                using var res = await http.GetAsync(url);
                if (!res.IsSuccessStatusCode)
                {
                    return ApiResult<byte[]>.Fail((int)res.StatusCode, await res.Content.ReadAsStringAsync());
                }
                var bytes = await res.Content.ReadAsByteArrayAsync();
                return ApiResult<byte[]>.Success(bytes, (int)res.StatusCode);
            }
            catch (Exception ex)
            {
                return ApiResult<byte[]>.Fail(0, ex.Message);
            }
        }

        private async Task<ApiResult<T>> PostJsonAsync<T>(string url, object payload, Func<JsonElement, T> map)
        {
            try
            {
                using var res = await http.PostAsJsonAsync(url, payload, jsonOptions);
                if (!res.IsSuccessStatusCode)
                {
                    return ApiResult<T>.Fail((int)res.StatusCode, await res.Content.ReadAsStringAsync());
                }
                var element = await res.Content.ReadFromJsonAsync<JsonElement>(jsonOptions);
                return ApiResult<T>.Success(map(element), (int)res.StatusCode);
            }
            catch (Exception ex)
            {
                return ApiResult<T>.Fail(0, ex.Message);
            }
        }

        private Task<ApiResult<T>> PostJsonAsync<T>(string url, object payload, Func<JsonElement, T> map, CancellationToken cancellationToken)
            => PostJsonAsync(url, payload, map);

        private async Task<ApiResult<T>> PutJsonAsync<T>(string url, object payload, Func<JsonElement, T> map)
        {
            try
            {
                using var res = await http.PutAsJsonAsync(url, payload, jsonOptions);
                if (!res.IsSuccessStatusCode)
                {
                    return ApiResult<T>.Fail((int)res.StatusCode, await res.Content.ReadAsStringAsync());
                }
                var element = await res.Content.ReadFromJsonAsync<JsonElement>(jsonOptions);
                return ApiResult<T>.Success(map(element), (int)res.StatusCode);
            }
            catch (Exception ex)
            {
                return ApiResult<T>.Fail(0, ex.Message);
            }
        }

        private async Task<ApiResult<bool>> DeleteAsync(string url)
        {
            try
            {
                using var res = await http.DeleteAsync(url);
                if (!res.IsSuccessStatusCode)
                {
                    return ApiResult<bool>.Fail((int)res.StatusCode, await res.Content.ReadAsStringAsync());
                }
                return ApiResult<bool>.Success(true, (int)res.StatusCode);
            }
            catch (Exception ex)
            {
                return ApiResult<bool>.Fail(0, ex.Message);
            }
        }

        private async Task<ApiResult<bool>> DeleteJsonAsync(string url, object payload)
        {
            try
            {
                using var request = new HttpRequestMessage(HttpMethod.Delete, url)
                {
                    Content = JsonContent.Create(payload, options: jsonOptions)
                };
                using var res = await http.SendAsync(request);
                if (!res.IsSuccessStatusCode)
                {
                    return ApiResult<bool>.Fail((int)res.StatusCode, await res.Content.ReadAsStringAsync());
                }
                return ApiResult<bool>.Success(true, (int)res.StatusCode);
            }
            catch (Exception ex)
            {
                return ApiResult<bool>.Fail(0, ex.Message);
            }
        }
    }
}
