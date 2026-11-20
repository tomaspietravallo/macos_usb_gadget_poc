#!/bin/zsh
set -euo pipefail

repo_dir=${0:A:h}
source_kext="$repo_dir/usbgadget.kext"
installed_kext="/Library/Extensions/usbgadget.kext"

if [[ ! -f "$source_kext/Contents/MacOS/usbgadget" ]]; then
    print -u2 -- "Missing bundled usbgadget.kext"
    exit 1
fi

sudo ditto "$source_kext" "$installed_kext"
sudo chown -R root:wheel "$installed_kext"
sudo kmutil load -p "$installed_kext"

print -- "Load requested. Approve the extension in System Settings if prompted,"
print -- "restart, then run: kmutil showloaded | grep gay.bowser.usbgadget"
