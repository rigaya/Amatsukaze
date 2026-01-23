using System.Collections.Generic;

namespace Amatsukaze.Shared
{
    public static class QueueSettingExtensions
    {
        public static IReadOnlyList<ProcMode> QueueProcModes { get; } = new[]
        {
            ProcMode.Batch,
            ProcMode.Test,
            ProcMode.DrcsCheck,
            ProcMode.CMCheck
        };

        public static IReadOnlyList<int> PriorityList { get; } = new[] { 1, 2, 3, 4, 5 };

        public static string GetProcModeDisplay(ProcMode mode)
        {
            switch (mode)
            {
                case ProcMode.Batch:
                    return "通常";
                case ProcMode.Test:
                    return "テスト";
                case ProcMode.DrcsCheck:
                    return "DRCSチェック";
                case ProcMode.CMCheck:
                    return "CM解析";
                case ProcMode.AutoBatch:
                    return "自動追加";
                default:
                    return "不明モード";
            }
        }
    }
}
