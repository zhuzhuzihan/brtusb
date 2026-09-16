# brtusb — BARROT BRTLink Bluetooth USB 驱动 (Linux)

从 Windows 驱动包 `BARROT Bluetooth 5.4 USB Adapter Driver V1.1` 逆向移植的
Linux 内核驱动。

## 逆向结论摘要

### 设备与驱动组成

| 文件 | 说明 |
|---|---|
| `amd64/brtlinkusb_54.sys` | HCI USB 传输驱动 (PID `33FA:0010`, BT5.4) |
| `amd64/brtlinkusb.sys`    | 同一份代码 (PID `33FA:0001`, 旧款) |
| `Packet/brbtusb.exe`      | BRLink 用户态辅助程序 (Delphi) |
| `BRLink.cab` → `btmgr.exe` 等 | 完整用户态蓝牙协议栈 |

内核驱动源文件名(二进制内嵌 PDB 路径):
`btusb/usbdrv54/csrbc01_usb.c` — 基于 CSR "csrbc01" DDK 示例移植,
`bttl.ini` 中 `Manufacture=Cambridge Silicon Radio` 证实为 CSR BlueCore 兼容
芯片克隆。

### USB 传输 (标准 HCI H:2)

| HCI 包 | 通道 | Windows URB | 反汇编位置 |
|---|---|---|---|
| Command | EP0 类请求 `21 00 00 00 len 00` | `URB_FUNCTION_CLASS_DEVICE (0x1A)` | `0x23f04` |
| Event | Interrupt IN | `URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER (0x9)` | `0x27ab0` |
| ACL | Bulk IN/OUT | `(0x9)` | `0x28bb0` / `0x26ee0` |
| SCO | Isochronous IN/OUT | `(0xA)` + `SELECT_INTERFACE (0x1)` 切 altsetting | `0x2bac0`, `0x2fb91` |

内核驱动本身**不下载固件**;补丁全部由用户态栈 (btmgr.exe) 以 HCI vendor
命令下发。因此 Linux 侧无需固件文件,BlueZ 负责协议栈。

### 厂商私有行为 (已完整逆向)

1. **HID 模式切换魔数** — `BTCUSB_SendHCICommand (0x23cd0)`:
   所有 HCI 命令先与 7 字节魔数 `EE EE 01 02 03 04 05` 比较,命中则
   **不发送到设备**,置标志 `devext+0x360 = 1`,直接返回成功
   (日志 "Switch to HID mode command found!")。

2. **HID 切换控制传输** — `BTCUSB_HCI2HID (0x23b30)`:
   收到 `IOCTL_BTCUSB_HCI_HID_SWITCH_COMMAND` (dispatcher `0x298f8`) 后
   清标志并发送 EP0 vendor 控制传输:

   ```
   bmRequestType = 0x00   (host→device, vendor, device)
   bRequest      = 0x00
   wValue        = 0x0001
   wIndex        = 0x0000
   wLength       = 0
   ```

   让 dongle 退出 HCI 模式重新枚举为 HID 设备。

3. **DFU 接口** — 驱动创建第二设备对象 `CSRDFU%d` 暴露 CSR 风格 DFU 接口;
   Linux 侧无需内核支持 (usbfs/dfu-util 可直接访问)。

### IOCTL 一览 (用户态接口,仅文档意义)

`SEND_HCI_COMMAND / SEND_HCI_COMMAND_LEGACY / GET_HCI_EVENT / GET_HCI_DATA /
SEND_HCI_DATA (+LEGACY) / START_SCO_DATA / STOP_SCO_DATA / SEND_SCO_DATA /
RECV_SCO_DATA / SEND_CONTROL_TRANSFER / RESET_DEVICE / GET_DEVICE_DESCRIPTOR /
GET_CONFIG_DESCRIPTOR / GET_DRIVER_NAME / GET_VERSION / HCI_HID_SWITCH_COMMAND /
BLOCK_HCI_DATA / BLOCK_HCI_EVENT` — CTL_CODE 基址 `0x22xxxx`。

## 本驱动映射到 Linux 的设计

| Windows | Linux (brtusb.c) |
|---|---|
| EP0 类请求发 HCI Command | `usb_fill_control_urb` + setup `21 00 00 00` |
| 中断 IN 收 Event | `brtusb_intr_complete` 常驻 resubmit |
| Bulk 收发 ACL | `brtusb_bulk_complete` / waker |
| ISO 收发 SCO + SET_INTERFACE | `brtusb_isoc_work` 按 `hdev->notify` 切 altsetting |
| 魔数命令拦截 | `send_frame` 中检测 `EE EE 01 02 03 04 05` |
| `IOCTL_..._HID_SWITCH` | sysfs `hid_switch` 属性 |
| `IOCTL_RESET_DEVICE` | BlueZ HCI Reset (`HCI_QUIRK_RESET_ON_CLOSE`) |
| 选择性挂起 | `supports_autosuspend` + SCO 期间拒绝 suspend |

## 编译安装

```sh
cd linux
make -C /lib/modules/$(uname -r)/build M=$PWD modules
sudo make -C /lib/modules/$(uname -r)/build M=$PWD modules_install
sudo depmod -a
sudo modprobe brtusb
```

## 使用

插入 dongle 后:

```sh
$ hciconfig -a hci0        # 或 bluetoothctl list
$ bluetoothctl power on
```

HID 模式切换(会让设备重新枚举,需重新插拔才能回到 HCI 模式):

```sh
echo 1 | sudo tee /sys/bus/usb/drivers/brtusb/*/hid_switch
```

模块参数:

- `pass_magic_command=1` — 不拦截 `EE EE...` 魔数,原样下发设备
- `disable_sco=1` — 禁用 isochronous SCO 通道

## 与主线上游合流的说明

若希望直接用内核自带 `btusb`,只需在 `drivers/bluetooth/btusb.c` 的
`ID table` 增补:

```c
{ USB_DEVICE(0x33fa, 0x0001), .driver_info = BTUSB_CSR },
{ USB_DEVICE(0x33fa, 0x0010), .driver_info = BTUSB_CSR },
```

本独立驱动适合不想重编整树、或需要保留 HID 切换私有功能的场景。
