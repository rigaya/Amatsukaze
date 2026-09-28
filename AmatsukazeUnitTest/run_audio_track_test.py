#!/usr/bin/env python3
"""既存Linuxビルドのヘッダ設定を使い、音声プランと無音生成を単独で検証する。"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build_dir", type=Path)
parser.add_argument("output_dir", type=Path)
parser.add_argument("--integration", action="store_true", help="StreamReform合成入力とPacketCacheコピーも検証する")
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
command += ["-O0", "-ffunction-sections", "-fdata-sections", "-DAUDIO_TRACK_STANDALONE"]
command += [str(project / "Amatsukaze" / name) for name in ("AudioTrackPlanner.cpp", "AudioTrackBuilder.cpp", "StringUtils.cpp")]
command += [str(Path(__file__).with_name("AudioTrackTest.cpp")), "-Wl,--gc-sections", "-o", str(output / "audio_track_test")]
(output / "build-command.json").write_text(json.dumps(command, ensure_ascii=False, indent=2))
with (output / "build.log").open("w") as log:
    subprocess.run(command, cwd=entry["directory"], stdout=log, stderr=subprocess.STDOUT, check=True)
with (output / "test.log").open("w") as log:
    subprocess.run([str(output / "audio_track_test"), str(output)], stdout=log, stderr=subprocess.STDOUT, check=True)
print((output / "test.log").read_text(), end="")

if args.integration:
    command = command[:command.index("-DAUDIO_TRACK_STANDALONE")]
    command += [str(project / "Amatsukaze" / name) for name in (
        "StreamReform.cpp", "StreamUtils.cpp", "StringUtils.cpp", "FileUtils.cpp", "OSUtil.cpp", "AdtsParser.cpp",
        "Mpeg2TsParser.cpp", "AudioTrackPlanner.cpp", "AudioTrackBuilder.cpp", "PacketCache.cpp")]
    command += [str(Path(__file__).with_name("AudioTrackStreamReformTest.cpp")),
                str(build / "common/libcommon.a"), str(build / "libfaad/libfaad.a"),
                "-Wl,--gc-sections", "-o", str(output / "stream_reform_test")]
    (output / "stream-build-command.json").write_text(json.dumps(command, ensure_ascii=False, indent=2))
    with (output / "stream-build.log").open("w") as log:
        subprocess.run(command, cwd=entry["directory"], stdout=log, stderr=subprocess.STDOUT, check=True)
    with (output / "stream-test.log").open("w") as log:
        subprocess.run([str(output / "stream_reform_test"), str(output)], stdout=log, stderr=subprocess.STDOUT, check=True)
    print((output / "stream-test.log").read_text(), end="")

faad_command = [command[0], "-std=c++17", "-I" + str(project / "include_gpl"),
                str(Path(__file__).with_name("AudioTrackFaadSilenceTest.cpp")),
                str(build / "libfaad/libfaad.a"), "-o", str(output / "faad_silence_test")]
(output / "faad-build-command.json").write_text(json.dumps(faad_command, ensure_ascii=False, indent=2))
with (output / "faad-build.log").open("w") as log:
    subprocess.run(faad_command, stdout=log, stderr=subprocess.STDOUT, check=True)
with (output / "faad-test.log").open("w") as log:
    subprocess.run([str(output / "faad_silence_test"), str(output)], stdout=log, stderr=subprocess.STDOUT, check=True)
print((output / "faad-test.log").read_text(), end="")

# 生成した全レイアウトとBuilder出力を最後までデコードし、無音とパケット数を確認する。
import array
checks = []
files = [(output / ("silent" + str(ch) + ".aac"), 200) for ch in range(1, 7)]
if args.integration:
    files += [(output / ("builder-dual-" + str(track) + ".aac"), 3) for track in range(3)]
    files += [(output / "builder-pce-source.aac", 1)]
for path, expected_packets in files:
    decode = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-f", "s16le", "-"], capture_output=True, check=True)
    samples = array.array("h", decode.stdout)
    maximum = max(map(abs, samples), default=0)
    probe = subprocess.run(["ffprobe", "-v", "error", "-count_packets", "-select_streams", "a:0",
                            "-show_entries", "stream=channels,nb_read_packets", "-of", "json", str(path)],
                           capture_output=True, text=True, check=True)
    streams = json.loads(probe.stdout)["streams"]
    checks.append({"file": path.name, "returncode": decode.returncode, "samples": len(samples),
                   "max_abs": maximum, "stderr": decode.stderr.decode(), "probe": streams})
    assert samples and maximum == 0 and not decode.stderr, path.name + "のフルデコードが無音になりません"
    assert len(streams) == 1 and int(streams[0]["nb_read_packets"]) == expected_packets, path.name + "のAACパケット数が不正です"
(output / "ffmpeg-decode.json").write_text(json.dumps(checks, ensure_ascii=False, indent=2))
print("ffmpegフルデコード・無音・AACパケット数の確認成功")
