#!/bin/sh
# setup_macos.sh — Generate a minimal, single-target Xcode project
# for babelstreamer-filter, meant for manual Developer ID signing and notarization
# via Xcode itself (Product > Archive, then Organizer > Distribute App >
# Developer ID).
#
# Set CODESIGN_IDENT / CODESIGN_TEAM before running if you want the project
# generated with Manual signing already pointed at your Developer ID
# Application certificate, e.g.:
#   export CODESIGN_IDENT="Developer ID Application: <Name> (F298DQ53KB)"
#   export CODESIGN_TEAM="F298DQ53KB"
# Otherwise you can just pick the identity/team in Xcode's Signing & Capabilities.
#
# Requirements:
#   • Xcode command-line tools (xcode-select --install)
#   • CMake 3.28+ (brew install cmake)
#   • whisper.cpp built at ~/Projects/whisper/  (see ../Client/README for details)
#
# After running this script, open the project with:
#   open build_macos/babelstreamer-filter.xcodeproj

set -e
cd "$(dirname "$0")"

echo "==> Configuring babelstreamer-filter (macOS Universal, Xcode)"
cmake --preset macos

echo ""
echo "==> Done. Opening Xcode..."
open build_macos/babelstreamer-filter.xcodeproj
