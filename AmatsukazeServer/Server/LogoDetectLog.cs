using System;
using System.IO;

namespace Amatsukaze.Server
{
    internal static class LogoDetectLog
    {
        public static string DescribeAutoDetectStage(int stage)
        {
            return stage switch
            {
                1 => "初期フレーム走査",
                2 => "仮推定とFrameGate準備",
                3 => "最終推定と矩形確定",
                4 => "完了",
                _ => "待機中"
            };
        }

        public static string DescribeLogoGenPhase(float progress)
        {
            if (progress >= 99.9f)
            {
                return "完了";
            }
            if (progress >= 75.0f)
            {
                return "ロゴ再構築(2回目)";
            }
            if (progress >= 50.0f)
            {
                return "ロゴ再構築(1回目)";
            }
            return "初期ロゴ生成";
        }
    }

    internal sealed class LogoAutoDetectProgressLogger
    {
        private readonly string prefix;
        private readonly string fileName;

        private int lastStage = -1;
        private int lastOverallBucket = -1;
        private int lastReadBucket = -1;
        private DateTime lastLogAtUtc = DateTime.MinValue;

        public LogoAutoDetectProgressLogger(string prefix, string filePath)
        {
            this.prefix = prefix;
            fileName = Path.GetFileName(filePath);
        }

        public void Report(int stage, float stageProgress, float progress, int nread, int total)
        {
            var stageName = LogoDetectLog.DescribeAutoDetectStage(stage);
            var overallPercent = ClampPercent(progress * 100.0f);
            var stagePercent = ClampPercent(stageProgress * 100.0f);
            var readBucket = nread / 2000;
            var nowUtc = DateTime.UtcNow;

            if (stage != lastStage)
            {
                Util.AddLog($"{prefix} phase開始: {stageName} overall={overallPercent:F1}% stage={stagePercent:F1}% read={nread}/{total} file={fileName}", null);
                lastStage = stage;
                lastOverallBucket = overallPercent == 100.0f ? 10 : (int)(overallPercent / 10.0f);
                lastReadBucket = readBucket;
                lastLogAtUtc = nowUtc;
                return;
            }

            var overallBucket = overallPercent == 100.0f ? 10 : (int)(overallPercent / 10.0f);
            var shouldLog =
                overallBucket > lastOverallBucket ||
                readBucket > lastReadBucket + 1 ||
                (nowUtc - lastLogAtUtc) >= TimeSpan.FromSeconds(10);

            if (!shouldLog)
            {
                return;
            }

            Util.AddLog($"{prefix} phase進捗: {stageName} overall={overallPercent:F1}% stage={stagePercent:F1}% read={nread}/{total} file={fileName}", null);
            lastOverallBucket = overallBucket;
            lastReadBucket = readBucket;
            lastLogAtUtc = nowUtc;
        }

        private static float ClampPercent(float value)
        {
            return Math.Max(0.0f, Math.Min(100.0f, value));
        }
    }
}
