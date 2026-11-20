# macos_usb_gadget_poc

This repository contains a proof-of-concept for interfacing with IOUSBDeviceFamily on ARM64 MacBooks. It implements a HID keyboard that can live type to external devices.

> [!NOTE]
> Fork of https://github.com/shinyquagsire23/macos_usb_gadget_poc , extended to send texts from a buffer and/or live type, including function keys.

> [!WARNING]
> I only made this fork because i just needed an external keyboard to get into the BIOS of an old computer and had no usb keyboard lying around. Do not use for any serious use cases. Shout out [@joacobottiroli](https://github.com/joacobottiroli) for volunteering a keyboard anyways, and [@NaPrado](https://github.com/NaPrado) for helping me test this.

Tested on an M2 Pro Macbook (macOS 27), against an unsuspecting Android Phone, and a linux machine.

**Requirements:**

 - SIP must be disabled
 - `usbgadget.kext` must be loaded

**Caveats:**

 - The original example hardcoded the right port on a 2021 MBP. This fork selects a `usb-drdN` controller in the app or with `--port`; the app marks controllers with a connected host. Controller IDs are not physical port labels.
 - This can break network sharing on the selected port after use. It's fixable via reboot, or by calling `IOUSBDeviceDescriptionAppendInterfaceToConfiguration` with all the original NCM functions.

**What does the kext do?**

 - `usbgadget.kext` is used to bypass an entitlement check in `IOUSBDeviceController::createUSBDevice` for `com.apple.private.usbdevice.setdescription`. This can normally be worked around via disabling AMFI, but disabling AMFI was extremely unstable on my system (Firefox would hang, Discord would fail to launch). This might have been able to be worked around by injecting into a process with the entitlement, but macOS does not use `lockdownd`.
 - Setting the property `PublishConfiguration` in `gay_bowser_usbgadget` will forward the dict to the parent `IOUSBController` (with `USBDeviceCommand=SetDeviceConfiguration`). Since this is done from a kernel thread, it bypasses the entitlement check.
 - There's a version that doesn't use the kext (ie, requires `amfi_get_out_of_my_way=1`) in the initial commit of the repo.

**Compiling/Running:**

 - Run `zsh load.sh` to install/load `usbgadget.kext`. You'll have to approve the kext once in System Security before it'll work, and macOS may require a restart.
   - If you want to build from scratch, there's an Xcode project.
 - `cd usb_device && make clean && make && ./usb_device --list` lists the available controllers. To send text, run `printf 'Hello\n' | ./usb_device --port usb-drd2` (replace the controller ID). Block sends accept up to 1024 bytes of US keyboard ASCII.
 - `zsh ui/build.sh` builds `ui/build/USB Gadget Keyboard.app`. In the app, choose a controller, start live typing, and click the capture area. The app marks connected controllers and auto-selects one when exactly one is connected. F1–F12, Delete, Backspace, and Escape support key down/up; the **Hold a BIOS key** controls keep resending a key until release. The receiving host's keyboard layout affects typed characters.
