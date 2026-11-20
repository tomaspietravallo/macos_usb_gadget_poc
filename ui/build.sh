#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_dir=${script_dir:h}
app_dir="$script_dir/build/USB Gadget Keyboard.app"

make -C "$repo_dir/usb_device"
mkdir -p "$app_dir/Contents/MacOS" "$app_dir/Contents/Resources"
swiftc -parse-as-library "$script_dir/USBGadgetUI.swift" -o "$app_dir/Contents/MacOS/USBGadgetUI" -framework SwiftUI
cp "$repo_dir/usb_device/usb_device" "$app_dir/Contents/Resources/usb_device"
cat > "$app_dir/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>USBGadgetUI</string>
<key>CFBundleIdentifier</key><string>local.usbgadget.keyboard</string>
<key>CFBundleName</key><string>USB Gadget Keyboard</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>LSMinimumSystemVersion</key><string>13.0</string>
</dict></plist>
PLIST
codesign --force --deep --sign - "$app_dir"
print -r -- "$app_dir"
