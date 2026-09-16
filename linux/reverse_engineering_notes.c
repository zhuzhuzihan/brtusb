// ==== FUNC BTCUSB_SendHCICommand @ 0x23cd0 (decompiled)
// (Ghidra not available for arm64 native decompiler at analysis time;
//  recovered from objdump -d .text, addresses are RVA+ImageBase 0x10000)

NTSTATUS BTCUSB_SendHCICommand(PDEVICE_EXTENSION dx, // rcx -> [rsp+0xf0]
                               PVOID  ctrl,          // rdx -> [rsp+0xf8] (HCI cmd ctx, buffer @ +0x18)
                               ULONG  len)           // r8  -> [rsp+0x100]
{
    UCHAR pattern[7] = { 0xEE, 0xEE, 0x01, 0x02, 0x03, 0x04, 0x05 };
    NTSTATUS status = 0;

    if (ctrl->Buffer == NULL || len == 0)
        return STATUS_INVALID_PARAMETER;              // 0xC000000D

    if (memcmp(ctrl->Buffer, pattern, 7) == 0) {      // call 0x31d72
        // "Switch to HID mode command found!"
        dx->HidSwitchPending = 1;                     // *(ULONG*)((char*)dx + 0x360) = 1
        return STATUS_SUCCESS;                        // command is swallowed
    }

    // URB_FUNCTION_CLASS_DEVICE (0x1A), Length 0x88
    urb = ExAllocatePoolWithTag(NonPagedPool, 0x88, 'Wdm ');
    if (!urb) return STATUS_INSUFFICIENT_RESOURCES;   // 0xC000009A

    urb->Hdr.Length   = 0x88;
    urb->Hdr.Function = 0x1A;                         // class request on default pipe
    urb->TransferBufferLength = len;                  // [rsp+0x100]
    urb->TransferBufferMDL    = NULL;
    urb->UsbdDeviceHandle     = ctrl->DeviceObject;   // ctx->0x18
    urb->TransferFlags        = 0;                    // OUT
    // SetupPacket zeroed except wLength = len (class-specific H:2 HCI command)

    status = BTCUSB_CallUSBD(dx, urb);                // 0x115a0
    ExFreePool(urb);

    if (status == 0xC000009C) {                       // USB_STATUS_DEVICE_STALL
        // "resetting pipe" -> CSRBC01_ResetPipe (0x12550) on the control pipe
        BTCUSB_ResetPipe(dx, dx->PipeHandle[?]);
    }
    return status;
}

// ==== FUNC BTCUSB_HCI2HID @ 0x23b30
NTSTATUS BTCUSB_HCI2HID(PDEVICE_EXTENSION dx)   // arg kept at [rsp+0x80]
{
    // URB_FUNCTION_VENDOR_DEVICE (0x17), Length 0x88
    urb = ExAllocatePoolWithTag(NonPagedPool, 0x88, 'Wdm ');
    if (!urb) return STATUS_INSUFFICIENT_RESOURCES;

    urb->Hdr.Length   = 0x88;
    urb->Hdr.Function = 0x17;
    urb->TransferFlags        = 0;     // OUT
    urb->TransferBufferLength = 0;     // no data stage
    urb->TransferBuffer       = NULL;
    urb->TransferBufferMDL    = NULL;
    // SetupPacket: 00 00 01 00 00 00 00 00
    //   bmRequestType = 0x00 (host->device, vendor, device)
    //   bRequest      = 0x00
    //   wValue        = 0x0001
    //   wIndex        = 0x0000
    //   wLength       = 0x0000
    status = BTCUSB_CallUSBD(dx, urb);
    ExFreePool(urb);
    return status;
}

// ==== IOCTL_BTCUSB_HCI_HID_SWITCH_COMMAND handler (inside dispatcher @ 0x298f8)
//   BTCUSB_AbortPipes-ish call (0x127e0), then:
//   if (dx->HidSwitchPending) {          // +0x360
//        dx->HidSwitchPending = 0;
//        BTCUSB_HCI2HID(dx);             // 0x23b30
//   }

// ==== Endpoint/pipe wiring (BTCUSB_ConfigureDevice @ 0x11c80)
//   - USBD_ParseConfigurationDescriptorEx(cfg, cfg, i, 0, -1, -1, -1) per
//     interface (AlternateSetting 0), USBD_CreateConfigurationRequestEx,
//     USBD_SelectConfiguration
//   - URB functions used across the driver:
//       0x0 select-config, 0x1 select-interface (SCO altsetting),
//       0x2 abort-pipe, 0x9 bulk/interrupt (event in, acl in/out),
//       0xA isoch (sco in/out), 0xB get-descriptor,
//       0xD/0x10 set/clear feature (remote wakeup),
//       0x13 get-status, 0x17 vendor-device (HID switch),
//       0x1A class-device (HCI command)
//   - ISO transfer flags: USBD_START_ISO_TRANSFER_ASAP (0x40) |
//       USBD_ISO_START_FRAME_RANGE (0x80) | USBD_SHORT_TRANSFER_OK (0x20)

// ==== Data path completions
//   CSRBC01_Interrupt_Complete (0x27ab0)  -> HCI event   -> BTUSB_ProcessEventPacket
//   CSRBC01_HCIData_Complete    (0x28bb0)  -> ACL rx     -> BTUSB_ProcessBulkPacket
//   cusb_iso_read_complete      (0x2bac0)  -> SCO rx
//   On USBD_STATUS_CANCELED / device error: pipes aborted, queues flushed
