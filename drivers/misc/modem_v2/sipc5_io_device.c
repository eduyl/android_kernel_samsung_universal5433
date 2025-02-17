/* /linux/drivers/misc/modem_if/sipc5_io_device.c
 *
 * Copyright (C) 2010 Samsung Electronics.
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

#include <linux/init.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/if_arp.h>
#include <linux/ip.h>
#include <linux/if_ether.h>
#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/ratelimit.h>

#include <linux/platform_data/modem.h>
#include "modem_prj.h"
#include "modem_utils.h"

enum iod_debug_flag_bit {
	IOD_DEBUG_IPC_LOOPBACK,
};

static unsigned long dbg_flags;
module_param(dbg_flags, ulong, S_IRUGO | S_IWUSR | S_IWGRP);
MODULE_PARM_DESC(dbg_flags, "sipc iodevice debug flags\n");

static int fd_waketime = (2 * HZ);
module_param(fd_waketime, int, S_IRUGO);
MODULE_PARM_DESC(fd_waketime, "fd wake lock timeout");

/* to change the maximum rx queue size later for test purpose */
static int rxq_max;
module_param(rxq_max, int, S_IRUGO);
MODULE_PARM_DESC(rxq_max, "global maximum rx queue size");

static ssize_t show_waketime(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	unsigned int msec;
	char *p = buf;

	msec = jiffies_to_msecs(fd_waketime);

	p += sprintf(buf, "raw waketime : %ums\n", msec);

	return p - buf;
}

static ssize_t store_waketime(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long msec;
	int ret;
	struct miscdevice *miscdev = dev_get_drvdata(dev);
	struct io_device *iod = container_of(miscdev, struct io_device,
			miscdev);

	ret = kstrtoul(buf, 10, &msec);
	if (ret)
		return count;

	iod->waketime = msecs_to_jiffies(msec);
	fd_waketime = msecs_to_jiffies(msec);

	return count;
}

static struct device_attribute attr_waketime =
	__ATTR(waketime, S_IRUGO | S_IWUSR, show_waketime, store_waketime);

static ssize_t show_loopback(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct miscdevice *miscdev = dev_get_drvdata(dev);
	struct modem_shared *msd =
		container_of(miscdev, struct io_device, miscdev)->msd;
	unsigned char *ip = (unsigned char *)&msd->loopback_ipaddr;
	char *p = buf;

	p += sprintf(buf, "%u.%u.%u.%u en(%d)\n", ip[0], ip[1], ip[2], ip[3],
		test_bit(IOD_DEBUG_IPC_LOOPBACK, &dbg_flags));

	return p - buf;
}

static ssize_t store_loopback(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct miscdevice *miscdev = dev_get_drvdata(dev);
	struct modem_shared *msd =
		container_of(miscdev, struct io_device, miscdev)->msd;
	struct io_device *iod = to_io_device(miscdev);

	msd->loopback_ipaddr = ipv4str_to_be32(buf, count);
	if (msd->loopback_ipaddr)
		set_bit(IOD_DEBUG_IPC_LOOPBACK, &dbg_flags);
	else
		clear_bit(IOD_DEBUG_IPC_LOOPBACK, &dbg_flags);

	mif_info("loopback(%s), en(%d)\n", buf,
				test_bit(IOD_DEBUG_IPC_LOOPBACK, &dbg_flags));
	if (msd->loopback_start)
		msd->loopback_start(iod, msd);

	return count;
}

static struct device_attribute attr_loopback =
	__ATTR(loopback, S_IRUGO | S_IWUSR, show_loopback, store_loopback);

static struct sk_buff *sipc5_hdr_create_ipcloopback(struct io_device *iod,
			struct sipc_hdr *hdr, struct sk_buff *skb);
static int sipc5_recv_demux(struct io_device *iod,
			struct link_device *ld, struct sk_buff *skb);
static int sipc5_recv_demux_ipcloopback(struct io_device *iod,
			struct link_device *ld, struct sk_buff *skb);

static ssize_t show_ipcloopback(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct miscdevice *miscdev = dev_get_drvdata(dev);
	struct modem_shared *msd =
		container_of(miscdev, struct io_device, miscdev)->msd;
	char *p = buf;

	p += sprintf(buf, "%d\n", msd->ipcloopback_enable);

	return p - buf;
}

static ssize_t store_ipcloopback(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct miscdevice *miscdev = dev_get_drvdata(dev);
	struct modem_shared *msd =
		container_of(miscdev, struct io_device, miscdev)->msd;
	struct io_device *lb_iod = get_iod_with_channel(msd, SIPC5_CH_ID_FMT_9);
	struct io_device *ipc_iod =
				get_iod_with_channel(msd, SIPC5_CH_ID_FMT_0);

	if (*buf == '0') {
		msd->ipcloopback_enable = 0;

		lb_iod->ops.header_create = sipc5_hdr_create_ipcloopback;
		ipc_iod->ops.recv_demux = sipc5_recv_demux;

		mif_info("ipc loobpack disable: %d\n", msd->ipcloopback_enable);
	} else {
		msd->ipcloopback_enable = 1;

		lb_iod->ops.header_create = sipc5_hdr_create_ipcloopback;
		ipc_iod->ops.recv_demux = sipc5_recv_demux_ipcloopback;

		mif_info("ipc loobpack enable: %d\n", msd->ipcloopback_enable);
	}

	return count;
}

static struct device_attribute attr_ipcloopback =
__ATTR(ipcloopback, S_IRUGO | S_IWUSR, show_ipcloopback, store_ipcloopback);

static void iodev_showtxlink(struct io_device *iod, void *args)
{
	char **p = (char **)args;
	struct link_device *ld = get_current_link(iod);

	if (iod->io_typ == IODEV_NET && IS_CONNECTED(iod, ld))
		*p += sprintf(*p, "%s: %s\n", iod->name, ld->name);
}

static ssize_t show_txlink(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct miscdevice *miscdev = dev_get_drvdata(dev);
	struct modem_shared *msd =
		container_of(miscdev, struct io_device, miscdev)->msd;
	char *p = buf;

	iodevs_for_each(msd, iodev_showtxlink, &p);

	return p - buf;
}

static ssize_t store_txlink(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	/* don't change without gpio dynamic switching */
	return -EINVAL;
}

static struct device_attribute attr_txlink =
	__ATTR(txlink, S_IRUGO | S_IWUSR, show_txlink, store_txlink);

static ssize_t show_dm_state(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	mif_info("We do not support this sysfs since SIPC5.0\n");
	return 0;
}

static ssize_t store_dm_state(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	mif_info("We do not support this sysfs since SIPC5.0\n");
	return count;
}

static struct device_attribute attr_dm_state =
	__ATTR(dm_state, S_IRUGO | S_IWUSR, show_dm_state, store_dm_state);

/**
 * SIPC functions
 *
 */
#define MAX_RXDATA_SIZE		0x0E00	/* 4 * 1024 - 512 */
static unsigned sipc5_get_packet_len(struct sipc5_link_hdr *hdr)
{
	return (hdr->cfg & SIPC5_HDR_EXT) ? *((u32 *)&hdr->len) : hdr->len;
}

/* TODO unused */
#if 0
static unsigned sipc5_get_pad_len(struct sipc5_link_hdr *hdr, unsigned len)
{
	return (hdr->cfg & SIPC5_HDR_PAD) ? sipc5_calc_padding_size(len) : 0;
}
#endif

static u32 sipc5_get_header_size(struct sipc_hdr *hdr, unsigned len)
{
	if (hdr->multifmt)
		return SIPC5_HDR_LEN_CTRL;
	else if (len > 0xFFFF - SIPC5_HDR_LEN_MAX)
		return SIPC5_HDR_LEN_EXT;
	else
		return SIPC5_HDR_LEN;
}

/**
 * SIPC header_create() family functions
 *
 * Create SIPC5 header
 *
 * sipc5_hdr_create_skb()
 *	Create common SIPC5 header
 *
 * sipc5_hdr_create_legacy_rfs()
 *	Create SIPC5 header with IPC4.1 RFS header
 *	Because IMC modem used the 256KB rfs packet and RILD check the full
 *	packet with RFS header len, kernel remove the SIPC5 header and add the
 *	lagacy RFS header.
 *
 * sipc5_hdr_create_multifmt()
 *	TBD
 *
 * sipc5_hdr_create_skb_handover()
 *	Remove the ethernet frame When use `handover' with Network Bridge,
 *	user -> bridge device(rmnet0) -> real rmnet(xxxx_rmnet0) -> here.
 *	bridge device is ethernet device unlike xxxx_rmnet(net device).
 *	We remove the an ethernet header of skb before using skb->len,
 *	because bridge device added an ethernet header to skb.
 *
 * RETURN
 *	Returns the socket buffer that added SIPC5 header.
 *
 **/
static struct sk_buff *sipc5_hdr_create(struct io_device *iod,
				struct sipc_hdr *hdr, struct sk_buff *skb)
{
	struct sipc5_link_hdr *sipc5h;
	struct link_device *ld = get_current_link(iod);

	if (hdr->hdr_size == SIPC5_HDR_LEN_EXT) {
		sipc5h = (struct sipc5_link_hdr *)
					skb_push(skb, SIPC5_HDR_LEN_EXT);
		sipc5h->cfg = SIPC5_HDR_CFG_START | SIPC5_HDR_EXT;
		*((u32 *)&sipc5h->len) = (u32)(skb->len);
	} else {
		sipc5h = (struct sipc5_link_hdr *)
					skb_push(skb, SIPC5_HDR_LEN);
		sipc5h->cfg = SIPC5_HDR_CFG_START;
		sipc5h->len = (u16)(skb->len);
	}
	sipc5h->ch = iod->id;

	/* Should check the alignment for dynamic switch link dev*/
	if (ld->aligned) {
		sipc5h->cfg |= SIPC5_HDR_PAD;
		skb_set_tail_pointer(skb, SIPC_ALIGN(skb->len));
		skb->len = SIPC_ALIGN(skb->len);
	}
	skbpriv(skb)->sipch = (void *)sipc5h;
	return skb;
}

static struct sk_buff *sipc5_hdr_create_ipcloopback(struct io_device *iod,
				struct sipc_hdr *hdr, struct sk_buff *skb)
{
	struct sipc5_link_hdr *sipc5h;
	struct link_device *ld = get_current_link(iod);

	sipc5h = (struct sipc5_link_hdr *)
				skb_push(skb, SIPC5_HDR_LEN);
	sipc5h->cfg = SIPC5_HDR_CFG_START;
	sipc5h->len = (u16)(skb->len);

	mif_info("send ipcloopback data: %d\n", skb->len);
	sipc5h->ch = SIPC5_CH_ID_FMT_0;

	/* Should check the alignment for dynamic switch link dev*/
	if (ld->aligned) {
		sipc5h->cfg |= SIPC5_HDR_PAD;
		skb_set_tail_pointer(skb, SIPC_ALIGN(skb->len));
		skb->len = SIPC_ALIGN(skb->len);
	}
	skbpriv(skb)->sipch = (void *)sipc5h;
	return skb;
}

static struct sk_buff *sipc5_hdr_create_legacy_rfs(struct io_device *iod,
				struct sipc_hdr *hdr, struct sk_buff *skb)
{
	if (*(u32 *)skb->data != skb->len) {
		mif_err("rfs length fault: len=%u, skb->len=%u\n",
			*(u32 *)skb->data, skb->len);
	}
	skb_pull(skb, sizeof(u32)); /* remove the RFS header from rild */
	return sipc5_hdr_create(iod, hdr, skb);
}

static struct sk_buff *sipc5_hdr_create_multifmt(struct io_device *iod,
				struct sipc_hdr *hdr, struct sk_buff *skb)
{
	struct sipc5_link_hdr *sipc5h;
	struct link_device *ld = get_current_link(iod);

	if (hdr->multifmt) {
		sipc5h = (struct sipc5_link_hdr *)
					skb_push(skb, SIPC5_HDR_LEN_CTRL);
		sipc5h->cfg = SIPC5_HDR_CFG_START | SIPC5_HDR_CONTROL;
		sipc5h->len = skb->len;
		sipc5h->ext.ctl = hdr->multifmt;
		sipc5h->ch = iod->id;

		/* Should check the alignment for dynamic switch link dev*/
		if (ld->aligned) {
			sipc5h->cfg |= SIPC5_HDR_PAD;
			skb_set_tail_pointer(skb, SIPC_ALIGN(skb->len));
			skb->len = SIPC_ALIGN(skb->len);
		}
		skbpriv(skb)->sipch = (void *)sipc5h;
		return skb;
	}
	return sipc5_hdr_create(iod, hdr, skb);
}

/* TODO: not verified */
static struct sk_buff *sipc5_hdr_create_handover(struct io_device *iod,
				struct sipc_hdr *hdr, struct sk_buff *skb)
{
	skb_pull(skb, sizeof(struct ethhdr));
	return sipc5_hdr_create(iod, hdr, skb);
}

/**
 * header_multifmt_length() family
 *
 * multifmt_id
 *	holds multifmt information id value (1 ~ 127)
 *
 * sipc_get_multifmt_id()
 *     retrun the unique multifmt information id
 *
 * sipc_hdr_multifmt_length()
 *	update the hdr->multifmt and return frame length
 *
 */

static atomic_t multifmt_id = ATOMIC_INIT(-1);

static inline int sipc_get_multifmt_id(void)
{
	return atomic_inc_return(&multifmt_id) % SIPC_MULTIFMT_ID_MAX + 1;
}

static u32 sipc_hdr_multifmt_length(struct io_device *iod,
		struct sipc_hdr *hdr, size_t copied, size_t len)
{
	u32 remains, frame_len = len;

	if (unlikely(iod->format != IPC_FMT))
		goto exit;

	if (len > iod->multi_len) {
		remains = len - copied;
		frame_len = min_t(unsigned, remains, iod->multi_len);
		if (!hdr->multifmt) {
			hdr->multifmt = sipc_get_multifmt_id();
			hdr->multifmt |= SIPC_MULTIFMT_MOREBIT;
		}
		if (frame_len >= remains) /* check last frame */
			hdr->multifmt &= ~SIPC_MULTIFMT_MOREBIT;
		/* max frame_len includes header size */
		if (hdr->multifmt && (frame_len == iod->multi_len))
			frame_len -= SIPC5_HDR_LEN_CTRL;
	}
exit:
	return frame_len;
}

/*
 * sipc5_hdr_parse() family functions
 *
 * Parse ipc header config and remove header from rx packet
 *
 */
static int sipc5_hdr_parse(struct io_device *iod, struct sk_buff *skb)
{
	struct sipc5_link_hdr *sipc5h = (struct sipc5_link_hdr *)skb->data;

	/* First frame - remove SIPC5 header */
	if (unlikely(!sipc5_start_valid(sipc5h))) {
		mif_err("SIPC5 wrong CFG(0x%02x)%s\n", skb->data[0], iod->name);
		return -EBADMSG;
	}
	skb_trim(skb, sipc5_get_packet_len(sipc5h)); /* remove pad */
	skbpriv(skb)->sipch = (void *)skb->data;
	skb_pull_inline(skb, sipc5_get_hdr_len(sipc5h));

	return 0;
}

static int sipc5_hdr_parse_multifmt_continue(struct io_device *iod,
					struct sk_buff *skb, unsigned size)
{
	struct sipc5_link_hdr *sipc5h =
				(struct sipc5_link_hdr *)skbpriv(skb)->sipch;

	if (sipc5h->cfg & SIPC5_HDR_CTRL && (sipc5h->ext.ctl & 0x80)) {
		mif_info("multi-frame\n");
		return 1;
	}
	return 0;
}

static int sipc5_hdr_parse_fragment(struct io_device *iod, struct sk_buff *skb)
{
	struct link_device *ld = skbpriv(skb)->ld;
	struct header_data *hdr = &fragdata(iod, ld)->h_post;
	struct sipc5_link_hdr *sipc5h;
	unsigned header_len;
	int ret;

	if (hdr->frag_len)
		return 0;

	ret = sipc5_hdr_parse(iod, skb);
	if (ret < 0)
		return ret;

	sipc5h = (struct sipc5_link_hdr *)skbpriv(skb)->sipch;
	hdr->len = sipc5_get_packet_len(sipc5h);
	header_len = sipc5_get_hdr_len(sipc5h); /* Header len*/
	memcpy(hdr->hdr, sipc5h, header_len);
	hdr->frag_len = header_len;

	return 0;
}

static int sipc5_hdr_parse_fragment_continue(struct io_device *iod,
					struct sk_buff *skb, unsigned size)
{
	struct link_device *ld = skbpriv(skb)->ld;
	struct header_data *hdr = &fragdata(iod, ld)->h_post;

	hdr->frag_len += size;
	if (hdr->len - hdr->frag_len == 0) {
		mif_debug("done frag=%u\n", hdr->frag_len);
		hdr->frag_len = 0;
		return 0;
	}

	if (hdr->len < hdr->frag_len) {
#ifndef CONFIG_LTE_MODEM_SHANNON
		panic("HSIC - fail sip5.0 framming\n");
#else
		if (iod->mc->ops.modem_force_crash_exit) {
			mif_info("sipc5.x framming error\n");
			iod->mc->ops.modem_force_crash_exit(iod->mc);
		}
#endif
	}

	mif_info("frag_len=%u, len=%u\n", hdr->frag_len, hdr->len);

	return 1;
}

static int sipc5_hdr_parse_legacy_rfs(struct io_device *iod,
							struct sk_buff *skb)
{
	struct link_device *ld = skbpriv(skb)->ld;
	struct header_data *hdr = &fragdata(iod, ld)->h_post;
	struct rfs_hdr rfs_head;
	int ret;

	if (hdr->frag_len)
		return 0;

	ret = sipc5_hdr_parse_fragment(iod, skb);
	if (unlikely(ret))
		return ret;

	memset(&rfs_head, 0x00, sizeof(struct rfs_hdr));
	/* packet length + rfs length - sipc5 hdr */
	rfs_head.len = hdr->len + sizeof(u32)
		- sipc5_get_hdr_len((struct sipc5_link_hdr *)hdr->hdr);
	memcpy(skb_push(skb, sizeof(u32)), &rfs_head.len, sizeof(u32));
	hdr->len += sizeof(u32);

	return 0;
}

static int sipc5_hdr_parse_legacy_rfs_continue(struct io_device *iod,
					struct sk_buff *skb, unsigned size)
{
	struct link_device *ld = skbpriv(skb)->ld;
	struct header_data *hdr = &fragdata(iod, ld)->h_post;

	hdr->frag_len += size;
	if (hdr->len - hdr->frag_len == 0) {
		mif_debug("done frag=%u\n", hdr->frag_len);
		hdr->frag_len = 0;
	}

	if (hdr->len < hdr->frag_len)
		panic("HSIC - fail sip5.0 framming\n");

	return 0;
}

static void skb_queue_move(struct sk_buff_head *dst, struct sk_buff_head *src)
{
	unsigned long flags;

	spin_lock_irqsave(&src->lock, flags);
	while (src->qlen)
		skb_queue_tail(dst, __skb_dequeue(src));
	spin_unlock_irqrestore(&src->lock, flags);
}

static struct io_device *get_frag_iod(struct io_device *iod,
		struct link_device *ld)
{
	struct header_data *hdr = &fragdata(iod, ld)->h_data;
	struct sipc5_link_hdr *sipc5h = (struct sipc5_link_hdr *)hdr->hdr;

	return (link_get_iod_with_channel(ld, sipc5h->ch)) ?: NULL;
}

/* TODO: It will be simplify without padding ... */
static int sipc5_recv_multipacket_to_each_skb(struct io_device *iod,
				struct link_device *ld,	struct sk_buff *skb_in)
{
	int ret = 0;
	struct sk_buff *skb;
	unsigned pkt_len, hdr_len, rest_len, temp;
	struct header_data *hdr = &fragdata(iod, ld)->h_data;
	struct sipc5_link_hdr *sipc_hdr;

	if (!hdr->frag_len)
		goto next_frame;

	/* Procss fragment packet */
	sipc_hdr = (struct sipc5_link_hdr *)hdr->hdr;
	hdr_len = sipc5_get_hdr_len(sipc_hdr);	/* Header length */
	if (hdr->frag_len < hdr_len) {
		/* Continue SIPC5 Header fragmentation */
		temp = hdr_len - hdr->frag_len;
		if (unlikely(skb_in->len < temp)) {
			mif_err("skb_in too small len=%d\n", skb_in->len);
			ret = -EINVAL;
			goto exit;
		}
		memcpy(hdr->hdr + hdr->frag_len, skb_in->data, temp);
		skb_pull_inline(skb_in, temp);
		hdr->frag_len += temp;
		hdr->len = sipc5_get_packet_len(sipc_hdr);

		/* TODO: SIPC5 raw packet was send to network stack, we should
		 * make a IP packet into a skb */

		skb = alloc_skb(hdr_len, GFP_ATOMIC);
		if (unlikely(!skb)) {
			mif_err("fragHeader skb alloc fail\n");
			ret = -ENOMEM;
			goto exit;
		}

		memcpy(skb_put(skb, hdr_len), hdr->hdr, hdr_len);
		skbpriv(skb)->ld = ld;
		skbpriv(skb)->iod = get_frag_iod(iod, ld);

		/* Send SIPC5 Header skb to next stage*/
		ret = iod->ops.recv_demux(iod, ld, skb);
		if (unlikely(ret < 0)) {
			dev_kfree_skb_any(skb);
			mif_err("skb enqueue fail\n");
			goto exit;
		}
	}

	/* rest fragment length */
	rest_len = hdr->len - hdr->frag_len;
	if (!rest_len) {
		memset(hdr, 0x00, sizeof(struct header_data));
		goto next_frame;
	}

	skb = skb_clone(skb_in, GFP_ATOMIC);
	if (unlikely(!skb)) {
		mif_err("rx skb clone fail\n");
		ret = -ENOMEM;
		goto exit;
	}

	/*pr_skb("frag", skb);*/
	mif_ipc_log(MIF_IPC_FLAG, iod->msd, skb->data, skb->len);
	skbpriv(skb)->iod = get_frag_iod(iod, ld);

	if (skb->len < rest_len) {	/* continous fragment packet */
		hdr->frag_len += skb->len;
		skb_pull_inline(skb_in, skb->len);
	} else {				/* last fragment packet */
		skb_trim(skb, rest_len);
		hdr->frag_len += skb->len;
		skb_pull_inline(skb_in, rest_len);
		memset(hdr, 0x00, sizeof(struct header_data));
		mif_debug("frag done\n");
	}
	goto send_next_stage;

next_frame:
	skb = skb_clone(skb_in, GFP_ATOMIC);
	if (unlikely(!skb)) {
		mif_err("rx skb clone fail\n");
		ret = -ENOMEM;
		goto exit;
	}

	/*pr_skb("new", skb);*/
	mif_ipc_log(MIF_IPC_AP2RL, iod->msd, skb->data, skb->len);
	sipc_hdr = (struct sipc5_link_hdr *)skb->data;
	if (unlikely(!sipc5_start_valid(sipc_hdr))) {
		mif_com_log(iod->msd, "SIPC5 wrong CFG(0x%02x) %s\n",
			skb->data[0], ld->name);
		mif_err("SIPC5 wrong CFG(0x%02x) %s\n", skb->data[0], ld->name);
		dev_kfree_skb_any(skb);
		ret = -EBADMSG;
		goto exit;
	}
	pkt_len = sipc5_get_packet_len(sipc_hdr); /* Packet length */
	if (skb->len < pkt_len) {
		/* data fragment - fragment skb or large RFS packets.
		 * Save the SIPC5 header to iod's fragdata buffer and send
		 * data continuosly, then if frame was complete, clear the
		 * fragdata buffer */
		memset(hdr, 0x00, sizeof(struct header_data));
		memcpy(hdr->hdr, skb->data, sipc5_get_hdr_len(sipc_hdr));
		hdr->len = pkt_len;
		hdr->frag_len = skb->len;

		skb_trim(skb,  skb->len); /* ignore padding data */
		skb->truesize = SKB_TRUESIZE(skb->len); /* tcp_rmem */
		skb_pull_inline(skb_in, skb_in->len);
	} else {
		skb_trim(skb, pkt_len);
		skb->truesize = SKB_TRUESIZE(pkt_len); /* tcp_rmem */
		skb_pull_inline(skb_in, pkt_len);
	}
	skbpriv(skb)->iod = get_frag_iod(iod, ld);
send_next_stage:
	ret =  iod->ops.recv_demux(iod, ld, skb);
	if (unlikely(ret < 0)) {
		dev_kfree_skb_any(skb);
		mif_err("skb enqueue fail\n");
		goto exit;
	}

	if (skb_in->len) {
		sipc_hdr = (struct sipc5_link_hdr *)skb_in->data;
		hdr_len = sipc5_get_hdr_len(sipc_hdr);	/* Header length */
		if (unlikely(skb_in->len < hdr_len)) {
			memcpy(hdr->hdr, skb_in->data, skb_in->len);
			hdr->frag_len = skb_in->len;
			/* exit and conitune next rx packet */
		} else {
			goto next_frame;
		}
	}
	mif_debug("end of packets\n");
exit:
	/* free multipacket skb */
	dev_kfree_skb_any(skb_in);
	return ret;
}

/*
 * sipcx_recv_mux() family function
 *
 * Find the io device with IPC header channel id
 *
 */

static int sipc5_recv_demux(struct io_device *iod,
				struct link_device *ld, struct sk_buff *skb)
{
	struct sipc5_link_hdr *sipc5h = (struct sipc5_link_hdr *)skb->data;
	struct io_device *real_iod = skbpriv(skb)->iod ?:
			link_get_iod_with_channel(ld, sipc5h->ch);

	if (unlikely(!real_iod)) {
		mif_err("Invalid real_iod, ch 0x%x\n", sipc5h->ch);
		return -EINVAL;
	}
	return real_iod->ops.recv_skb_packet(real_iod, ld, skb);
}

static int sipc5_recv_demux_ipcloopback(struct io_device *iod,
				struct link_device *ld, struct sk_buff *skb)
{
	struct sipc5_link_hdr *sipc5h = (struct sipc5_link_hdr *)skb->data;
	struct sipc_main_hdr *ipc = (struct sipc_main_hdr *)(skb->data +
			sipc5_get_hdr_len((struct sipc5_link_hdr *)skb->data));
	struct io_device *real_iod = skbpriv(skb)->iod ?:
			link_get_iod_with_channel(ld, sipc5h->ch);

	/* if valid hdr & main cmd 0x91, 0x90 */
	if (sipc5_start_valid(sipc5h) && sipc5h->ch == SIPC5_CH_ID_FMT_0
			&& (ipc->main_cmd == 0x90 || ipc->main_cmd == 0x91)) {
		struct header_data *hdr = &fragdata(iod, ld)->h_data;
		struct sipc5_link_hdr *frag_hdr =
			(struct sipc5_link_hdr *)hdr->hdr;

		frag_hdr->ch = SIPC5_CH_ID_FMT_9;
		real_iod = link_get_iod_with_channel(ld, SIPC5_CH_ID_FMT_9);
		mif_info("%s(%d): IPC_LB first pkt\n",
				real_iod->name, skb->len);
	}

	if (unlikely(!real_iod)) {
		mif_err("Invalid real_iod, ch 0x%x\n", sipc5h->ch);
		return -EINVAL;
	}

	mif_info("%s (%d)\n", real_iod->name, skb->len);
	return real_iod->ops.recv_skb_packet(real_iod, ld, skb);
}

/*
 * sipc_recv_skb() family functions
 *
 * Process single IPC packet.
 *
 */
static int sipc_recv_skb_miscdev(struct io_device *iod,
				struct link_device *ld,	struct sk_buff *skb)
{
	int qlen, ret = skb->len;
	struct sk_buff_head *rxq = &iod->sk_rx_q;

	if (!atomic_read(&iod->opened)) {
		dev_kfree_skb_any(skb);
		return ret;
	}

	/* If the client is NOT consuming the packets, don't accumulate
	 * the packets infinitely. Instead, drop the packets to avoid OOM. */
	qlen = max_t(int, rxq_max, iod->rxq_max);
	if (qlen)
		if (qlen < skb_queue_len(rxq)) {
			struct sk_buff *victim = skb_dequeue(rxq);
			if (victim)
				dev_kfree_skb_any(victim);
			printk_ratelimited(KERN_ERR
				"mif: pkt(iod=%s) dropped\n", iod->name);
		}

	if (iod->waketime)
		wake_lock_timeout(&iod->wakelock, iod->waketime);

	skb_queue_tail(rxq, skb);
	wake_up(&iod->wq);

	return ret;
}

static int sipc5_recv_skb_miscdev_multifmt(struct io_device *iod,
				struct link_device *ld,	struct sk_buff *skb)
{
	int ret;
	unsigned char id;
	struct sipc5_link_hdr *sipc5h = (struct sipc5_link_hdr *)skb->data;

	if (!atomic_read(&iod->opened)) {
		ret = skb->len;
		dev_kfree_skb_any(skb);
		return ret;
	}

	if (iod->waketime)
		wake_lock_timeout(&iod->wakelock, iod->waketime);

	if (sipc5h->cfg & SIPC5_HDR_CTRL) {
		id = sipc5h->ext.ctl & 0x7F;
		if (unlikely(id >= SIPC_MULTIFMT_ID_MAX + 1)) {
			mif_err("multi-packet boundary over = %d\n", id);
			return -EINVAL;
		}
		skb_queue_tail(&iod->sk_multi_q[id], skb);
		if (!(sipc5h->ext.ctl & 0x80)) {
			skb_queue_move(&iod->sk_rx_q, &iod->sk_multi_q[id]);
			wake_up(&iod->wq);
		}
	} else {
		skb_queue_tail(&iod->sk_rx_q, skb);
		wake_up(&iod->wq);
	}
	return skb->len;
}

#ifdef CONFIG_LINK_ETHERNET
#define sipc_type_trans eth_type_trans
#else
static __be16 sipc_type_trans(struct sk_buff *skb, struct net_device *dev)
{
	struct iphdr *iphdr = (struct iphdr *)skb->data;
	skb->dev = dev;
	/* to cover non-IP based loopback, set '0' when it's not an IP packet */
	if (iphdr->version == IP6VERSION)
		return htons(ETH_P_IPV6);
	else if (iphdr->version == IP4VERSION)
		return htons(ETH_P_IP);
	else
		return htons(0);
}
#endif

static int sipc_recv_skb_netdev(struct io_device *iod,
				struct link_device *ld,	struct sk_buff *skb)
{
	int ret;

	if (fd_waketime)
		wake_lock_timeout(&iod->wakelock, iod->waketime);

	if (iod->ops.header_parse) {
		ret = iod->ops.header_parse(iod, skb);
		if (unlikely(ret < 0)) {
			dev_kfree_skb_any(skb);
			return ret;
		}
	}

	skb->protocol = sipc_type_trans(skb, iod->ndev);
	iod->ndev->stats.rx_packets++;
	iod->ndev->stats.rx_bytes += skb->len;

	ret = (in_interrupt()) ? netif_rx(skb) : netif_rx_ni(skb);
	if (ret != NET_RX_SUCCESS)
		printk_ratelimited(KERN_ERR "%s: ERR! netif_rx fail (err %d)\n",
			iod->name, ret);

	return ret;
}

/* TODO: not verified */
static int sipc_recv_skb_netdev_handover(struct io_device *iod,
				struct link_device *ld,	struct sk_buff *skb)
{
	int ret;
	struct ethhdr *ehdr;
	const char source[ETH_ALEN] = SOURCE_MAC_ADDR;

	if (fd_waketime)
		wake_lock_timeout(&iod->wakelock, iod->waketime);

	if (iod->ops.header_parse) {
		ret = iod->ops.header_parse(iod, skb);
		if (unlikely(ret < 0)) {
			dev_kfree_skb_any(skb);
			return ret;
		}
	}

	skb->protocol = sipc_type_trans(skb, iod->ndev);
	skb_push(skb, sizeof(struct ethhdr));
	ehdr = (void *)skb->data;
	memcpy(ehdr->h_dest, iod->ndev->dev_addr, ETH_ALEN);
	memcpy(ehdr->h_source, source, ETH_ALEN);
	ehdr->h_proto = skb->protocol;
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb_reset_mac_header(skb);
	skb_pull(skb, sizeof(struct ethhdr));

	iod->ndev->stats.rx_packets++;
	iod->ndev->stats.rx_bytes += skb->len;

	ret = (in_interrupt()) ? netif_rx(skb) : netif_rx_ni(skb);
	if (ret != NET_RX_SUCCESS)
		mif_info("%s: ERR! netif_rx fail (err %d)\n", iod->name, ret);

	return ret;
}

static int io_dev_recv_skb_from_link_dev(struct io_device *iod,
		struct link_device *ld, struct sk_buff *skb)
{
	if (!skb || skb->len <= 0) {
		mif_info("%s: invalid skb\n", ld->name);
		return -EINVAL;
	}

	skbpriv(skb)->ld = ld;
	skbpriv(skb)->iod = NULL;

	if (!iod->ops.recv_skb_packet) {
		mif_err("ops->recv_skb_signle is mandatory!!\n");
		return -EINVAL;
	}

	/* Call SIPC header deframming chain :
	     recv_skb_fragment() -> recv_demux() -> recv_skb_packet()
	   - recv_skb_fragment()
		: Divide the linear multiframe packet to single skb then
		call the recv_demux()
		Ex) HSIC cdc-acm fmt/rfs serial SIPC muxed packets.
	   - recv_demux()
		: Find the real iodevice  frame SIPC muxed packet then
		call the recv_skb_packet()
		Ex) RFS over CDC-NCM or DPRAM, link device send with single
		SIPC muxed packet.
	   - recv_skb_packet()
		: Process rx packet with iod device type and SIPC header.
	*/
	if (iod->ops.recv_skb_fragment)
		return iod->ops.recv_skb_fragment(iod, ld, skb);
	if (iod->ops.recv_demux)
		return iod->ops.recv_demux(iod, ld, skb);

	return iod->ops.recv_skb_packet(iod, ld, skb);
}

static inline void sipc_print_ops(struct io_device *iod)
{
	/* Print SIPC mandatory ops */
	mif_info("%s: %pf(), %pf(), %pf(), %pf()\n", iod->name,
		iod->ops.header_create,	iod->ops.header_parse,
		iod->ops.recv_demux, iod->ops.recv_skb_packet);
	/* Print SIPC optional ops */
	if (iod->ops.multifmt_length)
		mif_info("multifmt_length = %pf()\n",
			iod->ops.multifmt_length);
	if (iod->ops.header_parse_continue)
		mif_info("header_parse_continue = %pf()\n",
			iod->ops.header_parse_continue);
	if (iod->ops.recv_skb_fragment)
		mif_info("recv_skb_fragment = %pf()\n",
			iod->ops.recv_skb_fragment);
}

static void sipc5_get_ops(struct io_device *iod)
{
	iod->headroom += SIPC_HDR_LEN_MAX;
	iod->ops.recv_demux = sipc5_recv_demux;
	iod->ops.header_create = sipc5_hdr_create;
	iod->ops.header_parse = sipc5_hdr_parse;

	if (iod->attr & IODEV_ATTR(ATTR_MULTIFMT)) {
		iod->ops.header_create = sipc5_hdr_create_multifmt;
		iod->ops.multifmt_length = sipc_hdr_multifmt_length;
		iod->ops.recv_skb_packet = sipc5_recv_skb_miscdev_multifmt;
		iod->ops.header_parse_continue =
			sipc5_hdr_parse_multifmt_continue;
	}
	if (iod->attr & IODEV_ATTR(ATTR_RX_FRAGMENT)) {
		iod->ops.header_parse = sipc5_hdr_parse_fragment;
		iod->ops.header_parse_continue =
					sipc5_hdr_parse_fragment_continue;
		iod->ops.recv_skb_fragment =
					sipc5_recv_multipacket_to_each_skb;
	}
	if (iod->attr & IODEV_ATTR(ATTR_LEGACY_RFS)) {
		iod->ops.header_create = sipc5_hdr_create_legacy_rfs;
		iod->ops.header_parse = sipc5_hdr_parse_legacy_rfs;
		iod->ops.header_parse_continue =
					sipc5_hdr_parse_legacy_rfs_continue;
	}
	if (iod->attr & IODEV_ATTR(ATTR_HANDOVER) && iod->io_typ == IODEV_NET
		&& iod->id >= PS_DATA_CH_0 && iod->id <= PS_DATA_CH_LAST) {
		iod->ops.header_create = sipc5_hdr_create_handover;
		iod->ops.recv_skb_packet = sipc_recv_skb_netdev_handover;
	}
	sipc_print_ops(iod);
}

/*TODO: not verified */
static void sipc4_get_ops(struct io_device *iod)
{
	/* TODO:*/
}

static void sipc_get_ops(struct io_device *iod)
{
	iod->ops.recv_skb_packet = (iod->io_typ == IODEV_NET)
		? sipc_recv_skb_netdev : sipc_recv_skb_miscdev;

	if (iod->attr & IODEV_ATTR(ATTR_SIPC4))
		return sipc4_get_ops(iod);
	if (iod->attr & IODEV_ATTR(ATTR_SIPC5))
		return sipc5_get_ops(iod);
}

/* inform the IO device that the modem is now online or offline or
 * crashing or whatever...
 */
static void io_dev_modem_state_changed(struct io_device *iod,
			enum modem_state state)
{
	mif_info("%s: %s state changed (state %d)\n",
		iod->name, iod->mc->name, state);

	iod->mc->phone_state = state;

	if (state == STATE_CRASH_RESET || state == STATE_CRASH_EXIT ||
	    state == STATE_CRASH_WATCHDOG || state == STATE_NV_REBUILDING) {
		wake_lock_timeout(&iod->wakelock, msecs_to_jiffies(2000));
		wake_up(&iod->wq);
	}
}

/**
 * io_dev_sim_state_changed
 * @iod:	IPC's io_device
 * @sim_online: SIM is online?
 */
static void io_dev_sim_state_changed(struct io_device *iod, bool sim_online)
{
	if (atomic_read(&iod->opened) == 0) {
		mif_info("%s: ERR! not opened\n", iod->name);
	} else if (iod->mc->sim_state.online == sim_online) {
		mif_info("%s: SIM state not changed\n", iod->name);
	} else {
		iod->mc->sim_state.online = sim_online;
		iod->mc->sim_state.changed = true;
		mif_info("%s: SIM state changed {online %d, changed %d}\n",
			iod->name, iod->mc->sim_state.online,
			iod->mc->sim_state.changed);
		wake_up(&iod->wq);
	}
}

static void iodev_dump_status(struct io_device *iod, void *args)
{
	if (iod->format == IPC_RAW && iod->io_typ == IODEV_NET) {
		struct link_device *ld = get_current_link(iod);
		mif_com_log(iod->mc->msd, "%s: %s\n", iod->name, ld->name);
	}
}

static int misc_open(struct inode *inode, struct file *filp)
{
	struct io_device *iod = to_io_device(filp->private_data);
	struct modem_shared *msd = iod->msd;
	struct link_device *ld;
	int ret;
	filp->private_data = (void *)iod;

	atomic_inc(&iod->opened);

	list_for_each_entry(ld, &msd->link_dev_list, list) {
		if (IS_CONNECTED(iod, ld) && ld->init_comm) {
			ret = ld->init_comm(ld, iod);
			if (ret < 0) {
				mif_info("%s: init_comm fail(%d)\n",
					ld->name, ret);
				atomic_dec(&iod->opened);
				return ret;
			}
		}
	}

	mif_info("%s (opened %d)\n", iod->name, atomic_read(&iod->opened));

	return 0;
}

static int misc_release(struct inode *inode, struct file *filp)
{
	struct io_device *iod = (struct io_device *)filp->private_data;
	struct modem_shared *msd = iod->msd;
	struct link_device *ld;

	if (atomic_dec_and_test(&iod->opened))
		skb_queue_purge(&iod->sk_rx_q);

	list_for_each_entry(ld, &msd->link_dev_list, list) {
		if (IS_CONNECTED(iod, ld)) {
			struct header_data *hdr = &fragdata(iod, ld)->h_post;
			if (hdr->frag_len > 0) {
				mif_info("fragdata should be cleared.(%d)\n",
								hdr->frag_len);
				memset(fragdata(iod, ld),
					0x00, sizeof(struct fragmented_data));
			}
			if (ld->terminate_comm)
				ld->terminate_comm(ld, iod);
		}
	}

	mif_info("%s (opened %d)\n", iod->name, atomic_read(&iod->opened));

	return 0;
}

static unsigned int misc_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct io_device *iod = (struct io_device *)filp->private_data;

	if (!iod)
		return POLLERR;

	if (skb_queue_empty(&iod->sk_rx_q))
		poll_wait(filp, &iod->wq, wait);

	switch (iod->mc->phone_state) {
	case STATE_BOOTING:
	case STATE_ONLINE:
		if (!iod->mc->sim_state.changed) {
			if (!skb_queue_empty(&iod->sk_rx_q))
				return POLLIN | POLLRDNORM;
			else /* wq is waken up without rx, return for wait */
				return 0;
		}
		/* fall through, when sim state has been changed */
	case STATE_CRASH_EXIT:
	case STATE_CRASH_RESET:
	case STATE_CRASH_WATCHDOG:
	case STATE_NV_REBUILDING:
		/* report crash only if iod is fmt/boot device */
		if (iod->format == IPC_FMT || iod->format == IPC_BOOT)
			return POLLHUP;
		/* otherwise,
		 * give delay to prevent infinite sys_poll call from select()
		 * without 'sleep' user call takes almost 100% cpu usage when
		 * it is looked up by 'top' command.
		 */
		msleep(20);
		break;
	case STATE_OFFLINE:
		/* fall through */
	default:
		break;
	}

	return 0;
}

static char cpinfo_buf[MAX_CPINFO_SIZE] = {0,};
static long misc_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int p_state;
	struct io_device *iod = (struct io_device *)filp->private_data;
	struct link_device *ld = get_current_link(iod);
	unsigned long size;
	int ret;
	int tx_link;

	switch (cmd) {
	case IOCTL_MODEM_ON:
		mif_info("%s: IOCTL_MODEM_ON\n", iod->name);
		return iod->mc->ops.modem_on(iod->mc);

	case IOCTL_MODEM_OFF:
		mif_info("%s: IOCTL_MODEM_OFF\n", iod->name);
		return iod->mc->ops.modem_off(iod->mc);

	case IOCTL_MODEM_RESET:
		mif_info("%s: IOCTL_MODEM_RESET\n", iod->name);
		return iod->mc->ops.modem_reset(iod->mc);

	case IOCTL_MODEM_BOOT_ON:
		mif_info("%s: IOCTL_MODEM_BOOT_ON\n", iod->name);
		return iod->mc->ops.modem_boot_on(iod->mc);

	case IOCTL_MODEM_BOOT_OFF:
		mif_info("%s: IOCTL_MODEM_BOOT_OFF\n", iod->name);
		return iod->mc->ops.modem_boot_off(iod->mc);

	case IOCTL_MODEM_BOOT_DONE:
		mif_info("%s: IOCTL_MODEM_BOOT_DONE\n", iod->name);
		if (iod->mc->ops.modem_boot_done)
			return iod->mc->ops.modem_boot_done(iod->mc);
		else
			return 0;

	case IOCTL_MODEM_STATUS:
		mif_debug("%s: IOCTL_MODEM_STATUS\n", iod->name);

		p_state = iod->mc->phone_state;
		if ((p_state == STATE_CRASH_RESET) ||
			(p_state == STATE_CRASH_EXIT) ||
			(p_state == STATE_CRASH_WATCHDOG)) {
			mif_info("%s: IOCTL_MODEM_STATUS (state %d)\n",
				iod->name, p_state);
		} else if (iod->mc->sim_state.changed) {
			int s_state = iod->mc->sim_state.online ?
					STATE_SIM_ATTACH : STATE_SIM_DETACH;
			iod->mc->sim_state.changed = false;
			return s_state;
		} else if (p_state == STATE_NV_REBUILDING) {
			mif_info("%s: IOCTL_MODEM_STATUS (state %d)\n",
				iod->name, p_state);
			iod->mc->phone_state = STATE_ONLINE;
		}
		return p_state;

	case IOCTL_MODEM_PROTOCOL_SUSPEND:
		mif_debug("%s: IOCTL_MODEM_PROTOCOL_SUSPEND\n",
			iod->name);

		if (iod->format != IPC_MULTI_RAW)
			return -EINVAL;

		iodevs_for_each(iod->msd, iodev_netif_stop, 0);
		return 0;

	case IOCTL_MODEM_PROTOCOL_RESUME:
		mif_info("%s: IOCTL_MODEM_PROTOCOL_RESUME\n",
			iod->name);

		if (iod->format != IPC_MULTI_RAW)
			return -EINVAL;

		iodevs_for_each(iod->msd, iodev_netif_wake, 0);
		return 0;

	case IOCTL_MODEM_DUMP_START:
		mif_info("%s: IOCTL_MODEM_DUMP_START\n", iod->name);
		return ld->dump_start(ld, iod);

	case IOCTL_MODEM_DUMP_UPDATE:
		mif_debug("%s: IOCTL_MODEM_DUMP_UPDATE\n", iod->name);
		return ld->dump_update(ld, iod, arg);

	case IOCTL_MODEM_FORCE_CRASH_EXIT:
		mif_info("%s: IOCTL_MODEM_FORCE_CRASH_EXIT\n", iod->name);
		if (iod->mc->ops.modem_force_crash_exit)
			return iod->mc->ops.modem_force_crash_exit(iod->mc);
		return -EINVAL;

	case IOCTL_MODEM_CP_UPLOAD:
		mif_info("%s: IOCTL_MODEM_CP_UPLOAD\n", iod->name);
		if (copy_from_user(cpinfo_buf,
			(void __user *)arg, MAX_CPINFO_SIZE) != 0)
			return -EFAULT;
		mif_err("CP Crash - %s\n", cpinfo_buf);
		panic("CP Crash (%s) %s", iod->mc->name, cpinfo_buf);
		return 0;

	case IOCTL_MODEM_DUMP_RESET:
		mif_info("%s: IOCTL_MODEM_DUMP_RESET\n", iod->name);
		return iod->mc->ops.modem_dump_reset(iod->mc);

	case IOCTL_MIF_LOG_DUMP:
		iodevs_for_each(iod->msd, iodev_dump_status, 0);
		size = MAX_MIF_BUFF_SIZE;
		ret = copy_to_user((void __user *)arg, &size,
			sizeof(unsigned long));
		if (ret < 0)
			return -EFAULT;

		return mif_dump_log(iod->mc->msd, iod);

	case IOCTL_MODEM_SET_TX_LINK:
		mif_info("%s: IOCTL_MODEM_SET_TX_LINK\n", iod->name);
		if (copy_from_user(&tx_link, (void __user *)arg, sizeof(int)))
			return -EFAULT;

		mif_info("cur link: %d, new link: %d\n",
				ld->link_type, tx_link);

		if (ld->link_type != tx_link) {
			mif_info("change link: %d -> %d\n",
				ld->link_type, tx_link);
			ld = find_linkdev(iod->msd, tx_link);
			if (!ld) {
				mif_err("find_linkdev(%d) fail\n", tx_link);
				return -ENODEV;
			}

			set_current_link(iod, ld);

			ld = get_current_link(iod);
			mif_info("%s tx_link change success\n",	ld->name);
		}

		return 0;
	case IOCTL_MODEM_WATCHDOG_CRASH:
		mif_info("%s: IOCTL_MODEM_CP_WATCHDOG_CRASH\n", iod->name);
		iod->mc->phone_state = STATE_CRASH_WATCHDOG;
		return 0;

	default:
		 /* If you need to handle the ioctl for specific link device,
		  * then assign the link ioctl handler to ld->ioctl
		  * It will be call for specific link ioctl */
		if (ld->ioctl)
			return ld->ioctl(ld, iod, cmd, arg);

		mif_info("%s: ERR! cmd 0x%X not defined.\n", iod->name, cmd);
		return -EINVAL;
	}
	return 0;
}

static void check_ipc_loopback(struct io_device *iod, struct sk_buff *skb)
{
	struct sipc_main_hdr *ipc;
	struct sk_buff *skb_bk;
	struct link_device *ld = skbpriv(skb)->ld;
	int ret;


	if (!skb || !test_bit(IOD_DEBUG_IPC_LOOPBACK, &dbg_flags)
						|| iod->format != IPC_FMT)
		return;

	ipc = (struct sipc_main_hdr *)(skb->data + sipc5_get_hdr_len(
					(struct sipc5_link_hdr *)skb->data));
	if (ipc->main_cmd == 0x90) { /* loop-back */
		int timeout;
		skb_bk = skb_clone(skb, GFP_KERNEL);
		if (!skb_bk) {
			mif_err("SKB clone fail\n");
			return;
		}
		ret = ld->send(ld, iod,	skb_bk);
		if (ret < 0)
			mif_err("ld->send fail (%s, err %d)\n", iod->name, ret);
		/* Because CP IPC loop period is 5 second, we can try variery
		 * timmging with wakelock 2000 ~ 4555ms */
		timeout = (jiffies & 0x1ff) + HZ * 2;
		wake_lock_timeout(&iod->wakelock, timeout);
		mif_debug("lb wakelock = %d\n", jiffies_to_msecs(timeout));
	}
	return;
}

/* It send continous transation with sperate skb, BOOTDATA size should divided
 * with 512B(USB packet size) and remove the zero length packet for mark not
 * end of packet.
 */
#define MAX_BOOTDATA_SIZE	0xE00	/* EBL package format*/
static size_t _boot_write(struct io_device *iod, const char __user *buf,
								size_t count)
{
	int rest_len = count, frame_len = 0;
	char *cur = (char *)buf;
	struct sk_buff *skb = NULL;
	struct link_device *ld = get_current_link(iod);
	int ret;

	while (rest_len) {
		frame_len = min(rest_len, MAX_BOOTDATA_SIZE);
		skb = alloc_skb(frame_len, GFP_KERNEL);
		if (!skb) {
			mif_err("fail alloc skb (%d)\n", __LINE__);
			return -ENOMEM;
		}
		if (copy_from_user(
				skb_put(skb, frame_len), cur, frame_len) != 0) {
			dev_kfree_skb_any(skb);
			return -EFAULT;
		}
		rest_len -= frame_len;
		cur += frame_len;

		/* non-zerolength packet*/
		if (rest_len)
			skbpriv(skb)->nzlp = true;

		mif_debug("rest=%d, frame=%d, nzlp=%d\n", rest_len, frame_len,
							skbpriv(skb)->nzlp);
		ret = ld->send(ld, iod, skb);
		if (ret < 0) {
			dev_kfree_skb_any(skb);
			return ret;
		}
	}
	return count;
}

static ssize_t misc_write(struct file *filp, const char __user *data,
			size_t count, loff_t *fpos)
{
	struct io_device *iod = (struct io_device *)filp->private_data;
	struct link_device *ld = get_current_link(iod);
	struct sk_buff *skb;
	struct sipc_hdr hdr = {.multifmt = 0};
	int ret;
	unsigned len, copied = 0, tx_size;

multifmt:
	len = (iod->ops.multifmt_length) ?
		iod->ops.multifmt_length(iod, &hdr, copied, count) : count;

	skb = alloc_skb(len + iod->headroom + 3, GFP_KERNEL);
	if (!skb) {
		if (len > MAX_BOOTDATA_SIZE && iod->format == IPC_BOOT)
			return _boot_write(iod, data, len);

		mif_err("alloc_skb fail(%s, %d+)\n", iod->name, len);
		return -ENOMEM;
	}

	if (iod->headroom) {
		if (iod->attr & IODEV_ATTR(ATTR_CDC_NCM)) {
			skb_reserve(skb, iod->headroom);
		} else {
			hdr.hdr_size = sipc5_get_header_size(&hdr, len);
			skb_reserve(skb, hdr.hdr_size);
		}
	}

	if (copy_from_user(skb_put(skb, len), data + copied, len) != 0) {
		if (skb)
			dev_kfree_skb_any(skb);
		return -EFAULT;
	}
	copied += len;

	if (iod->ops.header_create)
		iod->ops.header_create(iod, &hdr, skb);

	tx_size = skb->len;
	ret = ld->send(ld, iod, skb);
	if (ret < 0) {
		mif_err("ld->send fail (%s, err %d)\n", iod->name, ret);
		return ret;
	}

	if (ret != tx_size)
		mif_info("wrong tx size (%s, count:%d copied:%d ret:%d)\n",
			iod->name, count, tx_size, ret);

	if (iod->format == IPC_FMT && copied < count) {
		mif_info("continoue fmt multiframe(%u/%u)\n", copied, count);
		goto multifmt;
	}

	return count;
}

static ssize_t misc_read(struct file *filp, char *buf, size_t count,
			loff_t *fpos)
{
	struct io_device *iod = (struct io_device *)filp->private_data;
	struct sk_buff_head *rxq = &iod->sk_rx_q;
	struct sk_buff *skb;
	unsigned copied = 0, len;
	int status = 0;

continue_multiframe:
	skb = skb_dequeue(rxq);
	if (!skb) {
		printk_ratelimited(KERN_ERR "%s: ERR! no data in rxq\n", iod->name);
		goto exit;
	}

	check_ipc_loopback(iod, skb);

	if (iod->ops.header_parse) {
		status = iod->ops.header_parse(iod, skb);
		if (unlikely(status < 0)) {
			dev_kfree_skb_any(skb);
			return status;
		}
	}

	len = min(skb->len, count - copied);
	if (copy_to_user(buf + copied, skb->data, len)) {
		mif_info("%s: ERR! copy_to_user fail\n", iod->name);
		skb_queue_head(rxq, skb);
		return -EFAULT;
	}
	copied += len;
	skb_pull_inline(skb, len);
	if (iod->ops.header_parse_continue)
		status = iod->ops.header_parse_continue(iod, skb, len);

	mif_debug("%s: data:%d count:%d copied:%d qlen:%d\n",
				iod->name, len, count, copied, rxq->qlen);

	if (skb->len)
		skb_queue_head(rxq, skb);
	else
		dev_kfree_skb_any(skb);

	if (status == 1 && count - copied) {
		mif_info("multi_frame\n");
		goto continue_multiframe;
	}
exit:
	return copied;
}

static const struct file_operations misc_io_fops = {
	.owner = THIS_MODULE,
	.open = misc_open,
	.release = misc_release,
	.poll = misc_poll,
	.unlocked_ioctl = misc_ioctl,
	.write = misc_write,
	.read = misc_read,
};

static int vnet_open(struct net_device *ndev)
{
	struct vnet *vnet = netdev_priv(ndev);
	struct io_device *iod = vnet->iod;

	mif_info("%s\n", iod->name);

	netif_start_queue(ndev);
	atomic_inc(&iod->opened);
	list_add(&iod->node_ndev, &iod->msd->activated_ndev_list);
	return 0;
}

static int vnet_stop(struct net_device *ndev)
{
	struct vnet *vnet = netdev_priv(ndev);
	struct io_device *iod = vnet->iod;

	mif_info("%s\n", iod->name);

	atomic_dec(&iod->opened);
	netif_stop_queue(ndev);
	skb_queue_purge(&iod->sk_rx_q);
	list_del(&iod->node_ndev);
	return 0;
}

static int vnet_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct vnet *vnet = netdev_priv(ndev);
	struct io_device *iod = vnet->iod;
	struct link_device *ld = get_current_link(iod);
	struct sk_buff *skb_new = skb;
	unsigned long tx_bytes = skb->len;
	int tailroom = (ld->aligned > 0) ? SIPC_ALIGN_PAD_MAX : 0;
	int ret;

	if (skb_headroom(skb) < iod->headroom || skb_tailroom(skb) < tailroom) {
		mif_info("%s: skb needs copy_expand hr=%d tr=%d, iod hr=%d\n",
			iod->name,
			skb_headroom(skb), skb_tailroom(skb),
			iod->headroom);
		skb_new = skb_copy_expand(skb, iod->headroom, 3, GFP_ATOMIC);
		if (!skb_new) {
			mif_err("%s: skb_copy_expand fail\n", iod->name);
			return NETDEV_TX_BUSY;
		}
		tx_bytes = skb_new->len;
		dev_kfree_skb_any(skb);
	}

	if (iod->ops.header_create) {
		struct sipc_hdr hdr = {.multifmt = 0};
		iod->ops.header_create(iod, &hdr, skb_new);
	}

	ret = ld->send(ld, iod, skb_new);
	if (ret < 0) {
		netif_stop_queue(ndev);
		mif_info("%s: ERR! ld->send fail (err %d)\n", iod->name, ret);
		switch (ret) {
		case -EINVAL:
			mif_info("%s: not available\n", iod->name);
			return -ENODEV;
			break;
		default:
			return NETDEV_TX_BUSY;
		}
	}
	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += tx_bytes;

	return NETDEV_TX_OK;
}

static struct net_device_ops vnet_ops = {
	.ndo_open = vnet_open,
	.ndo_stop = vnet_stop,
	.ndo_start_xmit = vnet_xmit,
};

/* ehter ops for CDC-NCM */
#ifdef CONFIG_LINK_ETHERNET
static struct net_device_ops vnet_ether_ops = {
	.ndo_open = vnet_open,
	.ndo_stop = vnet_stop,
	.ndo_start_xmit = vnet_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};
#endif
static void vnet_setup(struct net_device *ndev)
{
	ndev->netdev_ops = &vnet_ops;
	ndev->type = ARPHRD_PPP;
	ndev->flags = IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST;
	ndev->addr_len = 0;
	ndev->hard_header_len = 0;
	ndev->tx_queue_len = 1000;
	ndev->mtu = ETH_DATA_LEN;
	ndev->watchdog_timeo = 5 * HZ;
	ndev->needed_headroom = SIPC_HDR_LEN_MAX;
	ndev->needed_tailroom = SIPC_ALIGN_PAD_MAX;
}

static void vnet_setup_ether(struct net_device *ndev)
{
	vnet_setup(ndev);
	ndev->type = ARPHRD_ETHER;
	ndev->flags |= IFF_SLAVE;
	ndev->addr_len = ETH_ALEN;
	random_ether_addr(ndev->dev_addr);
}

int sipc5_init_io_device(struct io_device *iod)
{
	int i, ret = 0;
	struct vnet *vnet;

	/* Get modem state from modem control device */
	iod->modem_state_changed = io_dev_modem_state_changed;

	iod->sim_state_changed = io_dev_sim_state_changed;

	/* Get data from link device */
	mif_debug("%s: SIPC version = %d\n", iod->name, iod->ipc_version);
	iod->recv_skb = io_dev_recv_skb_from_link_dev;

	if (iod->attr & IODEV_ATTR(ATTR_CDC_NCM))
		iod->headroom += sizeof(struct ethhdr);

	if (iod->attr & IODEV_ATTR(ATTR_MULTIFMT))
		for (i = 0; i <= SIPC_MULTIFMT_ID_MAX; i++)
			skb_queue_head_init(&iod->sk_multi_q[i]);

	/* Register misc or net device */
	switch (iod->io_typ) {
	case IODEV_MISC:
		init_waitqueue_head(&iod->wq);
		skb_queue_head_init(&iod->sk_rx_q);

		iod->miscdev.minor = MISC_DYNAMIC_MINOR;
		iod->miscdev.name = iod->name;
		iod->miscdev.fops = &misc_io_fops;

		ret = misc_register(&iod->miscdev);
		if (ret)
			mif_info("%s: ERR! misc_register failed\n", iod->name);

		/* TODO: to be delete after discuss with RIL-Team
		  Below SYSFS can switch between XMM6360 silent logging channel
		  and CSVT raw channel over SIPC4.x, but JA and later projects
		  use the SIPC5.0 and dose not needed it.
		  Before remove that, link device should map CH28 and ACM2 */
		if (iod->id == SIPC_CH_ID_CPLOG1) {
			ret = device_create_file(iod->miscdev.this_device,
							&attr_dm_state);
			if (ret)
				mif_err("failed to create `dm_state' : %s\n",
					iod->name);
			else
				mif_info("dm_state : %s, sucess\n", iod->name);
		}

		break;

	case IODEV_NET:
		skb_queue_head_init(&iod->sk_rx_q);
		INIT_LIST_HEAD(&iod->node_ndev);
#ifdef CONFIG_LINK_ETHERNET
		iod->ndev = alloc_etherdev(0);
		if (!iod->ndev) {
			mif_err("failed to alloc netdev\n");
			return -ENOMEM;
		}
		iod->ndev->netdev_ops = &vnet_ether_ops;
		iod->ndev->watchdog_timeo = 5 * HZ;
#else
		if (iod->use_handover)
			iod->ndev = alloc_netdev(0, iod->name,
						vnet_setup_ether);
		else
			iod->ndev = alloc_netdev(0, iod->name, vnet_setup);

		if (!iod->ndev) {
			mif_info("%s: ERR! alloc_netdev fail\n", iod->name);
			return -ENOMEM;
		}
#endif
		/*register_netdev parsing % */
		strcpy(iod->ndev->name, "rmnet%d");

		if (iod->attr & IODEV_ATTR(ATTR_CDC_NCM))
			iod->ndev->needed_headroom += sizeof(struct ethhdr);

		ret = register_netdev(iod->ndev);
		if (ret) {
			mif_info("%s: ERR! register_netdev fail\n", iod->name);
			free_netdev(iod->ndev);
		}

		mif_debug("iod 0x%p\n", iod);
		vnet = netdev_priv(iod->ndev);
		mif_debug("vnet 0x%p\n", vnet);
		vnet->iod = iod;
		break;

	case IODEV_DUMMY:
		skb_queue_head_init(&iod->sk_rx_q);

		iod->miscdev.minor = MISC_DYNAMIC_MINOR;
		iod->miscdev.name = iod->name;
		iod->miscdev.fops = &misc_io_fops;

		ret = misc_register(&iod->miscdev);
		if (ret)
			mif_info("%s: ERR! misc_register fail\n", iod->name);
		ret = device_create_file(iod->miscdev.this_device,
					&attr_waketime);
		if (ret)
			mif_info("%s: ERR! device_create_file fail\n",
				iod->name);
		ret = device_create_file(iod->miscdev.this_device,
				&attr_loopback);
		if (ret)
			mif_err("failed to create `loopback file' : %s\n",
					iod->name);
		ret = device_create_file(iod->miscdev.this_device,
				&attr_txlink);
		if (ret)
			mif_err("failed to create `txlink file' : %s\n",
					iod->name);
		ret = device_create_file(iod->miscdev.this_device,
				&attr_ipcloopback);
		if (ret)
			mif_err("failed to create `ipcloopback file' : %s\n",
					iod->name);
		break;

	default:
		mif_info("%s: ERR! wrong io_type %d\n", iod->name, iod->io_typ);
		return -EINVAL;
	}
	sipc_get_ops(iod);

	return ret;
}

