#!/usr/bin/env python3
"""Verify a live recording or compare a diagnostic Matroska export with its raw samples."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def require(condition, message):
    """Stop verification with a readable error instead of reporting a partial check as success."""
    if not condition:
        raise SystemExit(message)


def probe(path, *args):
    """Read the muxer's stream, packet, or decoded-frame observations as JSON."""
    return json.loads(subprocess.check_output([
        "ffprobe", "-v", "error", "-of", "json", *args, str(path),
    ]))


def compare_decoded(path, raw, stream, output_args):
    """Compare incrementally so verification needs neither a second raw file nor a large buffer."""
    command = ["ffmpeg", "-nostdin", "-v", "warning", "-xerror", "-err_detect", "crccheck+explode",
               "-i", str(path), "-map", stream, *output_args, "pipe:1"]
    digest = hashlib.sha256()
    offset = 0
    # The FFV1 decoder can log a CRC mismatch and still return success.
    with (raw.open("rb") as source, tempfile.TemporaryFile() as errors,
          subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors) as process):
        while reference := source.read(1024 * 1024):
            if process.stdout.read(len(reference)) != reference:
                process.kill()
                raise SystemExit(f"{raw.name}: decoded bytes differ in block starting at byte {offset}.")
            digest.update(reference)
            offset += len(reference)
        extra = process.stdout.read(1)
        if extra:
            process.kill()
        require(not extra and process.wait() == 0, f"{raw.name}: extra data or decode failure.")
        errors.seek(0)
        require(b"crc mismatch" not in errors.read().lower(), f"{raw.name}: FFV1 slice CRC mismatch.")
    print(f"{raw.name}: {offset} bytes match, SHA-256 {digest.hexdigest()}")


def check_pts(packets, indices, clock, shift):
    """Check each written PTS against the fitted host time within half a Matroska tick."""
    for packet, index in zip(packets, indices):
        expected = (clock["start_ns"] + index * clock["step_ns"] + shift) / 1_000_000
        require(abs(int(packet["pts"]) - expected) <= 0.501,
                f"Packet timestamp differs from the capture clock at index {index}.")


def check_stream_hash(path, stream, output_args, byte_count, expected):
    """Decode with passthrough cadence and compare the bytes to the live recorder's digest."""
    command = ["ffmpeg", "-nostdin", "-v", "warning", "-xerror", "-err_detect", "crccheck+explode",
               "-i", str(path), "-map", stream, *output_args, "pipe:1"]
    digest = hashlib.sha256()
    size = 0
    with tempfile.TemporaryFile() as errors, subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=errors
    ) as process:
        while chunk := process.stdout.read(1024 * 1024):
            size += len(chunk)
            digest.update(chunk)
        # stderr shares a file offset with the child; let it finish before rewinding the log.
        result = process.wait()
        errors.seek(0)
        require(result == 0 and b"crc mismatch" not in errors.read().lower(),
                f"{stream}: decode or slice CRC failure.")
    require(size == byte_count and digest.hexdigest() == expected,
            f"{stream}: decoded bytes differ from the capture report.")
    print(f"{stream}: {size} decoded bytes match the capture SHA-256 {expected}")


def check_live(directory, report):
    """Verify the advertised codec's guarantees, including finalized partial recordings."""
    require(report.get("finalized") and report.get("video_frames_submitted")
            and report.get("audio_samples_submitted"),
            f"Capture has no finalized audio and video to verify ({report.get('stop_reason', 'unknown')}).")
    profile = report.get("profile", "ffv1")
    # An unknown profile used to skip both hash checks and could be reported as verified.
    require(profile in ("ffv1", "prores", "h264", "h264-hw", "raw"),
            f"Unknown capture profile: {profile!r}.")
    media = report.get("media", report.get("matroska"))
    require(isinstance(media, str) and media and Path(media).name == media,
            "Capture report must name a media file in the capture directory.")
    path = directory / media
    if profile in ("ffv1", "raw"):
        check_stream_hash(path, "0:v:0", [
            "-fps_mode", "passthrough", "-pix_fmt", "yuyv422", "-f", "rawvideo",
        ], report["video_frames_submitted"] * 720 * 480 * 2, report["submitted_yuyv_sha256"])
    else:
        # Lossy video has no decoded hash match. Decode both tracks before checking the PCM.
        subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-xerror", "-i", str(path),
                        "-map", "0:v:0", "-map", "0:a:0", "-fps_mode", "passthrough",
                        "-f", "null", "-"], check=True)
        print(f"{profile}: both tracks decode successfully.")
    if profile in ("ffv1", "prores", "raw"):
        check_stream_hash(path, "0:a:0", ["-c:a", "pcm_s16le", "-f", "s16le"],
                          report["audio_samples_submitted"] * 4, report["submitted_pcm_sha256"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    directory = parser.parse_args().directory
    report = directory / "capture.json"
    if report.is_file():
        check_live(directory, json.loads(report.read_text()))
        return
    path = directory / "capture.mkv"
    timing = json.loads((directory / "timing.json").read_text())
    mkv = timing.get("matroska", {})
    require(timing["complete"] and mkv.get("written"), "The capture has no complete Matroska output.")

    video, audio = probe(path, "-show_streams")["streams"]
    require(video["codec_name"] == "ffv1" and video["pix_fmt"] == "yuv422p"
            and video["field_order"] == "tt" and video["avg_frame_rate"] == "30000/1001"
            and (video["width"], video["height"]) == (720, 480), "Unexpected video track metadata.")
    require(audio["codec_name"] == "pcm_s16le" and audio["sample_rate"] == "48000"
            and audio["channels"] == 2, "Unexpected audio track metadata.")
    require(video["time_base"] == audio["time_base"] == "1/1000", "Unexpected Matroska timestamp scale.")

    frames = probe(path, "-select_streams", "v:0", "-show_frames", "-show_entries",
                   "frame=pts,interlaced_frame,top_field_first")["frames"]
    require(len(frames) == len(timing["video_fields"]) and all(
        frame["interlaced_frame"] == 1 and frame["top_field_first"] == 1 for frame in frames
    ), "Decoded video frames lost interlace flags or changed count.")
    check_pts(frames, range(0, 2 * len(frames), 2), mkv["video_field_clock"], mkv["timestamp_shift_ns"])

    packets = probe(path, "-select_streams", "a:0", "-show_packets", "-show_entries",
                    "packet=pts,size")["packets"]
    sample = 0
    indices = []
    for packet in packets:
        size = int(packet["size"])
        require(size > 0 and size % 4 == 0, "PCM packet ends inside a stereo sample.")
        indices.append(sample)
        sample += size // 4
    require(sample == sum(block["samples"] for block in timing["audio_blocks"]), "PCM sample count changed.")
    check_pts(packets, indices, mkv["audio_sample_clock"], mkv["timestamp_shift_ns"])
    print(f"Verified timestamp mapping, {len(frames)} interlaced frames, and {sample} stereo samples.")

    compare_decoded(path, directory / "video.yuyv", "0:v:0", [
        "-fps_mode", "passthrough", "-pix_fmt", "yuyv422", "-f", "rawvideo",
    ])
    compare_decoded(path, directory / "audio.s16le", "0:a:0", ["-c:a", "pcm_s16le", "-f", "s16le"])


if __name__ == "__main__":
    main()
