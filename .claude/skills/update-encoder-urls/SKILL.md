---
name: update-encoder-urls
description: GitHub Workflowの build_windows_package.yml と docker/Dockerfile で使用している x264/x265/svt-av1 のダウンロードURLを rigaya/AutoBuildForAviUtlPlugins の最新リリースに更新する
disable-model-invocation: true
---

## 手順

1. `https://api.github.com/repos/rigaya/AutoBuildForAviUtlPlugins/releases/latest` を WebFetch して、最新リリースのアセット一覧を取得する。x264, x265, svt-av1（SvtAv1EncApp）を含むアセットの名前と `browser_download_url` を抽出する。

2. 以下の2ファイルのURLを最新リリースのものに更新する:
   - `.github/workflows/build_windows_package.yml` の `X264_URL`, `X265_URL`, `SVT_URL` 環境変数（Windows 用 `_x64.zip` のアセットを使う）
   - `docker/Dockerfile` の x264/x265/svt-av1 の wget URL（Linux 用 `_amd64_linux` のアセットを使う）

3. リリースタグだけでなく、ファイル名中のバージョン番号が変わっている場合もあるので注意すること（例: `SvtAv1EncApp_4.0.1_x64_clang.zip` → `SvtAv1EncApp_4.0.1-56_x64_clang.zip`）。

4. 変更内容のサマリを表示する（更新前後のリリースタグ、各パッケージのバージョン変更有無）。
