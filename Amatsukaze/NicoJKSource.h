#pragma once
#include <optional>
#include <regex>
#include <cmath>
#include <sstream>

// 取得元指定と番組開始の補助情報は別に保持する。
struct NicoJKSourceSpec {
    tstring file;
    tstring ref;
    std::optional<int> refSid;
    tstring refMode;
    // 先行放送のコメント(replace|merge)。取得元の指定がなければ、nicojk_assが録画を探して基準にする。
    tstring alt;
    // 調査用の置き場所。対象のWAV・本編の区間・nicojk_assの作業ファイルとレポートを残す。
    tstring dumpDir;
    std::optional<int> channel;
    std::optional<double> start, programStart, offset;
    bool specified = false;
    bool strictFile = false;
};
inline double ParseNicoJKOffset(const tstring& value) {
    try {
        size_t pos = 0;
        const double r = std::stod(value, &pos);
        if (pos == value.size() && std::isfinite(r) && std::abs(r) < 1e12) return r;
    } catch (const std::exception&) {}
    THROW(ArgumentException, "実況のずれ量が不正です");
    return 0;
}
inline int ParseNicoJKChannel(tstring value) {
    if (value.substr(0, 2) == _T("jk")) value = value.substr(2);
    try {
        if (!value.empty() && value.find_first_not_of(_T("0123456789")) == tstring::npos) {
            const int r = std::stoi(value);
            if (r > 0) return r;
        }
    } catch (const std::exception&) {}
    THROW(ArgumentException, "実況チャンネルが不正です");
    return 0;
}
inline double ParseNicoJKTime(const tstring& value) {
    // 暦の計算で処理し、実行環境のタイムゾーンへ依存しない。
    const std::basic_regex<tchar> compact(_T("([0-9]{4})([0-9]{2})([0-9]{2})([0-9]{2})([0-9]{2})([0-9]{2})"));
    const std::basic_regex<tchar> iso(_T("([0-9]{4})-([0-9]{2})-([0-9]{2})T([0-9]{2}):([0-9]{2}):([0-9]{2})(Z|[+-][0-9]{2}:[0-9]{2})?"));
    std::match_results<tstring::const_iterator> m;
    if (std::regex_match(value, m, compact) || std::regex_match(value, m, iso)) {
        int y = std::stoi(m[1]), month = std::stoi(m[2]), d = std::stoi(m[3]);
        const int h = std::stoi(m[4]), minute = std::stoi(m[5]), sec = std::stoi(m[6]);
        const bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
        const int days[] = {31,28 + (int)leap,31,30,31,30,31,31,30,31,30,31};
        if (y < 1 || month < 1 || month > 12 || d < 1 || d > days[month-1] || h > 23 || minute > 59 || sec > 59)
            THROW(ArgumentException, "実況の基準時刻が不正です");
        int zone = 9 * 3600;
        if (m.size() > 7 && m[7].matched) {
            const auto z = m[7].str();
            zone = 0;
            if (z != _T("Z")) {
                const int zh = std::stoi(z.substr(1,2)), zm = std::stoi(z.substr(4,2));
                if (zh > 23 || zm > 59) THROW(ArgumentException, "実況の時差が不正です");
                zone = (zh * 3600 + zm * 60) * (z[0] == _T('-') ? -1 : 1);
            }
        }
        y -= month <= 2;
        const int era = y / 400;
        const unsigned yo = y - era * 400;
        const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        return ((long long)era * 146097 + yo * 365 + yo / 4 - yo / 100 + doy - 719468) * 86400.0 + h * 3600 + minute * 60 + sec - zone;
    }
    try {
        if (!value.empty() && value.find_first_not_of(_T("0123456789"), value[0] == _T('-') ? 1 : 0) == tstring::npos) {
            const auto r = std::stoll(value);
            if (std::abs((double)r) < 1e12) return (double)r;
        }
    } catch (const std::exception&) {}
    THROW(ArgumentException, "実況の基準時刻が不正です");
    return 0;
}

inline std::string TrimNicoJKValue(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    return begin == std::string::npos ? "" : text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}
inline tstring ParseNicoJKRefMode(const tstring& value) {
    if (value != _T("replace") && value != _T("merge")) {
        THROW(ArgumentException, "実況のモードはreplaceまたはmergeで指定してください");
    }
    return value;
}
inline void ValidateNicoJKReference(const NicoJKSourceSpec& source) {
    if (!source.ref.empty() && (source.channel || source.start || source.offset)) {
        THROW(ArgumentException, "基準録画とchannel/start/offsetは同時に指定できません");
    }
}

inline NicoJKSourceSpec ParseNicoJKIni(const std::vector<std::string>& lines) {
    NicoJKSourceSpec spec;
    spec.specified = true;
    for (size_t i = 0; i < lines.size(); ++i) {
        auto line = lines[i];
        if (i == 0 && line.substr(0, 3) == "\xef\xbb\xbf") line.erase(0, 3);
        line = TrimNicoJKValue(line);
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) THROW(ArgumentException, "実況指定ファイルに等号がありません");
        const auto key = TrimNicoJKValue(line.substr(0, eq));
        const auto value = char_to_tstring(TrimNicoJKValue(line.substr(eq + 1)), CP_UTF8);
        if (value.empty()) THROW(ArgumentException, "実況指定ファイルの値が空です");
        if (key == "channel") spec.channel = ParseNicoJKChannel(value);
        else if (key == "start") spec.start = ParseNicoJKTime(value);
        else if (key == "offset") spec.offset = ParseNicoJKOffset(value);
        else if (key == "file") { spec.file = value; spec.strictFile = true; }
        else if (key == "ref") spec.ref = value;
        else if (key == "ref-mode") spec.refMode = ParseNicoJKRefMode(value);
        else THROW(ArgumentException, "実況指定ファイルに未知のキーがあります");
    }
    return spec;
}
inline NicoJKSourceSpec MergeNicoJKSource(const NicoJKSourceSpec& cli, NicoJKSourceSpec base) {
    if (!cli.file.empty()) { base.file = cli.file; base.strictFile = true; }
    if (cli.channel) base.channel = cli.channel;
    if (cli.start) base.start = cli.start;
    if (cli.offset) base.offset = cli.offset;
    if (!cli.ref.empty()) base.ref = cli.ref;
    if (cli.refSid) base.refSid = cli.refSid;
    if (!cli.refMode.empty()) base.refMode = cli.refMode;
    if (!cli.alt.empty()) base.alt = cli.alt;
    if (!cli.dumpDir.empty()) base.dumpDir = cli.dumpDir;
    base.programStart = cli.programStart;
    base.specified = base.specified || cli.specified || !base.ref.empty();
    ValidateNicoJKReference(base);
    return base;
}
