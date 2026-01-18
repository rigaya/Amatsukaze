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
        Task<ApiResult<bool>> EndServerAsync();
        Task<ApiResult<bool>> CancelSleepAsync();

        Task<ApiResult<QueueView>> GetQueueAsync(QueueFilter? filter = null);
        Task<ApiResult<string>> AddQueueAsync(AddQueueRequest req);
        Task<ApiResult<bool>> ChangeQueueAsync(ChangeItemData req);
        Task<ApiResult<bool>> PauseQueueAsync(PauseRequest req);
        Task<ApiResult<bool>> CancelAddQueueAsync();

        Task<ApiResult<List<LogItemView>>> GetEncodeLogsAsync();
        Task<ApiResult<List<CheckLogItemView>>> GetCheckLogsAsync();
        Task<ApiResult<LogFileContent>> GetLogFileAsync(DateTime? encodeStart, DateTime? checkStart);
        Task<ApiResult<string>> GetEncodeCsvAsync();
        Task<ApiResult<string>> GetCheckCsvAsync();

        Task<ApiResult<List<JsonElement>>> GetProfilesAsync();
        Task<ApiResult<bool>> AddProfileAsync(JsonElement profile);
        Task<ApiResult<bool>> UpdateProfileAsync(JsonElement profile, string? newName = null);
        Task<ApiResult<bool>> RemoveProfileAsync(string name);

        Task<ApiResult<List<JsonElement>>> GetAutoSelectsAsync();
        Task<ApiResult<bool>> AddAutoSelectAsync(JsonElement profile);
        Task<ApiResult<bool>> UpdateAutoSelectAsync(JsonElement profile, string? newName = null);
        Task<ApiResult<bool>> RemoveAutoSelectAsync(string name);

        Task<ApiResult<List<ServiceView>>> GetServicesAsync();
        Task<ApiResult<bool>> UpdateServiceAsync(JsonElement update);
        Task<ApiResult<bool>> UploadLogoAsync(int serviceId, int logoIdx, byte[] pngBytes);
        Task<ApiResult<List<DrcsView>>> GetDrcsAsync();
        Task<ApiResult<bool>> AddDrcsAsync(JsonElement image);

        Task<ApiResult<JsonElement>> GetSettingAsync();
        Task<ApiResult<bool>> UpdateSettingAsync(JsonElement setting);
        Task<ApiResult<MakeScriptData>> GetMakeScriptAsync();
        Task<ApiResult<bool>> UpdateMakeScriptAsync(MakeScriptData data);
        Task<ApiResult<MakeScriptPreview>> GetMakeScriptPreviewAsync();
        Task<ApiResult<bool>> UpdateFinishSettingAsync(FinishSetting setting);

        Task<ApiResult<ConsoleView>> GetConsoleAsync();

        Task<ApiResult<LogoAnalyzeStatus>> StartLogoAnalyzeAsync(LogoAnalyzeRequest req);
        Task<ApiResult<LogoAnalyzeStatus>> GetLogoAnalyzeStatusAsync(string jobId);
        Task<ApiResult<byte[]>> GetLogoAnalyzeImageAsync(string jobId);
        Task<ApiResult<byte[]>> GetLogoAnalyzeFileAsync(string jobId);
    }
}
