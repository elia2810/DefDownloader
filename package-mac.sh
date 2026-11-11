#!/bin/sh
# Builds DefDownloader and produces a self-contained dist/DefDownloader.app and dist/DefDownloader-macos-arm64.dmg
# that run on Macs without Qt installed.
set -eu
cd "$(dirname "$0")"

QT_PREFIX="${QT_PREFIX:-$(brew --prefix qt)}"
QT_PLUGINS="$("$QT_PREFIX/bin/qtpaths" --query QT_INSTALL_PLUGINS)"
APP=dist/DefDownloader.app

# Same deployment target as Qt, so the app itself never raises the minimum
# macOS version (the bundled Homebrew libraries decide it).
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=14.4
cmake --build build

# Work on a copy: macdeployqt rewrites the bundle, the build tree stays usable.
rm -rf dist
mkdir dist
cp -R build/DefDownloader.app "$APP"

# Homebrew ships Qt as separate formulae, so macdeployqt cannot resolve the
# optional plugins (svg, pdf, virtual keyboard). A Widgets app only needs the
# platform and style plugins: copy those and let macdeployqt bundle their
# dependencies.
mkdir -p "$APP/Contents/PlugIns/platforms" "$APP/Contents/PlugIns/styles" "$APP/Contents/Resources"
cp "$QT_PLUGINS/platforms/libqcocoa.dylib" "$APP/Contents/PlugIns/platforms/"
cp "$QT_PLUGINS/styles/libqmacstyle.dylib" "$APP/Contents/PlugIns/styles/"
printf '[Paths]\nPlugins = PlugIns\n' > "$APP/Contents/Resources/qt.conf"

# Tools shipped inside the app, used when the Mac has none installed.
# ffmpeg and ffprobe are the Homebrew ones (macdeployqt bundles their
# libraries like Qt's); yt-dlp, spotdl and instaloader are standalone
# executables prepared by fetch-tools-mac.sh.
BIN="$APP/Contents/MacOS"
for tool in ffmpeg ffprobe; do
    cp "$(brew --prefix ffmpeg)/bin/$tool" "$BIN/$tool"
    chmod u+w "$BIN/$tool"
done
for tool in yt-dlp spotdl instaloader; do
    if [ -f "vendor/mac/$tool" ]; then
        cp "vendor/mac/$tool" "$BIN/$tool"
        chmod u+wx "$BIN/$tool"
    else
        echo "warning: vendor/mac/$tool missing, $tool is not bundled (run ./fetch-tools-mac.sh)" >&2
    fi
done

"$QT_PREFIX/bin/macdeployqt" "$APP" -no-plugins -no-codesign \
    -executable="$APP/Contents/PlugIns/platforms/libqcocoa.dylib" \
    -executable="$APP/Contents/PlugIns/styles/libqmacstyle.dylib" \
    -executable="$BIN/ffmpeg" \
    -executable="$BIN/ffprobe"

# macdeployqt leaves @rpath references between Homebrew libraries untouched
# (libbrotlidec -> libbrotlicommon): point them at the copies in the bundle.
for lib in "$APP"/Contents/Frameworks/*.dylib; do
    install_name_tool -id "@executable_path/../Frameworks/$(basename "$lib")" "$lib" 2>/dev/null
    otool -L "$lib" | awk 'NR > 1 && $1 ~ /^@rpath\// { print $1 }' | while read -r dep; do
        name="$(basename "$dep")"
        if [ -f "$APP/Contents/Frameworks/$name" ]; then
            install_name_tool -change "$dep" "@loader_path/$name" "$lib" 2>/dev/null
        fi
    done
done

# Nothing in the bundle may still point at the local Homebrew install.
if find "$APP" -type f \( -name '*.dylib' -o -perm -u+x \) ! -name yt-dlp ! -name spotdl ! -name instaloader -exec otool -L {} + 2>/dev/null \
        | grep -E '^\s+(/opt/homebrew|/usr/local|@rpath)'; then
    echo "error: the bundle still depends on libraries outside of it" >&2
    exit 1
fi

# Ad-hoc signature: required to run on Apple Silicon. It is not a Developer ID
# signature, so other Macs will still ask for confirmation on first launch.
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"

# Disk image with the usual drag-to-Applications layout.
STAGE="$(mktemp -d)"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname DefDownloader -srcfolder "$STAGE" -ov -format UDZO dist/DefDownloader-macos-arm64.dmg >/dev/null
rm -rf "$STAGE"

echo "Created $APP and dist/DefDownloader-macos-arm64.dmg"
