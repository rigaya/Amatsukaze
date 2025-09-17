# Dockerでの利用方法

## 前提条件

### NVIDIA Driverのインストール

AmatsukazeでCUDA、NVEncを使用する場合に必要

```sh
# インストール可能なものを確認
rigaya@rigaya8-u2404:~$ ubuntu-drivers devices
== /sys/devices/pci0000:00/0000:00:01.0/0000:01:00.0 ==
modalias : pci:v000010DEd00001B80sv000019DAsd00001426bc03sc00i00
vendor   : NVIDIA Corporation
model    : GP104 [GeForce GTX 1080]
driver   : nvidia-driver-570-server - distro non-free
driver   : nvidia-driver-470 - distro non-free
driver   : nvidia-driver-535 - distro non-free
driver   : nvidia-driver-580 - distro non-free
driver   : nvidia-driver-580-server - distro non-free
driver   : nvidia-driver-570 - distro non-free
driver   : nvidia-driver-535-server - distro non-free
driver   : nvidia-driver-550 - distro non-free recommended
driver   : nvidia-driver-470-server - distro non-free
driver   : xserver-xorg-video-nouveau - distro free builtin

# 最新版をインストール
rigaya@rigaya8-u2404:~$ sudo apt install nvidia-driver-580
```

### Intel Driverのインストール

AmatsukazeでQSVを使用する場合に必要

```sh
sudo apt-get install -y gpg-agent wget
wget -qO - https://repositories.intel.com/gpu/intel-graphics.key | sudo gpg --yes --dearmor --output /usr/share/keyrings/intel-graphics.gpg
echo "deb [arch=amd64,i386 signed-by=/usr/share/keyrings/intel-graphics.gpg] https://repositories.intel.com/gpu/ubuntu noble unified" | \
  sudo tee /etc/apt/sources.list.d/intel-gpu-noble.list
sudo apt update
sudo apt install intel-media-va-driver-non-free intel-opencl-icd libmfx1 libmfx-gen1.2 libva-drm2 libva-x11-2 libigfxcmrt7
```

### dockerのインストール

```sh
# dockerのインストール
sudo apt-get update
sudo apt-get install ca-certificates curl
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc

echo \
  "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu \
  $(. /etc/os-release && echo "${UBUNTU_CODENAME:-$VERSION_CODENAME}") stable" | \
  sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
sudo apt-get update

sudo apt-get install docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin

sudo usermod -a -G docker $USER
```

### NVIDIA Container Toolkit のインストール

AmatsukazeでCUDA、NVEncを使用する場合に必要

バージョンは[公式ページ](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html)に沿って変更したほうが良いかも

```sh
curl -fsSL https://nvidia.github.io/libnvidia-container/gpgkey | sudo gpg --dearmor -o /usr/share/keyrings/nvidia-container-toolkit-keyring.gpg \
  && curl -s -L https://nvidia.github.io/libnvidia-container/stable/deb/nvidia-container-toolkit.list | \
    sed 's#deb https://#deb [signed-by=/usr/share/keyrings/nvidia-container-toolkit-keyring.gpg] https://#g' | \
    sudo tee /etc/apt/sources.list.d/nvidia-container-toolkit.list
sudo apt-get update

export NVIDIA_CONTAINER_TOOLKIT_VERSION=1.17.8-1
sudo apt-get install -y \
      nvidia-container-toolkit=${NVIDIA_CONTAINER_TOOLKIT_VERSION} \
      nvidia-container-toolkit-base=${NVIDIA_CONTAINER_TOOLKIT_VERSION} \
      libnvidia-container-tools=${NVIDIA_CONTAINER_TOOLKIT_VERSION} \
      libnvidia-container1=${NVIDIA_CONTAINER_TOOLKIT_VERSION}
# dockerを再起動
sudo systemctl restart docker
```

## インストール手順

```sh
cd docker
# ディレクトリ構成の作成
./setup.sh
# 必要に応じてvolumesのマウント対象等を調整
vi compose.yml
```

## 起動

```sh
# EPGStation, Amatsukazeを実行するユーザーIDとグループIDを指定
export RUN_UID=$(id -u)
export RUN_GID=$(id -g)
# 起動
docker compose up -d
```

## 停止

```sh
docker compose down
```

## 更新

```sh
# EPGStation, Amatsukazeを実行するユーザーIDとグループIDを指定
export RUN_UID=$(id -u)
export RUN_GID=$(id -g)
# 更新
docker compose pull
docker compose build --pull
# 最新のイメージを元に起動
docker compose up -d
```

## 設定

| | ポート |
|:--|:--:|
| Amastuakze | 32768 |

| | 保存場所 |
|:--|:--|
| 録画データ              | ```./recorded``` |
| エンコード結果データ    | ```./encoded``` |
| Amatsuakze avsファイル  | ```./avs``` |
| Amatsuakze avsファイル  | ```./bat``` |
| Amatsuakze 設定ファイル | ```./config``` |
| Amatsuakze drcs外字     | ```./drcs``` |
| Amatsuakze ロゴデータ   | ```./logo``` |
| Amatsuakze プロファイル | ```./profile``` |