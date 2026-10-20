#!/usr/bin/env bash
# Copies ios/ to the Mac, builds it there, installs on the iPhone and launches the app.
# Usage: scripts/ios_deploy.sh [setup|build|install|run]   (default: run = build + install + launch)
#   setup — one-time: installs the LaunchAgent that signs builds inside the GUI session.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MAC="${QBT_MAC:-qbtmac}"
REMOTE_DIR="QBTCameraStudio/ios"
DEVICE="${QBT_DEVICE:-00008120-00181D063A98201E}" # iPhone 14 Pro Max
BUNDLE_ID="com.qbt.camerastudio.P9Z5K52DB3"
APP="build/Build/Products/Release-iphoneos/QBTCamera.app"
AGENT_LABEL="com.qbt.camerastudio.buildagent"
STEP="${1:-run}"

if [ "$STEP" = setup ]; then
    echo "==> Install build agent"
    tar -C "$ROOT/ios/Tools" -cf - . | ssh "$MAC" "mkdir -p ~/QBTCameraStudio/agent ~/QBTCameraStudio/.agent \
        ~/Library/LaunchAgents && tar -C ~/QBTCameraStudio/agent -xf - \
        && sed \"s|__HOME__|\$HOME|g\" ~/QBTCameraStudio/agent/$AGENT_LABEL.plist > ~/Library/LaunchAgents/$AGENT_LABEL.plist \
        && (launchctl bootout gui/\$(id -u)/$AGENT_LABEL 2>/dev/null || true) \
        && launchctl bootstrap gui/\$(id -u) ~/Library/LaunchAgents/$AGENT_LABEL.plist && echo agent ready"
    exit 0
fi

echo "==> Sync sources to $MAC:~/$REMOTE_DIR"
tar -C "$ROOT/ios" --exclude=build --exclude=xcuserdata -cf - . |
    ssh "$MAC" "mkdir -p ~/$REMOTE_DIR && cd ~/$REMOTE_DIR \
        && find . -mindepth 1 -maxdepth 1 ! -name build -exec rm -rf {} + && tar -xf -"

echo "==> Build (via agent in GUI session)"
ssh "$MAC" 'S=~/QBTCameraStudio/.agent; rm -f $S/status; echo '"$DEVICE"' > $S/request
    for i in $(seq 1 600); do [ -f $S/status ] && break; sleep 1; done
    cat $S/build.log 2>/dev/null | grep -vE "CoreSimulator|IDERunDestination" | tail -30
    [ -f $S/status ] || { echo "build agent did not respond (run: scripts/ios_deploy.sh setup)"; exit 1; }
    exit $(cat $S/status)'
[ "$STEP" = build ] && exit 0

echo "==> Install on $DEVICE"
ssh "$MAC" "cd ~/$REMOTE_DIR && xcrun devicectl device install app --device $DEVICE $APP"
[ "$STEP" = install ] && exit 0

echo "==> Launch"
ssh "$MAC" "xcrun devicectl device process launch --terminate-existing --device $DEVICE $BUNDLE_ID"
