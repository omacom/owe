#!/bin/sh
# Container entry for test/e2e/run.sh. It builds owe from the read-only source
# tree at /src, runs both test suites, and plays video through the real daemon
# and renderer with desktop OpenGL and with OpenGL ES.
#
# With --build-only, it builds the binaries into $OWE_E2E_BIN and stops.
# test/e2e/run-gpu.sh uses this mode.
set -eu

src=/src
build=$HOME/build
bin=${OWE_E2E_BIN:-$HOME/bin}
# Code under QEMU runs slower than on native hardware, so the test time
# limits are 10 times longer by default.
multiplier=${OWE_E2E_TIMEOUT_MULTIPLIER:-10}

echo "== Machine: $(uname -m)"

echo "== Build"
meson setup "$build" "$src" --buildtype=release
ninja -C "$build"

# owed starts the owe-render binary that sits next to it, as after an install.
mkdir -p "$bin"
cp "$build/src/daemon/owed" "$build/src/render/owe-render" "$build/src/cli/owe" \
    "$build/test/owe-live-test" "$bin/"
if [ "${1:-}" = --build-only ]; then
    exit 0
fi

echo "== Test suite"
tests=$(meson test -C "$build" --no-rebuild --list | sed 's/^owe://')
if [ "${OWE_E2E_EMULATED:-0}" = 1 ]; then
    # The transition test counts frames in fixed time windows. QEMU is too slow
    # for those windows, also for the code before the OpenGL ES fallback. Native
    # hardware, such as the arm64 CI runner, runs the test.
    echo "Skipped under QEMU: transition"
    tests=$(printf '%s\n' "$tests" | grep -vx transition)
fi
meson test -C "$build" --print-errorlogs --timeout-multiplier "$multiplier" $tests

echo "== Lock feed plugin"
cmake -S "$src/qml-plugin" -B "$HOME/build-qml" -DBUILD_TESTING=ON
cmake --build "$HOME/build-qml"
ctest --test-dir "$HOME/build-qml" --output-on-failure

echo "== Playback with desktop OpenGL"
python3 "$src/test/e2e/e2e.py" "$bin" --api opengl
echo "== Playback with OpenGL ES"
python3 "$src/test/e2e/e2e.py" "$bin" --api gles

echo "== All checks passed on $(uname -m)"
