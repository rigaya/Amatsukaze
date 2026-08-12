using System;
using System.Collections.Generic;

namespace Amatsukaze.Shared
{
    // 更新チェック全体の表示状態
    public sealed class UpdateStatusView
    {
        public bool CheckEnabled { get; set; }
        public bool Supported { get; set; }
        public string? UnsupportedReason { get; set; }
        public string? EnvironmentWarning { get; set; }
        public DateTime? LastCheckedAt { get; set; }
        // 次回の定期チェック予定時刻
        public DateTime? NextCheckAt { get; set; }
        public bool HasUpdate { get; set; }
        // 再起動後に適用する本体更新が保留されているか
        public bool HasPendingSelfUpdate { get; set; }
        // 直前の本体更新結果。updater からの復帰時に読み取り、次の再起動まで保持する
        public SelfUpdateResultView? LastSelfUpdateResult { get; set; }
        // 実行中の更新適用ジョブ。存在しない場合は null
        public string? ActiveApplyJobId { get; set; }
        public List<UpdateItemView> Items { get; set; } = new List<UpdateItemView>();
    }

    // updater から復帰した直後の本体更新結果
    public sealed class SelfUpdateResultView
    {
        public string Status { get; set; } = "";
        public string? Version { get; set; }
        public string? ErrorCode { get; set; }
    }

    // 更新対象ごとの表示状態
    public sealed class UpdateItemView
    {
        public string Id { get; set; } = "";
        public string DisplayName { get; set; } = "";
        public string? InstalledVersion { get; set; }
        public string? LatestVersion { get; set; }
        // UpToDate、UpdateAvailable、Unknown、Unsupported、Disabled のいずれか
        public string State { get; set; } = "";
        // State を決定した機械判定用の理由コード
        public string? StateReason { get; set; }
        // 適用後にサーバーの再起動が必要か
        public bool RequiresRestart { get; set; }
        public long DownloadSizeBytes { get; set; }
        // GitHub のリリースノートページ
        public string? ReleaseUrl { get; set; }
        // 更新アセットの直接ダウンロードURL
        public string? AssetUrl { get; set; }
        // 現在のサーバーがこの対象を適用できるか
        public bool CanApply { get; set; }
        // 適用できない理由コード
        public string? CannotApplyReason { get; set; }
    }

    // 非同期更新ジョブの進捗状態
    public sealed class UpdateJobView
    {
        public string JobId { get; set; } = "";
        public string? TxId { get; set; }
        public string? CurrentTargetId { get; set; }
        public string? CurrentStage { get; set; }
        public long ReceivedBytes { get; set; }
        public long TotalBytes { get; set; }
        public double SpeedBytesPerSec { get; set; }
        public bool Finished { get; set; }
        public bool Succeeded { get; set; }
        public List<string> RecentLogLines { get; set; } = new List<string>();
        // 適用対象ごとの完了結果
        public List<UpdateTargetResultView> TargetResults { get; set; } =
            new List<UpdateTargetResultView>();
    }

    // 更新適用ジョブの対象別結果
    public sealed class UpdateTargetResultView
    {
        public string TargetId { get; set; } = "";
        public bool Succeeded { get; set; }
        public string? ErrorCode { get; set; }
        public string? Message { get; set; }
    }

    // 更新適用リクエスト
    public sealed class UpdateApplyRequest
    {
        public List<string> TargetIds { get; set; } = new List<string>();
    }

    // 更新中止リクエスト
    public sealed class UpdateCancelRequest
    {
        public string JobId { get; set; } = "";
    }
}
