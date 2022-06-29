/*
 * Copyright (c) 2015 MediaTek Inc.
 * Author:
 *  Zhigang.Wei <zhigang.wei@mediatek.com>
 *  Chunfeng.Yun <chunfeng.yun@mediatek.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "xhci.h"
#include "xhci-mtk.h"

#define SS_BW_BOUNDARY	51000
/* table 5-5. High-speed Isoc Transaction Limits in usb_20 spec */
#define HS_BW_BOUNDARY	6144
/* usb2 spec section11.18.1: at most 188 FS bytes per microframe */
#define FS_PAYLOAD_MAX 188
/*
 * max number of microframes for split transfer,
 * for fs isoc in : 1 ss + 1 idle + 7 cs
 */
#define TT_MICROFRAMES_MAX 9

/* mtk scheduler bitmasks */
#define EP_BPKTS(p)	((p) & 0x3f)
#define EP_BCSCOUNT(p)	(((p) & 0x7) << 8)
#define EP_BBM(p)	((p) << 11)
#define EP_BOFFSET(p)	((p) & 0x3fff)
#define EP_BREPEAT(p)	(((p) & 0x7fff) << 16)

static int is_fs_or_ls(enum usb_device_speed speed)
{
	return speed == USB_SPEED_FULL || speed == USB_SPEED_LOW;
}

/*
* get the index of bandwidth domains array which @ep belongs to.
*
* the bandwidth domain array is saved to @sch_array of struct xhci_hcd_mtk,
* each HS root port is treated as a single bandwidth domain,
* but each SS root port is treated as two bandwidth domains, one for IN eps,
* one for OUT eps.
* @real_port value is defined as follow according to xHCI spec:
* 1 for SSport0, ..., N+1 for SSportN, N+2 for HSport0, N+3 for HSport1, etc
* so the bandwidth domain array is organized as follow for simplification:
* SSport0-OUT, SSport0-IN, ..., SSportX-OUT, SSportX-IN, HSport0, ..., HSportY
*/
static int get_bw_index(struct xhci_hcd *xhci, struct usb_device *udev,
	struct usb_host_endpoint *ep)
{
	struct xhci_virt_device *virt_dev;
	int bw_index;

	virt_dev = xhci->devs[udev->slot_id];

	if (udev->speed == USB_SPEED_SUPER) {
		if (usb_endpoint_dir_out(&ep->desc))
			bw_index = (virt_dev->real_port - 1) * 2;
		else
			bw_index = (virt_dev->real_port - 1) * 2 + 1;
	} else {
		/* add one more for each SS port */
		bw_index = virt_dev->real_port + xhci->num_usb3_ports - 1;
	}

	return bw_index;
}

static u32 get_esit(struct xhci_ep_ctx *ep_ctx)
{
	u32 esit;

	esit = 1 << CTX_TO_EP_INTERVAL(le32_to_cpu(ep_ctx->ep_info));
	if (esit > XHCI_MTK_MAX_ESIT)
		esit = XHCI_MTK_MAX_ESIT;

	return esit;
}

static struct mu3h_sch_ep_info *create_sch_ep(struct usb_device *udev,
	struct usb_host_endpoint *ep, struct xhci_ep_ctx *ep_ctx)
{
	struct mu3h_sch_ep_info *sch_ep;
	u32 len_bw_budget_table;
	size_t mem_size;

	if (is_fs_or_ls(udev->speed))
		len_bw_budget_table = TT_MICROFRAMES_MAX;
	else if ((udev->speed == USB_SPEED_SUPER)
			&& usb_endpoint_xfer_isoc(&ep->desc))
		len_bw_budget_table = get_esit(ep_ctx);
	else
		len_bw_budget_table = 1;

	mem_size = sizeof(struct mu3h_sch_ep_info) +
			len_bw_budget_table * sizeof(u32);
	sch_ep = kzalloc(mem_size, GFP_KERNEL);
	if (!sch_ep)
		return ERR_PTR(-ENOMEM);

	sch_ep->ep = ep;

	return sch_ep;
}

static void setup_sch_info(struct usb_device *udev,
		struct xhci_ep_ctx *ep_ctx, struct mu3h_sch_ep_info *sch_ep)
{
	u32 ep_type;
	u32 maxpkt;
	u32 max_burst;
	u32 mult;
	u32 esit_pkts;
	u32 max_esit_payload;
	u32 *bwb_table = sch_ep->bw_budget_table;
	int i;

	ep_type = CTX_TO_EP_TYPE(le32_to_cpu(ep_ctx->ep_info2));
	maxpkt = MAX_PACKET_DECODED(le32_to_cpu(ep_ctx->ep_info2));
	max_burst = CTX_TO_MAX_BURST(le32_to_cpu(ep_ctx->ep_info2));
	mult = CTX_TO_EP_MULT(le32_to_cpu(ep_ctx->ep_info));
	max_esit_payload =
		(CTX_TO_MAX_ESIT_PAYLOAD_HI(
			le32_to_cpu(ep_ctx->ep_info)) << 16) |
		 CTX_TO_MAX_ESIT_PAYLOAD(le32_to_cpu(ep_ctx->tx_info));

	sch_ep->esit = get_esit(ep_ctx);
	sch_ep->offset = 0;
	sch_ep->burst_mode = 0;
	sch_ep->repeat = 0;

	if (udev->speed == USB_SPEED_HIGH) {
		sch_ep->cs_count = 0;

		/*
		 * usb_20 spec section5.9
		 * a single microframe is enough for HS synchromous endpoints
		 * in a interval
		 */
		sch_ep->num_budget_microframes = 1;

		/*
		 * xHCI spec section6.2.3.4
		 * @max_burst is the number of additional transactions
		 * opportunities per microframe
		 */
		sch_ep->pkts = max_burst + 1;
		sch_ep->bw_cost_per_microframe = maxpkt * sch_ep->pkts;
		bwb_table[0] = sch_ep->bw_cost_per_microframe;
	} else if (udev->speed == USB_SPEED_SUPER) {
		/* usb3_r1 spec section4.4.7 & 4.4.8 */
		sch_ep->cs_count = 0;
		sch_ep->burst_mode = 1;
		/*
		 * some device's (d)wBytesPerInterval is set as 0,
		 * then max_esit_payload is 0, so evaluate esit_pkts from
		 * mult and burst
		 */
		esit_pkts = DIV_ROUND_UP(max_esit_payload, maxpkt);
		if (esit_pkts == 0)
			esit_pkts = (mult + 1) * (max_burst + 1);

		if (ep_type == INT_IN_EP || ep_type == INT_OUT_EP) {
			sch_ep->pkts = esit_pkts;
			sch_ep->num_budget_microframes = 1;
			bwb_table[0] = maxpkt * sch_ep->pkts;
		}

		if (ep_type == ISOC_IN_EP || ep_type == ISOC_OUT_EP) {
			u32 remainder;

			if (sch_ep->esit == 1)
				sch_ep->pkts = esit_pkts;
			else if (esit_pkts <= sch_ep->esit)
				sch_ep->pkts = 1;
			else
				sch_ep->pkts = roundup_pow_of_two(esit_pkts)
					/ sch_ep->esit;

			sch_ep->num_budget_microframes =
				DIV_ROUND_UP(esit_pkts, sch_ep->pkts);

			sch_ep->repeat = !!(sch_ep->num_budget_microframes > 1);
			sch_ep->bw_cost_per_microframe = maxpkt * sch_ep->pkts;

			remainder = sch_ep->bw_cost_per_microframe;
			remainder *= sch_ep->num_budget_microframes;
			remainder -= (maxpkt * esit_pkts);
			for (i = 0; i < sch_ep->num_budget_microframes - 1; i++)
				bwb_table[i] = sch_ep->bw_cost_per_microframe;

			/* last one <= bw_cost_per_microframe */
			bwb_table[i] = remainder;
		}
	} else if (is_fs_or_ls(udev->speed)) {
		sch_ep->pkts = 1; /* at most one packet for each microframe */
		sch_ep->cs_count = DIV_ROUND_UP(maxpkt, FS_PAYLOAD_MAX);
		sch_ep->num_budget_microframes = sch_ep->cs_count + 2;
		sch_ep->bw_cost_per_microframe =
			(maxpkt < FS_PAYLOAD_MAX) ? maxpkt : FS_PAYLOAD_MAX;

		/* init budget table */
		if (ep_type == ISOC_OUT_EP) {
			for (i = 0; i < sch_ep->num_budget_microframes; i++)
				bwb_table[i] =	sch_ep->bw_cost_per_microframe;
		} else if (ep_type == INT_OUT_EP) {
			/* only first one consumes bandwidth, others as zero */
			bwb_table[0] = sch_ep->bw_cost_per_microframe;
		} else { /* INT_IN_EP or ISOC_IN_EP */
			bwb_table[0] = 0; /* start split */
			bwb_table[1] = 0; /* idle */
			for (i = 2; i < sch_ep->num_budget_microframes; i++)
				bwb_table[i] =	sch_ep->bw_cost_per_microframe;
		}
	}
}

/* Get maximum bandwidth when we schedule at offset slot. */
static u32 get_max_bw(struct mu3h_sch_bw_info *sch_bw,
	struct mu3h_sch_ep_info *sch_ep, u32 offset)
{
	u32 num_esit;
	u32 max_bw = 0;
	u32 bw;
	int i;
	int j;

	num_esit = XHCI_MTK_MAX_ESIT / sch_ep->esit;
	for (i = 0; i < num_esit; i++) {
		u32 base = offset + i * sch_ep->esit;

		for (j = 0; j < sch_ep->num_budget_microframes; j++) {
			bw = sch_bw->bus_bw[base + j] +
					sch_ep->bw_budget_table[j];
			if (bw > max_bw)
				max_bw = bw;
		}
	}
	return max_bw;
}

static void update_bus_bw(struct mu3h_sch_bw_info *sch_bw,
	struct mu3h_sch_ep_info *sch_ep, bool used)
{
	u32 num_esit;
	u32 base;
	int i;
	int j;

	num_esit = XHCI_MTK_MAX_ESIT / sch_ep->esit;
	for (i = 0; i < num_esit; i++) {
		base = sch_ep->offset + i * sch_ep->esit;
		for (j = 0; j < sch_ep->num_budget_microframes; j++) {
			if (used)
				sch_bw->bus_bw[base + j] +=
					sch_ep->bw_budget_table[j];
			else
				sch_bw->bus_bw[base + j] -=
					sch_ep->bw_budget_table[j];
		}
	}
}

static bool sch_offset_used(struct mu3h_sch_bw_info *sch_bw,
	u32 offset)
{
	struct mu3h_sch_ep_info *sch_ep;
	bool used = false;

	list_for_each_entry(sch_ep, &sch_bw->bw_ep_list, endpoint) {
		if (sch_ep->offset == offset) {
			used = true;
			break;
		}
	}
	return used;
}

static int check_sch_bw(struct usb_device *udev,
	struct mu3h_sch_bw_info *sch_bw, struct mu3h_sch_ep_info *sch_ep)
{
	u32 offset;
	u32 esit;
	u32 min_bw;
	u32 min_index;
	u32 worst_bw;
	u32 bw_boundary;

	esit = sch_ep->esit;

	/*
	 * Search through all possible schedule microframes.
	 * and find a microframe where its worst bandwidth is minimum.
	 */
	min_bw = ~0;
	min_index = 0;
	for (offset = 0; offset < esit; offset++) {
		if ((offset + sch_ep->num_budget_microframes) > sch_ep->esit)
			break;

		/*
		 * usb_20 spec section11.18:
		 * must never schedule Start-Split in Y6
		 */
		if (is_fs_or_ls(udev->speed) && (offset % 8 == 6))
			continue;

		worst_bw = get_max_bw(sch_bw, sch_ep, offset);
		if (min_bw > worst_bw) {
			min_bw = worst_bw;
			min_index = offset;
		} else if (min_bw == worst_bw) {
			if (sch_offset_used(sch_bw, min_index) &&
					!sch_offset_used(sch_bw, offset))
				min_index = offset;
		}
		if (min_bw == 0)
			break;
	}
	sch_ep->offset = min_index;

	bw_boundary = (udev->speed == USB_SPEED_SUPER)
				? SS_BW_BOUNDARY : HS_BW_BOUNDARY;

	/* check bandwidth */
	if (min_bw > bw_boundary)
		return -ERANGE;

	/* update bus bandwidth info */
	update_bus_bw(sch_bw, sch_ep, 1);

	return 0;
}

static bool need_bw_sch(struct usb_host_endpoint *ep,
	enum usb_device_speed speed, int has_tt)
{
	/* only for periodic endpoints */
	if (usb_endpoint_xfer_control(&ep->desc)
		|| usb_endpoint_xfer_bulk(&ep->desc))
		return false;

	/*
	 * for LS & FS periodic endpoints which its device is not behind
	 * a TT are also ignored, root-hub will schedule them directly,
	 * but need set @bpkts field of endpoint context to 1.
	 */
	if (is_fs_or_ls(speed) && !has_tt)
		return false;

	return true;
}

int xhci_mtk_sch_init(struct xhci_hcd_mtk *mtk)
{
	struct mu3h_sch_bw_info *sch_array;
	int num_usb_bus;
	int i;

	/* ss IN and OUT are separated */
	num_usb_bus = mtk->num_u3_ports * 2 + mtk->num_u2_ports;

	sch_array = kcalloc(num_usb_bus, sizeof(*sch_array), GFP_KERNEL);
	if (sch_array == NULL)
		return -ENOMEM;

	for (i = 0; i < num_usb_bus; i++)
		INIT_LIST_HEAD(&sch_array[i].bw_ep_list);

	mtk->sch_array = sch_array;

	return 0;
}
EXPORT_SYMBOL_GPL(xhci_mtk_sch_init);

void xhci_mtk_sch_exit(struct xhci_hcd_mtk *mtk)
{
	kfree(mtk->sch_array);
}
EXPORT_SYMBOL_GPL(xhci_mtk_sch_exit);

int xhci_mtk_add_ep_quirk(struct usb_hcd *hcd, struct usb_device *udev,
		struct usb_host_endpoint *ep)
{
	struct xhci_hcd_mtk *mtk = hcd_to_mtk(hcd);
	struct xhci_hcd *xhci;
	struct xhci_ep_ctx *ep_ctx;
	struct xhci_slot_ctx *slot_ctx;
	struct xhci_virt_device *virt_dev;
	struct mu3h_sch_bw_info *sch_bw;
	struct mu3h_sch_ep_info *sch_ep;
	struct mu3h_sch_bw_info *sch_array;
	unsigned int ep_index;
	int bw_index;
	int ret = 0;

	xhci = hcd_to_xhci(hcd);
	virt_dev = xhci->devs[udev->slot_id];
	ep_index = xhci_get_endpoint_index(&ep->desc);
	slot_ctx = xhci_get_slot_ctx(xhci, virt_dev->in_ctx);
	ep_ctx = xhci_get_ep_ctx(xhci, virt_dev->in_ctx, ep_index);
	sch_array = mtk->sch_array;

	xhci_dbg(xhci, "%s() type:%d, speed:%d, mpkt:%d, dir:%d, ep:%p\n",
		__func__, usb_endpoint_type(&ep->desc), udev->speed,
		usb_endpoint_maxp(&ep->desc),
		usb_endpoint_dir_in(&ep->desc), ep);

	if (!need_bw_sch(ep, udev->speed, slot_ctx->tt_info & TT_SLOT)) {
		/*
		 * set @bpkts to 1 if it is LS or FS periodic endpoint, and its
		 * device does not connected through an external HS hub
		 */
		if (usb_endpoint_xfer_int(&ep->desc)
			|| usb_endpoint_xfer_isoc(&ep->desc))
			ep_ctx->reserved[0] |= cpu_to_le32(EP_BPKTS(1));

		return 0;
	}

	bw_index = get_bw_index(xhci, udev, ep);
	sch_bw = &sch_array[bw_index];

	sch_ep = create_sch_ep(udev, ep, ep_ctx);
	if (IS_ERR_OR_NULL(sch_ep))
		return -ENOMEM;

	setup_sch_info(udev, ep_ctx, sch_ep);

	ret = check_sch_bw(udev, sch_bw, sch_ep);
	if (ret) {
		xhci_err(xhci, "Not enough bandwidth!\n");
		kfree(sch_ep);
		return -ENOSPC;
	}

	list_add_tail(&sch_ep->endpoint, &sch_bw->bw_ep_list);

	ep_ctx->reserved[0] |= cpu_to_le32(EP_BPKTS(sch_ep->pkts)
		| EP_BCSCOUNT(sch_ep->cs_count) | EP_BBM(sch_ep->burst_mode));
	ep_ctx->reserved[1] |= cpu_to_le32(EP_BOFFSET(sch_ep->offset)
		| EP_BREPEAT(sch_ep->repeat));

	xhci_dbg(xhci, " PKTS:%x, CSCOUNT:%x, BM:%x, OFFSET:%x, REPEAT:%x\n",
			sch_ep->pkts, sch_ep->cs_count, sch_ep->burst_mode,
			sch_ep->offset, sch_ep->repeat);

	return 0;
}
EXPORT_SYMBOL_GPL(xhci_mtk_add_ep_quirk);

void xhci_mtk_drop_ep_quirk(struct usb_hcd *hcd, struct usb_device *udev,
		struct usb_host_endpoint *ep)
{
	struct xhci_hcd_mtk *mtk = hcd_to_mtk(hcd);
	struct xhci_hcd *xhci;
	struct xhci_ep_ctx *ep_ctx;
	struct xhci_slot_ctx *slot_ctx;
	struct xhci_virt_device *virt_dev;
	struct mu3h_sch_bw_info *sch_array;
	struct mu3h_sch_bw_info *sch_bw;
	struct mu3h_sch_ep_info *sch_ep;
	unsigned int ep_index;
	int bw_index;

	xhci = hcd_to_xhci(hcd);
	virt_dev = xhci->devs[udev->slot_id];
	ep_index = xhci_get_endpoint_index(&ep->desc);
	slot_ctx = xhci_get_slot_ctx(xhci, virt_dev->in_ctx);
	ep_ctx = xhci_get_ep_ctx(xhci, virt_dev->in_ctx, ep_index);
	sch_array = mtk->sch_array;

	xhci_dbg(xhci, "%s() type:%d, speed:%d, mpks:%d, dir:%d, ep:%p\n",
		__func__, usb_endpoint_type(&ep->desc), udev->speed,
		usb_endpoint_maxp(&ep->desc),
		usb_endpoint_dir_in(&ep->desc), ep);

	if (!need_bw_sch(ep, udev->speed, slot_ctx->tt_info & TT_SLOT))
		return;

	bw_index = get_bw_index(xhci, udev, ep);
	sch_bw = &sch_array[bw_index];

	list_for_each_entry(sch_ep, &sch_bw->bw_ep_list, endpoint) {
		if (sch_ep->ep == ep) {
			update_bus_bw(sch_bw, sch_ep, 0);
			list_del(&sch_ep->endpoint);
			kfree(sch_ep);
			break;
		}
	}

	ep_ctx->reserved[0] = 0;
	ep_ctx->reserved[1] = 0;
}
EXPORT_SYMBOL_GPL(xhci_mtk_drop_ep_quirk);