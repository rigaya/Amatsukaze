#!/usr/bin/env python3
"""実ビルドのlibavcodecを使い、CONVERTのフレーム数・配置・遅延を合成入力で検証する。"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import array
import math
import random

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build_dir", type=Path)
parser.add_argument("output_dir", type=Path)
parser.add_argument("--reuse-build", action="store_true", help="既存の検証バイナリを使う")
args = parser.parse_args()
build = args.build_dir.resolve()
output = args.output_dir.resolve()
output.mkdir(parents=True, exist_ok=True)
project = Path(__file__).resolve().parents[1]
entries = json.loads((build / "compile_commands.json").read_text())
entry = next(item for item in entries if item["file"].endswith("StreamReform.cpp"))
command = shlex.split(entry["command"])
command = command[:command.index("-MD")]
command = [item for item in command if not item.startswith("-O") and item != "-fvisibility=hidden"]
command += ["-O0", "-ffunction-sections", "-fdata-sections"]
command += [str(project / "Amatsukaze" / name) for name in (
    "StreamUtils.cpp", "StringUtils.cpp", "FileUtils.cpp", "OSUtil.cpp", "AdtsParser.cpp",
    "Mpeg2TsParser.cpp", "AudioTrackPlanner.cpp", "AudioTrackBuilder.cpp", "AudioTrackConverter.cpp", "PacketCache.cpp", "ReaderWriterFFmpeg.cpp")]
command += [str(Path(__file__).with_name("AudioTrackConvertTest.cpp")),
            str(build / "common/libcommon.a"), str(build / "libfaad/libfaad.a")]
pkg_dirs = list(build.glob("*/build/lib/pkgconfig"))
assert pkg_dirs, "ビルド済みFFmpegのpkg-config設定がありません"
env = dict(os.environ, PKG_CONFIG_PATH=":".join(str(path) for path in pkg_dirs))
command += shlex.split(subprocess.check_output(
    ["pkg-config", "--static", "--libs", "libavcodec", "libswresample", "libavutil"], env=env, text=True))
command += ["-Wl,--gc-sections", "-o", str(output / "audio_track_convert_test")]
(output / "build-command.json").write_text(json.dumps(command, ensure_ascii=False, indent=2))
if not args.reuse_build:
    with (output / "build.log").open("w") as log:
        subprocess.run(command, cwd=entry["directory"], stdout=log, stderr=subprocess.STDOUT, check=True)
binary = output / "audio_track_convert_test"
with (output / "channels.log").open("w") as log:
    subprocess.run([str(binary)], stdout=log, stderr=subprocess.STDOUT, check=True)
normalizer = 1 + 2 * 0.707
expected_coefficients = [(1 / normalizer, 0), (0, 1 / normalizer),
                         (0.707 / normalizer,) * 2, (0, 0),
                         (0.707 / normalizer, 0), (0, 0.707 / normalizer)]
for line in (output / "channels.log").read_text().splitlines():
    if line.startswith("係数 "):
        _, channel, left, right = line.split()
        assert all(abs(value - expected) < 2e-6 for value, expected in zip(
            [float(left), float(right)], expected_coefficients[int(channel)])), line


def run(command):
    result = subprocess.run(command, capture_output=True, check=True)
    assert not result.stderr, result.stderr.decode(errors="replace")
    return result.stdout


def split_adts(data):
    frames = []
    position = 0
    while position < len(data):
        assert data[position] == 255 and data[position + 1] & 0xf0 == 0xf0
        size = ((data[position + 3] & 3) << 11) | (data[position + 4] << 3) | (data[position + 5] >> 5)
        assert size >= 7 and position + size <= len(data)
        frames.append(data[position:position + size])
        position += size
    return frames


# 固定シードの広帯域入力と複数周波数で、周期波の相関ピーク誤判定を避ける。
rng = random.Random(20260928)
sample_count = 160 * 1024
sources = {}
for channels in (1, 2, 6):
    pcm = array.array("f", (0.05 * math.sin(2 * math.pi * (337 + 211 * channel) * sample / 48000)
                    + 0.02 * rng.gauss(0, 1) for sample in range(sample_count) for channel in range(channels)))
    raw = output / f"source-{channels}.f32"
    raw.write_bytes(pcm.tobytes())
    aac = output / f"source-{channels}.aac"
    run(["ffmpeg", "-v", "error", "-y", "-f", "f32le", "-ar", "48000", "-ac", str(channels),
         "-i", str(raw), "-c:a", "aac", "-b:a", str(channels * 64000), "-f", "adts", str(aac)])
    sources[channels] = split_adts(aac.read_bytes())


def decode(path, channels):
    data = run(["ffmpeg", "-v", "error", "-f", "aac", "-i", str(path), "-f", "f32le", "-"])
    pcm = array.array("f")
    pcm.frombytes(data)
    return [pcm[index:index + channels] for index in range(0, len(pcm), channels)]


checks = []
for src, dst in ((2, 6), (6, 2), (1, 2), (2, 1)):
    for count, prefix, suffix in ((1, 0, 0), (2, 0, 0), (96, 0, 0),
                                  (1, 8, 8), (2, 8, 8), (96, 8, 8), (12, 0, 8), (12, 8, 0)):
        name = f"convert-{src}-{dst}-n{count}-pre{prefix}-post{suffix}"
        selected = sources[dst][10:10 + prefix] + sources[src][20:20 + count] + sources[dst][30:30 + suffix]
        path = output / (name + "-input.aac")
        path.write_bytes(b"".join(selected))
        refs = output / (name + ".refs")
        refs.write_text("".join(f"{index} {int(prefix <= index < prefix + count)}\n" for index in range(len(selected))))
        result_path = output / (name + ".aac")
        with (output / (name + ".log")).open("w") as log:
            subprocess.run([str(binary), str(path), str(refs), str(result_path), str(dst)],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        actual_frames = split_adts(result_path.read_bytes())
        assert len(actual_frames) == len(selected), name + "のフレーム数が一致しません"
        assert actual_frames[:prefix] == selected[:prefix], name + "の前方COPYが変化しました"
        if suffix:
            assert actual_frames[-suffix:] == selected[-suffix:], name + "の後方COPYが変化しました"
        actual = decode(result_path, dst)
        assert len(actual) == len(selected) * 1024, name + "のデコード後サンプル数が不正です"
        result = {"case": name, "frames": len(actual_frames), "samples": len(actual)}
        if count >= 96:
            # CONVERT元の範囲だけを単独デコードし、両デコーダの初期重なりを除いた内部で比較する。
            source_path = output / (name + "-reference.aac")
            source_path.write_bytes(b"".join(selected[prefix:prefix + count]))
            decoded_source = decode(source_path, src)
            if src == 6:
                expected_signals = [[(row[side] + 0.707 * (row[2] + row[4 + side])) / normalizer
                                     for row in decoded_source] for side in (0, 1)]
            elif src == 2 and dst == 1:
                expected_signals = [[(row[0] + row[1]) / 2 for row in decoded_source]]
            elif src == 1:
                expected_signals = [[row[0] for row in decoded_source]] * 2
            else:
                expected_signals = [[row[channel] for row in decoded_source] for channel in (0, 1)]
            begin, end = 4 * 1024, (count - 4) * 1024
            channel_checks = []
            for channel, reference in enumerate(expected_signals):
                converted = [row[channel] for row in actual[prefix * 1024:(prefix + count) * 1024]]
                correlations = []
                for lag in range(-32, 33):
                    a = reference[begin:end]
                    b = converted[begin + lag:end + lag]
                    correlations.append(sum(x * y for x, y in zip(a, b)) / math.sqrt(
                        sum(x * x for x in a) * sum(x * x for x in b)))
                lag = correlations.index(max(correlations)) - 32
                channel_checks.append({"channel": channel, "lag_samples": lag, "correlation": max(correlations)})
                result["channels"] = channel_checks
                (output / "correlation-current.json").write_text(json.dumps(result, ensure_ascii=False, indent=2))
                assert lag == 0, name + "の相関ずれ: " + str(lag)
                assert max(correlations) > 0.9, name + "の信号相関が低すぎます"
            if dst == 6:
                maximum = max(abs(row[channel]) for row in actual[(prefix + 4) * 1024:(prefix + count - 4) * 1024]
                              for channel in range(2, 6))
                result["extra_channel_max_abs"] = maximum
                assert maximum < 1e-6, name + "の追加4チャンネルが無音になりません"
        checks.append(result)
        (output / "results.json").write_text(json.dumps(checks, ensure_ascii=False, indent=2))
# PCE付きの2SCE無音をcfg0デュアルモノとして作り、両言語の分離後CONVERTを通す。
def dual_silence_frame():
    bits = ""
    def add(value, width):
        nonlocal bits
        bits += format(value, f"0{width}b")
    def align():
        nonlocal bits
        bits += "0" * (-len(bits) % 8)
    for value, width in ((5, 3), (0, 4), (1, 2), (3, 4), (2, 4), (0, 4), (0, 4),
                         (0, 2), (0, 3), (0, 4), (0, 1), (0, 1), (0, 1)):
        add(value, width)
    for channel in range(2):
        add(0, 1)
        add(channel, 4)
    align()
    add(0, 8)
    for channel in range(2):
        add(0, 3)
        add(channel, 4)
        add(100, 8)
        add(0, 14)
    add(7, 3)
    align()
    raw = int(bits, 2).to_bytes(len(bits) // 8, "big")
    length = 7 + len(raw)
    return bytes([255, 241, 76, (length >> 11), (length >> 3) & 255,
                  ((length & 7) << 5) | 31, 252]) + raw

for language in (0, 1):
    name = f"convert-dual-lang{language + 1}"
    path = output / (name + "-input.aac")
    path.write_bytes(dual_silence_frame() * 12)
    refs = output / (name + ".refs")
    refs.write_text("".join(f"{index} 1\n" for index in range(12)))
    result_path = output / (name + ".aac")
    with (output / (name + ".log")).open("w") as log:
        subprocess.run([str(binary), str(path), str(refs), str(result_path), "2", str(language)],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    frames = split_adts(result_path.read_bytes())
    actual = decode(result_path, 2)
    assert len(frames) == 12 and len(actual) == 12 * 1024, name + "のフレーム数が不正です"
    maximum = max(abs(sample) for row in actual for sample in row)
    assert maximum == 0, name + "の無音がゼロになりません"
    checks.append({"case": name, "frames": 12, "samples": len(actual), "max_abs": maximum})
(output / "results.json").write_text(json.dumps(checks, ensure_ascii=False, indent=2))
print(f"CONVERT合成入力 {len(checks)} ケース: フレーム数・COPY保持・全デコード・内部ずれ検証成功")
