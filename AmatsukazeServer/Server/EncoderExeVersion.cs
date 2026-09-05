using System;
using System.Text.RegularExpressions;

namespace Amatsukaze.Server
{
    /// <summary>
    /// エンコーダー実行ファイルの版数表記を比較可能な4要素へ正規化する。
    /// FileVersionはエンコーダーごとに独自の接尾辞を含むため、単純なVersion.Parseは使用しない。
    /// </summary>
    internal static class EncoderExeVersion
    {
        private static readonly Regex X264FileVersionRegex = new Regex(
            @"^\s*\d+\.\d+\.(?<revision>\d+)",
            RegexOptions.Compiled | RegexOptions.CultureInvariant);

        private static readonly Regex X265FileVersionRegex = new Regex(
            @"^\s*(?<major>\d+)\.(?<minor>\d+)\+(?<revision>\d+)",
            RegexOptions.Compiled | RegexOptions.CultureInvariant);

        private static readonly Regex StandardVersionRegex = new Regex(
            @"^\s*(?<major>\d+)(?:\.(?<minor>\d+))?(?:\.(?<build>\d+))?(?:\.(?<revision>\d+))?\s*$",
            RegexOptions.Compiled | RegexOptions.CultureInvariant);

        private static readonly Regex X264FilenameRegex = new Regex(
            @"^x264_(?:rev)?(?<revision>\d+)(?=_|$)",
            RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);

        private static readonly Regex X262FilenameRegex = new Regex(
            @"^x262_(?:rev)?(?<revision>\d+)(?=_|$)",
            RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);

        private static readonly Regex X265FilenameRegex = new Regex(
            @"^x265_(?<major>\d+)\.(?<minor>\d+)(?:\+(?<revision>\d+))?(?=_|$)",
            RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);

        internal static Version ParseFileVersion(string value, EncoderType type)
        {
            Match match;
            switch (type)
            {
                case EncoderType.x264:
                case EncoderType.x262:
                    // 実例: 0.165.3223M 0480cb0 / 0.165.3349 72a1323
                    match = X264FileVersionRegex.Match(value ?? string.Empty);
                    if (match.Success && TryGetInt(match, "revision", out var x26xRevision))
                    {
                        return new Version(x26xRevision, 0, 0, 0);
                    }
                    break;
                case EncoderType.x265:
                    // 実例: 4.3+22-98eec4057
                    match = X265FileVersionRegex.Match(value ?? string.Empty);
                    if (match.Success
                        && TryGetInt(match, "major", out var x265Major)
                        && TryGetInt(match, "minor", out var x265Minor)
                        && TryGetInt(match, "revision", out var x265Revision))
                    {
                        return new Version(x265Major, x265Minor, 0, x265Revision);
                    }
                    break;
            }

            // 純粋な数値版数は不足要素を0で補い、常に4要素にする。
            match = StandardVersionRegex.Match(value ?? string.Empty);
            if (!match.Success || !TryGetInt(match, "major", out var major))
            {
                return null;
            }
            return new Version(
                major,
                GetOptionalInt(match, "minor"),
                GetOptionalInt(match, "build"),
                GetOptionalInt(match, "revision"));
        }

        internal static Version ParseFilename(string filename, EncoderType type)
        {
            Match match;
            switch (type)
            {
                case EncoderType.x264:
                    match = X264FilenameRegex.Match(filename ?? string.Empty);
                    return match.Success && TryGetInt(match, "revision", out var x264Revision)
                        ? new Version(x264Revision, 0, 0, 0) : null;
                case EncoderType.x262:
                    match = X262FilenameRegex.Match(filename ?? string.Empty);
                    return match.Success && TryGetInt(match, "revision", out var x262Revision)
                        ? new Version(x262Revision, 0, 0, 0) : null;
                case EncoderType.x265:
                    match = X265FilenameRegex.Match(filename ?? string.Empty);
                    return match.Success
                        && TryGetInt(match, "major", out var major)
                        && TryGetInt(match, "minor", out var minor)
                        ? new Version(major, minor, 0, GetOptionalInt(match, "revision")) : null;
                case EncoderType.SVTAV1:
                    return SvtAv1Version.TryParseFilename(filename, out var version) ? version : null;
                default:
                    return null;
            }
        }

        private static bool TryGetInt(Match match, string groupName, out int value)
        {
            return int.TryParse(match.Groups[groupName].Value, out value);
        }

        private static int GetOptionalInt(Match match, string groupName)
        {
            return match.Groups[groupName].Success
                && int.TryParse(match.Groups[groupName].Value, out var value) ? value : 0;
        }
    }
}
