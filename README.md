# brtusb — Linux driver for BARROT BRTLink Bluetooth USB adapters

[![Build brtusb](https://github.com/zhuzhuzihan/brtusb/actions/workflows/build.yml/badge.svg)](https://github.com/zhuzhuzihan/brtusb/actions/workflows/build.yml)

Out-of-tree Linux kernel driver for the **BARROT BRTLink Bluetooth USB
adapters** (Bluetooth 5.4 CSR BlueCore-compatible dongles), reverse-engineered
from the vendor's Windows driver package
("BARROT Bluetooth 5.4 USB Adapter Driver V1.1").

## Supported hardware

| USB ID | Device |
|---|---|
| `33FA:0001` | BRTLink Bluetooth dongle |
| `33FA:0010` | BRTLink Bluetooth 5.4 dongle |

## Highlights

* Standard Bluetooth HCI USB (H:2) transport: HCI commands via the default
  endpoint, events via interrupt IN, ACL via bulk, SCO via isochronous
  endpoints with automatic alternate-setting switching
* Faithful reproduction of the vendor driver's private behaviour:
  * interception of the `EE EE 01 02 03 04 05` magic HCI command
    ("Switch to HID mode command found!")
  * vendor HID-mode switch control transfer, also exposed as the
    `/sys/.../hid_switch` sysfs attribute
* No firmware file required — the kernel driver never downloads firmware;
  the vendor stack performs all patching through HCI vendor commands
* Version-adaptive: builds against kernels from 6.1 up to and including 7.0
  (handles both `hci_dev->notify` API generations and the 7.0
  `hci_set_quirk()` change)
* CI: clang/LLVM builds across 6.1 / 6.6 / 6.12 / 6.17 via GitHub Actions

## Quick start

```sh
git clone https://github.com/zhuzhuzihan/brtusb.git
cd brtusb/linux
make -C /lib/modules/$(uname -r)/build M=$PWD modules   # add LLVM=1 for clang
sudo make -C /lib/modules/$(uname -r)/build M=$PWD modules_install
sudo depmod -a
sudo modprobe brtusb
```

Details, module parameters and the HID-mode switch usage are documented in
[linux/README.md](linux/README.md).

## Documentation

* [linux/README.md](linux/README.md) — usage and the Linux mapping of the
  Windows driver behaviour
* [linux/reverse_engineering_notes.c](linux/reverse_engineering_notes.c) —
  pseudocode reconstruction of the two vendor-specific routines with binary
  addresses (`BTCUSB_SendHCICommand`, `BTCUSB_HCI2HID`, endpoint wiring)

## Legal

* This project contains **original reverse-engineered code only**. The
  proprietary BARROT/BRLink Windows binaries (`brtlinkusb*.sys`, `btmgr.exe`,
  the MSI/CAB payloads) are *not* included and *not* redistributed.
* Firmware patching is performed by the Bluetooth host stack (BlueZ) through
  HCI vendor commands; no vendor firmware is shipped in this repository.
* See [LICENSE](LICENSE) (GPL-3.0) for the license of this code.

## Acknowledgements

Development was **AI-assisted**: static analysis (objdump/radare2 disassembly,
function-level string mapping) was done manually, and an LLM was used to help
reconstruct the pseudo-code, write the driver, and set up CI. All conclusions
were cross-checked against the binary (URB function codes, control-transfer
parameters, byte-level magic values).

This project is promoted and tested in the [LINUX DO](https://linux.do)
community — thanks to the 佬友 for testing and feedback. 💙

Note: as a Linux kernel module this driver links against GPL-2.0-only kernel
symbols; `MODULE_LICENSE("GPL")` reflects the kernel-facing license. The
project source itself is provided under GPL-3.0 at the author's discretion.

## License

GPL-3.0 — see [LICENSE](LICENSE).
