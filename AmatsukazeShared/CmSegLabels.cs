using System;
using System.Collections.Generic;
using System.Linq;

namespace Amatsukaze.Shared
{
    // サーバーと正解入力画面で共通の分類定義を使用する。
    public static class CmSegLabels
    {
        public static IReadOnlyList<(string Key, string Id, string Name)> Classes { get; } =
            Array.AsReadOnly(new (string Key, string Id, string Name)[]
            {
                ("1", "main", "本編"), ("2", "sponsor", "提供"),
                ("3", "sponsor_over_main", "提供読上げ(本編映像)"), ("4", "next", "次回予告"),
                ("5", "endcard", "エンドカード"), ("6", "self_promo", "自番組告知"),
                ("7", "other_promo", "番宣(別番組予告)"), ("8", "cm", "CM"),
                ("9", "station", "局ID/ジングル"), ("0", "unknown", "不明/その他")
            });

        public static bool IsValid(string label) =>
            Classes.Any(choice => string.Equals(choice.Id, label, StringComparison.Ordinal));

        public static string InitialLabel(string jlsLabel, bool jlsKeep) => jlsLabel switch
        {
            "L" or "Mix" or "L-Edge(add)" or "N-Edge(add)" => "main",
            "Sponsor(add)" or "Sponsor(cut)" => "sponsor",
            "Trailer(add)" or "Trailer" or "Trailer(cut-cancel)" => "next",
            "Endcard(add)" => "endcard",
            "Trailer(cut)" => "other_promo",
            "CM" or "Nologo" or "Nologo(cut)" or "N-Edge(cut)" or "L-Edge(cut)" => "cm",
            "Border" or "Border15s" or "" => jlsKeep ? "main" : "cm",
            _ => "unknown"
        };
    }
}
