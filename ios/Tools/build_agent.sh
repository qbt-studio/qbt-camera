#!/bin/bash
# Runs as a LaunchAgent inside the logged-in GUI session, where the login keychain is
# available for code signing (over plain SSH codesign fails with errSecInternalComponent).
# Triggered by writing the target device UDID to ~/QBTCameraStudio/.agent/request;
# writes build.log and status.
STATE="$HOME/QBTCameraStudio/.agent"
[ -f "$STATE/request" ] || exit 0
DEVICE="$(tr -d '[:space:]' < "$STATE/request")"
rm -f "$STATE/request" "$STATE/status"

# A concrete device lets Xcode register it with the (free) team so the profile includes it.
DESTINATION="generic/platform=iOS"
[ -n "$DEVICE" ] && DESTINATION="platform=iOS,id=$DEVICE"

cd "$HOME/QBTCameraStudio/ios" || { echo 1 > "$STATE/status"; exit 1; }
xcodebuild -project QBTCamera.xcodeproj -scheme QBTCamera -configuration Release \
    -destination "$DESTINATION" -derivedDataPath build \
    -allowProvisioningUpdates -allowProvisioningDeviceRegistration -quiet build > "$STATE/build.log" 2>&1
echo $? > "$STATE/status"
