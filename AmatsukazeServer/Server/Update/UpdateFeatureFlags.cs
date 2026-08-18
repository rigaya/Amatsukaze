using System;
using System.Collections.Generic;
using System.Linq;

namespace Amatsukaze.Server.Update
{
    // 未公開機能を配布物に含めたまま無効化するためのゲート。
    internal static class UpdateFeatureFlags
    {
        internal const string SelfUpdateEnvironmentVariable = "AMT_ENABLE_SELF_UPDATE";
        private const string EnabledNumericValue = "1";
        private const string EnabledTextValue = "true";

        internal static readonly bool SelfUpdateEnabled = IsEnabled(
            Environment.GetEnvironmentVariable(SelfUpdateEnvironmentVariable));

        internal static IReadOnlyList<UpdateTargetDef> FilterTargets(
            IReadOnlyList<UpdateTargetDef> targets, bool selfUpdateEnabled)
        {
            return selfUpdateEnabled
                ? targets
                : targets.Where(target => !target.IsApplication).ToArray();
        }

        internal static UpdateTargetDef FindTarget(IReadOnlyList<UpdateTargetDef> targets,
            string targetId, bool selfUpdateEnabled)
        {
            return FilterTargets(targets, selfUpdateEnabled).FirstOrDefault(target =>
                string.Equals(target.Id, targetId, StringComparison.OrdinalIgnoreCase));
        }

        private static bool IsEnabled(string value) =>
            string.Equals(value, EnabledNumericValue, StringComparison.Ordinal) ||
            string.Equals(value, EnabledTextValue, StringComparison.OrdinalIgnoreCase);
    }
}
