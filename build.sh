#!/usr/bin/env bash
# drosaka Voice — one-line build: ./build.sh
# Builds AU + VST3 (Release) and installs them into
#   ~/Library/Audio/Plug-Ins/Components   (AU)
#   ~/Library/Audio/Plug-Ins/VST3         (VST3)
# Then rescan plug-ins in Live (Settings > Plug-Ins > Rescan).
set -euo pipefail
cd "$(dirname "$0")"

JUCE_TAG="8.0.4"

# --- toolchain checks (Milestone 0) -----------------------------------------
if ! xcode-select -p >/dev/null 2>&1; then
  echo "Xcode command line tools missing: run  xcode-select --install"; exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
  if command -v brew >/dev/null 2>&1; then
    echo "Installing CMake via Homebrew..."; brew install cmake
  else
    echo "CMake missing and Homebrew not found. Install Homebrew (https://brew.sh) then re-run."; exit 1
  fi
fi
if [ ! -f JUCE/CMakeLists.txt ]; then
  echo "Cloning JUCE $JUCE_TAG..."
  git clone --depth 1 --branch "$JUCE_TAG" https://github.com/juce-framework/JUCE.git JUCE
fi

# --- build --------------------------------------------------------------------
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j "$(sysctl -n hw.ncpu)"

# Make macOS forget the old AU so Live sees the new build.
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true

echo
echo "Installed:"
ls -d ~/Library/Audio/Plug-Ins/Components/"drosaka Voice.component" ~/Library/Audio/Plug-Ins/VST3/"drosaka Voice.vst3" 2>/dev/null || true
echo
echo "Optional AU check:  auval -v aumu DrVc Sfrq"
echo "Then in Live: Settings > Plug-Ins > Rescan."
