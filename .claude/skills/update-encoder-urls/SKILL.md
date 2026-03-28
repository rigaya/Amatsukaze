---
name: update-encoder-urls
description: リリース準備で x264/x265/svt-av1 の実行ファイルダウンロード先を更新する。対象は .github/workflows/build_windows_package.yml と docker/Dockerfile。Dockerfile では加えて QSVEnc/NVEnc/VCEEnc/tsreplace の .deb 取得バージョンも更新する。
disable-model-invocation: true
---

## 目的

- リリース準備のための更新

## ダウンロード元

- x264 / x265 / svt-av1（SvtAv1EncApp）: `https://github.com/rigaya/AutoBuildForAviUtlPlugins/releases/` の最新版
- Docker 用 Linux .deb（QSVEnc / NVEnc / VCEEnc / tsreplace）: 各リポジトリの Releases 最新版
  - `https://github.com/rigaya/QSVEnc/releases`
  - `https://github.com/rigaya/NVEnc/releases`
  - `https://github.com/rigaya/VCEEnc/releases`
  - `https://github.com/rigaya/tsreplace/releases`

## 更新対象

- `.github/workflows/build_windows_package.yml`（x264 / x265 / svt-av1 のみ）
- `docker/Dockerfile`（上記に加え `QSVENCC_VER` / `NVENCC_VER` / `VCEENCC_VER` / `TSREPLACE_VER`）

## 手順

### AutoBuildForAviUtlPlugins（x264 / x265 / svt-av1）

1. `https://api.github.com/repos/rigaya/AutoBuildForAviUtlPlugins/releases/latest` を取得し、x264/x265/svt-av1（SvtAv1EncApp）の最新版アセット名と `browser_download_url` を確認する。
2. `.github/workflows/build_windows_package.yml` の `X264_URL`, `X265_URL`, `SVT_URL` を更新する（Windows 用 `_x64.zip` アセットを使用）。
3. `docker/Dockerfile` の x264/x265/svt-av1 のダウンロード URL を更新する（Linux 用 `_amd64_linux` アセットを使用）。
4. ファイル名中のバージョン差分（例: `4.0.1` → `4.0.1-56`）も確認し、URL 全体を一致させる。

### Docker 用エンコーダ／ツール（QSVEnc / NVEnc / VCEEnc / tsreplace）

5. 各リポジトリの `releases/latest` API でタグ名（バージョン文字列）を確認する。
   - `https://api.github.com/repos/rigaya/QSVEnc/releases/latest`
   - `https://api.github.com/repos/rigaya/NVEnc/releases/latest`
   - `https://api.github.com/repos/rigaya/VCEEnc/releases/latest`
   - `https://api.github.com/repos/rigaya/tsreplace/releases/latest`
6. `docker/Dockerfile` の `ENV` を、取得したタグに合わせて更新する。
   - `QSVENCC_VER` … `qsvencc_${QSVENCC_VER}_${ARCH}.deb` と一致させる
   - `NVENCC_VER` … `nvencc_${NVENCC_VER}_${ARCH}.deb` と一致させる
   - `VCEENCC_VER` … `vceencc_${VCEENCC_VER}_${ARCH}.deb` と一致させる（`ENABLE_VCEENCC=1` ビルド時に使用）
   - `TSREPLACE_VER` … `tsreplace_${TSREPLACE_VER}_${ARCH}.deb` と一致させる
7. リリースに同名の `.deb` が無い場合は、タグ名とアセット名の規則が変わっていないか Releases ページを確認する。

### 確認

8. 更新後に次で反映を確認する。

```bash
rg -n "x264|x265|svt-av1|SvtAv1EncApp|AutoBuildForAviUtlPlugins|QSVENCC_VER|NVENCC_VER|VCEENCC_VER|TSREPLACE_VER|QSVEnc|NVEnc|VCEEnc|tsreplace" .github/workflows/build_windows_package.yml docker/Dockerfile
```
