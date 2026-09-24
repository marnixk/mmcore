#!/usr/bin/env bash
# Regenerate the README showcase GIFs from the recorded MP4 clips.
#
# GitHub's Markdown sanitizer strips <video> tags from READMEs, so the demos
# are animated GIFs (which autoplay inline); the MP4s stay next to them for
# click-through and for anyone who wants the original quality.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="${1:-${REPO_ROOT}/docs/showcase}"
FPS="${FPS:-12}"
WIDTH="${WIDTH:-640}"

command -v ffmpeg >/dev/null || { echo "ffmpeg not found" >&2; exit 1; }

shopt -s nullglob
for mp4 in "${DIR}"/*.mp4; do
	gif="${mp4%.mp4}.gif"
	echo ">> ${mp4##*/} -> ${gif##*/}"
	ffmpeg -v error -y -i "${mp4}" \
		-vf "fps=${FPS},scale=${WIDTH}:-1:flags=lanczos,split[s0][s1];[s0]palettegen=stats_mode=diff[p];[s1][p]paletteuse=dither=none" \
		-loop 0 "${gif}"
done
