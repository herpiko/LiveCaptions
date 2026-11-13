#!/bin/sh
# Builds Live Captions with the sherpa-onnx speech recognition backend and
# installs it, with its speech models, system-wide.
#
# Usage:  ./install.sh                  (installs under /usr/local)
#         PREFIX=/opt/lc ./install.sh   (installs somewhere else)
#
# Run it as your normal user: it asks for your password (sudo) only for the
# final install step. Downloads (about 1.5 GB, mostly models) are kept in
# ~/.cache/livecaptions-install so running it again is quick.
set -eu

cd "$(dirname "$0")"
ROOT=$(pwd)
PREFIX=${PREFIX:-/usr/local}
# Outside the source tree on purpose: meson drops include paths that point
# into the source tree from the CMake-built April subproject, which then
# can't find the ONNX Runtime headers.
DEPS="${LIVECAPTIONS_DEPS:-${XDG_CACHE_HOME:-$HOME/.cache}/livecaptions-install}"
BUILD="$ROOT/build"

SHERPA_VERSION=v2.0.1
SHERPA_NAME=sherpa-onnx-$SHERPA_VERSION-linux-x64-shared
SHERPA_URL=https://github.com/k2-fsa/sherpa-onnx/releases/download/$SHERPA_VERSION/$SHERPA_NAME.tar.bz2
ORT_NAME=onnxruntime-linux-x64-1.14.1
ORT_URL=https://github.com/microsoft/onnxruntime/releases/download/v1.14.1/$ORT_NAME.tgz
MODELS_URL=https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models

# Live models, plus Parakeet for refining finished lines
DEFAULT_MODEL=sherpa-onnx-nemotron-3.5-asr-streaming-0.6b-560ms-int8-2026-06-11
SHERPA_MODELS="$DEFAULT_MODEL
sherpa-onnx-nemo-streaming-fast-conformer-transducer-en-480ms-int8
sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8"
APRIL_MODEL=april-english-dev-01110_en.april
APRIL_URL=https://april.sapples.net/$APRIL_MODEL

LIBDIR="$PREFIX/lib/livecaptions"
MODELDIR="$PREFIX/share/livecaptions/models"

say() { printf '\n==> %s\n' "$*"; }

if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ]; then
    echo "Please run ./install.sh without sudo; it asks for your password when it installs." >&2
    exit 1
fi
SUDO=""
[ "$(id -u)" -eq 0 ] || SUDO=sudo

for tool in curl tar meson ninja cmake pkg-config cc; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Please install '$tool' first." >&2; exit 1; }
done

# fetch <url> <file>: download unless already there
fetch() {
    [ -f "$2" ] && return 0
    echo "Downloading $(basename "$2")"
    curl -fL --retry 3 -o "$2.part" "$1"
    mv "$2.part" "$2"
}

mkdir -p "$DEPS/dl" "$DEPS/models"

say "Getting sherpa-onnx $SHERPA_VERSION"
if [ ! -f "$DEPS/$SHERPA_NAME/lib/libsherpa-onnx-c-api.so" ]; then
    fetch "$SHERPA_URL" "$DEPS/dl/$SHERPA_NAME.tar.bz2"
    tar -xjf "$DEPS/dl/$SHERPA_NAME.tar.bz2" -C "$DEPS"
fi

# April (the original engine) is compiled against the ONNX Runtime 1.14
# headers but linked to the newer runtime that ships with sherpa-onnx, so the
# app loads a single ONNX Runtime.
say "Preparing ONNX Runtime"
if [ ! -f "$DEPS/ort/include/onnxruntime_c_api.h" ]; then
    fetch "$ORT_URL" "$DEPS/dl/$ORT_NAME.tgz"
    tar -xzf "$DEPS/dl/$ORT_NAME.tgz" -C "$DEPS/dl"
    rm -rf "$DEPS/ort"
    mkdir -p "$DEPS/ort"
    cp -r "$DEPS/dl/$ORT_NAME/include" "$DEPS/ort/include"
fi
mkdir -p "$DEPS/ort/lib"
cp "$DEPS/$SHERPA_NAME/lib/libonnxruntime.so" "$DEPS/ort/lib/"

say "Getting speech models"
for m in $SHERPA_MODELS; do
    if [ ! -f "$DEPS/models/$m/tokens.txt" ]; then
        fetch "$MODELS_URL/$m.tar.bz2" "$DEPS/dl/$m.tar.bz2"
        tar -xjf "$DEPS/dl/$m.tar.bz2" -C "$DEPS/models"
        rm -f "$DEPS/dl/$m.tar.bz2"
    fi
done
fetch "$APRIL_URL" "$DEPS/models/$APRIL_MODEL"

say "Building"
if [ -d .git ] || [ -f .git ]; then
    git submodule update --init --recursive
fi

export ONNX_ROOT="$DEPS/ort"
set -- --prefix="$PREFIX" --buildtype=release \
    -Dsherpa_onnx_dir="$DEPS/$SHERPA_NAME" \
    -Dmodel_path="$MODELDIR/$DEFAULT_MODEL/tokens.txt" \
    -Dc_link_args="-Wl,-rpath,$LIBDIR"
if [ -f "$BUILD/build.ninja" ]; then
    meson setup --reconfigure "$BUILD" "$@"
else
    meson setup "$BUILD" "$@"
fi
ninja -C "$BUILD"

say "Installing to $PREFIX (may ask for your password)"
$SUDO meson install -C "$BUILD" --no-rebuild

# Private runtime libraries; the app's RUNPATH points here
$SUDO install -d "$LIBDIR"
$SUDO rm -f "$LIBDIR"/libonnxruntime.so*
$SUDO install -m 644 "$DEPS/$SHERPA_NAME/lib/libonnxruntime.so" \
                     "$DEPS/$SHERPA_NAME/lib/libsherpa-onnx-c-api.so" "$LIBDIR/"

# Speech models (only what the app loads, not the sample audio)
$SUDO install -d "$MODELDIR"
$SUDO install -m 644 "$DEPS/models/$APRIL_MODEL" "$MODELDIR/"
for m in $SHERPA_MODELS; do
    $SUDO install -d "$MODELDIR/$m"
    for f in "$DEPS/models/$m"/*.onnx "$DEPS/models/$m/tokens.txt"; do
        $SUDO install -m 644 "$f" "$MODELDIR/$m/"
    done
done

say "Done. Quit Live Captions completely and start it again to use the new version."
