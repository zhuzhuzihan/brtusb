/*
 * brtusb.c  -  Bluetooth HCI USB transport driver for BARROT BRLink adapters
 *
 *  Reverse-engineered from the Windows driver package
 *  "BARROT Bluetooth 5.4 USB Adapter Driver V1.1":
 *
 *    amd64/brtlinkusb_54.sys  (V1.0.0.10, PID 0x0010, BT 5.4 dongle)
 *    amd64/brtlinkusb.sys     (same code base, PID 0x0001, older dongle)
 *    source file names inside the binary: btusb/usbdrv54/csrbc01_*.c
 *
 *  Driver architecture on Windows
 *  ------------------------------
 *  brtlinkusb_54.sys is a pure HCI USB transport (H:2) in the style of the
 *  old CSR "csrbc01" DDK sample.  It selects USB configuration 0 and runs:
 *
 *    HCI Command  ->  default endpoint (EP0) class-specific request
 *                     bmRequestType 0x21, bRequest 0x00, wValue 0, wIndex 0
 *                     (URB_FUNCTION_CLASS_DEVICE @ 0x23f04)
 *    HCI Event    <-  interrupt IN endpoint
 *                     (CSRBC01_Interrupt_Complete @ 0x27ab0)
 *    ACL data     <-> bulk IN / bulk OUT endpoints
 *                     (CSRBC01_HCIData_Complete @ 0x28bb0,
 *                      BTUSB_ProcessBulkPacket @ 0x26ee0)
 *    SCO data     <-> isochronous IN / OUT endpoints; alternate setting is
 *                     switched with SET_INTERFACE depending on the number of
 *                     active SCO connections (URB_FUNCTION_SELECT_INTERFACE
 *                     sites @ 0x2fb91 and 0x206f6; ISO URBs use
 *                     USBD_START_ISO_TRANSFER_ASAP | USBD_ISO_START_FRAME_RANGE)
 *
 *  All Bluetooth profile logic and firmware patching is performed by the
 *  user-mode BRLink stack (btmgr.exe) over custom IOCTLs
 *  (IOCTL_BTCUSB_SEND_HCI_COMMAND / GET_HCI_EVENT / SEND_HCI_DATA /
 *  START_SCO_DATA / SEND_CONTROL_TRANSFER / RESET_DEVICE / ...).  The kernel
 *  driver downloads no firmware; vendor patches flow through standard HCI
 *  vendor commands from user space.  On Linux the in-kernel HCI transport
 *  plus BlueZ user space is the direct equivalent.
 *
 *  Vendor specific behaviour recovered from the binary
 *  ---------------------------------------------------
 *  1. BTCUSB_SendHCICommand (0x23cd0) compares every HCI command against
 *     the 7 byte magic  EE EE 01 02 03 04 05.  On a match the command is
 *     NOT forwarded to the device; the driver sets a "HID switch pending"
 *     flag (device extension +0x360) and returns success:
 *         "Switch to HID mode command found!"
 *
 *  2. IOCTL_BTCUSB_HCI_HID_SWITCH_COMMAND (dispatch @ 0x298f8) clears that
 *     flag and calls BTCUSB_HCI2HID (0x23b30), which issues a vendor
 *     control transfer on EP0:
 *
 *         bmRequestType 0x00   host-to-device, vendor, device recipient
 *         bRequest       0x00
 *         wValue         0x0001
 *         wIndex         0x0000
 *         wLength        0
 *
 *     ordering the dongle to leave HCI mode and re-enumerate as a USB HID
 *     device (BRLink HID / "light control" feature).
 *
 *  This driver implements the same transport for the Linux Bluetooth
 *  subsystem and reproduces the vendor hooks:
 *
 *    /sys/.../hid_switch     write 1 -> vendor HID mode switch control xfer
 *    HCI command EE EE 01 02 03 04 05 -> intercepted and translated into
 *    the same vendor control transfer (module parameter
 *    pass_magic_command=1 forwards the command to the device instead).
 *
 *  SPDX-License-Identifier: GPL-2.0
 *  Copyright (C) 2026  Reverse-engineered port of the BARROT BRLink driver
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/usb.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#define VERSION "1.1"

#include <linux/version.h>

/*
 * hci_dev->notify API history:
 *   <= 6.11 : void (*)(struct hci_dev *, unsigned int evt)
 *   6.12-6.17: void (*)(struct hci_dev *, unsigned int num_bands,
 *                      int air_mode)
 *   >= 7.0  : void (*)(struct hci_dev *, unsigned int evt)   (re-introduced)
 * HCI_QUIRK bitmap access:
 *   <= 6.17 : set_bit(HCI_QUIRK_*, &hdev->quirks)
 *   >= 7.0  : hci_set_quirk(hdev, HCI_QUIRK_*)
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0) && \
    LINUX_VERSION_CODE < KERNEL_VERSION(7, 0, 0)
#define BRTUSB_NOTIFY_BANDS	1
#else
#define BRTUSB_NOTIFY_BANDS	0
#endif

/* Framing: number of isochronous packets per SCO RX URB (same layout the
 * Windows driver uses when building the ISO read URBs) */
#define BRTUSB_MAX_ISOC_FRAMES	10

static bool pass_magic_command = false;
static bool disable_sco = false;

module_param(pass_magic_command, bool, 0644);
MODULE_PARM_DESC(pass_magic_command,
		 "Forward the EE EE 01 02 03 04 05 command to the device "
		 "instead of performing the HID mode switch");
module_param(disable_sco, bool, 0644);
MODULE_PARM_DESC(disable_sco, "Disable the isochronous SCO transport");

/* runtime state flags */
#define BRTUSB_INTR_RUNNING	0	/* interrupt IN pipe active   */
#define BRTUSB_BULK_RUNNING	1	/* bulk IN pipe active        */
#define BRTUSB_ISOC_RUNNING	2	/* isochronous pipes active   */
#define BRTUSB_SUSPENDED	3	/* device suspended           */

struct brtusb_data {
	struct hci_dev		*hdev;
	struct usb_device	*udev;
	struct usb_interface	*intf;
	struct usb_interface	*isoc;

	unsigned long		flags;

	struct work_struct	isoc_work;
	struct work_struct	waker;

	struct sk_buff_head	txq;

	struct usb_anchor	tx_anchor;
	struct usb_anchor	intr_anchor;
	struct usb_anchor	bulk_anchor;
	struct usb_anchor	isoc_anchor;

	struct usb_endpoint_descriptor *intr_ep;
	struct usb_endpoint_descriptor *bulk_tx_ep;
	struct usb_endpoint_descriptor *bulk_rx_ep;
	struct usb_endpoint_descriptor *isoc_tx_ep;
	struct usb_endpoint_descriptor *isoc_rx_ep;

	__u8			isoc_altsetting;
	unsigned int		sco_num;
	int			air_mode;
};

/* H:2 setup packet for HCI commands on EP0 (class request) */
static u8 __aligned(8) brtusb_cmd_setup[8] = {
	0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static int brtusb_send_hid_switch(struct brtusb_data *data);

/* USB identity recovered from brtlinkusb_54.inf:
 *   %BARROT_DriverDesc%=UsbDDI, USB\VID_33FA&PID_0010   (BT 5.4 dongle)
 *   (brtlinkusb.inf:                 USB\VID_33FA&PID_0001)
 */
static const struct usb_device_id brtusb_table[] = {
	{ USB_DEVICE(0x33fa, 0x0001) },		/* BRTLink BT dongle      */
	{ USB_DEVICE(0x33fa, 0x0010) },		/* BRTLink BT 5.4 dongle  */
	{ }
};
MODULE_DEVICE_TABLE(usb, brtusb_table);

/* ---------------------------------------------------------------------- */
/* TX completion                                                           */
/* ---------------------------------------------------------------------- */

static void brtusb_tx_complete(struct urb *urb)
{
	struct sk_buff *skb = urb->context;
	struct hci_dev *hdev = (struct hci_dev *)skb->dev;

	BT_DBG("%s urb %p status %d count %d", hdev->name, urb,
	       urb->status, urb->actual_length);

	if (urb->status && urb->status != -ENOENT &&
	    urb->status != -ECONNRESET && urb->status != -ESHUTDOWN)
		BT_ERR("%s tx urb failed with %d", hdev->name, urb->status);

	usb_free_urb(urb);
	kfree_skb(skb);
}

static int brtusb_submit_tx_urb(struct hci_dev *hdev, struct urb *urb)
{
	struct brtusb_data *data = hci_get_drvdata(hdev);
	int err;

	usb_anchor_urb(urb, &data->tx_anchor);

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err) {
		if (err != -EPERM && err != -ENODEV)
			BT_ERR("%s tx urb submit failed (%d)",
			       hdev->name, err);
		usb_unanchor_urb(urb);
	}

	usb_free_urb(urb);
	return err;
}

/* ---------------------------------------------------------------------- */
/* RX completions                                                          */
/* ---------------------------------------------------------------------- */

static void brtusb_intr_complete(struct urb *urb)
{
	struct hci_dev *hdev = urb->context;
	struct brtusb_data *data = hci_get_drvdata(hdev);
	struct sk_buff *skb;
	int err;

	BT_DBG("%s urb %p status %d count %d", hdev->name, urb,
	       urb->status, urb->actual_length);

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		return;

	switch (urb->status) {
	case 0:
		/* == BTUSB_ProcessEventPacket (event in) */
		if (urb->actual_length > 0) {
			hdev->stat.byte_rx += urb->actual_length;

			skb = bt_skb_alloc(urb->actual_length, GFP_ATOMIC);
			if (!skb) {
				hdev->stat.err_rx++;
				break;
			}
			skb->dev = (void *)hdev;
			memcpy(skb_put(skb, urb->actual_length),
			       urb->transfer_buffer, urb->actual_length);
			hci_skb_pkt_type(skb) = HCI_EVENT_PKT;

			hci_recv_frame(hdev, skb);
		} else {
			hdev->stat.err_rx++;
		}
		break;

	case -EOVERFLOW:
		BT_ERR("%s event urb overflow", hdev->name);
		hdev->stat.err_rx++;
		break;

	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		return;

	default:
		BT_ERR("%s event urb failed with %d",
		       hdev->name, urb->status);
		clear_bit(BRTUSB_INTR_RUNNING, &data->flags);
		break;
	}

	usb_mark_last_busy(data->udev);

	if (test_bit(BRTUSB_INTR_RUNNING, &data->flags)) {
		err = usb_submit_urb(urb, GFP_ATOMIC);
		if (err == -EPERM || err == -ENODEV)
			return;
		if (err) {
			BT_ERR("%s event urb resubmit failed (%d)",
			       hdev->name, err);
			clear_bit(BRTUSB_INTR_RUNNING, &data->flags);
		}
	}
}

static void brtusb_bulk_complete(struct urb *urb)
{
	struct hci_dev *hdev = urb->context;
	struct brtusb_data *data = hci_get_drvdata(hdev);
	struct sk_buff *skb;
	int err;

	BT_DBG("%s urb %p status %d count %d", hdev->name, urb,
	       urb->status, urb->actual_length);

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		return;

	switch (urb->status) {
	case 0:
		/* == BTUSB_ProcessBulkPacket (ACL in) */
		if (urb->actual_length > 0) {
			hdev->stat.byte_rx += urb->actual_length;

			skb = bt_skb_alloc(urb->actual_length, GFP_ATOMIC);
			if (!skb) {
				hdev->stat.err_rx++;
				break;
			}
			skb->dev = (void *)hdev;
			memcpy(skb_put(skb, urb->actual_length),
			       urb->transfer_buffer, urb->actual_length);
			hci_skb_pkt_type(skb) = HCI_ACLDATA_PKT;

			hci_recv_frame(hdev, skb);
		} else {
			hdev->stat.err_rx++;
		}
		break;

	case -EOVERFLOW:
		/* "Acl_In_Error>=1, reset acl incoming pipe" */
		BT_ERR("%s ACL urb overflow", hdev->name);
		hdev->stat.err_rx++;
		break;

	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		return;

	default:
		BT_ERR("%s ACL urb failed with %d", hdev->name, urb->status);
		clear_bit(BRTUSB_BULK_RUNNING, &data->flags);
		break;
	}

	if (test_bit(BRTUSB_BULK_RUNNING, &data->flags)) {
		err = usb_submit_urb(urb, GFP_ATOMIC);
		if (err == -EPERM || err == -ENODEV)
			return;
		if (err) {
			BT_ERR("%s bulk urb resubmit failed (%d)",
			       hdev->name, err);
			clear_bit(BRTUSB_BULK_RUNNING, &data->flags);
		}
	}
}

static void brtusb_isoc_rx_complete(struct urb *urb)
{
	struct hci_dev *hdev = urb->context;
	struct brtusb_data *data = hci_get_drvdata(hdev);
	struct sk_buff *skb;
	int i, err;

	BT_DBG("%s urb %p status %d count %d", hdev->name, urb,
	       urb->status, urb->actual_length);

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		return;

	if (urb->status == 0 && data->isoc_rx_ep) {
		for (i = 0; i < urb->number_of_packets; i++) {
			struct usb_iso_packet_descriptor *desc =
					urb->iso_frame_desc + i;

			if (desc->status || !desc->actual_length)
				continue;

			hdev->stat.byte_rx += desc->actual_length;

			skb = bt_skb_alloc(desc->actual_length, GFP_ATOMIC);
			if (!skb) {
				hdev->stat.err_rx++;
				continue;
			}
			skb->dev = (void *)hdev;
			memcpy(skb_put(skb, desc->actual_length),
			       urb->transfer_buffer + desc->offset,
			       desc->actual_length);
			hci_skb_pkt_type(skb) = HCI_SCODATA_PKT;

			hci_recv_frame(hdev, skb);
		}
	} else if (urb->status != -ENOENT &&
		   urb->status != -ECONNRESET &&
		   urb->status != -ESHUTDOWN) {
		/* "Read SCO cancelled or device removed" */
		BT_ERR("%s isoc rx urb failed with %d",
		       hdev->name, urb->status);
		clear_bit(BRTUSB_ISOC_RUNNING, &data->flags);
	}

	if (test_bit(BRTUSB_ISOC_RUNNING, &data->flags)) {
		err = usb_submit_urb(urb, GFP_ATOMIC);
		if (err == -EPERM || err == -ENODEV)
			return;
		if (err) {
			BT_ERR("%s isoc rx resubmit failed (%d)",
			       hdev->name, err);
			clear_bit(BRTUSB_ISOC_RUNNING, &data->flags);
		}
	}
}

static void brtusb_isoc_tx_complete(struct urb *urb)
{
	struct sk_buff *skb = urb->context;
	struct hci_dev *hdev = (struct hci_dev *)skb->dev;

	BT_DBG("%s urb %p status %d", hdev->name, urb, urb->status);

	if (urb->status && urb->status != -ENOENT &&
	    urb->status != -ECONNRESET && urb->status != -ESHUTDOWN)
		BT_ERR("%s isoc tx urb failed with %d",
		       hdev->name, urb->status);

	usb_free_urb(urb);
	kfree_skb(skb);
}

/* ---------------------------------------------------------------------- */
/* RX submission                                                           */
/* ---------------------------------------------------------------------- */

static int brtusb_submit_intr_urb(struct hci_dev *hdev, gfp_t mem_flags)
{
	struct brtusb_data *data = hci_get_drvdata(hdev);
	struct urb *urb;
	unsigned char *buf;
	unsigned int pipe;
	int err, size;

	if (!data->intr_ep)
		return -ENODEV;

	urb = usb_alloc_urb(0, mem_flags);
	if (!urb)
		return -ENOMEM;

	/* 0x100 byte event buffer in the Windows driver */
	size = le16_to_cpu(data->intr_ep->wMaxPacketSize);
	if (size < 16)
		size = 16;

	buf = kmalloc(size, mem_flags);
	if (!buf) {
		usb_free_urb(urb);
		return -ENOMEM;
	}

	pipe = usb_rcvintpipe(data->udev, data->intr_ep->bEndpointAddress);

	usb_fill_int_urb(urb, data->udev, pipe, buf, size,
			 brtusb_intr_complete, hdev,
			 data->intr_ep->bInterval);
	urb->transfer_flags |= URB_FREE_BUFFER;

	usb_anchor_urb(urb, &data->intr_anchor);

	err = usb_submit_urb(urb, mem_flags);
	if (err < 0) {
		if (err != -EPERM && err != -ENODEV)
			BT_ERR("%s intr urb submit failed (%d)",
			       hdev->name, err);
		usb_unanchor_urb(urb);
	}

	usb_free_urb(urb);
	return err;
}

static int brtusb_submit_bulk_urb(struct hci_dev *hdev, gfp_t mem_flags)
{
	struct brtusb_data *data = hci_get_drvdata(hdev);
	struct urb *urb;
	unsigned char *buf;
	unsigned int pipe;
	int err, size;

	if (!data->bulk_rx_ep)
		return -ENODEV;

	urb = usb_alloc_urb(0, mem_flags);
	if (!urb)
		return -ENOMEM;

	size = le16_to_cpu(data->bulk_rx_ep->wMaxPacketSize);
	buf = kmalloc(size, mem_flags);
	if (!buf) {
		usb_free_urb(urb);
		return -ENOMEM;
	}

	pipe = usb_rcvbulkpipe(data->udev, data->bulk_rx_ep->bEndpointAddress);

	usb_fill_bulk_urb(urb, data->udev, pipe, buf, size,
			  brtusb_bulk_complete, hdev);
	urb->transfer_flags |= URB_FREE_BUFFER;

	usb_mark_last_busy(data->udev);
	usb_anchor_urb(urb, &data->bulk_anchor);

	err = usb_submit_urb(urb, mem_flags);
	if (err < 0) {
		if (err != -EPERM && err != -ENODEV)
			BT_ERR("%s bulk urb submit failed (%d)",
			       hdev->name, err);
		usb_unanchor_urb(urb);
	}

	usb_free_urb(urb);
	return err;
}

static int brtusb_submit_isoc_rx(struct hci_dev *hdev)
{
	struct brtusb_data *data = hci_get_drvdata(hdev);
	struct urb *urb;
	unsigned char *buf;
	unsigned int pipe;
	int i, size, err;

	if (!data->isoc_rx_ep)
		return -ENODEV;

	urb = usb_alloc_urb(BRTUSB_MAX_ISOC_FRAMES, GFP_KERNEL);
	if (!urb)
		return -ENOMEM;

	size = le16_to_cpu(data->isoc_rx_ep->wMaxPacketSize);

	buf = kmalloc(size * BRTUSB_MAX_ISOC_FRAMES, GFP_KERNEL);
	if (!buf) {
		usb_free_urb(urb);
		return -ENOMEM;
	}

	pipe = usb_rcvisocpipe(data->udev, data->isoc_rx_ep->bEndpointAddress);

	urb->dev      = data->udev;
	urb->pipe     = pipe;
	urb->context  = hdev;
	urb->complete = brtusb_isoc_rx_complete;
	urb->interval = data->isoc_rx_ep->bInterval;

	urb->transfer_flags  = URB_ISO_ASAP | URB_FREE_BUFFER;
	urb->transfer_buffer = buf;
	urb->transfer_buffer_length = size * BRTUSB_MAX_ISOC_FRAMES;

	urb->number_of_packets = BRTUSB_MAX_ISOC_FRAMES;
	for (i = 0; i < BRTUSB_MAX_ISOC_FRAMES; i++) {
		urb->iso_frame_desc[i].offset = size * i;
		urb->iso_frame_desc[i].length = size;
	}

	usb_anchor_urb(urb, &data->isoc_anchor);
	err = usb_submit_urb(urb, GFP_KERNEL);
	if (err) {
		if (err != -EPERM && err != -ENODEV)
			BT_ERR("%s isoc rx submit failed (%d)",
			       hdev->name, err);
		usb_unanchor_urb(urb);
	}
	usb_free_urb(urb);
	return err;
}

/* ---------------------------------------------------------------------- */
/* ISO alternate setting selection                                         */
/* ---------------------------------------------------------------------- */

static int brtusb_switch_altsetting(struct hci_dev *hdev, int alts)
{
	struct brtusb_data *data = hci_get_drvdata(hdev);
	struct usb_interface *intf = data->isoc;
	int i, err;

	if (!intf || intf->num_altsetting == 1)
		return 0;

	if (alts < 0 || alts >= intf->num_altsetting)
		return -EINVAL;

	err = usb_set_interface(data->udev,
				intf->altsetting[0].desc.bInterfaceNumber,
				alts);
	if (err < 0) {
		BT_ERR("%s set interface alt %d failed (%d)",
		       hdev->name, alts, err);
		return err;
	}

	data->isoc_altsetting = alts;
	data->isoc_tx_ep = NULL;
	data->isoc_rx_ep = NULL;

	for (i = 0; i < intf->cur_altsetting->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *ep =
			&intf->cur_altsetting->endpoint[i].desc;

		if (usb_endpoint_is_isoc_out(ep))
			data->isoc_tx_ep = ep;
		else if (usb_endpoint_is_isoc_in(ep))
			data->isoc_rx_ep = ep;
	}

	if (!data->isoc_tx_ep || !data->isoc_rx_ep) {
		BT_ERR("%s missing SCO endpoints in alt %d",
		       hdev->name, alts);
		return -ENODEV;
	}

	return 0;
}

/* cusb_start_iso() / cusb_stop_iso() equivalent */
static void brtusb_isoc_work(struct work_struct *work)
{
	struct brtusb_data *data = container_of(work, struct brtusb_data,
						isoc_work);
	struct hci_dev *hdev = data->hdev;
	int new_alts = 0;
	int err, i;

	if (data->sco_num == data->isoc_altsetting)
		return;

	if (data->sco_num > 0)
		new_alts = 1;

	err = brtusb_switch_altsetting(hdev, new_alts);
	if (err < 0)
		return;

	if (data->sco_num > 0) {
		if (!test_and_set_bit(BRTUSB_ISOC_RUNNING, &data->flags)) {
			for (i = 0; i < 2; i++) {
				err = brtusb_submit_isoc_rx(hdev);
				if (err < 0) {
					clear_bit(BRTUSB_ISOC_RUNNING,
						  &data->flags);
					break;
				}
			}
		}
	} else {
		clear_bit(BRTUSB_ISOC_RUNNING, &data->flags);
		usb_kill_anchored_urbs(&data->isoc_anchor);
	}
}

#if BRTUSB_NOTIFY_BANDS
/* 6.12 - 6.17: num_bands carries the active SCO stream count */
static void brtusb_notify(struct hci_dev *hdev, unsigned int num_bands,
			  int air_mode)
{
	struct brtusb_data *data = hci_get_drvdata(hdev);

	BT_DBG("%s sco_num %u air_mode %d", hdev->name, num_bands, air_mode);

	if (num_bands == data->sco_num && air_mode == data->air_mode)
		return;

	data->sco_num = num_bands;
	data->air_mode = air_mode;

	schedule_work(&data->isoc_work);
}
#else
/* evt-based API (<= 6.11 and >= 7.0): derive the SCO count from the
 * connection hash, exactly like the old btusb_notify() and the
 * SELECT_INTERFACE logic of the Windows driver */
static void brtusb_notify(struct hci_dev *hdev, unsigned int evt)
{
	struct brtusb_data *data = hci_get_drvdata(hdev);
	unsigned int num;

	BT_DBG("%s evt %u", hdev->name, evt);

	num = hci_conn_num(hdev, SCO_LINK);
	if (num == data->sco_num)
		return;

	data->sco_num = num;
	data->air_mode = HCI_NOTIFY_ENABLE_SCO_CVSD;

	schedule_work(&data->isoc_work);
}
#endif

/* ---------------------------------------------------------------------- */
/* TX paths                                                                */
/* ---------------------------------------------------------------------- */

static struct urb *brtusb_alloc_isoc_tx_urb(struct hci_dev *hdev,
					    struct sk_buff *skb)
{
	struct brtusb_data *data = hci_get_drvdata(hdev);
	struct urb *urb;
	unsigned char *buf;
	unsigned int pipe;

	if (!data->isoc_tx_ep)
		return NULL;

	urb = usb_alloc_urb(1, GFP_ATOMIC);
	if (!urb)
		return NULL;

	buf = kmalloc(skb->len, GFP_ATOMIC);
	if (!buf) {
		usb_free_urb(urb);
		return NULL;
	}

	memcpy(buf, skb->data, skb->len);

	pipe = usb_sndisocpipe(data->udev, data->isoc_tx_ep->bEndpointAddress);

	urb->dev      = data->udev;
	urb->pipe     = pipe;
	urb->context  = skb;
	urb->complete = brtusb_isoc_tx_complete;
	urb->interval = data->isoc_tx_ep->bInterval;

	urb->transfer_flags  = URB_ISO_ASAP | URB_FREE_BUFFER;
	urb->transfer_buffer = buf;
	urb->transfer_buffer_length = skb->len;

	urb->number_of_packets = 1;
	urb->iso_frame_desc[0].offset = 0;
	urb->iso_frame_desc[0].length = skb->len;

	return urb;
}

static void brtusb_waker(struct work_struct *work)
{
	struct brtusb_data *data = container_of(work, struct brtusb_data,
						waker);
	struct hci_dev *hdev = data->hdev;
	struct sk_buff *skb;
	struct urb *urb = NULL;
	int err;

	while ((skb = skb_dequeue(&data->txq))) {
		switch (hci_skb_pkt_type(skb)) {
		case HCI_COMMAND_PKT:
			urb = usb_alloc_urb(0, GFP_ATOMIC);
			if (!urb)
				goto nomem;

			/* HCI command = class request on EP0, identical to
			 * the URB built at 0x23f04 (URB_FUNCTION_CLASS_DEVICE) */
			usb_fill_control_urb(urb, data->udev,
					     usb_sndctrlpipe(data->udev, 0),
					     (void *)brtusb_cmd_setup,
					     skb->data, skb->len,
					     brtusb_tx_complete, skb);
			urb->transfer_flags |= URB_FREE_BUFFER;
			break;

		case HCI_ACLDATA_PKT:
			if (!data->bulk_tx_ep) {
				kfree_skb(skb);
				continue;
			}

			urb = usb_alloc_urb(0, GFP_ATOMIC);
			if (!urb)
				goto nomem;

			usb_fill_bulk_urb(urb, data->udev,
					  usb_sndbulkpipe(data->udev,
					data->bulk_tx_ep->bEndpointAddress),
					  skb->data, skb->len,
					  brtusb_tx_complete, skb);
			urb->transfer_flags |= URB_FREE_BUFFER;
			break;

		case HCI_SCODATA_PKT:
			if (!data->isoc_tx_ep || disable_sco ||
			    data->sco_num == 0) {
				kfree_skb(skb);
				continue;
			}

			urb = brtusb_alloc_isoc_tx_urb(hdev, skb);
			if (!urb)
				goto nomem;
			break;

		default:
			kfree_skb(skb);
			continue;
		}

		/* strip the H:2 packet type prefix byte */
		skb_pull(skb, 1);

		err = brtusb_submit_tx_urb(hdev, urb);
		if (err) {
			/* urb was freed by submit helper */
			kfree_skb(skb);
			schedule_work(&data->waker);
			return;
		}
	}

	return;

nomem:
	/* requeue at the head and retry later; urb (if any) was freed */
	if (urb)
		usb_free_urb(urb);
	skb_queue_head(&data->txq, skb);
	schedule_work(&data->waker);
}

static int brtusb_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct brtusb_data *data = hci_get_drvdata(hdev);

	BT_DBG("%s", hdev->name);

	switch (hci_skb_pkt_type(skb)) {
	case HCI_COMMAND_PKT:
		/* Vendor hook recovered from BTCUSB_SendHCICommand (0x23cd0):
		 * magic 7 byte payload switches the dongle to HID mode */
		if (!pass_magic_command && skb->len >= 7 &&
		    !memcmp(skb->data + 1, "\xee\xee\x01\x02\x03\x04\x05", 7)) {
			BT_INFO("%s Switch to HID mode command found!",
				hdev->name);
			kfree_skb(skb);
			brtusb_send_hid_switch(data);
			return 0;
		}
		skb_queue_tail(&data->txq, skb);
		break;

	case HCI_ACLDATA_PKT:
		skb_queue_tail(&data->txq, skb);
		break;

	case HCI_SCODATA_PKT:
		if (data->isoc_tx_ep && !disable_sco)
			skb_queue_tail(&data->txq, skb);
		else
			kfree_skb(skb);
		break;

	default:
		kfree_skb(skb);
		return -EILSEQ;
	}

	schedule_work(&data->waker);
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Vendor: HID mode switch (BTCUSB_HCI2HID @ 0x23b30)                      */
/* ---------------------------------------------------------------------- */

static int brtusb_send_hid_switch(struct brtusb_data *data)
{
	struct hci_dev *hdev = data->hdev;
	int err;

	err = usb_control_msg_send(data->udev, 0,
				   0x00,		/* bRequest      */
				   0x00,		/* bmRequestType */
				   cpu_to_le16(0x0001),	/* wValue        */
				   cpu_to_le16(0x0000),	/* wIndex        */
				   NULL, 0, USB_CTRL_SET_TIMEOUT,
				   GFP_KERNEL);

	BT_INFO("%s HID mode switch sent (%d)", hdev->name, err);
	return err;
}

static ssize_t hid_switch_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct brtusb_data *data = usb_get_intfdata(intf);
	unsigned int val;
	int err;

	err = kstrtouint(buf, 0, &val);
	if (err)
		return err;

	if (val != 1)
		return -EINVAL;

	err = brtusb_send_hid_switch(data);
	if (err < 0)
		return err;

	return count;
}
static DEVICE_ATTR_WO(hid_switch);

static struct attribute *brtusb_attrs[] = {
	&dev_attr_hid_switch.attr,
	NULL,
};
static const struct attribute_group brtusb_attr_group = {
	.attrs = brtusb_attrs,
};
static const struct attribute_group *brtusb_groups[] = {
	&brtusb_attr_group,
	NULL,
};

/* ---------------------------------------------------------------------- */
/* hci_dev callbacks                                                       */
/* ---------------------------------------------------------------------- */

static int brtusb_open(struct hci_dev *hdev)
{
	struct brtusb_data *data = hci_get_drvdata(hdev);
	int err;

	BT_DBG("%s", hdev->name);

	err = brtusb_submit_intr_urb(hdev, GFP_KERNEL);
	if (err < 0)
		return err;

	set_bit(BRTUSB_INTR_RUNNING, &data->flags);

	err = brtusb_submit_bulk_urb(hdev, GFP_KERNEL);
	if (err < 0) {
		clear_bit(BRTUSB_INTR_RUNNING, &data->flags);
		usb_kill_anchored_urbs(&data->intr_anchor);
		return err;
	}

	set_bit(BRTUSB_BULK_RUNNING, &data->flags);

	return 0;
}

static int brtusb_close(struct hci_dev *hdev)
{
	struct brtusb_data *data = hci_get_drvdata(hdev);

	BT_DBG("%s", hdev->name);

	clear_bit(BRTUSB_INTR_RUNNING, &data->flags);
	clear_bit(BRTUSB_BULK_RUNNING, &data->flags);
	clear_bit(BRTUSB_ISOC_RUNNING, &data->flags);

	usb_kill_anchored_urbs(&data->intr_anchor);
	usb_kill_anchored_urbs(&data->bulk_anchor);
	usb_kill_anchored_urbs(&data->isoc_anchor);

	return 0;
}

static int brtusb_flush(struct hci_dev *hdev)
{
	struct brtusb_data *data = hci_get_drvdata(hdev);

	BT_DBG("%s", hdev->name);

	skb_queue_purge(&data->txq);
	usb_kill_anchored_urbs(&data->tx_anchor);

	return 0;
}

/* ---------------------------------------------------------------------- */
/* USB probe / disconnect / power management                               */
/* ---------------------------------------------------------------------- */

static int brtusb_probe(struct usb_interface *intf,
			const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct brtusb_data *data;
	struct hci_dev *hdev;
	int i, err;

	BT_DBG("intf %p id %p", intf, id);

	/* Only the standard Bluetooth wireless controller interface is an
	 * HCI device; the DFU / HID-mode interfaces that the Windows driver
	 * exposes as the separate "CSRDFU%d" device object are ignored. */
	if (intf->cur_altsetting->desc.bInterfaceClass != USB_CLASS_WIRELESS_CONTROLLER ||
	    intf->cur_altsetting->desc.bInterfaceSubClass != 1 ||
	    intf->cur_altsetting->desc.bInterfaceProtocol != 1)
		return -ENODEV;

	data = devm_kzalloc(&intf->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	for (i = 0; i < intf->cur_altsetting->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *ep =
			&intf->cur_altsetting->endpoint[i].desc;

		if (usb_endpoint_is_int_in(ep))
			data->intr_ep = ep;
		else if (usb_endpoint_is_bulk_out(ep))
			data->bulk_tx_ep = ep;
		else if (usb_endpoint_is_bulk_in(ep))
			data->bulk_rx_ep = ep;
	}

	if (!data->intr_ep || !data->bulk_tx_ep || !data->bulk_rx_ep) {
		BT_ERR("%s: required HCI endpoints not found",
		       udev->product ? udev->product : "BARROT BT");
		return -ENODEV;
	}

	data->udev = udev;
	data->intf = intf;

	INIT_WORK(&data->isoc_work, brtusb_isoc_work);
	INIT_WORK(&data->waker, brtusb_waker);
	skb_queue_head_init(&data->txq);

	init_usb_anchor(&data->tx_anchor);
	init_usb_anchor(&data->intr_anchor);
	init_usb_anchor(&data->bulk_anchor);
	init_usb_anchor(&data->isoc_anchor);

	usb_set_intfdata(intf, data);

	/* Locate the isochronous interface used for SCO (the driver parses
	 * every interface of the active configuration, looking for the one
	 * with alternate settings) */
	for (i = 0; i < udev->actconfig->desc.bNumInterfaces; i++) {
		struct usb_interface *ifc = usb_ifnum_to_if(udev, i);

		if (!ifc || ifc == intf)
			continue;

		if (ifc->num_altsetting > 1) {
			data->isoc = usb_get_intf(ifc);
			break;
		}
	}

	hdev = hci_alloc_dev();
	if (!hdev) {
		err = -ENOMEM;
		goto err_free_isoc;
	}

	data->hdev = hdev;
	hdev->bus = HCI_USB;
	hci_set_drvdata(hdev, data);

	/* CSR BlueCore clones need the reset-on-close quirk: the Windows
	 * stack always reinitialises the transport on close */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
	hci_set_quirk(hdev, HCI_QUIRK_RESET_ON_CLOSE);
#else
	set_bit(HCI_QUIRK_RESET_ON_CLOSE, &hdev->quirks);
#endif

	hdev->open  = brtusb_open;
	hdev->close = brtusb_close;
	hdev->flush = brtusb_flush;
	hdev->send  = brtusb_send_frame;

	if (data->isoc && !disable_sco)
		hdev->notify = brtusb_notify;

	err = hci_register_dev(hdev);
	if (err < 0)
		goto err_free_hdev;

	usb_enable_autosuspend(udev);

	BT_INFO("BARROT BRLink Bluetooth USB adapter (bcdDevice %04x) registered",
		le16_to_cpu(udev->descriptor.bcdDevice));

	return 0;

err_free_hdev:
	hci_free_dev(hdev);
err_free_isoc:
	if (data->isoc)
		usb_put_intf(data->isoc);
	usb_set_intfdata(intf, NULL);
	return err;
}

static void brtusb_disconnect(struct usb_interface *intf)
{
	struct brtusb_data *data = usb_get_intfdata(intf);
	struct hci_dev *hdev;

	BT_DBG("intf %p", intf);

	if (!data)
		return;

	hdev = data->hdev;
	usb_set_intfdata(intf, NULL);

	hci_unregister_dev(hdev);

	cancel_work_sync(&data->isoc_work);
	cancel_work_sync(&data->waker);

	usb_kill_anchored_urbs(&data->tx_anchor);
	usb_kill_anchored_urbs(&data->intr_anchor);
	usb_kill_anchored_urbs(&data->bulk_anchor);
	usb_kill_anchored_urbs(&data->isoc_anchor);

	if (data->isoc) {
		usb_set_intfdata(data->isoc, NULL);
		usb_put_intf(data->isoc);
	}

	hci_free_dev(hdev);
}

static int brtusb_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct brtusb_data *data = usb_get_intfdata(intf);

	BT_DBG("intf %p", intf);

	/* active SCO stream: refuse suspend (same policy as the Windows
	 * selective suspend logic, NeedSupportSelectiveSuspend=0) */
	if (data->sco_num > 0)
		return -EBUSY;

	set_bit(BRTUSB_SUSPENDED, &data->flags);
	cancel_work_sync(&data->isoc_work);

	clear_bit(BRTUSB_INTR_RUNNING, &data->flags);
	clear_bit(BRTUSB_BULK_RUNNING, &data->flags);
	clear_bit(BRTUSB_ISOC_RUNNING, &data->flags);

	usb_kill_anchored_urbs(&data->intr_anchor);
	usb_kill_anchored_urbs(&data->bulk_anchor);
	usb_kill_anchored_urbs(&data->isoc_anchor);
	usb_kill_anchored_urbs(&data->tx_anchor);

	return 0;
}

static int brtusb_resume(struct usb_interface *intf)
{
	struct brtusb_data *data = usb_get_intfdata(intf);
	int err = 0;

	BT_DBG("intf %p", intf);

	clear_bit(BRTUSB_SUSPENDED, &data->flags);

	if (test_bit(HCI_RUNNING, &data->hdev->flags)) {
		err = brtusb_submit_intr_urb(data->hdev, GFP_NOIO);
		if (!err)
			set_bit(BRTUSB_INTR_RUNNING, &data->flags);

		err = brtusb_submit_bulk_urb(data->hdev, GFP_NOIO);
		if (!err)
			set_bit(BRTUSB_BULK_RUNNING, &data->flags);
	}

	return err;
}

static int brtusb_pre_reset(struct usb_interface *intf)
{
	struct brtusb_data *data = usb_get_intfdata(intf);

	BT_DBG("intf %p", intf);

	clear_bit(BRTUSB_INTR_RUNNING, &data->flags);
	clear_bit(BRTUSB_BULK_RUNNING, &data->flags);
	clear_bit(BRTUSB_ISOC_RUNNING, &data->flags);

	usb_kill_anchored_urbs(&data->intr_anchor);
	usb_kill_anchored_urbs(&data->bulk_anchor);
	usb_kill_anchored_urbs(&data->isoc_anchor);

	return 0;
}

static int brtusb_post_reset(struct usb_interface *intf)
{
	struct brtusb_data *data = usb_get_intfdata(intf);
	int err;

	BT_DBG("intf %p", intf);

	err = brtusb_submit_intr_urb(data->hdev, GFP_KERNEL);
	if (!err) {
		set_bit(BRTUSB_INTR_RUNNING, &data->flags);

		err = brtusb_submit_bulk_urb(data->hdev, GFP_KERNEL);
		if (!err)
			set_bit(BRTUSB_BULK_RUNNING, &data->flags);
	}

	return err;
}

static struct usb_driver brtusb_driver = {
	.name			= "brtusb",
	.probe			= brtusb_probe,
	.disconnect		= brtusb_disconnect,
	.suspend		= brtusb_suspend,
	.resume			= brtusb_resume,
	.pre_reset		= brtusb_pre_reset,
	.post_reset		= brtusb_post_reset,
	.id_table		= brtusb_table,
	.supports_autosuspend	= 1,
	.dev_groups		= brtusb_groups,
};

module_usb_driver(brtusb_driver);

MODULE_AUTHOR("Reverse-engineered from the BARROT BRLink Windows driver");
MODULE_DESCRIPTION("Bluetooth HCI USB driver for BARROT BRTLink adapters");
MODULE_LICENSE("GPL");
MODULE_VERSION(VERSION);
