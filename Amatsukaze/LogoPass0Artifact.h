#pragma once

#include "../common/rgy_tchar.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace logopass0 {

// pass0成果物の公開先。baseは正規化済みの絶対パスで保持する。
struct ArtifactPaths {
    tstring base;
    tstring amtSource;
    tstring trimAvs;
    tstring ready;
};

// resume-dir直下だけを成果物の公開先として受け入れる。
bool ResolveArtifactPaths(const tstring& resumeDir, const tstring& requestedBase,
    ArtifactPaths& paths, tstring& error);

// 一つでも最終成果物が存在すれば、前回失敗分を含めて再利用・上書きを禁止する。
bool HasExistingArtifact(const ArtifactPaths& paths);

// 同数時は先頭を選ぶ。フレーム数0の映像も、実際に存在する映像として扱う。
int SelectLargestVideoIndex(const std::vector<int>& frameCounts);

using TempFileWriter = std::function<bool(FILE* file, tstring& error)>;
using TempPathGenerator = std::function<tstring(const tstring& finalPath, unsigned int attempt)>;

// 既存finalを置換せずに一時ファイルを公開する。成功したfinal名をpublishOrderへ順に記録する。
// POSIXでlink後の一時ファイル削除に失敗した場合は、finalは公開済みでもfalseを返す。
bool PublishFileAtomically(const tstring& finalPath, const TempFileWriter& writer,
    tstring& error, std::vector<tstring>* publishOrder = nullptr,
    const TempPathGenerator& tempPathGenerator = TempPathGenerator());

bool CopyFileAtomically(const tstring& sourcePath, const tstring& finalPath,
    tstring& error, std::vector<tstring>* publishOrder = nullptr);
bool WriteUtf8TextAtomically(const std::string& text, const tstring& finalPath,
    tstring& error, std::vector<tstring>* publishOrder = nullptr);

// amts、trim、readyの順序を固定し、readyは前二者の成功後にだけ公開する。
bool PublishArtifactsWithWriters(const ArtifactPaths& paths, const TempFileWriter& amtWriter,
    const TempFileWriter& trimWriter, tstring& error, std::vector<tstring>* publishOrder = nullptr);
bool PublishArtifacts(const ArtifactPaths& paths, const tstring& sourceAmtPath,
    const std::string& trimAvsText, tstring& error, std::vector<tstring>* publishOrder = nullptr);

} // namespace logopass0
