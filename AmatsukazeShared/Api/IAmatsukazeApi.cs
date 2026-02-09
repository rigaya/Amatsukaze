using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Threading.Tasks;

namespace Amatsukaze.Shared
{
    public interface IAmatsukazeApi
    {
        Task<ApiResult<Snapshot>> GetSnapshotAsync();

        Task<ApiResult<SystemSnapshot>> GetSystemAsync();
        Task<ApiResult<ServerEnvironmentView>> GetServerEnvironmentAsync();
        Task<ApiResult<LatestReleaseInfo>> GetLatestReleaseAsync();
        Task<ApiResult<UiStateView>> GetUiStateAsync();
        Task<ApiResult<bool>> EndServerAsync();
        Task<ApiResult<bool>> CancelSleepAsync();

        Task<ApiResult<QueueView>> GetQueueAsync(QueueFilter? filter = null);
        Task<ApiResult<QueueChangesView>> GetQueueChangesAsync(long sinceVersion);
        Task<ApiResult<MessageChangesView>> GetMessageChangesAsync(long sinceId, string? page = null, string? requestId = null, string? levels = null, int max = 50);
        Task<ApiResult<string>> AddQueueAsync(AddQueueRequest req);
        Task<ApiResult<bool>> ChangeQueueAsync(ChangeItemData req);
        Task<ApiResult<bool>> MoveQueueManyAsync(QueueMoveManyRequest req);
        Task<ApiResult<bool>> PauseQueueAsync(PauseRequest req);
        Task<ApiResult<bool>> CancelAddQueueAsync();

        Task<ApiResult<List<LogItemView>>> GetEncodeLogsAsync();
        Task<ApiResult<List<CheckLogItemView>>> GetCheckLogsAsync();
        Task<ApiResult<PagedResult<LogItemView>>> GetEncodeLogsPageAsync(int offset, int limit);
        Task<ApiResult<PagedResult<CheckLogItemView>>> GetCheckLogsPageAsync(int offset, int limit);
        Task<ApiResult<LogFileContent>> GetLogFileAsync(DateTime? encodeStart, DateTime? checkStart);
        Task<ApiResult<string>> GetEncodeCsvAsync();
        Task<ApiResult<string>> GetCheckCsvAsync();

        Task<ApiResult<List<JsonElement>>> GetProfilesAsync();
        Task<ApiResult<ProfileOptions>> GetProfileOptionsAsync();
        Task<ApiResult<bool>> AddProfileAsync(JsonElement profile);
        Task<ApiResult<bool>> UpdateProfileAsync(JsonElement profile, string? newName = null);
        Task<ApiResult<bool>> RemoveProfileAsync(string name);

        Task<ApiResult<List<JsonElement>>> GetAutoSelectsAsync();
        Task<ApiResult<AutoSelectOptionsView>> GetAutoSelectOptionsAsync();
        Task<ApiResult<bool>> AddAutoSelectAsync(JsonElement profile);
        Task<ApiResult<bool>> UpdateAutoSelectAsync(JsonElement profile, string? newName = null);
        Task<ApiResult<bool>> RemoveAutoSelectAsync(string name);

        Task<ApiResult<List<ServiceView>>> GetServicesAsync();
        Task<ApiResult<bool>> UpdateServiceAsync(JsonElement update);
        Task<ApiResult<bool>> UploadLogoAsync(byte[] lgdBytes, int? serviceId, int? imgWidth, int? imgHeight);
        Task<ApiResult<LogoProbeResponse>> ProbeLogoAsync(byte[] lgdBytes);
        Task<ApiResult<List<ServiceSettingView>>> GetServiceSettingsAsync();
        Task<ApiResult<ServiceOptions>> GetServiceOptionsAsync();
        Task<ApiResult<bool>> UpdateServiceLogoPeriodAsync(int serviceId, LogoPeriodUpdateRequest request);
        Task<ApiResult<bool>> UpdateServiceLogoEnabledAsync(int serviceId, LogoEnabledUpdateRequest request);
        Task<ApiResult<bool>> RemoveServiceLogoAsync(int serviceId, LogoFileNameRequest request);
        Task<ApiResult<bool>> AddNoLogoAsync(int serviceId);
        Task<ApiResult<bool>> RemoveNoLogoAsync(int serviceId);
        Task<ApiResult<bool>> RequestLogoRescanAsync();
        Task<ApiResult<List<DrcsView>>> GetDrcsAsync();
        Task<ApiResult<bool>> AddDrcsAsync(JsonElement image);
        Task<ApiResult<bool>> UpdateDrcsMapAsync(string md5, string? mapStr);
        Task<ApiResult<bool>> DeleteDrcsMapAsync(string md5);
        Task<ApiResult<DrcsAppearanceResponse>> GetDrcsAppearanceAsync(string md5);

        Task<ApiResult<JsonElement>> GetSettingAsync();
        Task<ApiResult<bool>> UpdateSettingAsync(JsonElement setting);
        Task<ApiResult<MakeScriptData>> GetMakeScriptAsync();
        Task<ApiResult<List<string>>> GetAddQueueBatFilesAsync();
        Task<ApiResult<List<string>>> GetQueueFinishBatFilesAsync();
        Task<ApiResult<bool>> UpdateMakeScriptAsync(MakeScriptData data);
        Task<ApiResult<MakeScriptPreview>> GetMakeScriptPreviewAsync();
        Task<ApiResult<MakeScriptPreview>> GetMakeScriptPreviewAsync(MakeScriptGenerateRequest request);
        Task<ApiResult<string>> GetMakeScriptFileAsync(MakeScriptGenerateRequest request);
        Task<ApiResult<bool>> UpdateFinishSettingAsync(FinishSetting setting);

        Task<ApiResult<ConsoleTaskView>> GetConsoleTaskAsync(int taskId);
        Task<ApiResult<ConsoleTaskChangesView>> GetConsoleTaskChangesAsync(int taskId, long sinceVersion);

        Task<ApiResult<LogoAnalyzeStatus>> StartLogoAnalyzeAsync(LogoAnalyzeStartRequest req);
        Task<ApiResult<LogoAnalyzeStatus>> GetLogoAnalyzeStatusAsync(string jobId);
        Task<ApiResult<byte[]>> GetLogoAnalyzeImageAsync(string jobId);
        Task<ApiResult<byte[]>> GetLogoAnalyzeFileAsync(string jobId);
        Task<ApiResult<bool>> ApplyLogoAnalyzeAsync(string jobId);
        Task<ApiResult<bool>> DiscardLogoAnalyzeAsync(string jobId);
        Task<ApiResult<LogoPreviewSessionResponse>> CreateLogoPreviewSessionAsync(LogoPreviewSessionRequest req);
        Task<ApiResult<bool>> DeleteLogoPreviewSessionAsync(string sessionId);

        Task<ApiResult<PathSuggestResponse>> GetPathSuggestAsync(PathSuggestRequest req);
    }
}
