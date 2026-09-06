# 字幕生成

Amatsukazeでは、Whisperを使用した音声からの字幕生成が可能です。

字幕モードで「Whisperで生成」を選択することで、音声から文字起こしによりSRT字幕を生成することができます。

Amatsukazeでは、
- faster-whisperをベースとする[whisper-standalone-win](https://github.com/Purfview/whisper-standalone-win)
- [whisp-carrier](https://github.com/CVN-68/whisp-carrier)
- [whisper.cpp](https://github.com/ggml-org/whisper.cpp)
に対応しています。

導入の容易さと精度の観点で、[whisper-standalone-win](https://github.com/Purfview/whisper-standalone-win)を推奨します。

ただ、[whisper-standalone-win](https://github.com/Purfview/whisper-standalone-win)はNVIDIA GPUとCPUのみの対応となっているほか、RTX50xxへ最適化されたバージョンはPro版のみ(有償)となっています。

そのため、RTX50xxへ最適化されたライブラリが使用したい場合、[whisp-carrier](https://github.com/CVN-68/whisp-carrier)を活用ください。

また、CPUでの字幕生成は時間を要するため、NVIDIA GPUのない環境で、Intel/AMD GPUやIntel NPUを使用したい場合には、[whisper.cpp](https://github.com/ggml-org/whisper.cpp)を試してみてください(ただし、導入難易度は高めです)。

---
## whisper-standalone-win (faster-whisperの実装)

[whisper-standalone-win](https://github.com/Purfview/whisper-standalone-win)からFaster-Whisper-XXLをダウンロードし、適当な場所に展開します。


<img src="../data/amatsukaze_20251101_whisper.png" width="552">

### Amatsukazeでの設定方法

[基本設定]タブで展開した場所にあるfaster-whisper-xxlのパスを指定してください。

その後、プロファイルの[字幕モード]でWhisperで字幕を生成するよう指定してください。

faster-whisperの場合、whisper-optionは特に指定しなくてもまずは問題なく動作します。

なお、初回実行時にはモデルのダウンロードが行われるため、時間がかかります。

---
## whisp-carrier

### Windows版

[whisp-carrier の最新リリース](https://github.com/CVN-68/whisp-carrier/releases/latest)からアーカイブをダウンロードし、適当な場所に展開し、[基本設定]タブのWhisperパスに、展開したフォルダ内の `whisp-carrier.exe` を直接指定してください。

初回実行時にはWhisperモデルがダウンロードされるため、時間と空き容量が必要です。

### Linux版（venv・NVIDIA CUDA）

Linuxでは、PythonのvenvにCUDA版PyTorchとwhisp-carrierの依存パッケージを導入して実行できます。あらかじめNVIDIAドライバーを導入し、`nvidia-smi` を実行してGPUが表示されることを確認してください。

CUDAライブラリは、PyTorchのCUDA 12.8版wheelから導入されます。

まず、Pythonのvenv、ビルドに必要なツール、ffmpeg、TEN VADが必要とするC++ランタイムを導入します。

```bash
sudo apt update
sudo apt install -y python3 python3-venv python3-dev build-essential ffmpeg libc++1 libc++abi1
```

whisp-carrierを取得し、リポジトリ内にvenvを作成します。

```bash
git clone https://github.com/CVN-68/whisp-carrier.git
cd whisp-carrier

python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip

# CUDA 12.8版を先に導入する
python -m pip install torch==2.8.0 torchaudio==2.8.0 --index-url https://download.pytorch.org/whl/cu128
python -m pip install -r requirements.txt
```

次のコマンドでCUDAが認識されていることを確認します。`--version` の出力に `backend=cuda`、`CUDA: True`、使用するGPU名が表示され、`--checkcuda` が `1` 以上を返せば正常です。

```bash
python whisp_carrier.py --version
python whisp_carrier.py --checkcuda
```

次に、venv内のPythonでwhisp-carrierを起動するラッパースクリプト `whisp-carrier.sh` をリポジトリ直下に作成します。

```bash
#!/bin/sh
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$SCRIPT_DIR/.venv/bin/python" "$SCRIPT_DIR/whisp_carrier.py" "$@"
```

実行権限を付け、単体でCUDAを認識できることを確認します。

```bash
chmod +x whisp-carrier.sh
./whisp-carrier.sh --checkcuda
```

[基本設定]タブのWhisperパスには、作成した `whisp-carrier.sh` のパスを指定してください。プロファイルの[字幕モード]でWhisperによる字幕生成を選択します。通常、whisper-optionは空のままで構いません。

---
## whisper.cpp

whisper.cppを使用する場合は、実行ファイルのビルド・モデルの準備等を自分で行う必要があります。

### ビルド

Intel GPU/NPUを使用する場合と、AMD GPUを使用する場合で、ビルド方法が異なります。

**前提**
下記は導入済みとします。

- Visual Studio 2022 (Windows) / gcc,g++ (Linux)
- cmake

#### Intel GPU / Intel NPU向けにビルドする

whisper.cppの[OpenVINO実装](https://github.com/ggml-org/whisper.cpp#openvino-support)を使用します。

[OpenVINO](https://storage.openvinotoolkit.org/repositories/openvino/packages/)をダウンロードして、適当な場所に展開します。

また、Intel TBBが必要になるため、[OneAPI](https://www.intel.com/content/www/us/en/developer/tools/oneapi/base-toolkit-download.html)をダウンロードしてインストールします。

Windowsでは、"Intel oneAPI command prompt for Intel 64 for Visual Studio 2022" を開きます。

```-DAVX512_FOUND```は使用する環境に合わせて指定してください。AVX512有効でビルドをしてしまうとAVX512非対応の環境で異常終了してしまうためです。

```bat
<OpenVINOの展開先>\setupvars.bat

# whisper.cppをOpenVINO有効でビルド
git clone https://github.com/ggml-org/whisper.cpp.git
cd whisper.cpp
cmake -B build -DCMAKE_CXX_FLAGS_RELEASE="/MT /Ox /Ob2 /DNDEBUG" -DWHISPER_OPENVINO=1 -DAVX512_FOUND=[ON,OFF]
cmake --build build -j --config Release
```

```build\bin\Release```に実行ファイルが生成されます。

このディレクトリに下記dllをコピーします。

- OpenVINO関連のdll
  ```<OpenVINOの展開先>\runtime\bin\intel64\Release\openvino*.dll```すべて
  
- TBB関連のdll
  ```C:\Program Files (x86)\Intel\oneAPI\tbb\latest\bin\tbb*.dll```すべて

#### AMD GPU向けにビルドする

whisper.cppの[Vulkan実装](https://github.com/ggml-org/whisper.cpp#vulkan-gpu-support)を使用します。

[Vulkan SDK](https://vulkan.lunarg.com/sdk/home)をインストールします。インストールするコンポーネントはデフォルトで構いません。

Windowsでは、"x64 Native Tools Command Prompt for VS 2022"を開き、下記のようにビルドします。

```-DAVX512_FOUND```は使用する環境に合わせて指定してください。AVX512有効でビルドをしてしまうとAVX512非対応の環境で異常終了してしまうためです。

```bat
# whisper.cppをVulkan有効でビルド
git clone https://github.com/ggml-org/whisper.cpp.git
cd whisper.cpp
cmake -B build -DCMAKE_CXX_FLAGS_RELEASE="/MT /Ox /Ob2 /DNDEBUG" -DGGML_VULKAN=1 -DAVX512_FOUND=[ON,OFF]
cmake --build build -j --config Release
```

```build\bin\Release```に実行ファイルが生成されます。

### モデルのダウンロード

事前にモデルのダウンロードを行います。下記では、```large-v3-turbo```の例を示しますが、利用するモデルに応じて適宜読み替えてください。

```bat
cd models
download-ggml-model.cmd large-v3-turbo
```

```ggml-large-v3-turbo.bin``` がダウンロードされます。

Intel GPU / Intel NPUを使用する場合は、さらに追加で下記操作が必要です。

```bat
cd models
python -m venv openvino_conv_env
openvino_conv_env\Scripts\activate
python -m pip install --upgrade pip
pip install -r requirements-openvino.txt
pip install onnxscript
python convert-whisper-to-openvino.py --model large-v3-turbo
```

```ggml-large-v3-turbo-encoder-openvino.[bin/xml]```が生成されます。

### Amatsukazeでの設定方法

[基本設定]タブでビルドした```whisper-cli```のパスを指定してください。

また、プロファイル設定では、[字幕モード]でWhisperで字幕を生成するよう指定してください。

<img src="../data/amatsukaze_20251106_whispercpp.png" width="552">

[whisper-model]は**未指定**を選択してください。(この設定欄はfaster-whisper用です)

[whisper-option]は、whisper.cppの場合、下記を**ベースに**調整してみてください。

```-m <使用するモデルへのパス> --language ja --split-on-word --entropy-thold 2.8 --max-context 8```

- -m <使用するモデルへのパス>
  
  使用するモデルへのパスは、例えば```ggml-large-v3-turbo.bin```へのフルパスとしてください。

- --language ja
  
  whisper.cppはデフォルトが英語なので、指定が必要です。自動判定したいときは、```--language auto```とします。

- --split-on-word --entropy-thold 2.8 --max-context 8
  
  whisper.cppはfaster-whisperと異なり、音楽等認識が安定しないときに繰り返し同じ文字列を返す傾向があり、それを抑制するオプションです。

また、OpenVINOビルドでは、下記指定が追加で必要です。

- Intel GPUを使用する場合
  
  ```--ov-e-device GPU```

- Intel NPUを使用する場合

  ```--ov-e-device NPU```
