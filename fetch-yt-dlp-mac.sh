#!/bin/sh
# Downloads the official standalone yt-dlp for macOS into vendor/mac/yt-dlp,
# checked against the SHA-256 list of the same release. package-mac.sh then
# ships it inside the app.
#
#   ./fetch-yt-dlp-mac.sh             latest release
#   ./fetch-yt-dlp-mac.sh 2026.10.26  a specific release
set -eu
cd "$(dirname "$0")"

TAG="${1:-latest}"
if [ "$TAG" = latest ]; then
    BASE="https://github.com/yt-dlp/yt-dlp/releases/latest/download"
else
    BASE="https://github.com/yt-dlp/yt-dlp/releases/download/$TAG"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
curl -fL --progress-bar -o "$TMP/yt-dlp_macos" "$BASE/yt-dlp_macos"
curl -fsSL -o "$TMP/SHA2-256SUMS" "$BASE/SHA2-256SUMS"

expected="$(awk '$2 == "yt-dlp_macos" { print $1 }' "$TMP/SHA2-256SUMS")"
actual="$(shasum -a 256 "$TMP/yt-dlp_macos" | awk '{ print $1 }')"
if [ -z "$expected" ] || [ "$expected" != "$actual" ]; then
    echo "error: checksum mismatch for yt-dlp_macos (expected '$expected', got '$actual')" >&2
    exit 1
fi

mkdir -p vendor/mac
mv "$TMP/yt-dlp_macos" vendor/mac/yt-dlp
chmod +x vendor/mac/yt-dlp
echo "vendor/mac/yt-dlp ready (sha256 $actual)"
