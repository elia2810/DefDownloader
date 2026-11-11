#!/bin/sh
# Gets the command-line tools that package-mac.sh ships inside the app and
# puts them in vendor/mac/ (not in git):
#
#   yt-dlp       official standalone build, checked against the release's SHA-256 list
#   spotdl       official standalone build
#   instaloader  no official macOS build exists: built here from PyPI with PyInstaller
#
# ffmpeg and ffprobe come from Homebrew and are handled by package-mac.sh.
# Needs curl and python3 (set PYTHON to use a specific interpreter).
set -eu
cd "$(dirname "$0")"

OUT=vendor/mac
mkdir -p "$OUT"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "== yt-dlp"
BASE="https://github.com/yt-dlp/yt-dlp/releases/latest/download"
curl -fL --progress-bar -o "$TMP/yt-dlp_macos" "$BASE/yt-dlp_macos"
curl -fsSL -o "$TMP/SHA2-256SUMS" "$BASE/SHA2-256SUMS"
expected="$(awk '$2 == "yt-dlp_macos" { print $1 }' "$TMP/SHA2-256SUMS")"
actual="$(shasum -a 256 "$TMP/yt-dlp_macos" | awk '{ print $1 }')"
if [ -z "$expected" ] || [ "$expected" != "$actual" ]; then
    echo "error: checksum mismatch for yt-dlp_macos (expected '$expected', got '$actual')" >&2
    exit 1
fi
mv "$TMP/yt-dlp_macos" "$OUT/yt-dlp"
chmod +x "$OUT/yt-dlp"

echo "== spotdl"
# The asset name contains the version, so it is looked up through the API.
url="$(curl -fsSL https://api.github.com/repos/spotDL/spotify-downloader/releases/latest \
    | python3 -c 'import json, sys
assets = json.load(sys.stdin)["assets"]
print(next(a["browser_download_url"] for a in assets if a["name"].endswith("-darwin")))')"
curl -fL --progress-bar -o "$TMP/spotdl" "$url"
mv "$TMP/spotdl" "$OUT/spotdl"
chmod +x "$OUT/spotdl"

echo "== instaloader"
BUILD="$OUT/.instaloader-build"
rm -rf "$BUILD"
mkdir -p "$BUILD"
"${PYTHON:-python3}" -m venv "$BUILD/venv"
"$BUILD/venv/bin/pip" install --quiet --disable-pip-version-check instaloader pyinstaller
printf 'from instaloader.__main__ import main\n\nmain()\n' > "$BUILD/run_instaloader.py"
"$BUILD/venv/bin/pyinstaller" --onefile --name instaloader --log-level WARN --noconfirm \
    --distpath "$OUT" --workpath "$BUILD/work" --specpath "$BUILD" "$BUILD/run_instaloader.py"
rm -rf "$BUILD"

echo "Ready in $OUT:"
for tool in yt-dlp spotdl instaloader; do
    printf '  %-12s %s\n' "$tool" "$(shasum -a 256 "$OUT/$tool" | awk '{ print $1 }')"
done
