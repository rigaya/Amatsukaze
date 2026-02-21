---
name: update-encoder-urls
description: リリース準備で x264/x265/svt-av1 の実行ファイルダウンロード先を更新する。対象は .github/workflows/build_windows_package.yml と docker/Dockerfile。
disable-model-invocation: true
---

## 目的

- リリース準備のための更新

## ダウンロード元

- `https://github.com/rigaya/AutoBuildForAviUtlPlugins/releases/` の最新版

## 更新対象

- `.github/workflows/build_windows_package.yml`
- `docker/Dockerfile`

## 手順

1. `https://api.github.com/repos/rigaya/AutoBuildForAviUtlPlugins/releases/latest` を取得し、x264/x265/svt-av1（SvtAv1EncApp）の最新版アセット名と `browser_download_url` を確認する。
2. `.github/workflows/build_windows_package.yml` の `X264_URL`, `X265_URL`, `SVT_URL` を更新する（Windows 用 `_x64.zip` アセットを使用）。
3. `docker/Dockerfile` の x264/x265/svt-av1 のダウンロード URL を更新する（Linux 用 `_amd64_linux` アセットを使用）。
4. ファイル名中のバージョン差分（例: `4.0.1` → `4.0.1-56`）も確認し、URL 全体を一致させる。
5. 更新後に `rg -n "x264|x265|svt-av1|SvtAv1EncApp|AutoBuildForAviUtlPlugins" .github/workflows/build_windows_package.yml docker/Dockerfile` で反映を確認する。
