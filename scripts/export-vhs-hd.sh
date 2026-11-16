#!/bin/bash
set -euo pipefail

# Edit these defaults or set the variables for a single export.
scale=${VHS_SCALE:-2}
preset=${QTGMC_PRESET:-Slower}
profile=${PRORES_PROFILE:-standard}

usage() {
    cat <<EOF
Usage: $0 INTERLACED_CAPTURE OUTPUT.mov

VHS_SCALE       2 for NNEDI3 doubling (default)
NO_SCALE        Set to 1 to keep capture dimensions, overriding VHS_SCALE
QTGMC_PRESET    QTGMC preset name (default Slower)
PRORES_PROFILE  proxy, lt, standard (default), or hq

The output keeps the capture's pixel aspect ratio at either size.
EOF
}

if [[ $# -eq 1 && "$1" == --help ]]; then
    usage
    exit 0
fi
if [[ $# -ne 2 ]]; then
    usage >&2
    exit 2
fi
case "${NO_SCALE:-0}" in
    0) ;;
    1) scale=1 ;;
    *) echo "NO_SCALE must be 0 or 1." >&2; exit 2 ;;
esac
if [[ "$scale" != 1 && "$scale" != 2 ]]; then
    echo "VHS_SCALE must be 1 or 2." >&2
    exit 2
fi
case "$profile" in
    proxy|lt|standard|hq) ;;
    *) echo "PRORES_PROFILE must be proxy, lt, standard, or hq." >&2; exit 2 ;;
esac

script_dir=$(cd "$(dirname "$0")" && pwd)
master=$1
output=$2
[[ "$master" = /* ]] || master="$PWD/$master"
[[ "$output" = /* ]] || output="$PWD/$output"
if [[ -e "$output" || -L "$output" ]]; then
    echo "Output already exists: $output" >&2
    exit 1
fi

field_order=$(ffprobe -v error -select_streams v:0 -show_entries stream=field_order -of csv=p=0 "$master")
if [[ "$field_order" != tt ]]; then
    echo "Expected a top-field-first interlaced USB-Live2 capture." >&2
    exit 1
fi

# Uniform scaling preserves display geometry when the original pixel ratio is carried into MOV.
source_sar=$(ffprobe -v error -select_streams v:0 -show_entries stream=sample_aspect_ratio -of csv=p=0 "$master")
if [[ ! "$source_sar" =~ ^[1-9][0-9]*:[1-9][0-9]*$ ]]; then
    echo "Could not read the capture's pixel aspect ratio." >&2
    exit 1
fi
pixel_aspect="${source_sar%:*}/${source_sar#*:}"

# Y4M starts video at zero. Restore its original start against the unchanged audio timeline.
# Round the offset once to keep every frame on the same 60000/1001 grid.
# shortcut: Video runs at nominal NTSC cadence, use source timecodes if long-run drift matters.
video_start=$(ffprobe -v error -select_streams v:0 -show_entries stream=start_time -of csv=p=0 "$master")
if [[ ! "$video_start" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "Could not read the capture's video start timestamp." >&2
    exit 1
fi

# A failed VapourSynth render can leave a readable prefix. Publish only after both processes succeed.
temp_dir=$(mktemp -d "$(dirname "$output")/.vhs-hd.XXXXXX")
trap 'rm -rf -- "$temp_dir"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# A workspace installation can sit alongside any user-supplied PYTHONPATH.
PYTHONPATH="${PYTHONPATH:+$PYTHONPATH:}$script_dir/../.qtgmc" \
    vspipe --arg "source=$master" --arg "scale=$scale" --arg "preset=$preset" \
        --container y4m --progress "$script_dir/vhs-qtgmc.vpy" - |
    ffmpeg -hide_banner -nostats -nostdin -xerror -copyts -f yuv4mpegpipe -i - -i "$master" \
        -map 0:v:0 -map 1:a:0 \
        -vf "setsar=sar=${pixel_aspect}:max=2147483647,colorspace=iall=smpte170m:irange=tv:all=bt709:range=tv:format=yuv422p10,settb=1/60000,setpts=PTS-STARTPTS+round(${video_start}/TB)" \
        -enc_time_base:v filter -fps_mode:v passthrough \
        -c:v prores_ks -profile:v "$profile" -pix_fmt yuv422p10le \
        -c:a copy -video_track_timescale 60000 -movie_timescale 60000 -movflags +faststart \
        "$temp_dir/export.mov"

ln -h "$temp_dir/export.mov" "$output"
echo "Saved $output"
