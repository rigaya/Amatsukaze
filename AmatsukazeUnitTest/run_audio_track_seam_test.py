#!/usr/bin/env python3
"""連続した同じPCMから作ったAACで、有音のCOPY/CONVERT境界残差を測定する。"""
import argparse
import array
import json
import math
from pathlib import Path
import random
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path, help="run_audio_track_convert_test.pyで生成した検証バイナリ")
parser.add_argument("output_dir", type=Path)
parser.add_argument("--experiment", action="store_true", help="既存素材で仕様外の同レイアウトCONVERT追加を比較する")
parser.add_argument("--oracle", action="store_true", help="実TSには無い同レイアウト前packetをhidden挿入した比較")
parser.add_argument("--long-gap-source", type=Path, help="長欠落検証に使う2ch有音AAC素材")
parser.add_argument("--expect-silence", action="store_true", help="長欠落検証の出力が完全無音であることを必須にする")
args = parser.parse_args()
output = args.output_dir.resolve()
output.mkdir(parents=True, exist_ok=True)
binary = args.binary.resolve()
RATE = 48000
FRAME = 1024
PREFIX = 48
CONVERT = 96
SUFFIX = 48
TOTAL = PREFIX + CONVERT + SUFFIX


def run(command):
    result = subprocess.run(command, capture_output=True, check=True)
    assert not result.stderr, result.stderr.decode(errors="replace")
    return result.stdout


def split_adts(data):
    frames = []
    position = 0
    while position < len(data):
        assert data[position] == 255 and data[position + 1] & 240 == 240
        length = ((data[position + 3] & 3) << 11) | (data[position + 4] << 3) | (data[position + 5] >> 5)
        assert length >= 7 and position + length <= len(data)
        frames.append(data[position:position + length])
        position += length
    return frames


def decode(path, channels):
    samples = array.array("f")
    samples.frombytes(run(["ffmpeg", "-v", "error", "-f", "aac", "-i", str(path), "-f", "f32le", "-"]))
    return [samples[index:index + channels] for index in range(0, len(samples), channels)]


def long_gap(source_path):
    source_frames = split_adts(source_path.read_bytes())
    assert len(source_frames) > 40
    silence = bytes.fromhex("fff14c8001dffc2064000190000e")
    source = output / "long-gap-input.aac"
    source.write_bytes(source_frames[40] + silence * 12)
    pts = [0] + [9000000 + index * 1920 for index in range(12)]
    Path(str(source) + ".pts").write_text("".join(f"{value}\n" for value in pts))
    refs = output / "long-gap.refs"
    refs.write_text("".join(f"{index} 1\n" for index in range(1, 13)))
    result_path = output / "long-gap-output.aac"
    with (output / "long-gap-builder.log").open("w") as log:
        subprocess.run([str(binary), str(source), str(refs), str(result_path), "6"],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    actual_frames = split_adts(result_path.read_bytes())
    actual = decode(result_path, 6)
    assert len(actual_frames) == 12 and len(actual) == 12 * FRAME
    maximum = max(abs(value) for row in actual for value in row)
    per_frame_maximum = [max(abs(value) for row in actual[index * FRAME:(index + 1) * FRAME] for value in row)
                         for index in range(12)]
    result = {"physical_frames": 13, "output_frames": 12, "output_samples_per_channel": len(actual),
              "first_source_pts": pts[0], "first_output_source_pts": pts[1], "gap_seconds": 100,
              "planned_input": "2ch手組み完全無音12枚、前の有音は出力対象外", "max_abs": maximum,
              "frame_max_abs": per_frame_maximum, "leakage_detected": maximum != 0}
    (output / "long-gap-results.json").write_text(json.dumps(result, ensure_ascii=False, indent=2))
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if args.expect_silence:
        assert maximum == 0, "100秒前の有音historyが無音CONVERTへ漏れました"


if args.long_gap_source:
    long_gap(args.long_gap_source.resolve())
    raise SystemExit(0)


def experiment(oracle=False):
    # 通常の成果物を上書きせず、参照maskだけ変えた仕様外比較として保存する。
    source = output / "seam-input.aac"
    selected = split_adts(source.read_bytes())
    prefix_name = "oracle" if oracle else "experiment"
    if oracle:
        # 実TSには無い直前の同レイアウトpacketを物理入力へ追加し、出力参照では飛ばす。
        surround_frames = split_adts((output / "continuous-6.aac").read_bytes())
        boundary = PREFIX + CONVERT
        hidden = surround_frames[boundary - 1]
        source = output / "oracle-hidden-input.aac"
        source.write_bytes(b"".join(selected[:boundary] + [hidden] + selected[boundary:]))
    pcm_values = array.array("f")
    pcm_values.frombytes((output / "continuous-2.f32").read_bytes())
    pcm = [pcm_values[index:index + 2] for index in range(0, len(pcm_values), 2)]
    original = [(0.0, 0.0)] * FRAME + pcm[:(TOTAL - 1) * FRAME]
    full = [row[:2] for row in decode(output / "continuous-6.aac", 6)[:TOTAL * FRAME]]
    references = {"原PCM_遅延1024補正": original, "元5.1全長デコード": full}
    experiments = []
    for extra in ((0, 2) if oracle else (0, 1, 2, 4)):
        handoff = PREFIX + CONVERT + extra
        if extra == 0 and not oracle:
            result_path = output / "seam-output.aac"
        else:
            refs = output / f"{prefix_name}-extra{extra}.refs"
            refs.write_text("".join(f"{index + int(oracle and index >= PREFIX + CONVERT)} {int(PREFIX <= index < handoff)}\n" for index in range(TOTAL)))
            result_path = output / f"{prefix_name}-extra{extra}.aac"
            with (output / f"{prefix_name}-extra{extra}.log").open("w") as log:
                subprocess.run([str(binary), str(source), str(refs), str(result_path), "6"],
                               stdout=log, stderr=subprocess.STDOUT, check=True)
        frames = split_adts(result_path.read_bytes())
        preserved = frames[:PREFIX] == selected[:PREFIX] and frames[handoff:] == selected[handoff:]
        assert len(frames) == TOTAL and preserved
        actual = decode(result_path, 6)
        assert len(actual) == TOTAL * FRAME
        windows = {"CONVERT内部": ((PREFIX + 4) * FRAME, (PREFIX + CONVERT - 4) * FRAME),
                   "元終了境界±1024": ((PREFIX + CONVERT - 1) * FRAME, (PREFIX + CONVERT + 1) * FRAME),
                   "新handoff境界±1024": ((handoff - 1) * FRAME, (handoff + 1) * FRAME),
                   "handoff直後1frame": (handoff * FRAME, (handoff + 1) * FRAME)}
        measurements = {}
        for label, reference in references.items():
            stats = {}
            for region, (begin, end) in windows.items():
                residual = [actual[index][channel] - reference[index][channel]
                            for index in range(begin, end) for channel in (0, 1)]
                error = math.sqrt(sum(value * value for value in residual) / len(residual))
                stats[region] = {"residual_rms": error, "residual_peak": max(map(abs, residual))}
                if region != "CONVERT内部":
                    correlations = []
                    a = [reference[index][channel] for index in range(begin, end) for channel in (0, 1)]
                    for lag in range(-32, 33):
                        b = [actual[index + lag][channel] for index in range(begin, end) for channel in (0, 1)]
                        correlations.append(sum(x * y for x, y in zip(a, b)) / math.sqrt(
                            sum(x * x for x in a) * sum(y * y for y in b)))
                    stats[region]["local_lag_samples"] = correlations.index(max(correlations)) - 32
                    stats[region]["local_correlation"] = max(correlations)
            interior = stats["CONVERT内部"]["residual_rms"]
            for region in windows:
                if region != "CONVERT内部":
                    stats[region]["boundary_to_interior_rms_ratio"] = stats[region]["residual_rms"] / interior
            measurements[label] = stats
        begin, end = windows["CONVERT内部"]
        correlations = []
        a = [original[index][0] for index in range(begin, end)]
        for lag in range(-32, 33):
            b = [actual[index + lag][0] for index in range(begin, end)]
            correlations.append(sum(x * y for x, y in zip(a, b)) / math.sqrt(
                sum(x * x for x in a) * sum(y * y for y in b)))
        lag = correlations.index(max(correlations)) - 32
        assert lag == 0
        experiments.append({"extra_convert_frames": extra, "oracle_hidden_frame": oracle,
                            "physical_frames": len(selected) + int(oracle), "frames": len(frames), "copy_preserved": preserved,
                            "lag_samples": lag, "correlation": max(correlations),
                            "handoff_seconds": handoff * FRAME / RATE, "measurements": measurements})
    (output / (prefix_name + "-results.json")).write_text(json.dumps(experiments, ensure_ascii=False, indent=2))
    print(json.dumps(experiments, ensure_ascii=False, indent=2))


if args.experiment or args.oracle:
    experiment(args.oracle)
    raise SystemExit(0)


# 両レイアウトに同じ連続信号を入れ、番組内容の違いを残差測定から除く。
rng = random.Random(2026092802)
pcm = []
for sample in range(TOTAL * FRAME):
    time = sample / RATE
    left = (0.08 * math.sin(2 * math.pi * 337 * time) + 0.035 * math.sin(2 * math.pi * 997 * time)
            + 0.02 * math.sin(2 * math.pi * 1723 * time) + 0.01 * rng.gauss(0, 1))
    right = (0.07 * math.sin(2 * math.pi * 541 * time) + 0.04 * math.sin(2 * math.pi * 1229 * time)
             + 0.025 * math.sin(2 * math.pi * 2017 * time) + 0.01 * rng.gauss(0, 1))
    pcm.append((left, right))
sources = {}
full_decoded = {}
for channels in (2, 6):
    raw = output / f"continuous-{channels}.f32"
    values = array.array("f", (value for row in pcm for value in (row if channels == 2 else (*row, 0, 0, 0, 0))))
    raw.write_bytes(values.tobytes())
    path = output / f"continuous-{channels}.aac"
    run(["ffmpeg", "-v", "error", "-y", "-f", "f32le", "-ar", str(RATE), "-ac", str(channels),
         "-i", str(raw), "-c:a", "aac", "-b:a", str(192000 if channels == 2 else 384000), "-f", "adts", str(path)])
    sources[channels] = split_adts(path.read_bytes())
    full_decoded[channels] = decode(path, channels)
    assert len(sources[channels]) >= TOTAL

selected = sources[6][:PREFIX] + sources[2][PREFIX:PREFIX + CONVERT] + sources[6][PREFIX + CONVERT:TOTAL]
source = output / "seam-input.aac"
source.write_bytes(b"".join(selected))
refs = output / "seam.refs"
refs.write_text("".join(f"{index} {int(PREFIX <= index < PREFIX + CONVERT)}\n" for index in range(TOTAL)))
result_path = output / "seam-output.aac"
with (output / "builder.log").open("w") as log:
    subprocess.run([str(binary), str(source), str(refs), str(result_path), "6"],
                   stdout=log, stderr=subprocess.STDOUT, check=True)
actual_frames = split_adts(result_path.read_bytes())
assert len(actual_frames) == TOTAL
assert actual_frames[:PREFIX] == selected[:PREFIX] and actual_frames[-SUFFIX:] == selected[-SUFFIX:]
actual = decode(result_path, 6)
assert len(actual) == TOTAL * FRAME
run(["ffmpeg", "-v", "error", "-y", "-f", "aac", "-i", str(result_path), str(output / "seam-output.wav")])

# AAC初回エンコードの1フレーム遅延を原PCM基準へ加える。
original_pcm = [(0.0, 0.0)] * FRAME + pcm[:(TOTAL - 1) * FRAME]
full_surround = [row[:2] for row in full_decoded[6][:TOTAL * FRAME]]
full_layout_source = ([row[:2] for row in full_decoded[6][:PREFIX * FRAME]]
                      + [row[:2] for row in full_decoded[2][PREFIX * FRAME:(PREFIX + CONVERT) * FRAME]]
                      + [row[:2] for row in full_decoded[6][(PREFIX + CONVERT) * FRAME:TOTAL * FRAME]])
reset_source = []
for name, channels, begin, end in (("prefix", 6, 0, PREFIX), ("convert", 2, PREFIX, PREFIX + CONVERT),
                                  ("suffix", 6, PREFIX + CONVERT, TOTAL)):
    path = output / (name + "-reset-reference.aac")
    path.write_bytes(b"".join(sources[channels][begin:end]))
    reset_source += [row[:2] for row in decode(path, channels)]
references = {"原PCM_遅延1024補正": original_pcm, "元5.1全長デコード": full_surround,
              "各元レイアウト全長デコード": full_layout_source, "各区間デコーダリセット": reset_source}
intervals = {"CONVERT内部": ((PREFIX + 4) * FRAME, (PREFIX + CONVERT - 4) * FRAME),
             "開始境界±1024": ((PREFIX - 1) * FRAME, (PREFIX + 1) * FRAME),
             "終了境界±1024": ((PREFIX + CONVERT - 1) * FRAME, (PREFIX + CONVERT + 1) * FRAME),
             "開始前1frame": ((PREFIX - 1) * FRAME, PREFIX * FRAME),
             "開始後1frame": (PREFIX * FRAME, (PREFIX + 1) * FRAME),
             "終了前1frame": ((PREFIX + CONVERT - 1) * FRAME, (PREFIX + CONVERT) * FRAME),
             "終了後1frame": ((PREFIX + CONVERT) * FRAME, (PREFIX + CONVERT + 1) * FRAME)}


def rms(values):
    return math.sqrt(sum(value * value for value in values) / len(values))


results = {}
for label, reference in references.items():
    stats = {}
    for region, (begin, end) in intervals.items():
        residual = [actual[index][channel] - reference[index][channel] for index in range(begin, end) for channel in (0, 1)]
        signal = [reference[index][channel] for index in range(begin, end) for channel in (0, 1)]
        error = rms(residual)
        stats[region] = {"residual_rms": error, "reference_rms": rms(signal),
                         "actual_rms": rms([actual[index][channel] for index in range(begin, end) for channel in (0, 1)]),
                         "residual_peak": max(map(abs, residual)),
                         "residual_dbfs": 20 * math.log10(error) if error else None}
    interior = stats["CONVERT内部"]["residual_rms"]
    for region in ("開始境界±1024", "終了境界±1024"):
        stats[region]["boundary_to_interior_rms_ratio"] = stats[region]["residual_rms"] / interior
    results[label] = stats

# 内部で原PCMに対するずれを測り、外部AACの1024遅延補正を独立に確認する。
begin, end = intervals["CONVERT内部"]
correlations = []
for lag in range(-64, 65):
    a = [original_pcm[index][0] for index in range(begin, end)]
    b = [actual[index + lag][0] for index in range(begin, end)]
    correlations.append(sum(x * y for x, y in zip(a, b)) / math.sqrt(sum(x * x for x in a) * sum(y * y for y in b)))
lag = correlations.index(max(correlations)) - 64
report = {"frames": TOTAL, "convert_frames": CONVERT, "sample_rate": RATE,
          "boundaries_seconds": [PREFIX * FRAME / RATE, (PREFIX + CONVERT) * FRAME / RATE],
          "lag_samples": lag, "correlation": max(correlations), "measurements": results}
(output / "results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2))
assert lag == 0, "原PCM基準でサンプルずれが発生しました: " + str(lag)
print(json.dumps(report, ensure_ascii=False, indent=2))
