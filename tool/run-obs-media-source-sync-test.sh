#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
OBS_ROOT=${OBS_ROOT:-/Users/mstarzak/work/obs-studio-32.1.2}
OBS_BUILD=${OBS_BUILD:-$OBS_ROOT/build_macos}
QT_DEPS=${QT_DEPS:-$OBS_ROOT/.deps/obs-deps-qt6-2025-08-23-universal}
OBS_DEPS=${OBS_DEPS:-$OBS_ROOT/.deps/obs-deps-2025-08-23-universal}
HARNESS="$SCRIPT_DIR/obs-media-source-sync-harness"
SOURCE="$SCRIPT_DIR/obs-media-source-sync-harness.cpp"

if [ ! -x "$HARNESS" ] || [ "$SOURCE" -nt "$HARNESS" ] || [ "$ROOT_DIR/src/sync-test-output.hpp" -nt "$HARNESS" ]; then
	xcrun clang++ -std=c++17 \
		-I"$ROOT_DIR" \
		-I"$OBS_ROOT/libobs" \
		-I"$OBS_ROOT/libobs/callback" \
		-I"$OBS_BUILD/config" \
		-I"$OBS_DEPS/include" \
		-I"$QT_DEPS/lib/QtWidgets.framework/Headers" \
		-I"$QT_DEPS/lib/QtGui.framework/Headers" \
		-I"$QT_DEPS/lib/QtCore.framework/Headers" \
		-F"$QT_DEPS/lib" \
		-F"$OBS_BUILD/libobs/RelWithDebInfo" \
		-framework libobs \
		-framework QtWidgets \
		-framework QtGui \
		-framework QtCore \
		-Wl,-rpath,"$OBS_BUILD/libobs/RelWithDebInfo" \
		-Wl,-rpath,"$OBS_BUILD/libobs-metal/RelWithDebInfo" \
		-Wl,-rpath,"$OBS_BUILD/libobs-opengl/RelWithDebInfo" \
		-Wl,-rpath,"$OBS_BUILD/frontend/api/RelWithDebInfo" \
		-Wl,-rpath,"$QT_DEPS/lib" \
		-Wl,-rpath,"$OBS_DEPS/lib" \
		-o "$HARNESS" "$SOURCE"
fi

export DYLD_FRAMEWORK_PATH="$OBS_BUILD/libobs/RelWithDebInfo${DYLD_FRAMEWORK_PATH:+:$DYLD_FRAMEWORK_PATH}"
export DYLD_LIBRARY_PATH="$OBS_BUILD/libobs-metal/RelWithDebInfo:$OBS_BUILD/libobs-opengl/RelWithDebInfo:$OBS_BUILD/frontend/api/RelWithDebInfo:$QT_DEPS/lib:$OBS_DEPS/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"

exec "$HARNESS" \
	--obs-root "$OBS_ROOT" \
	--plugin "$ROOT_DIR/release-obs-32.1.2/obs-audio-video-sync-dock.plugin" \
	--media "$ROOT_DIR/release/av-offset-pattern-3000.mp4" \
	"$@"
