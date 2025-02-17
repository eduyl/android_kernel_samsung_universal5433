/*
 * Header file describing the internal (inter-module) DHD interfaces.
 *
 * Provides type definitions and function prototypes used to link the
 * DHD OS, bus, and protocol modules.
 *
 * Copyright (C) 1999-2017, Broadcom Corporation
 * 
 *      Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2 (the "GPL"),
 * available at http://www.broadcom.com/licenses/GPLv2.php, with the
 * following added to such license:
 * 
 *      As a special exception, the copyright holders of this software give you
 * permission to link this software with independent modules, and to copy and
 * distribute the resulting executable under terms of your choice, provided that
 * you also meet, for each linked independent module, the terms and conditions of
 * the license of that module.  An independent module is a module which is not
 * derived from this software.  The special exception does not apply to any
 * modifications of the software.
 * 
 *      Notwithstanding the above, under no circumstances may you combine this
 * software in any way with any other Broadcom software provided under a license
 * other than the GPL, without Broadcom's express prior written consent.
 *
 * $Id: dhd_msgbuf.c 692310 2017-03-27 13:03:10Z $
 */
#include <typedefs.h>
#include <osl.h>

#include <bcmutils.h>
#include <bcmmsgbuf.h>
#include <bcmendian.h>

#include <dngl_stats.h>
#include <dhd.h>
#include <dhd_proto.h>
#include <dhd_bus.h>
#include <dhd_dbg.h>

#include <siutils.h>


#include <dhd_flowring.h>

#include <pcie_core.h>
#include <bcmpcie.h>
#include <dhd_pcie.h>

/*
 * PCIE D2H DMA Complete Sync Modes
 *
 * Firmware may interrupt the host, prior to the D2H Mem2Mem DMA completes into
 * Host system memory. A WAR using one of 3 approaches is needed:
 * 1. Dongle places ia modulo-253 seqnum in last word of each D2H message
 * 2. XOR Checksum, with epoch# in each work item. Dongle builds an XOR checksum
 *    writes in the last word of each work item. Each work item has a seqnum
 *    number = sequence num % 253.
 * 3. Read Barrier: Dongle does a host memory read access prior to posting an
 *    interrupt.
 * Host does not participate with option #3, other than reserving a host system
 * memory location for the dongle to read.
 */
#define PCIE_D2H_SYNC
#define PCIE_D2H_SYNC_WAIT_TRIES	(512UL)
#define PCIE_D2H_SYNC_NUM_OF_STEPS	(3UL)
#define PCIE_D2H_SYNC_DELAY		(50UL)	/* in terms of usecs */

#define RETRIES 2		/* # of retries to retrieve matching ioctl response */
#define IOCTL_HDR_LEN	12

#define DEFAULT_RX_BUFFERS_TO_POST	256
#define RXBUFPOST_THRESHOLD			32
#define RX_BUF_BURST				16

#define DHD_STOP_QUEUE_THRESHOLD	200
#define DHD_START_QUEUE_THRESHOLD	100

#define MODX(x, n)	((x) & ((n) -1))
#define align(x, n)	(MODX(x, n) ? ((x) - MODX(x, n) + (n)) : ((x) - MODX(x, n)))
#define RX_DMA_OFFSET		8
#define IOCT_RETBUF_SIZE	(RX_DMA_OFFSET + WLC_IOCTL_MAXLEN)

#define DMA_D2H_SCRATCH_BUF_LEN	8
#define DMA_ALIGN_LEN		4
#define DMA_XFER_LEN_LIMIT	0x400000

#define DHD_FLOWRING_IOCTL_BUFPOST_PKTSZ		8192

#define DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D		1
#define DHD_FLOWRING_MAX_EVENTBUF_POST			8
#define DHD_FLOWRING_MAX_IOCTLRESPBUF_POST		8

#define DHD_PROT_FUNCS	22

typedef struct dhd_mem_map {
	void *va;
	dmaaddr_t pa;
	void *dmah;
} dhd_mem_map_t;

typedef struct dhd_dmaxfer {
	dhd_mem_map_t	srcmem;
	dhd_mem_map_t	destmem;
	uint32		len;
	uint32		srcdelay;
	uint32		destdelay;
} dhd_dmaxfer_t;

#define TXP_FLUSH_NITEMS
#define TXP_FLUSH_MAX_ITEMS_FLUSH_CNT	48

typedef struct msgbuf_ring {
	bool		inited;
	uint16		idx;
	uchar		name[24];
	dhd_mem_map_t	ring_base;
#ifdef TXP_FLUSH_NITEMS
	void*		start_addr;
	uint16		pend_items_count;
#endif /* TXP_FLUSH_NITEMS */
	ring_mem_t	*ringmem;
	ring_state_t	*ringstate;
#if defined(PCIE_D2H_SYNC)
	uint32      seqnum;
#endif  /* PCIE_D2H_SYNC */
} msgbuf_ring_t;

#if defined(PCIE_D2H_SYNC)
/* Custom callback attached based upon D2H DMA Sync mode used in dongle. */
typedef uint8 (* d2h_sync_cb_t)(dhd_pub_t *dhd, msgbuf_ring_t *ring,
                                volatile cmn_msg_hdr_t *msg, int msglen);
#endif /* PCIE_D2H_SYNC */

typedef struct dhd_prot {
	osl_t *osh;		/* OSL handle */
	uint32 reqid;
	uint32 lastcmd;
	uint32 pending;
	uint16 rxbufpost;
	uint16 max_rxbufpost;
	uint16 max_eventbufpost;
	uint16 max_ioctlrespbufpost;
	uint16 cur_event_bufs_posted;
	uint16 cur_ioctlresp_bufs_posted;
	uint16 active_tx_count;
	uint16 max_tx_count;
	uint16 txp_threshold;
	/* Ring info */
	msgbuf_ring_t	*h2dring_txp_subn;
	msgbuf_ring_t	*h2dring_rxp_subn;
	msgbuf_ring_t	*h2dring_ctrl_subn;	/* Cbuf handle for H2D ctrl ring */
	msgbuf_ring_t	*d2hring_tx_cpln;
	msgbuf_ring_t	*d2hring_rx_cpln;
	msgbuf_ring_t	*d2hring_ctrl_cpln;	/* Cbuf handle for D2H ctrl ring */
	uint32		rx_dataoffset;
	dhd_mem_map_t	retbuf;
	dhd_mem_map_t	ioctbuf;	/* For holding ioct request buf */
	dhd_mb_ring_t	mb_ring_fn;

	uint32		d2h_dma_scratch_buf_len; /* For holding ioct request buf */
	dhd_mem_map_t	d2h_dma_scratch_buf;	/* For holding ioct request buf */

	uint32	h2d_dma_writeindx_buf_len; /* For holding dma ringupd buf - submission write */
	dhd_mem_map_t 	h2d_dma_writeindx_buf;	/* For holding dma ringupd buf - submission write */

	uint32	h2d_dma_readindx_buf_len; /* For holding dma ringupd buf - submission read */
	dhd_mem_map_t	h2d_dma_readindx_buf;	/* For holding dma ringupd buf - submission read */

	uint32	d2h_dma_writeindx_buf_len; /* For holding dma ringupd buf - completion write */
	dhd_mem_map_t	d2h_dma_writeindx_buf;	/* For holding dma ringupd buf - completion write */

	uint32	d2h_dma_readindx_buf_len; /* For holding dma ringupd buf - completion read */
	dhd_mem_map_t	d2h_dma_readindx_buf;	/* For holding dma ringupd buf - completion read */

#if defined(PCIE_D2H_SYNC)
	d2h_sync_cb_t d2h_sync_cb; /* Sync on D2H DMA done: SEQNUM or XORCSUM */
	ulong d2h_sync_wait_max; /* max number of wait loops to receive one msg */
	ulong d2h_sync_wait_tot; /* total wait loops */
#endif  /* PCIE_D2H_SYNC */
	dhd_dmaxfer_t	dmaxfer;
	bool		dmaxfer_in_progress;

	uint16		ioctl_seq_no;
	uint16		data_seq_no;
	uint16		ioctl_trans_id;
	void		*pktid_map_handle;
	uint16		rx_metadata_offset;
	uint16		tx_metadata_offset;
	uint16          rx_cpln_early_upd_idx;
	struct mutex	ioctl_mutex;	/* Make IOCTL singleton in Prot Layer */
} dhd_prot_t;

static int dhdmsgbuf_query_ioctl(dhd_pub_t *dhd, int ifidx, uint cmd,
	void *buf, uint len, uint8 action);
static int dhd_msgbuf_set_ioctl(dhd_pub_t *dhd, int ifidx, uint cmd,
	void *buf, uint len, uint8 action);
static int dhdmsgbuf_cmplt(dhd_pub_t *dhd, uint32 id, uint32 len, void* buf, void* retbuf);

static int dhd_msgbuf_rxbuf_post(dhd_pub_t *dhd);
static int dhd_prot_rxbufpost(dhd_pub_t *dhd, uint16 count);
static void dhd_prot_return_rxbuf(dhd_pub_t *dhd, uint16 rxcnt);
static void dhd_prot_rxcmplt_process(dhd_pub_t *dhd, void* buf, uint16 msglen);
static void dhd_prot_event_process(dhd_pub_t *dhd, void* buf, uint16 len);
static int dhd_prot_process_msgtype(dhd_pub_t *dhd, msgbuf_ring_t *ring, uint8* buf, uint16 len);
static int dhd_process_msgtype(dhd_pub_t *dhd, msgbuf_ring_t *ring, uint8* buf, uint16 len);

static void dhd_prot_noop(dhd_pub_t *dhd, void * buf, uint16 msglen);
static void dhd_prot_txstatus_process(dhd_pub_t *dhd, void * buf, uint16 msglen);
static void dhd_prot_ioctcmplt_process(dhd_pub_t *dhd, void * buf, uint16 msglen);
static void dhd_prot_ioctack_process(dhd_pub_t *dhd, void * buf, uint16 msglen);
static void dhd_prot_ringstatus_process(dhd_pub_t *dhd, void * buf, uint16 msglen);
static void dhd_prot_genstatus_process(dhd_pub_t *dhd, void * buf, uint16 msglen);
static void* dhd_alloc_ring_space(dhd_pub_t *dhd, msgbuf_ring_t *ring,
	uint16 msglen, uint16 *alloced);
static int dhd_fillup_ioct_reqst_ptrbased(dhd_pub_t *dhd, uint16 len, uint cmd, void* buf,
	int ifidx);
static INLINE void dhd_prot_packet_free(dhd_pub_t *dhd, uint32 pktid, uint8 buf_type);
static INLINE void *dhd_prot_packet_get(dhd_pub_t *dhd, uint32 pktid, uint8 buf_type);
static void dmaxfer_free_dmaaddr(dhd_pub_t *dhd, dhd_dmaxfer_t *dma);
static int dmaxfer_prepare_dmaaddr(dhd_pub_t *dhd, uint len, uint srcdelay,
	uint destdelay, dhd_dmaxfer_t *dma);
static void dhdmsgbuf_dmaxfer_compare(dhd_pub_t *dhd, void *buf, uint16 msglen);
static void dhd_prot_process_flow_ring_create_response(dhd_pub_t *dhd, void* buf, uint16 msglen);
static void dhd_prot_process_flow_ring_delete_response(dhd_pub_t *dhd, void* buf, uint16 msglen);
static void dhd_prot_process_flow_ring_flush_response(dhd_pub_t *dhd, void* buf, uint16 msglen);




#ifdef DHD_RX_CHAINING
#define PKT_CTF_CHAINABLE(dhd, ifidx, evh, prio, h_sa, h_da, h_prio) \
	(!ETHER_ISNULLDEST(((struct ether_header *)(evh))->ether_dhost) && \
	 !ETHER_ISMULTI(((struct ether_header *)(evh))->ether_dhost) && \
	 !eacmp((h_da), ((struct ether_header *)(evh))->ether_dhost) && \
	 !eacmp((h_sa), ((struct ether_header *)(evh))->ether_shost) && \
	 ((h_prio) == (prio)) && (dhd_ctf_hotbrc_check((dhd), (evh), (ifidx))) && \
	 ((((struct ether_header *)(evh))->ether_type == HTON16(ETHER_TYPE_IP)) || \
	 (((struct ether_header *)(evh))->ether_type == HTON16(ETHER_TYPE_IPV6))))

static INLINE void BCMFASTPATH dhd_rxchain_reset(rxchain_info_t *rxchain);
static void BCMFASTPATH dhd_rxchain_frame(dhd_pub_t *dhd, void *pkt, uint ifidx);
static void BCMFASTPATH dhd_rxchain_commit(dhd_pub_t *dhd);

#define DHD_PKT_CTF_MAX_CHAIN_LEN	64
#endif /* DHD_RX_CHAINING */

static uint16 dhd_msgbuf_rxbuf_post_ctrlpath(dhd_pub_t *dhd, bool event_buf, uint32 max_to_post);
static int dhd_msgbuf_rxbuf_post_ioctlresp_bufs(dhd_pub_t *pub);
static int dhd_msgbuf_rxbuf_post_event_bufs(dhd_pub_t *pub);

static void dhd_prot_ring_detach(dhd_pub_t *dhd, msgbuf_ring_t * ring);
static void dhd_ring_init(dhd_pub_t *dhd, msgbuf_ring_t *ring);
static msgbuf_ring_t* prot_ring_attach(dhd_prot_t * prot, char* name, uint16 max_item,
	uint16 len_item, uint16 ringid);
static void* prot_get_ring_space(msgbuf_ring_t *ring, uint16 nitems, uint16 * alloced);
static void dhd_set_dmaed_index(dhd_pub_t *dhd, uint8 type, uint16 ringid, uint16 new_index);
static uint16 dhd_get_dmaed_index(dhd_pub_t *dhd, uint8 type, uint16 ringid);
static void prot_ring_write_complete(dhd_pub_t *dhd, msgbuf_ring_t * ring, void* p, uint16 len);
static void prot_upd_read_idx(dhd_pub_t *dhd, msgbuf_ring_t * ring);
static uint8* prot_get_src_addr(dhd_pub_t *dhd, msgbuf_ring_t *ring, uint16 *available_len);
static void prot_store_rxcpln_read_idx(dhd_pub_t *dhd, msgbuf_ring_t *ring);
static void prot_early_upd_rxcpln_read_idx(dhd_pub_t *dhd, msgbuf_ring_t * ring);

typedef void (*dhd_msgbuf_func_t)(dhd_pub_t *dhd, void * buf, uint16 msglen);
static dhd_msgbuf_func_t table_lookup[DHD_PROT_FUNCS] = {
	dhd_prot_noop,              /* 0 is invalid message type */
	dhd_prot_genstatus_process, /* MSG_TYPE_GEN_STATUS */
	dhd_prot_ringstatus_process, /* MSG_TYPE_RING_STATUS */
	NULL,
	dhd_prot_process_flow_ring_create_response, /* MSG_TYPE_FLOW_RING_CREATE_CMPLT */
	NULL,
	dhd_prot_process_flow_ring_delete_response, /* MSG_TYPE_FLOW_RING_DELETE_CMPLT */
	NULL,
	dhd_prot_process_flow_ring_flush_response, /* MSG_TYPE_FLOW_RING_FLUSH_CMPLT */
	NULL,
	dhd_prot_ioctack_process, /* MSG_TYPE_IOCTLPTR_REQ_ACK */
	NULL,
	dhd_prot_ioctcmplt_process, /* MSG_TYPE_IOCTL_CMPLT */
	NULL,
	dhd_prot_event_process, /* MSG_TYPE_WL_EVENT */
	NULL,
	dhd_prot_txstatus_process, /* MSG_TYPE_TX_STATUS */
	NULL,
	dhd_prot_rxcmplt_process, /* MSG_TYPE_RX_CMPLT */
	NULL,
	dhdmsgbuf_dmaxfer_compare, /* MSG_TYPE_LPBK_DMAXFER_CMPLT */
	NULL,
};


#if defined(PCIE_D2H_SYNC)

/*
 * D2H DMA to completion callback handlers. Based on the mode advertised by the
 * dongle through the PCIE shared region, the appropriate callback will be
 * registered in the proto layer to be invoked prior to precessing any message
 * from a D2H DMA ring. If the dongle uses a read barrier or another mode that
 * does not require host participation, then a noop callback handler will be
 * bound that simply returns the msgtype.
 */
static void dhd_prot_d2h_sync_livelock(dhd_pub_t *dhd, uint32 seqnum,
                                       uint32 tries, uchar *msg, int msglen);
static uint8 dhd_prot_d2h_sync_seqnum(dhd_pub_t *dhd, msgbuf_ring_t *ring,
                                      volatile cmn_msg_hdr_t *msg, int msglen);
static uint8 dhd_prot_d2h_sync_xorcsum(dhd_pub_t *dhd, msgbuf_ring_t *ring,
                                       volatile cmn_msg_hdr_t *msg, int msglen);
static uint8 dhd_prot_d2h_sync_none(dhd_pub_t *dhd, msgbuf_ring_t *ring,
                                    volatile cmn_msg_hdr_t *msg, int msglen);
static void dhd_prot_d2h_sync_init(dhd_pub_t *dhd, dhd_prot_t * prot);

/* Debug print a livelock avert by dropping a D2H message */
static void
dhd_prot_d2h_sync_livelock(dhd_pub_t *dhd, uint32 seqnum, uint32 tries,
                           uchar *msg, int msglen)
{
	DHD_ERROR(("LIVELOCK DHD<%p> seqnum<%u:%u> tries<%u> max<%lu> tot<%lu>\n",
		dhd, seqnum, seqnum% D2H_EPOCH_MODULO, tries,
		dhd->prot->d2h_sync_wait_max, dhd->prot->d2h_sync_wait_tot));
	prhex("D2H MsgBuf Failure", (uchar *)msg, msglen);
	dhd_dump_to_kernelog(dhd);
#ifdef SUPPORT_LINKDOWN_RECOVERY
#ifdef CONFIG_ARCH_MSM
	dhd->bus->no_cfg_restore = TRUE;
#endif /* CONFIG_ARCH_MSM */
	dhd->hang_reason = HANG_REASON_MSGBUF_LIVELOCK;
	dhd_os_send_hang_message(dhd);
#endif /* SUPPORT_LINKDOWN_RECOVERY */
}

/* Sync on a D2H DMA to complete using SEQNUM mode */
static uint8 BCMFASTPATH
dhd_prot_d2h_sync_seqnum(dhd_pub_t *dhd, msgbuf_ring_t *ring,
                         volatile cmn_msg_hdr_t *msg, int msglen)
{
	uint32 tries;
	uint32 ring_seqnum = ring->seqnum % D2H_EPOCH_MODULO;
	int num_words = msglen / sizeof(uint32); /* num of 32bit words */
	volatile uint32 *marker = (uint32 *)msg + (num_words - 1); /* last word */
	dhd_prot_t *prot = dhd->prot;
	uint32 step = 0;
	uint32 delay = PCIE_D2H_SYNC_DELAY;
	uint32 total_tries = 0;

	ASSERT(msglen == RING_LEN_ITEMS(ring));

	BCM_REFERENCE(delay);
	/*
	 * For retries we have to make some sort of stepper algorithm.
	 * We see that every time when the Dongle comes out of the D3
	 * Cold state, the first D2H mem2mem DMA takes more time to
	 * complete, leading to livelock issues.
	 *
	 * Case 1 - Apart from Host CPU some other bus master is
	 * accessing the DDR port, probably page close to the ring
	 * so, PCIE does not get a change to update the memory.
	 * Solution - Increase the number of tries.
	 *
	 * Case 2 - The 50usec delay given by the Host CPU is not
	 * sufficient for the PCIe RC to start its work.
	 * In this case the breathing time of 50usec given by
	 * the Host CPU is not sufficient.
	 * Solution: Increase the delay in a stepper fashion.
	 * This is done to ensure that there are no
	 * unwanted extra delay introdcued in normal conditions.
	 */
	for (step = 1; step <= PCIE_D2H_SYNC_NUM_OF_STEPS; step++) {
		for (tries = 0; tries < PCIE_D2H_SYNC_WAIT_TRIES; tries++) {
			uint32 msg_seqnum = *marker;
			if (ltoh32(msg_seqnum) == ring_seqnum) { /* dma upto last word done */
				ring->seqnum++; /* next expected sequence number */
				goto dma_completed;
			}

			total_tries = ((step-1) * PCIE_D2H_SYNC_WAIT_TRIES) + tries;

			if (total_tries > prot->d2h_sync_wait_max) {
				prot->d2h_sync_wait_max = total_tries;
			}

			OSL_CACHE_INV(msg, msglen); /* invalidate and try again */
			OSL_CPU_RELAX(); /* CPU relax for msg_seqnum  value to update */
#ifdef CONFIG_ARCH_MSM8996
			/* For ARM there is no pause in cpu_relax, so add extra delay */
			OSL_DELAY(delay * step);
#endif /* CONFIG_ARCH_MSM8996 */
		} /* for PCIE_D2H_SYNC_WAIT_TRIES */
	} /* for number of steps */

	dhd_prot_d2h_sync_livelock(dhd, ring->seqnum, total_tries, (uchar *)msg, msglen);

	ring->seqnum++; /* skip this message ... leak of a pktid */
	return 0; /* invalid msgtype 0 -> noop callback */

dma_completed:

	prot->d2h_sync_wait_tot += total_tries;
	return msg->msg_type;
}

/* Sync on a D2H DMA to complete using XORCSUM mode */
static uint8 BCMFASTPATH
dhd_prot_d2h_sync_xorcsum(dhd_pub_t *dhd, msgbuf_ring_t *ring,
                          volatile cmn_msg_hdr_t *msg, int msglen)
{
	uint32 tries;
	uint32 prot_checksum = 0; /* computed checksum */
	int num_words = msglen / sizeof(uint32); /* num of 32bit words */
	uint8 ring_seqnum = ring->seqnum % D2H_EPOCH_MODULO;
	dhd_prot_t *prot = dhd->prot;
	uint32 step = 0;
	uint32 delay = PCIE_D2H_SYNC_DELAY;
	uint32 total_tries = 0;

	ASSERT(msglen == RING_LEN_ITEMS(ring));

	BCM_REFERENCE(delay);

	/*
	 * For retries we have to make some sort of stepper algorithm.
	 * We see that every time when the Dongle comes out of the D3
	 * Cold state, the first D2H mem2mem DMA takes more time to
	 * complete, leading to livelock issues.
	 *
	 * Case 1 - Apart from Host CPU some other bus master is
	 * accessing the DDR port, probably page close to the ring
	 * so, PCIE does not get a change to update the memory.
	 * Solution - Increase the number of tries.
	 *
	 * Case 2 - The 50usec delay given by the Host CPU is not
	 * sufficient for the PCIe RC to start its work.
	 * In this case the breathing time of 50usec given by
	 * the Host CPU is not sufficient.
	 * Solution: Increase the delay in a stepper fashion.
	 * This is done to ensure that there are no
	 * unwanted extra delay introdcued in normal conditions.
	 */
	for (step = 1; step <= PCIE_D2H_SYNC_NUM_OF_STEPS; step++) {
		for (tries = 0; tries < PCIE_D2H_SYNC_WAIT_TRIES; tries++) {
			prot_checksum = bcm_compute_xor32((volatile uint32 *)msg, num_words);
			if (prot_checksum == 0U) { /* checksum is OK */
				if (msg->epoch == ring_seqnum) {
					ring->seqnum++; /* next expected sequence number */
					goto dma_completed;
				}
			}

			total_tries = ((step-1) * PCIE_D2H_SYNC_WAIT_TRIES) + tries;

			if (total_tries > prot->d2h_sync_wait_max) {
				prot->d2h_sync_wait_max = total_tries;
			}

			OSL_CACHE_INV(msg, msglen); /* invalidate and try again */
			OSL_CPU_RELAX(); /* CPU relax for msg_seqnum  value to update */
#ifdef CONFIG_ARCH_MSM8996
			/* For ARM there is no pause in cpu_relax, so add extra delay */
			OSL_DELAY(delay * step);
#endif /* CONFIG_ARCH_MSM8996 */

		} /* for PCIE_D2H_SYNC_WAIT_TRIES */
	} /* for number of steps */

	dhd_prot_d2h_sync_livelock(dhd, ring->seqnum, total_tries, (uchar *)msg, msglen);

	ring->seqnum++; /* skip this message ... leak of a pktid */
	return 0; /* invalid msgtype 0 -> noop callback */

dma_completed:

	prot->d2h_sync_wait_tot += total_tries;
	return msg->msg_type;
}

/* Do not sync on a D2H DMA */
static uint8 BCMFASTPATH
dhd_prot_d2h_sync_none(dhd_pub_t *dhd, msgbuf_ring_t *ring,
                       volatile cmn_msg_hdr_t *msg, int msglen)
{
	return msg->msg_type;
}

/* Initialize the D2H DMA Sync mode, per D2H ring seqnum and dhd stats */
static void
dhd_prot_d2h_sync_init(dhd_pub_t *dhd, dhd_prot_t * prot)
{
	prot->d2h_sync_wait_max = 0UL;
	prot->d2h_sync_wait_tot = 0UL;

	prot->d2hring_tx_cpln->seqnum = D2H_EPOCH_INIT_VAL;
	prot->d2hring_rx_cpln->seqnum = D2H_EPOCH_INIT_VAL;
	prot->d2hring_ctrl_cpln->seqnum = D2H_EPOCH_INIT_VAL;

	if (dhd->d2h_sync_mode & PCIE_SHARED_D2H_SYNC_SEQNUM)
		prot->d2h_sync_cb = dhd_prot_d2h_sync_seqnum;
	else if (dhd->d2h_sync_mode & PCIE_SHARED_D2H_SYNC_XORCSUM)
		prot->d2h_sync_cb = dhd_prot_d2h_sync_xorcsum;
	else
		prot->d2h_sync_cb = dhd_prot_d2h_sync_none;
}

#endif /* PCIE_D2H_SYNC */

/*
 * +---------------------------------------------------------------------------+
 * PktId Map: Provides a native packet pointer to unique 32bit PktId mapping.
 * The packet id map, also includes storage for some packet parameters that
 * may be saved. A native packet pointer along with the parameters may be saved
 * and a unique 32bit pkt id will be returned. Later, the saved packet pointer
 * and the metadata may be retrieved using the previously allocated packet id.
 * +---------------------------------------------------------------------------+
 */

/*
 * PktId (Locker) #0 is never allocated and is considered invalid.
 *
 * On request for a pktid, a value DHD_PKTID_INVALID must be treated as a
 * depleted pktid pool and must not be used by the caller.
 *
 * Likewise, a caller must never free a pktid of value DHD_PKTID_INVALID.
 */
#define DHD_PKTID_INVALID	(0U)
#define DHD_IOCTL_REQ_PKTID	0xFFFE


#define MAX_PKTID_ITEMS		(8192) /* Maximum number of pktids supported */

/*
 * DHD_PKTID_AUDIT_ENABLED: Audit of PktIds in DHD for duplicate alloc and frees
 *
 * DHD_PKTID_AUDIT_MAP: Audit the LIFO or FIFO PktIdMap allocator
 * DHD_PKTID_AUDIT_RING: Audit the pktid during producer/consumer ring operation
 *
 * CAUTION: When DHD_PKTID_AUDIT_ENABLED is defined,
 *    either DHD_PKTID_AUDIT_MAP or DHD_PKTID_AUDIT_RING may be selected.
 */

/* Disable Host side Pktid Audit, enabling this makes the mem corruption
 * problem disappear
 */
/* #define DHD_PKTID_AUDIT_ENABLED */

#if defined(DHD_PKTID_AUDIT_ENABLED)

/* Audit the pktidmap allocator */
/* #define DHD_PKTID_AUDIT_MAP */

/* Audit the pktid during production/consumption of workitems */
#define DHD_PKTID_AUDIT_RING

#if defined(DHD_PKTID_AUDIT_MAP) && defined(DHD_PKTID_AUDIT_RING)
#error "May only enabled audit of MAP or RING, at a time."
#endif /* DHD_PKTID_AUDIT_MAP && DHD_PKTID_AUDIT_RING */

#define DHD_DUPLICATE_ALLOC	1
#define DHD_DUPLICATE_FREE	2
#define DHD_TEST_IS_ALLOC	3
#define DHD_TEST_IS_FREE	4

#define USE_DHD_PKTID_AUDIT_LOCK 1

#ifdef USE_DHD_PKTID_AUDIT_LOCK
#define DHD_PKTID_AUDIT_LOCK_INIT(osh)		dhd_os_spin_lock_init(osh)
#define DHD_PKTID_AUDIT_LOCK_DEINIT(osh, lock)	dhd_os_spin_lock_deinit(osh, lock)
#define DHD_PKTID_AUDIT_LOCK(lock)		dhd_os_spin_lock(lock)
#define DHD_PKTID_AUDIT_UNLOCK(lock, flags)	dhd_os_spin_unlock(lock, flags)

#else

#define DHD_PKTID_AUDIT_LOCK_INIT(osh)		(void *)(1)
#define DHD_PKTID_AUDIT_LOCK_DEINIT(osh, lock)	do { /* noop */ } while (0)
#define DHD_PKTID_AUDIT_LOCK(lock)		0
#define DHD_PKTID_AUDIT_UNLOCK(lock, flags)	do { /* noop */ } while (0)

#endif /* !USE_DHD_PKTID_AUDIT_LOCK */

#endif /* DHD_PKTID_AUDIT_ENABLED */

#if defined(DHD_PKTID_AUDIT_ENABLED)
/*
 * Going back to 1 from 2, we are any way using LIFO method now.
 * Also note that if this is 2 then our times would 2 * 8192 + 1
 * But the bit map is only for 16K. Increasing the bitmap size for
 * more than 16K failed. So doing this change to fit in into the
 * bit map size.
 */
#define DHD_PKTIDMAP_FIFO	1

#else

/*
 * Uses a FIFO dll with Nx more pktids instead of a LIFO stack.
 * If you wish to enable pktidaudit in firmware with FIFO PktId allocator, then
 * the total number of PktIds managed by the pktidaudit must be multiplied by
 * this DHD_PKTIDMAP_FIFO factor.
 */
#define DHD_PKTIDMAP_FIFO  4

#endif /* DHD_PKTID_AUDIT_ENABLED */

typedef void * dhd_pktid_map_handle_t; /* opaque handle to a pktid map */

/* Construct a packet id mapping table, returing an opaque map handle */
static dhd_pktid_map_handle_t *dhd_pktid_map_init(void *osh, uint32 num_items);

/* Destroy a packet id mapping table, freeing all packets active in the table */
static void dhd_pktid_map_fini(dhd_pktid_map_handle_t *map);

/* Determine number of pktids that are available */
static INLINE uint32 dhd_pktid_map_avail_cnt(dhd_pktid_map_handle_t *handle);

/* Allocate a unique pktid against which a pkt and some metadata is saved */
static INLINE uint32 dhd_pktid_map_reserve(dhd_pktid_map_handle_t *handle,
                                           void *pkt, uint8 buf_type);
static INLINE void dhd_pktid_map_save(dhd_pktid_map_handle_t *handle, void *pkt,
                       uint32 nkey, dmaaddr_t physaddr, uint32 len, uint8 dma,
                       uint8 buf_type);
static uint32 dhd_pktid_map_alloc(dhd_pktid_map_handle_t *map, void *pkt,
                                  dmaaddr_t physaddr, uint32 len, uint8 dma,
                                  uint8 buf_type);

/* Return an allocated pktid, retrieving previously saved pkt and metadata */
static void *dhd_pktid_map_free(dhd_pktid_map_handle_t *map, uint32 id,
                                dmaaddr_t *physaddr, uint32 *len,
                                uint8 buf_type);

#ifdef USE_DHD_PKTID_LOCK
#define DHD_PKTID_LOCK_INIT(osh)		dhd_os_spin_lock_init(osh)
#define DHD_PKTID_LOCK_DEINIT(osh, lock)	dhd_os_spin_lock_deinit(osh, lock)
#define DHD_PKTID_LOCK(lock)			dhd_os_spin_lock(lock)
#define DHD_PKTID_UNLOCK(lock, flags)		dhd_os_spin_unlock(lock, flags)
#else
#define DHD_PKTID_LOCK_INIT(osh)		(void *)(1)
#define DHD_PKTID_LOCK_DEINIT(osh, lock)	do { \
							BCM_REFERENCE(osh); \
							BCM_REFERENCE(lock); \
						} while (0)
#define DHD_PKTID_LOCK(lock)			0
#define DHD_PKTID_UNLOCK(lock, flags)		do { \
							BCM_REFERENCE(lock); \
							BCM_REFERENCE(flags); \
						} while (0)
#endif /* !USE_DHD_PKTID_LOCK */

/* Packet metadata saved in packet id mapper */

typedef enum pkt_buf_type {
	BUFF_TYPE_DATA_TX = 0,
	BUFF_TYPE_DATA_RX,
	BUFF_TYPE_IOCTL_RX,
	BUFF_TYPE_EVENT_RX,
	 BUFF_TYPE_NO_CHECK
} pkt_buf_type_t;

typedef struct dhd_pktid_item {
#if defined(DHD_PKTIDMAP_FIFO)
	dll_t       list_node; /* MUST BE FIRST field */
	uint32      nkey;
#endif
	bool        inuse;    /* tag an item to be in use */
	uint8       dma;      /* map direction: flush or invalidate */
	uint8       buf_type;
			      /* This filed is used to colour the
			       * buffer pointers held in the locker.
			       */
	uint16      len;      /* length of mapped packet's buffer */
	void        *pkt;     /* opaque native pointer to a packet */
	dmaaddr_t   physaddr; /* physical address of mapped packet's buffer */
} dhd_pktid_item_t;

typedef struct dhd_pktid_map {
	void        *osh;
	uint32	    items;    /* total items in map */
	uint32      avail;    /* total available items */
	uint32      failures; /* lockers unavailable count */
	/* Spinlock to protect dhd_pktid_map in process/tasklet context */
	void        *pktid_lock; /* Used when USE_DHD_PKTID_LOCK is defined */

#if defined(DHD_PKTID_AUDIT_ENABLED)
	void	*pktid_audit_lock;
	struct bcm_mwbmap *pktid_audit; /* multi word bitmap based audit */
#endif /* DHD_PKTID_AUDIT_ENABLED */

	/* Unique PktId Allocator: FIFO dll, or LIFO:stack of keys */
#if defined(DHD_PKTIDMAP_FIFO)
	dll_t       list_free; /* allocate from head, free to tail */
	dll_t       list_inuse;
#else  /* ! DHD_PKTIDMAP_FIFO */
	uint32      keys[MAX_PKTID_ITEMS + 1]; /* stack of unique pkt ids */
#endif /* ! DHD_PKTIDMAP_FIFO */
	dhd_pktid_item_t lockers[0];           /* metadata storage */
} dhd_pktid_map_t;

#define DHD_PKTID_ITEM_SZ               (sizeof(dhd_pktid_item_t))

#if defined(DHD_PKTIDMAP_FIFO)
/* A 4x pool of pktids are managed with FIFO allocation. */
#define DHD_PKIDMAP_ITEMS(items)        (items * DHD_PKTIDMAP_FIFO)
#define DHD_PKTID_MAP_SZ(items)         (sizeof(dhd_pktid_map_t) + \
			(DHD_PKTID_ITEM_SZ * ((DHD_PKTIDMAP_FIFO * (items)) + 1)))
#else /* ! DHD_PKTIDMAP_FIFO */
#define DHD_PKIDMAP_ITEMS(items)        (items)
#define DHD_PKTID_MAP_SZ(items)         (sizeof(dhd_pktid_map_t) + \
	                                     (DHD_PKTID_ITEM_SZ * ((items) + 1)))
#endif /* ! DHD_PKTIDMAP_FIFO */

#define NATIVE_TO_PKTID_INIT(osh, items) dhd_pktid_map_init((osh), (items))
#define NATIVE_TO_PKTID_FINI(map)        dhd_pktid_map_fini(map)
#define NATIVE_TO_PKTID_CLEAR(map)       dhd_pktid_map_clear(map)

#define NATIVE_TO_PKTID_RSV(map, pkt, buf_type)    dhd_pktid_map_reserve((map), (pkt), (buf_type))
#define NATIVE_TO_PKTID_SAVE(map, pkt, nkey, pa, len, dma, buf_type) \
	dhd_pktid_map_save((map), (void *)(pkt), (nkey), (pa), (uint32)(len), \
	(uint8)dma, (uint8)buf_type)

#define NATIVE_TO_PKTID(map, pkt, pa, len, dma, buf_type) \
	dhd_pktid_map_alloc((map), (void *)(pkt), (pa), (uint32)(len), \
	(uint8)dma, (uint8)buf_type)

#define PKTID_TO_NATIVE(map, pktid, pa, len, buf_type) \
	dhd_pktid_map_free((map), (uint32)(pktid), \
	                   (dmaaddr_t *)&(pa), (uint32 *)&(len), (uint8)buf_type)

#define PKTID_AVAIL(map)                 dhd_pktid_map_avail_cnt(map)

#if defined(DHD_PKTID_AUDIT_ENABLED)

static int dhd_pktid_audit(dhd_pktid_map_t *pktid_map, uint32 pktid,
	const int test_for, const char *errmsg);

/* Call back into OS layer to take the dongle dump and panic */
#ifdef DHD_DEBUG_PAGEALLOC
extern void dhd_pktaudit_fail_cb(void);
#endif /* DHD_DEBUG_PAGEALLOC */

static int
dhd_pktid_audit(dhd_pktid_map_t *pktid_map, uint32 pktid, const int test_for,
	const char *errmsg)
{
#define DHD_PKT_AUDIT_STR "ERROR: %16s Host PktId Audit: "

#if defined(DHD_PKTIDMAP_FIFO)
	const uint32 max_pktid_items = (MAX_PKTID_ITEMS * DHD_PKTIDMAP_FIFO);
#else
	const uint32 max_pktid_items = (MAX_PKTID_ITEMS);
#endif /* DHD_DEBUG_PAGEALLOC */
	struct bcm_mwbmap *handle;
	u32	flags;

	if (pktid_map == (dhd_pktid_map_t *)NULL) {
		DHD_ERROR((DHD_PKT_AUDIT_STR "Pkt id map NULL\n", errmsg));
		return BCME_OK;
	}

	flags = DHD_PKTID_AUDIT_LOCK(pktid_map->pktid_audit_lock);

	handle = pktid_map->pktid_audit;
	if (handle == (struct bcm_mwbmap *)NULL) {
		DHD_ERROR((DHD_PKT_AUDIT_STR "Handle NULL\n", errmsg));
		DHD_PKTID_AUDIT_UNLOCK(pktid_map->pktid_audit_lock, flags);
		return BCME_OK;
	}

	if ((pktid == DHD_PKTID_INVALID) || (pktid > max_pktid_items)) {
		DHD_ERROR((DHD_PKT_AUDIT_STR "PktId<%d> invalid\n", errmsg, pktid));
		/* lock is released in "error" */
		goto error;
	}

	if (pktid == DHD_IOCTL_REQ_PKTID) {
		DHD_PKTID_AUDIT_UNLOCK(pktid_map->pktid_audit_lock, flags);
		return BCME_OK;
	}

	switch (test_for) {
		case DHD_DUPLICATE_ALLOC:
			if (!bcm_mwbmap_isfree(handle, pktid)) {
				DHD_ERROR((DHD_PKT_AUDIT_STR "PktId<%d> alloc duplicate\n",
					errmsg, pktid));
				goto error;
			}
			bcm_mwbmap_force(handle, pktid);
			break;

		case DHD_DUPLICATE_FREE:
			if (bcm_mwbmap_isfree(handle, pktid)) {
				DHD_ERROR((DHD_PKT_AUDIT_STR "PktId<%d> free duplicate\n",
					errmsg, pktid));
				goto error;
			}
			bcm_mwbmap_free(handle, pktid);
			break;

		case DHD_TEST_IS_ALLOC:
			if (bcm_mwbmap_isfree(handle, pktid)) {
				DHD_ERROR((DHD_PKT_AUDIT_STR "PktId<%d> is not allocated\n",
					errmsg, pktid));
				goto error;
			}
			break;

		case DHD_TEST_IS_FREE:
			if (!bcm_mwbmap_isfree(handle, pktid)) {
				DHD_ERROR((DHD_PKT_AUDIT_STR "PktId<%d> is not free",
					errmsg, pktid));
				goto error;
			}
			break;

		default:
			goto error;
	}

	DHD_PKTID_AUDIT_UNLOCK(pktid_map->pktid_audit_lock, flags);
	return BCME_OK;

error:

	DHD_PKTID_AUDIT_UNLOCK(pktid_map->pktid_audit_lock, flags);
	/* May insert any trap mechanism here ! */
	ASSERT(0);
	return BCME_ERROR;
}

#define DHD_PKTID_AUDIT(map, pktid, test_for) \
	dhd_pktid_audit((dhd_pktid_map_t *)(map), (pktid), (test_for), __FUNCTION__)

#endif /* DHD_PKTID_AUDIT_ENABLED */

/*
 * +---------------------------------------------------------------------------+
 * Packet to Packet Id mapper using a <numbered_key, locker> paradigm.
 *
 * dhd_pktid_map manages a set of unique Packet Ids range[1..MAX_PKTID_ITEMS].
 *
 * dhd_pktid_map_alloc() may be used to save some packet metadata, and a unique
 * packet id is returned. This unique packet id may be used to retrieve the
 * previously saved packet metadata, using dhd_pktid_map_free(). On invocation
 * of dhd_pktid_map_free(), the unique packet id is essentially freed. A
 * subsequent call to dhd_pktid_map_alloc() may reuse this packet id.
 *
 * Implementation Note:
 * Convert this into a <key,locker> abstraction and place into bcmutils !
 * Locker abstraction should treat contents as opaque storage, and a
 * callback should be registered to handle inuse lockers on destructor.
 *
 * +---------------------------------------------------------------------------+
 */

/* Allocate and initialize a mapper of num_items <numbered_key, locker> */
static dhd_pktid_map_handle_t *
dhd_pktid_map_init(void *osh, uint32 num_items)
{
	uint32 nkey;
	dhd_pktid_map_t *map;
	uint32 dhd_pktid_map_sz;
	uint32 map_items;

	ASSERT((num_items >= 1) && (num_items <= MAX_PKTID_ITEMS));
	dhd_pktid_map_sz = DHD_PKTID_MAP_SZ(num_items);

	if ((map = (dhd_pktid_map_t *)MALLOC(osh, dhd_pktid_map_sz)) == NULL) {
		DHD_ERROR(("%s:%d: MALLOC failed for size %d\n",
		           __FUNCTION__, __LINE__, dhd_pktid_map_sz));
		goto error;
	}
	bzero(map, dhd_pktid_map_sz);

	/* Initialize the lock that protects this structure */
	map->pktid_lock = DHD_PKTID_LOCK_INIT(osh);
	if (map->pktid_lock == NULL) {
		DHD_ERROR(("%s:%d: Lock init failed \r\n", __FUNCTION__, __LINE__));
		goto error;
	}

	map->osh = osh;
	map->items = num_items;
	map->avail = num_items;

	map_items = DHD_PKIDMAP_ITEMS(map->items);

#if defined(DHD_PKTID_AUDIT_ENABLED)
	/* Incarnate a hierarchical multiword bitmap for auditing pktid allocator */
	map->pktid_audit = bcm_mwbmap_init(osh, map_items + 1);
	if (map->pktid_audit == (struct bcm_mwbmap *)NULL) {
		DHD_ERROR(("%s:%d: pktid_audit init failed\r\n", __FUNCTION__, __LINE__));
		goto error;
	}

	map->pktid_audit_lock = DHD_PKTID_AUDIT_LOCK_INIT(osh);
#endif /* DHD_PKTID_AUDIT_ENABLED */

#if defined(DHD_PKTIDMAP_FIFO)

	/* Initialize all dll */
	dll_init(&map->list_free);
	dll_init(&map->list_inuse);

	/* Initialize and place all 4 x items in map's list_free */
	for (nkey = 0; nkey <= map_items; nkey++) {
		dll_init(&map->lockers[nkey].list_node);
		map->lockers[nkey].inuse = FALSE;
		map->lockers[nkey].nkey  = nkey;
		map->lockers[nkey].pkt   = NULL; /* bzero: redundant */
		map->lockers[nkey].len   = 0;
		/* Free at tail */
		dll_append(&map->list_free, &map->lockers[nkey].list_node);
	}

	/* Reserve pktid #0, i.e. DHD_PKTID_INVALID to be inuse */
	map->lockers[DHD_PKTID_INVALID].inuse = TRUE;
	dll_delete(&map->lockers[DHD_PKTID_INVALID].list_node);
	dll_append(&map->list_inuse, &map->lockers[DHD_PKTID_INVALID].list_node);

#else /* ! DHD_PKTIDMAP_FIFO */

	map->lockers[DHD_PKTID_INVALID].inuse = TRUE; /* tag locker #0 as inuse */

	for (nkey = 1; nkey <= map_items; nkey++) { /* locker #0 is reserved */
		map->keys[nkey] = nkey; /* populate with unique keys */
		map->lockers[nkey].inuse = FALSE;
		map->lockers[nkey].pkt   = NULL; /* bzero: redundant */
		map->lockers[nkey].len   = 0;
	}

#endif /* ! DHD_PKTIDMAP_FIFO */

	/* Reserve pktid #0, i.e. DHD_PKTID_INVALID to be inuse */
	map->lockers[DHD_PKTID_INVALID].inuse = TRUE; /* tag locker #0 as inuse */
	map->lockers[DHD_PKTID_INVALID].pkt   = NULL; /* bzero: redundant */
	map->lockers[DHD_PKTID_INVALID].len   = 0;

#if defined(DHD_PKTID_AUDIT_ENABLED)
	/* do not use dhd_pktid_audit() here, use bcm_mwbmap_force directly */
	bcm_mwbmap_force(map->pktid_audit, DHD_PKTID_INVALID);
#endif /* DHD_PKTID_AUDIT_ENABLED */

	return (dhd_pktid_map_handle_t *)map; /* opaque handle */

error:

	if (map) {
#if defined(DHD_PKTID_AUDIT_ENABLED)
		if (map->pktid_audit != (struct bcm_mwbmap *)NULL) {
			bcm_mwbmap_fini(osh, map->pktid_audit); /* Destruct pktid_audit */
			map->pktid_audit = (struct bcm_mwbmap *)NULL;
			if (map->pktid_audit_lock) {
				DHD_PKTID_AUDIT_LOCK_DEINIT(osh, map->pktid_audit_lock);
			}
		}
#endif /* DHD_PKTID_AUDIT_ENABLED */

		if (map->pktid_lock) {
			DHD_PKTID_LOCK_DEINIT(osh, map->pktid_lock);
		}

		MFREE(osh, map, dhd_pktid_map_sz);
	}

	return (dhd_pktid_map_handle_t *)NULL;
}

/*
 * Retrieve all allocated keys and free all <numbered_key, locker>.
 * Freeing implies: unmapping the buffers and freeing the native packet
 * This could have been a callback registered with the pktid mapper.
 */
static void
dhd_pktid_map_fini(dhd_pktid_map_handle_t *handle)
{
	void *osh;
	int nkey;
	dhd_pktid_map_t *map;
	uint32 dhd_pktid_map_sz;
	dhd_pktid_item_t *locker;
	uint32 map_items;
	unsigned long flags;

	if (handle == NULL)
		return;

	map = (dhd_pktid_map_t *)handle;
	flags = DHD_PKTID_LOCK(map->pktid_lock);

	osh = map->osh;
	dhd_pktid_map_sz = DHD_PKTID_MAP_SZ(map->items);

	nkey = 1; /* skip reserved KEY #0, and start from 1 */
	locker = &map->lockers[nkey];

	map_items = DHD_PKIDMAP_ITEMS(map->items);

	for (; nkey <= map_items; nkey++, locker++) {
		if (locker->inuse == TRUE) { /* numbered key still in use */
			locker->inuse = FALSE; /* force open the locker */

#if defined(DHD_PKTID_AUDIT_ENABLED)
			DHD_PKTID_AUDIT(map, nkey, DHD_DUPLICATE_FREE); /* duplicate frees */
#endif /* DHD_PKTID_AUDIT_ENABLED */
			{
				if (!PHYSADDRISZERO(locker->physaddr)) {
					/* This could be a callback registered with dhd_pktid_map */
					DMA_UNMAP(osh, locker->physaddr, locker->len,
						locker->dma, 0, 0);
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_CTRLBUF)
					if (locker->buf_type == BUFF_TYPE_IOCTL_RX ||
						locker->buf_type == BUFF_TYPE_EVENT_RX) {
						PKTFREE_STATIC(osh, (ulong*)locker->pkt, FALSE);
					} else {
#endif /* CONFIG_DHD_USE_STATIC_BUF && DHD_USE_STATIC_CTRLBUF */
						PKTFREE(osh, (ulong*)locker->pkt, FALSE);
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_CTRLBUF)
					}
#endif /* CONFIG_DHD_USE_STATIC_BUF && DHD_USE_STATIC_CTRLBUF */
				} else {
					DHD_ERROR(("%s: Invalid phyaddr 0\n", __FUNCTION__));
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_CTRLBUF)
					if (locker->buf_type == BUFF_TYPE_IOCTL_RX ||
						locker->buf_type == BUFF_TYPE_EVENT_RX) {
						PKTINVALIDATE_STATIC(osh, (ulong*)locker->pkt);
					} else {
#endif /* CONFIG_DHD_USE_STATIC_BUF && DHD_USE_STATIC_CTRLBUF */
						PKTFREE(osh, (ulong*)locker->pkt, FALSE);
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_CTRLBUF)
					}
#endif /* CONFIG_DHD_USE_STATIC_BUF && DHD_USE_STATIC_CTRLBUF */
				}
			}
		}
#if defined(DHD_PKTID_AUDIT_ENABLED)
		else {
			DHD_PKTID_AUDIT(map, nkey, DHD_TEST_IS_FREE);
		}
#endif /* DHD_PKTID_AUDIT_ENABLED */

		locker->pkt = NULL; /* clear saved pkt */
		locker->len = 0;
	}

#if defined(DHD_PKTID_AUDIT_ENABLED)
	if (map->pktid_audit != (struct bcm_mwbmap *)NULL) {
		bcm_mwbmap_fini(osh, map->pktid_audit); /* Destruct pktid_audit */
		map->pktid_audit = (struct bcm_mwbmap *)NULL;
		if (map->pktid_audit_lock) {
			DHD_PKTID_AUDIT_LOCK_DEINIT(osh, map->pktid_audit_lock);
		}
	}
#endif /* DHD_PKTID_AUDIT_ENABLED */

	DHD_PKTID_UNLOCK(map->pktid_lock, flags);
	DHD_PKTID_LOCK_DEINIT(osh, map->pktid_lock);

	MFREE(osh, handle, dhd_pktid_map_sz);
}

static void
dhd_pktid_map_clear(dhd_pktid_map_handle_t *handle)
{
	void *osh;
	int nkey;
	dhd_pktid_map_t *map;
	dhd_pktid_item_t *locker;
	uint32 map_items;
	unsigned long flags;

	DHD_TRACE(("%s\n", __FUNCTION__));

	if (handle == NULL)
		return;

	map = (dhd_pktid_map_t *)handle;
	flags = DHD_PKTID_LOCK(map->pktid_lock);

	osh = map->osh;
	map->failures = 0;

	nkey = 1; /* skip reserved KEY #0, and start from 1 */
	locker = &map->lockers[nkey];

	map_items = DHD_PKIDMAP_ITEMS(map->items);

	for (; nkey <= map_items; nkey++, locker++) {

#if !defined(DHD_PKTIDMAP_FIFO)
		map->keys[nkey] = nkey; /* populate with unique keys */
#endif /* ! DHD_PKTIDMAP_FIFO */

		if (locker->inuse == TRUE) { /* numbered key still in use */

			locker->inuse = FALSE; /* force open the locker */
#if defined(DHD_PKTID_AUDIT_ENABLED)
			DHD_PKTID_AUDIT(map, nkey, DHD_DUPLICATE_FREE); /* duplicate frees */
#endif /* DHD_PKTID_AUDIT_ENABLED */

#if defined(DHD_PKTIDMAP_FIFO)
			ASSERT(locker->nkey == nkey);
			dll_delete(&locker->list_node);
			dll_append(&map->list_free, &locker->list_node);
#endif /* DHD_PKTIDMAP_FIFO */

			DHD_TRACE(("%s free id%d\n", __FUNCTION__, nkey));
			if (!PHYSADDRISZERO(locker->physaddr)) {
				DMA_UNMAP(osh, locker->physaddr, locker->len,
					locker->dma, 0, 0);
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_CTRLBUF)
				if (locker->buf_type == BUFF_TYPE_IOCTL_RX ||
					locker->buf_type == BUFF_TYPE_EVENT_RX) {
					PKTFREE_STATIC(osh, (ulong*)locker->pkt, FALSE);
				} else {
#endif /* CONFIG_DHD_USE_STATIC_BUF && DHD_USE_STATIC_CTRLBUF */
					PKTFREE(osh, (ulong*)locker->pkt, FALSE);
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_CTRLBUF)
				}
#endif /* CONFIG_DHD_USE_STATIC_BUF && DHD_USE_STATIC_CTRLBUF */
			} else {
				DHD_ERROR(("%s: Invalid phyaddr 0\n", __FUNCTION__));
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_CTRLBUF)
				if (locker->buf_type == BUFF_TYPE_IOCTL_RX ||
					locker->buf_type == BUFF_TYPE_EVENT_RX) {
					PKTINVALIDATE_STATIC(osh, (ulong*)locker->pkt);
				} else {
#endif /* CONFIG_DHD_USE_STATIC_BUF && DHD_USE_STATIC_CTRLBUF */
					PKTFREE(osh, (ulong*)locker->pkt, FALSE);
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_CTRLBUF)
				}
#endif /* CONFIG_DHD_USE_STATIC_BUF && DHD_USE_STATIC_CTRLBUF */
			}
		}
#if defined(DHD_PKTID_AUDIT_ENABLED)
		else {
			DHD_PKTID_AUDIT(map, nkey, DHD_TEST_IS_FREE);
		}
#endif /* DHD_PKTID_AUDIT_ENABLED */

		locker->pkt = NULL; /* clear saved pkt */
		locker->len = 0;

	}
	map->avail = map->items;

	DHD_PKTID_UNLOCK(map->pktid_lock, flags);
}

/* Get the pktid free count */
static INLINE uint32 BCMFASTPATH
dhd_pktid_map_avail_cnt(dhd_pktid_map_handle_t *handle)
{
	dhd_pktid_map_t *map;
	unsigned long flags;
	uint32 avail;

	ASSERT(handle != NULL);
	map = (dhd_pktid_map_t *)handle;

	flags = DHD_PKTID_LOCK(map->pktid_lock);
	avail = map->avail;
	DHD_PKTID_UNLOCK(map->pktid_lock, flags);

	return avail;
}

/*
 * Allocate locker, save pkt contents, and return the locker's numbered key.
 * dhd_pktid_map_alloc() is not reentrant, and is the caller's responsibility.
 * Caller must treat a returned value DHD_PKTID_INVALID as a failure case,
 * implying a depleted pool of pktids.
 */
static INLINE uint32
__dhd_pktid_map_reserve(dhd_pktid_map_handle_t *handle, void *pkt, uint8 buf_type)
{
	uint32 nkey;
	dhd_pktid_map_t *map;
	dhd_pktid_item_t *locker;

	ASSERT(handle != NULL);
	map = (dhd_pktid_map_t *)handle;

	if (map->avail <= 0) { /* no more pktids to allocate */
		map->failures++;
		DHD_INFO(("%s:%d: failed, no free keys\n", __FUNCTION__, __LINE__));
		return DHD_PKTID_INVALID; /* failed alloc request */
	}
	ASSERT(map->avail <= map->items);

#if defined(DHD_PKTIDMAP_FIFO)

	ASSERT(!dll_empty(&map->list_free));

	/* Move list_free head item to inuse list, fetch key in head node */
	locker = (dhd_pktid_item_t *)dll_head_p(&map->list_free);
	dll_delete(&locker->list_node);
	nkey = locker->nkey;
	dll_append(&map->list_inuse, &locker->list_node);

#else /* ! DHD_PKTIDMAP_FIFO */

	nkey = map->keys[map->avail]; /* fetch a free locker, pop stack */
	locker = &map->lockers[nkey]; /* save packet metadata in locker */

#endif /* ! DHD_PKTIDMAP_FIFO */

	if ((map->avail > map->items) ||
		(nkey > DHD_PKIDMAP_ITEMS(map->items))) {
		map->failures++;
		DHD_ERROR(("%s:%d: failed to allocate a new pktid,"
			" map->avail<%u>, nkey<%u>, buf_type<%u>\n",
			__FUNCTION__, __LINE__, map->avail, nkey,
			buf_type));
		return DHD_PKTID_INVALID; /* failed alloc request */
	}

	map->avail--;

	locker->inuse = TRUE; /* reserve this locker */
	locker->pkt = pkt; /* pkt is saved, other params not yet saved. */
	locker->len = 0;

#if defined(DHD_PKTID_AUDIT_MAP)
	DHD_PKTID_AUDIT(map, nkey, DHD_DUPLICATE_ALLOC); /* Audit duplicate alloc */
#endif /* DHD_PKTID_AUDIT_MAP */

	ASSERT(nkey != DHD_PKTID_INVALID);
	return nkey; /* return locker's numbered key */
}

/* Wrapper that takes the required lock when called directly */
static INLINE uint32
dhd_pktid_map_reserve(dhd_pktid_map_handle_t *handle, void *pkt, uint8 buf_type)
{
	dhd_pktid_map_t *map;
	unsigned long flags;
	uint32 ret;

	ASSERT(handle != NULL);
	map = (dhd_pktid_map_t *)handle;
	flags = DHD_PKTID_LOCK(map->pktid_lock);
	ret = __dhd_pktid_map_reserve(handle, pkt, buf_type);
	DHD_PKTID_UNLOCK(map->pktid_lock, flags);

	return ret;
}

static INLINE void
__dhd_pktid_map_save(dhd_pktid_map_handle_t *handle, void *pkt, uint32 nkey,
	dmaaddr_t physaddr, uint32 len, uint8 dma, uint8 buf_type)
{
	dhd_pktid_map_t *map;
	dhd_pktid_item_t *locker;

	ASSERT(handle != NULL);
	map = (dhd_pktid_map_t *)handle;

	ASSERT((nkey != DHD_PKTID_INVALID) && (nkey <= DHD_PKIDMAP_ITEMS(map->items)));

	if ((nkey == DHD_PKTID_INVALID) || (nkey > DHD_PKIDMAP_ITEMS(map->items))) {
		DHD_ERROR(("%s:%d: Error! saving invalid pktid<%u> buf_type<%u>\n",
			__FUNCTION__, __LINE__, nkey, buf_type));
		return;
	}

	locker = &map->lockers[nkey];
	ASSERT((locker->pkt == pkt) && (locker->inuse == TRUE));

#if defined(DHD_PKTID_AUDIT_MAP)
	DHD_PKTID_AUDIT(map, nkey, DHD_TEST_IS_ALLOC); /* apriori, reservation */
#endif /* DHD_PKTID_AUDIT_MAP */

	locker->dma = dma; /* store contents in locker */
	locker->buf_type = buf_type;
	locker->physaddr = physaddr;
	locker->len = (uint16)len; /* 16bit len */
}

/* Wrapper that takes the required lock when called directly */
static INLINE void
dhd_pktid_map_save(dhd_pktid_map_handle_t *handle, void *pkt, uint32 nkey,
	dmaaddr_t physaddr, uint32 len, uint8 dma, uint8 buf_type)
{
	dhd_pktid_map_t *map;
	unsigned long flags;

	ASSERT(handle != NULL);
	map = (dhd_pktid_map_t *)handle;
	flags = DHD_PKTID_LOCK(map->pktid_lock);
	__dhd_pktid_map_save(handle, pkt, nkey, physaddr, len, dma, buf_type);
	DHD_PKTID_UNLOCK(map->pktid_lock, flags);
}

static uint32 BCMFASTPATH
dhd_pktid_map_alloc(dhd_pktid_map_handle_t *handle, void *pkt,
	dmaaddr_t physaddr, uint32 len, uint8 dma, uint8 buf_type)
{
	uint32 nkey;
	unsigned long flags;
	dhd_pktid_map_t *map;

	ASSERT(handle != NULL);
	map = (dhd_pktid_map_t *)handle;

	flags = DHD_PKTID_LOCK(map->pktid_lock);

	nkey = __dhd_pktid_map_reserve(handle, pkt, buf_type);
	if (nkey != DHD_PKTID_INVALID) {
		__dhd_pktid_map_save(handle, pkt, nkey, physaddr, len,
			dma, buf_type);
#if defined(DHD_PKTID_AUDIT_MAP)
		DHD_PKTID_AUDIT(map, nkey, DHD_TEST_IS_ALLOC); /* apriori, reservation */
#endif /* DHD_PKTID_AUDIT_MAP */
	}

	DHD_PKTID_UNLOCK(map->pktid_lock, flags);

	return nkey;
}

/*
 * Given a numbered key, return the locker contents.
 * dhd_pktid_map_free() is not reentrant, and is the caller's responsibility.
 * Caller may not free a pktid value DHD_PKTID_INVALID or an arbitrary pktid
 * value. Only a previously allocated pktid may be freed.
 */
static void * BCMFASTPATH
dhd_pktid_map_free(dhd_pktid_map_handle_t *handle, uint32 nkey,
	dmaaddr_t *physaddr, uint32 *len, uint8 buf_type)
{
	dhd_pktid_map_t *map;
	dhd_pktid_item_t *locker;
	void * pkt;
	unsigned long flags;

	ASSERT(handle != NULL);

	map = (dhd_pktid_map_t *)handle;

	flags = DHD_PKTID_LOCK(map->pktid_lock);

	ASSERT((nkey != DHD_PKTID_INVALID) && (nkey <= DHD_PKIDMAP_ITEMS(map->items)));

	if ((nkey == DHD_PKTID_INVALID) || (nkey > DHD_PKIDMAP_ITEMS(map->items))) {
		DHD_ERROR(("%s:%d: Error! Try to free invalid pktid<%u>, buf_type<%u>\n",
			__FUNCTION__, __LINE__, nkey, buf_type));
		return NULL;
	}

	locker = &map->lockers[nkey];

#if defined(DHD_PKTID_AUDIT_MAP)
	DHD_PKTID_AUDIT(map, nkey, DHD_DUPLICATE_FREE); /* Audit duplicate FREE */
#endif /* DHD_PKTID_AUDIT_MAP */

	if (locker->inuse == FALSE) { /* Debug check for cloned numbered key */
		DHD_ERROR(("%s:%d: Error! freeing invalid pktid<%u>\n",
		           __FUNCTION__, __LINE__, nkey));
		ASSERT(locker->inuse != FALSE);

		DHD_PKTID_UNLOCK(map->pktid_lock, flags);
		return NULL;
	}

	/* Check for the colour of the buffer i.e The buffer posted for TX,
	 * should be freed for TX completion. Similarly the buffer posted for
	 * IOCTL should be freed for IOCT completion etc.
	 */
	if ((buf_type != BUFF_TYPE_NO_CHECK) && (locker->buf_type != buf_type)) {
		DHD_ERROR(("%s:%d: Error! Invalid Buffer Free for pktid<%u> \n",
		           __FUNCTION__, __LINE__, nkey));
		ASSERT(locker->buf_type == buf_type);

		DHD_PKTID_UNLOCK(map->pktid_lock, flags);
		return NULL;
	}

	map->avail++;

#if defined(DHD_PKTIDMAP_FIFO)
	ASSERT(locker->nkey == nkey);

	dll_delete(&locker->list_node); /* Free locker to "tail" of free list */
	dll_append(&map->list_free, &locker->list_node);
#else /* ! DHD_PKTIDMAP_FIFO */
	map->keys[map->avail] = nkey; /* make this numbered key available */
#endif /* ! DHD_PKTIDMAP_FIFO */

	locker->inuse = FALSE; /* open and free Locker */

#if defined(DHD_PKTID_AUDIT_MAP)
	DHD_PKTID_AUDIT(map, nkey, DHD_TEST_IS_FREE);
#endif /* DHD_PKTID_AUDIT_MAP */

	*physaddr = locker->physaddr; /* return contents of locker */
	*len = (uint32)locker->len;

	pkt = locker->pkt;
	locker->pkt = NULL; /* Clear pkt */
	locker->len = 0;

	DHD_PKTID_UNLOCK(map->pktid_lock, flags);
	return pkt;
}

/* Linkage, sets prot link and updates hdrlen in pub */
int dhd_prot_attach(dhd_pub_t *dhd)
{
	uint alloced = 0;

	dhd_prot_t *prot;

	/* Allocate prot structure */
	if (!(prot = (dhd_prot_t *)DHD_OS_PREALLOC(dhd, DHD_PREALLOC_PROT,
		sizeof(dhd_prot_t)))) {
		DHD_ERROR(("%s: kmalloc failed\n", __FUNCTION__));
		goto fail;
	}
	memset(prot, 0, sizeof(*prot));

	prot->osh = dhd->osh;
	dhd->prot = prot;

	/* DMAing ring completes supported? FALSE by default  */
	dhd->dma_d2h_ring_upd_support = FALSE;
	dhd->dma_h2d_ring_upd_support = FALSE;

	/* Ring Allocations */
	/* 1.0	 H2D	TXPOST ring */
	if (!(prot->h2dring_txp_subn = prot_ring_attach(prot, "h2dtxp",
		H2DRING_TXPOST_MAX_ITEM, H2DRING_TXPOST_ITEMSIZE,
		BCMPCIE_H2D_TXFLOWRINGID))) {
		DHD_ERROR(("%s: kmalloc for H2D    TXPOST ring  failed\n", __FUNCTION__));
		goto fail;
	}

	/* 2.0	 H2D	RXPOST ring */
	if (!(prot->h2dring_rxp_subn = prot_ring_attach(prot, "h2drxp",
		H2DRING_RXPOST_MAX_ITEM, H2DRING_RXPOST_ITEMSIZE,
		BCMPCIE_H2D_MSGRING_RXPOST_SUBMIT))) {
		DHD_ERROR(("%s: kmalloc for H2D    RXPOST ring  failed\n", __FUNCTION__));
		goto fail;

	}

	/* 3.0	 H2D	CTRL_SUBMISSION ring */
	if (!(prot->h2dring_ctrl_subn = prot_ring_attach(prot, "h2dctrl",
		H2DRING_CTRL_SUB_MAX_ITEM, H2DRING_CTRL_SUB_ITEMSIZE,
		BCMPCIE_H2D_MSGRING_CONTROL_SUBMIT))) {
		DHD_ERROR(("%s: kmalloc for H2D    CTRL_SUBMISSION ring failed\n",
			__FUNCTION__));
		goto fail;

	}

	/* 4.0	 D2H	TX_COMPLETION ring */
	if (!(prot->d2hring_tx_cpln = prot_ring_attach(prot, "d2htxcpl",
		D2HRING_TXCMPLT_MAX_ITEM, D2HRING_TXCMPLT_ITEMSIZE,
		BCMPCIE_D2H_MSGRING_TX_COMPLETE))) {
		DHD_ERROR(("%s: kmalloc for D2H    TX_COMPLETION ring failed\n",
			__FUNCTION__));
		goto fail;

	}

	/* 5.0	 D2H	RX_COMPLETION ring */
	if (!(prot->d2hring_rx_cpln = prot_ring_attach(prot, "d2hrxcpl",
		D2HRING_RXCMPLT_MAX_ITEM, D2HRING_RXCMPLT_ITEMSIZE,
		BCMPCIE_D2H_MSGRING_RX_COMPLETE))) {
		DHD_ERROR(("%s: kmalloc for D2H    RX_COMPLETION ring failed\n",
			__FUNCTION__));
		goto fail;

	}

	/* 6.0	 D2H	CTRL_COMPLETION ring */
	if (!(prot->d2hring_ctrl_cpln = prot_ring_attach(prot, "d2hctrl",
		D2HRING_CTRL_CMPLT_MAX_ITEM, D2HRING_CTRL_CMPLT_ITEMSIZE,
		BCMPCIE_D2H_MSGRING_CONTROL_COMPLETE))) {
		DHD_ERROR(("%s: kmalloc for D2H    CTRL_COMPLETION ring failed\n",
			__FUNCTION__));
		goto fail;
	}

	/* Return buffer for ioctl */
	prot->retbuf.va = DMA_ALLOC_CONSISTENT(dhd->osh, IOCT_RETBUF_SIZE, DMA_ALIGN_LEN,
		&alloced, &prot->retbuf.pa, &prot->retbuf.dmah);
	if (prot->retbuf.va ==  NULL) {
		ASSERT(0);
		return BCME_NOMEM;
	}

	ASSERT(MODX((unsigned long)prot->retbuf.va, DMA_ALIGN_LEN) == 0);
	bzero(prot->retbuf.va, IOCT_RETBUF_SIZE);
	OSL_CACHE_FLUSH((void *) prot->retbuf.va, IOCT_RETBUF_SIZE);

	/* IOCTL request buffer */
	prot->ioctbuf.va = DMA_ALLOC_CONSISTENT(dhd->osh, IOCT_RETBUF_SIZE, DMA_ALIGN_LEN,
		&alloced, &prot->ioctbuf.pa, &prot->ioctbuf.dmah);

	if (prot->ioctbuf.va ==  NULL) {
		ASSERT(0);
		return BCME_NOMEM;
	}

	ASSERT(MODX((unsigned long)prot->ioctbuf.va, DMA_ALIGN_LEN) == 0);
	bzero(prot->ioctbuf.va, IOCT_RETBUF_SIZE);
	OSL_CACHE_FLUSH((void *) prot->ioctbuf.va, IOCT_RETBUF_SIZE);

	/* Scratch buffer for dma rx offset */
	prot->d2h_dma_scratch_buf_len = DMA_D2H_SCRATCH_BUF_LEN;
	prot->d2h_dma_scratch_buf.va = DMA_ALLOC_CONSISTENT(dhd->osh, DMA_D2H_SCRATCH_BUF_LEN,
		DMA_ALIGN_LEN, &alloced, &prot->d2h_dma_scratch_buf.pa,
		&prot->d2h_dma_scratch_buf.dmah);

	if (prot->d2h_dma_scratch_buf.va == NULL) {
		ASSERT(0);
		return BCME_NOMEM;
	}
	ASSERT(MODX((unsigned long)prot->d2h_dma_scratch_buf.va, DMA_ALIGN_LEN) == 0);
	bzero(prot->d2h_dma_scratch_buf.va, DMA_D2H_SCRATCH_BUF_LEN);
	OSL_CACHE_FLUSH((void *)prot->d2h_dma_scratch_buf.va, DMA_D2H_SCRATCH_BUF_LEN);


	/* PKTID handle INIT */
	prot->pktid_map_handle = NATIVE_TO_PKTID_INIT(dhd->osh, MAX_PKTID_ITEMS);
	if (prot->pktid_map_handle == NULL) {
		ASSERT(0);
		return BCME_NOMEM;
	}

	prot->dmaxfer.srcmem.va = NULL;
	prot->dmaxfer.destmem.va = NULL;
	prot->dmaxfer_in_progress = FALSE;

	prot->rx_metadata_offset = 0;
	prot->tx_metadata_offset = 0;

#ifdef DHD_RX_CHAINING
	dhd_rxchain_reset(&prot->rxchain);
#endif

	return 0;

fail:
#ifndef CONFIG_DHD_USE_STATIC_BUF
	if (prot != NULL)
		dhd_prot_detach(dhd);
#endif /* CONFIG_DHD_USE_STATIC_BUF */
	return BCME_NOMEM;
}

/* Init memory block on host DMA'ing indices */
int
dhd_prot_init_index_dma_block(dhd_pub_t *dhd, uint8 type, uint32 length)
{
	uint alloced = 0;

	dhd_prot_t *prot = dhd->prot;
	uint32 dma_block_size = 4 * length;

	if (prot == NULL) {
		DHD_ERROR(("prot is not inited\n"));
		return BCME_ERROR;
	}

	switch (type) {
		case HOST_TO_DNGL_DMA_WRITEINDX_BUFFER:
			/* ring update dma buffer for submission write */
			prot->h2d_dma_writeindx_buf_len = dma_block_size;
			prot->h2d_dma_writeindx_buf.va = DMA_ALLOC_CONSISTENT(dhd->osh,
				dma_block_size, DMA_ALIGN_LEN, &alloced,
				&prot->h2d_dma_writeindx_buf.pa,
				&prot->h2d_dma_writeindx_buf.dmah);

			if (prot->h2d_dma_writeindx_buf.va == NULL) {
				return BCME_NOMEM;
			}

			ASSERT(ISALIGNED(prot->h2d_dma_writeindx_buf.va, 4));
			bzero(prot->h2d_dma_writeindx_buf.va, dma_block_size);
			OSL_CACHE_FLUSH((void *)prot->h2d_dma_writeindx_buf.va, dma_block_size);
			DHD_ERROR(("H2D_WRITEINDX_ARRAY_HOST: %d-bytes "
				"inited for dma'ing h2d-w indices\n",
				prot->h2d_dma_writeindx_buf_len));
			break;

		case HOST_TO_DNGL_DMA_READINDX_BUFFER:
			/* ring update dma buffer for submission read */
			prot->h2d_dma_readindx_buf_len = dma_block_size;
			prot->h2d_dma_readindx_buf.va = DMA_ALLOC_CONSISTENT(dhd->osh,
				dma_block_size, DMA_ALIGN_LEN, &alloced,
				&prot->h2d_dma_readindx_buf.pa,
				&prot->h2d_dma_readindx_buf.dmah);
			if (prot->h2d_dma_readindx_buf.va == NULL) {
				return BCME_NOMEM;
			}

			ASSERT(ISALIGNED(prot->h2d_dma_readindx_buf.va, 4));
			bzero(prot->h2d_dma_readindx_buf.va, dma_block_size);
			OSL_CACHE_FLUSH((void *)prot->h2d_dma_readindx_buf.va, dma_block_size);
			DHD_ERROR(("H2D_READINDX_ARRAY_HOST %d-bytes "
				"inited for dma'ing h2d-r indices\n",
				prot->h2d_dma_readindx_buf_len));
			break;

		case DNGL_TO_HOST_DMA_WRITEINDX_BUFFER:
			/* ring update dma buffer for completion write */
			prot->d2h_dma_writeindx_buf_len = dma_block_size;
			prot->d2h_dma_writeindx_buf.va = DMA_ALLOC_CONSISTENT(dhd->osh,
				dma_block_size, DMA_ALIGN_LEN, &alloced,
				&prot->d2h_dma_writeindx_buf.pa,
				&prot->d2h_dma_writeindx_buf.dmah);

			if (prot->d2h_dma_writeindx_buf.va == NULL) {
				return BCME_NOMEM;
			}

			ASSERT(ISALIGNED(prot->d2h_dma_writeindx_buf.va, 4));
			bzero(prot->d2h_dma_writeindx_buf.va, dma_block_size);
			OSL_CACHE_FLUSH((void *)prot->d2h_dma_writeindx_buf.va, dma_block_size);
			DHD_ERROR(("D2H_WRITEINDX_ARRAY_HOST %d-bytes "
				"inited for dma'ing d2h-w indices\n",
				prot->d2h_dma_writeindx_buf_len));
			break;

		case DNGL_TO_HOST_DMA_READINDX_BUFFER:
			/* ring update dma buffer for completion read */
			prot->d2h_dma_readindx_buf_len = dma_block_size;
			prot->d2h_dma_readindx_buf.va = DMA_ALLOC_CONSISTENT(dhd->osh,
				dma_block_size, DMA_ALIGN_LEN, &alloced,
				&prot->d2h_dma_readindx_buf.pa,
				&prot->d2h_dma_readindx_buf.dmah);

			if (prot->d2h_dma_readindx_buf.va == NULL) {
				return BCME_NOMEM;
			}

			ASSERT(ISALIGNED(prot->d2h_dma_readindx_buf.va, 4));
			bzero(prot->d2h_dma_readindx_buf.va, dma_block_size);
			OSL_CACHE_FLUSH((void *)prot->d2h_dma_readindx_buf.va, dma_block_size);
			DHD_ERROR(("D2H_READINDX_ARRAY_HOST %d-bytes "
				"inited for dma'ing d2h-r indices\n",
				prot->d2h_dma_readindx_buf_len));
			break;

		default:
			DHD_ERROR(("%s: Unexpected option\n", __FUNCTION__));
			return BCME_BADOPTION;
	}

	return BCME_OK;

}

/* Unlink, frees allocated protocol memory (including dhd_prot) */
void dhd_prot_detach(dhd_pub_t *dhd)
{
	dhd_prot_t *prot = dhd->prot;

	/* Stop the protocol module */
	if (dhd->prot) {

		/* free up scratch buffer */
		if (prot->d2h_dma_scratch_buf.va) {
			DMA_FREE_CONSISTENT(dhd->osh, prot->d2h_dma_scratch_buf.va,
			DMA_D2H_SCRATCH_BUF_LEN, prot->d2h_dma_scratch_buf.pa,
			prot->d2h_dma_scratch_buf.dmah);
			prot->d2h_dma_scratch_buf.va = NULL;
		}
		/* free up ring upd buffer for submission writes */
		if (prot->h2d_dma_writeindx_buf.va) {
			DMA_FREE_CONSISTENT(dhd->osh, prot->h2d_dma_writeindx_buf.va,
			  prot->h2d_dma_writeindx_buf_len, prot->h2d_dma_writeindx_buf.pa,
			  prot->h2d_dma_writeindx_buf.dmah);
			prot->h2d_dma_writeindx_buf.va = NULL;
		}

		/* free up ring upd buffer for submission reads */
		if (prot->h2d_dma_readindx_buf.va) {
			DMA_FREE_CONSISTENT(dhd->osh, prot->h2d_dma_readindx_buf.va,
			  prot->h2d_dma_readindx_buf_len, prot->h2d_dma_readindx_buf.pa,
			  prot->h2d_dma_readindx_buf.dmah);
			prot->h2d_dma_readindx_buf.va = NULL;
		}

		/* free up ring upd buffer for completion writes */
		if (prot->d2h_dma_writeindx_buf.va) {
			DMA_FREE_CONSISTENT(dhd->osh, prot->d2h_dma_writeindx_buf.va,
			  prot->d2h_dma_writeindx_buf_len, prot->d2h_dma_writeindx_buf.pa,
			  prot->d2h_dma_writeindx_buf.dmah);
			prot->d2h_dma_writeindx_buf.va = NULL;
		}

		/* free up ring upd buffer for completion writes */
		if (prot->d2h_dma_readindx_buf.va) {
			DMA_FREE_CONSISTENT(dhd->osh, prot->d2h_dma_readindx_buf.va,
			  prot->d2h_dma_readindx_buf_len, prot->d2h_dma_readindx_buf.pa,
			  prot->d2h_dma_readindx_buf.dmah);
			prot->d2h_dma_readindx_buf.va = NULL;
		}

		/* ioctl return buffer */
		if (prot->retbuf.va) {
			DMA_FREE_CONSISTENT(dhd->osh, dhd->prot->retbuf.va,
			IOCT_RETBUF_SIZE, dhd->prot->retbuf.pa, dhd->prot->retbuf.dmah);
			dhd->prot->retbuf.va = NULL;
		}

		/* ioctl request buffer */
		if (prot->ioctbuf.va) {
			DMA_FREE_CONSISTENT(dhd->osh, dhd->prot->ioctbuf.va,
			IOCT_RETBUF_SIZE, dhd->prot->ioctbuf.pa, dhd->prot->ioctbuf.dmah);

			dhd->prot->ioctbuf.va = NULL;
		}


		/* 1.0	 H2D	TXPOST ring */
		dhd_prot_ring_detach(dhd, prot->h2dring_txp_subn);
		/* 2.0	 H2D	RXPOST ring */
		dhd_prot_ring_detach(dhd, prot->h2dring_rxp_subn);
		/* 3.0	 H2D	CTRL_SUBMISSION ring */
		dhd_prot_ring_detach(dhd, prot->h2dring_ctrl_subn);
		/* 4.0	 D2H	TX_COMPLETION ring */
		dhd_prot_ring_detach(dhd, prot->d2hring_tx_cpln);
		/* 5.0	 D2H	RX_COMPLETION ring */
		dhd_prot_ring_detach(dhd, prot->d2hring_rx_cpln);
		/* 6.0	 D2H	CTRL_COMPLETION ring */
		dhd_prot_ring_detach(dhd, prot->d2hring_ctrl_cpln);

		NATIVE_TO_PKTID_FINI(dhd->prot->pktid_map_handle);

#ifndef CONFIG_DHD_USE_STATIC_BUF
		MFREE(dhd->osh, dhd->prot, sizeof(dhd_prot_t));
#endif /* CONFIG_DHD_USE_STATIC_BUF */

		/* No need to do anything to prot->ioctl_mutex
		 * Unlike semaphores no memory is allocated during mutex_init
		 * So there is no call to free up the same.
		 */
		dhd->prot = NULL;
	}
}

void
dhd_prot_rx_dataoffset(dhd_pub_t *dhd, uint32 rx_offset)
{
	dhd_prot_t *prot = dhd->prot;
	prot->rx_dataoffset = rx_offset;
}


/* Initialize protocol: sync w/dongle state.
 * Sets dongle media info (iswl, drv_version, mac address).
 */
int dhd_sync_with_dongle(dhd_pub_t *dhd)
{
	int ret = 0;
	wlc_rev_info_t revinfo;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	/* Post event buffer after shim layer is attached */
	ret = dhd_msgbuf_rxbuf_post_event_bufs(dhd);
	if (ret <= 0) {
		DHD_ERROR(("%s : Post event buffer fail. ret = %d\n", __FUNCTION__, ret));
		return ret;
	}

	dhd_os_set_ioctl_resp_timeout(IOCTL_RESP_TIMEOUT);


#ifdef CUSTOMER_HW4
	/* Check the memdump capability */
	dhd_get_memdump_info(dhd);
#endif /* CUSTOMER_HW4 */

	/* Get the device rev info */
	memset(&revinfo, 0, sizeof(revinfo));
	ret = dhd_wl_ioctl_cmd(dhd, WLC_GET_REVINFO, &revinfo, sizeof(revinfo), FALSE, 0);
	if (ret < 0)
		goto done;

	dhd_process_cid_mac(dhd, TRUE);

	ret = dhd_preinit_ioctls(dhd);

	if (!ret)
		dhd_process_cid_mac(dhd, FALSE);

	/* Always assumes wl for now */
	dhd->iswl = TRUE;
done:
	return ret;
}

/* This function does all necessary initialization needed
* for IOCTL/IOVAR path
*/
int dhd_prot_init(dhd_pub_t *dhd)
{
	int ret = 0;
	dhd_prot_t *prot = dhd->prot;

	/* Max pkts in ring */
	prot->max_tx_count = H2DRING_TXPOST_MAX_ITEM;

	DHD_INFO(("%s:%d: MAX_TX_COUNT = %d\n", __FUNCTION__, __LINE__, prot->max_tx_count));

	/* Read max rx packets supported by dongle */
	dhd_bus_cmn_readshared(dhd->bus, &prot->max_rxbufpost, MAX_HOST_RXBUFS, 0);
	if (prot->max_rxbufpost == 0) {
		/* This would happen if the dongle firmware is not */
		/* using the latest shared structure template */
		prot->max_rxbufpost = DEFAULT_RX_BUFFERS_TO_POST;
	}
	DHD_INFO(("%s:%d: MAX_RXBUFPOST = %d\n", __FUNCTION__, __LINE__, prot->max_rxbufpost));

	prot->max_eventbufpost = DHD_FLOWRING_MAX_EVENTBUF_POST;
	prot->max_ioctlrespbufpost = DHD_FLOWRING_MAX_IOCTLRESPBUF_POST;

	prot->active_tx_count = 0;
	prot->data_seq_no = 0;
	prot->ioctl_seq_no = 0;
	prot->txp_threshold = TXP_FLUSH_MAX_ITEMS_FLUSH_CNT;

	prot->ioctl_trans_id = 1;

	/*
	 * Initialize the mutex that serializes the calls to
	 * dhd_prot_ioctl, if the ioctls are issued from
	 * multiple process/CPU contexts
	 */
	mutex_init(&prot->ioctl_mutex);

	/* Register the interrupt function upfront */
	/* remove corerev checks in data path */
	prot->mb_ring_fn = dhd_bus_get_mbintr_fn(dhd->bus);

	/* Initialise rings */
	/* 1.0	 H2D	TXPOST ring */
	if (dhd_bus_is_txmode_push(dhd->bus)) {
		dhd_ring_init(dhd, prot->h2dring_txp_subn);
	}

	/* 2.0	 H2D	RXPOST ring */
	dhd_ring_init(dhd, prot->h2dring_rxp_subn);
	/* 3.0	 H2D	CTRL_SUBMISSION ring */
	dhd_ring_init(dhd, prot->h2dring_ctrl_subn);
	/* 4.0	 D2H	TX_COMPLETION ring */
	dhd_ring_init(dhd, prot->d2hring_tx_cpln);
	/* 5.0	 D2H	RX_COMPLETION ring */
	dhd_ring_init(dhd, prot->d2hring_rx_cpln);
	/* 6.0	 D2H	CTRL_COMPLETION ring */
	dhd_ring_init(dhd, prot->d2hring_ctrl_cpln);

#if defined(PCIE_D2H_SYNC)
	dhd_prot_d2h_sync_init(dhd, prot);
#endif /* PCIE_D2H_SYNC */

	/* init the scratch buffer */
	dhd_bus_cmn_writeshared(dhd->bus, &prot->d2h_dma_scratch_buf.pa,
		sizeof(prot->d2h_dma_scratch_buf.pa), DNGL_TO_HOST_DMA_SCRATCH_BUFFER, 0);
	dhd_bus_cmn_writeshared(dhd->bus, &prot->d2h_dma_scratch_buf_len,
		sizeof(prot->d2h_dma_scratch_buf_len), DNGL_TO_HOST_DMA_SCRATCH_BUFFER_LEN, 0);

	/* If supported by the host, indicate the memory block
	 * for comletion writes / submission reads to shared space
	 */
	if (DMA_INDX_ENAB(dhd->dma_d2h_ring_upd_support)) {
		dhd_bus_cmn_writeshared(dhd->bus, &prot->d2h_dma_writeindx_buf.pa,
			sizeof(prot->d2h_dma_writeindx_buf.pa),
			DNGL_TO_HOST_DMA_WRITEINDX_BUFFER, 0);
		dhd_bus_cmn_writeshared(dhd->bus, &prot->h2d_dma_readindx_buf.pa,
			sizeof(prot->h2d_dma_readindx_buf.pa),
			HOST_TO_DNGL_DMA_READINDX_BUFFER, 0);
	}

	if (DMA_INDX_ENAB(dhd->dma_h2d_ring_upd_support)) {
		dhd_bus_cmn_writeshared(dhd->bus, &prot->h2d_dma_writeindx_buf.pa,
			sizeof(prot->h2d_dma_writeindx_buf.pa),
			HOST_TO_DNGL_DMA_WRITEINDX_BUFFER, 0);
		dhd_bus_cmn_writeshared(dhd->bus, &prot->d2h_dma_readindx_buf.pa,
			sizeof(prot->d2h_dma_readindx_buf.pa),
			DNGL_TO_HOST_DMA_READINDX_BUFFER, 0);

	}

	ret = dhd_msgbuf_rxbuf_post(dhd);
	ret = dhd_msgbuf_rxbuf_post_ioctlresp_bufs(dhd);

	return ret;
}

#define DHD_DBG_SHOW_METADATA	0
#if DHD_DBG_SHOW_METADATA
static void BCMFASTPATH
dhd_prot_print_metadata(dhd_pub_t *dhd, void *ptr, int len)
{
	uint8 tlv_t;
	uint8 tlv_l;
	uint8 *tlv_v = (uint8 *)ptr;

	if (len <= BCMPCIE_D2H_METADATA_HDRLEN)
		return;

	len -= BCMPCIE_D2H_METADATA_HDRLEN;
	tlv_v += BCMPCIE_D2H_METADATA_HDRLEN;

	while (len > TLV_HDR_LEN) {
		tlv_t = tlv_v[TLV_TAG_OFF];
		tlv_l = tlv_v[TLV_LEN_OFF];

		len -= TLV_HDR_LEN;
		tlv_v += TLV_HDR_LEN;
		if (len < tlv_l)
			break;
		if ((tlv_t == 0) || (tlv_t == WLFC_CTL_TYPE_FILLER))
			break;

		switch (tlv_t) {
		case WLFC_CTL_TYPE_TXSTATUS:
			bcm_print_bytes("METADATA TX_STATUS", tlv_v, tlv_l);
			break;

		case WLFC_CTL_TYPE_RSSI:
			bcm_print_bytes("METADATA RX_RSSI", tlv_v, tlv_l);
			break;

		case WLFC_CTL_TYPE_FIFO_CREDITBACK:
			bcm_print_bytes("METADATA FIFO_CREDITBACK", tlv_v, tlv_l);
			break;

		case WLFC_CTL_TYPE_TX_ENTRY_STAMP:
			bcm_print_bytes("METADATA TX_ENTRY", tlv_v, tlv_l);
			break;

		case WLFC_CTL_TYPE_RX_STAMP:
			bcm_print_bytes("METADATA RX_TIMESTAMP", tlv_v, tlv_l);
			break;

		case WLFC_CTL_TYPE_TRANS_ID:
			bcm_print_bytes("METADATA TRANS_ID", tlv_v, tlv_l);
			break;

		case WLFC_CTL_TYPE_COMP_TXSTATUS:
			bcm_print_bytes("METADATA COMP_TXSTATUS", tlv_v, tlv_l);
			break;

		default:
			bcm_print_bytes("METADATA UNKNOWN", tlv_v, tlv_l);
			break;
		}

		len -= tlv_l;
		tlv_v += tlv_l;
	}
}
#endif /* DHD_DBG_SHOW_METADATA */

static INLINE void BCMFASTPATH
dhd_prot_packet_free(dhd_pub_t *dhd, uint32 pktid, uint8 buf_type)
{
	void *PKTBUF;
	dmaaddr_t pa;
	uint32 pa_len;
	PKTBUF = PKTID_TO_NATIVE(dhd->prot->pktid_map_handle, pktid, pa,
		pa_len, buf_type);

	if (PKTBUF) {
		if (!PHYSADDRISZERO(pa)) {
			if (buf_type == BUFF_TYPE_DATA_TX) {
				DMA_UNMAP(dhd->osh, pa, (uint) pa_len, DMA_TX, 0, 0);
			} else {
				DMA_UNMAP(dhd->osh, pa, (uint) pa_len, DMA_RX, 0, 0);
			}
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_CTRLBUF)
			if (buf_type == BUFF_TYPE_IOCTL_RX ||
				buf_type == BUFF_TYPE_EVENT_RX) {
				PKTFREE_STATIC(dhd->osh, PKTBUF, FALSE);
			} else {
#endif /* CONFIG_DHD_USE_STATIC_BUF && DHD_USE_STATIC_CTRLBUF */
				PKTFREE(dhd->osh, PKTBUF, FALSE);
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_CTRLBUF)
			}
#endif /* CONFIG_DHD_USE_STATIC_BUF && DHD_USE_STATIC_CTRLBUF */
		} else {
			DHD_ERROR(("%s: Invalid phyaddr 0\n", __FUNCTION__));
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_CTRLBUF)
			if (buf_type == BUFF_TYPE_IOCTL_RX ||
				buf_type == BUFF_TYPE_EVENT_RX) {
				PKTINVALIDATE_STATIC(dhd->osh, PKTBUF);
			} else {
#endif /* CONFIG_DHD_USE_STATIC_BUF && DHD_USE_STATIC_CTRLBUF */
				PKTFREE(dhd->osh, PKTBUF, FALSE);
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_CTRLBUF)
			}
#endif /* CONFIG_DHD_USE_STATIC_BUF && DHD_USE_STATIC_CTRLBUF */
		}
	}
	return;
}

static INLINE void * BCMFASTPATH
dhd_prot_packet_get(dhd_pub_t *dhd, uint32 pktid, uint8 buf_type)
{
	void *PKTBUF;
	dmaaddr_t pa;
	uint32 pa_len;
	PKTBUF = PKTID_TO_NATIVE(dhd->prot->pktid_map_handle, pktid, pa, pa_len, buf_type);
	if (PKTBUF) {
		if (!PHYSADDRISZERO(pa)) {
			if (buf_type == BUFF_TYPE_DATA_TX) {
				DMA_UNMAP(dhd->osh, pa, (uint) pa_len, DMA_TX, 0, 0);
			} else {
				DMA_UNMAP(dhd->osh, pa, (uint) pa_len, DMA_RX, 0, 0);
			}
		} else {
			DHD_ERROR(("%s: Invalid phyaddr 0\n", __FUNCTION__));
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_CTRLBUF)
			if (buf_type == BUFF_TYPE_IOCTL_RX ||
				buf_type == BUFF_TYPE_EVENT_RX) {
				PKTINVALIDATE_STATIC(dhd->osh, PKTBUF);
			}
#endif /* CONFIG_DHD_USE_STATIC_BUF && DHD_USE_STATIC_CTRLBUF */
		}
	}

	return PKTBUF;
}

static int BCMFASTPATH
dhd_msgbuf_rxbuf_post(dhd_pub_t *dhd)
{
	dhd_prot_t *prot = dhd->prot;
	int16 fillbufs;
	uint16 cnt = 64;
	int retcount = 0;

	fillbufs = prot->max_rxbufpost - prot->rxbufpost;
	while (fillbufs > 0) {
		cnt--;
		if (cnt == 0) {
			/* find a better way to reschedule rx buf post if space not available */
			DHD_ERROR(("h2d rx post ring not available to post host buffers \n"));
			DHD_ERROR(("Current posted host buf count %d \n", prot->rxbufpost));
			break;
		}

		/* Post in a burst of 8 buffers ata time */
		fillbufs = MIN(fillbufs, RX_BUF_BURST);

		/* Post buffers */
		retcount = dhd_prot_rxbufpost(dhd, fillbufs);

		if (retcount > 0) {
			prot->rxbufpost += (uint16)retcount;

			/* how many more to post */
			fillbufs = prot->max_rxbufpost - prot->rxbufpost;
		} else {
			/* Make sure we don't run loop any further */
			fillbufs = 0;
		}
	}

	return 0;
}

/* Post count no of rx buffers down to dongle */
static int BCMFASTPATH
dhd_prot_rxbufpost(dhd_pub_t *dhd, uint16 count)
{
	void *p;
	uint16 pktsz = DHD_FLOWRING_RX_BUFPOST_PKTSZ;
	uint8 *rxbuf_post_tmp;
	host_rxbuf_post_t *rxbuf_post;
	void* msg_start;
	dmaaddr_t physaddr;
	uint32 pktlen;
	dhd_prot_t *prot = dhd->prot;
	msgbuf_ring_t * ring = prot->h2dring_rxp_subn;
	uint8 i = 0;
	uint16 alloced = 0;
	unsigned long flags;
	uint32 pktid;

	DHD_GENERAL_LOCK(dhd, flags);
	/* Claim space for 'count' no of messages */
	msg_start = (void *)dhd_alloc_ring_space(dhd, ring, count, &alloced);
	DHD_GENERAL_UNLOCK(dhd, flags);

	if (msg_start == NULL) {
		DHD_INFO(("%s:%d: Rxbufpost Msgbuf Not available\n", __FUNCTION__, __LINE__));
		return -1;
	}
	/* if msg_start !=  NULL, we should have alloced space for atleast 1 item */
	ASSERT(alloced > 0);

	rxbuf_post_tmp = (uint8*)msg_start;

	/* loop through each message */
	for (i = 0; i < alloced; i++) {
		rxbuf_post = (host_rxbuf_post_t *)rxbuf_post_tmp;
		/* Create a rx buffer */
		if ((p = PKTGET(dhd->osh, pktsz, FALSE)) == NULL) {
			DHD_ERROR(("%s:%d: PKTGET for rxbuf failed\n", __FUNCTION__, __LINE__));
			break;
		}

		pktlen = PKTLEN(dhd->osh, p);
		physaddr = DMA_MAP(dhd->osh, PKTDATA(dhd->osh, p), pktlen, DMA_RX, p, 0);
		if (PHYSADDRISZERO(physaddr)) {
			PKTFREE(dhd->osh, p, FALSE);
			DHD_ERROR(("Invalid phyaddr 0\n"));
			ASSERT(0);
			break;
		}

		PKTPULL(dhd->osh, p, prot->rx_metadata_offset);
		pktlen = PKTLEN(dhd->osh, p);

		/* CMN msg header */
		rxbuf_post->cmn_hdr.msg_type = MSG_TYPE_RXBUF_POST;
		rxbuf_post->cmn_hdr.if_id = 0;

		/* get the lock before calling NATIVE_TO_PKTID */
		DHD_GENERAL_LOCK(dhd, flags);

		pktid = htol32(NATIVE_TO_PKTID(dhd->prot->pktid_map_handle, p, physaddr,
			pktlen, DMA_RX, BUFF_TYPE_DATA_RX));

		/* free lock */
		DHD_GENERAL_UNLOCK(dhd, flags);

		if (pktid == DHD_PKTID_INVALID) {
			DMA_UNMAP(dhd->osh, physaddr, pktlen, DMA_RX, 0, 0);
			PKTFREE(dhd->osh, p, FALSE);
			DHD_ERROR(("Pktid pool depleted.\n"));
			break;
		}

		/* CMN msg header */
		rxbuf_post->cmn_hdr.msg_type = MSG_TYPE_RXBUF_POST;
		rxbuf_post->cmn_hdr.if_id = 0;

		rxbuf_post->data_buf_len = htol16((uint16)pktlen);
		rxbuf_post->data_buf_addr.high_addr = htol32(PHYSADDRHI(physaddr));
		rxbuf_post->data_buf_addr.low_addr =
			htol32(PHYSADDRLO(physaddr) + prot->rx_metadata_offset);

		if (prot->rx_metadata_offset) {
			rxbuf_post->metadata_buf_len = prot->rx_metadata_offset;
			rxbuf_post->metadata_buf_addr.high_addr = htol32(PHYSADDRHI(physaddr));
			rxbuf_post->metadata_buf_addr.low_addr  = htol32(PHYSADDRLO(physaddr));
		} else {
			rxbuf_post->metadata_buf_len = 0;
			rxbuf_post->metadata_buf_addr.high_addr = 0;
			rxbuf_post->metadata_buf_addr.low_addr  = 0;
		}

#if defined(DHD_PKTID_AUDIT_RING)
		DHD_PKTID_AUDIT(prot->pktid_map_handle, pktid, DHD_DUPLICATE_ALLOC);
#endif /* DHD_PKTID_AUDIT_RING */

		rxbuf_post->cmn_hdr.request_id = htol32(pktid);

		/* Move rxbuf_post_tmp to next item */
		rxbuf_post_tmp = rxbuf_post_tmp + RING_LEN_ITEMS(ring);
	}

	if (i < alloced) {
		if (RING_WRITE_PTR(ring) < (alloced - i))
			RING_WRITE_PTR(ring) = RING_MAX_ITEM(ring) - (alloced - i);
		else
			RING_WRITE_PTR(ring) -= (alloced - i);

		alloced = i;
	}

	/* Update the write pointer in TCM & ring bell */
	if (alloced > 0)
		prot_ring_write_complete(dhd, prot->h2dring_rxp_subn, msg_start, alloced);

	return alloced;
}

static int
dhd_prot_rxbufpost_ctrl(dhd_pub_t *dhd, bool event_buf)
{
	void *p;
	uint16 pktsz;
	ioctl_resp_evt_buf_post_msg_t *rxbuf_post;
	dmaaddr_t physaddr;
	uint32 pktlen;
	dhd_prot_t *prot = dhd->prot;
	uint16 alloced = 0;
	unsigned long flags;
	uint8 buf_type;
	uint32 pktid;

	if (dhd->busstate == DHD_BUS_DOWN) {
		DHD_ERROR(("%s: bus is already down.\n", __FUNCTION__));
		return -1;
	}

	if (event_buf) {
		/* Allocate packet for event buffer post */
		pktsz = DHD_FLOWRING_RX_BUFPOST_PKTSZ;
	} else {
		/* Allocate packet for ctrl/ioctl buffer post */
		pktsz = DHD_FLOWRING_IOCTL_BUFPOST_PKTSZ;
	}

#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_CTRLBUF)
	p = PKTGET_STATIC(dhd->osh, pktsz, FALSE);
#else
	p = PKTGET(dhd->osh, pktsz, FALSE);
#endif /* CONFIG_DHD_USE_STATIC_BUF && DHD_USE_STATIC_CTRLBUF */

	if (p == NULL) {
		DHD_ERROR(("%s:%d: PKTGET for %s rxbuf failed\n",
			__FUNCTION__, __LINE__, event_buf ? "event" :
			"ioctl"));
		return -1;
	}

	pktlen = PKTLEN(dhd->osh, p);
	physaddr = DMA_MAP(dhd->osh, PKTDATA(dhd->osh, p), pktlen, DMA_RX, p, 0);
	if (PHYSADDRISZERO(physaddr)) {

		DHD_ERROR(("Invalid phyaddr 0\n"));
		ASSERT(0);
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_CTRLBUF)
		PKTINVALIDATE_STATIC(dhd->osh, p);
		return -1;
#endif /* CONFIG_DHD_USE_STATIC_BUF && DHD_USE_STATIC_CTRLBUF */
		goto free_pkt_return;
	}

	DHD_GENERAL_LOCK(dhd, flags);
	rxbuf_post = (ioctl_resp_evt_buf_post_msg_t *)dhd_alloc_ring_space(dhd,
		prot->h2dring_ctrl_subn, DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D, &alloced);
	if (rxbuf_post == NULL) {
		DHD_GENERAL_UNLOCK(dhd, flags);
		DHD_ERROR(("%s:%d: Ctrl submit Msgbuf Not available to post buffer"
			" for %s\n", __FUNCTION__, __LINE__, event_buf ? "event" :
			"ioctl"));
		DMA_UNMAP(dhd->osh, physaddr, pktlen, DMA_RX, 0, 0);
		goto free_pkt_return;
	}

	buf_type = ((event_buf == 1) ? BUFF_TYPE_EVENT_RX :
		BUFF_TYPE_IOCTL_RX);

	pktid = htol32(NATIVE_TO_PKTID(dhd->prot->pktid_map_handle, p, physaddr,
		pktlen, DMA_RX, buf_type));

	if (pktid == DHD_PKTID_INVALID) {
		if (RING_WRITE_PTR(prot->h2dring_ctrl_subn) == 0) {
			RING_WRITE_PTR(prot->h2dring_ctrl_subn) =
				RING_MAX_ITEM(prot->h2dring_ctrl_subn) - 1;
		} else {
			RING_WRITE_PTR(prot->h2dring_ctrl_subn)--;
		}
		DHD_GENERAL_UNLOCK(dhd, flags);
		DMA_UNMAP(dhd->osh, physaddr, pktlen, DMA_RX, 0, 0);
		goto free_pkt_return;
	}

#if defined(DHD_PKTID_AUDIT_RING)
	DHD_PKTID_AUDIT(prot->pktid_map_handle, pktid, DHD_DUPLICATE_ALLOC);
#endif /* DHD_PKTID_AUDIT_RING */

	/* CMN msg header */
	if (event_buf) {
		rxbuf_post->cmn_hdr.msg_type = MSG_TYPE_EVENT_BUF_POST;
	} else {
		rxbuf_post->cmn_hdr.msg_type = MSG_TYPE_IOCTLRESP_BUF_POST;
	}

	rxbuf_post->cmn_hdr.if_id = 0;
	rxbuf_post->cmn_hdr.flags = 0;

	rxbuf_post->host_buf_len = htol16((uint16)PKTLEN(dhd->osh, p));
	rxbuf_post->host_buf_addr.high_addr = htol32(PHYSADDRHI(physaddr));
	rxbuf_post->host_buf_addr.low_addr  = htol32(PHYSADDRLO(physaddr));
	rxbuf_post->cmn_hdr.request_id = htol32(pktid);

	/* Update the write pointer in TCM & ring bell */
	prot_ring_write_complete(dhd, prot->h2dring_ctrl_subn, rxbuf_post,
		DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D);
	DHD_GENERAL_UNLOCK(dhd, flags);

	return 1;

free_pkt_return:
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_CTRLBUF)
	PKTFREE_STATIC(dhd->osh, p, FALSE);
#else
	PKTFREE(dhd->osh, p, FALSE);
#endif /* CONFIG_DHD_USE_STATIC_BUF && DHD_USE_STATIC_CTRLBUF */

	return -1;
}

static uint16
dhd_msgbuf_rxbuf_post_ctrlpath(dhd_pub_t *dhd, bool event_buf, uint32 max_to_post)
{
	uint32 i = 0;
	int32 ret_val;

	DHD_INFO(("max to post %d, event %d \n", max_to_post, event_buf));

	if (dhd->busstate == DHD_BUS_DOWN) {
		DHD_ERROR(("%s: bus is already down.\n", __FUNCTION__));
		return 0;
	}

	while (i < max_to_post) {
		ret_val  = dhd_prot_rxbufpost_ctrl(dhd, event_buf);
		if (ret_val < 0)
			break;
		i++;
	}
	DHD_INFO(("posted %d buffers to event_pool/ioctl_resp_pool %d\n", i, event_buf));
	return (uint16)i;
}

static int
dhd_msgbuf_rxbuf_post_ioctlresp_bufs(dhd_pub_t *dhd)
{
	dhd_prot_t *prot = dhd->prot;
	uint16 retcnt = 0;

	DHD_INFO(("ioctl resp buf post\n"));

	if (dhd->busstate == DHD_BUS_DOWN) {
		DHD_ERROR(("%s: bus is already down.\n", __FUNCTION__));
		return 0;
	}

	retcnt = dhd_msgbuf_rxbuf_post_ctrlpath(dhd, FALSE,
		prot->max_ioctlrespbufpost - prot->cur_ioctlresp_bufs_posted);
	prot->cur_ioctlresp_bufs_posted += retcnt;
	return retcnt;
}

static int
dhd_msgbuf_rxbuf_post_event_bufs(dhd_pub_t *dhd)
{
	dhd_prot_t *prot = dhd->prot;
	uint16 retcnt = 0;

	if (dhd->busstate == DHD_BUS_DOWN) {
		DHD_ERROR(("%s: bus is already down.\n", __FUNCTION__));
		return 0;
	}

	retcnt = dhd_msgbuf_rxbuf_post_ctrlpath(dhd, TRUE,
		prot->max_eventbufpost - prot->cur_event_bufs_posted);

	prot->cur_event_bufs_posted += retcnt;
	return retcnt;
}

bool BCMFASTPATH
dhd_prot_process_msgbuf_rxcpl(dhd_pub_t *dhd, uint bound)
{
	dhd_prot_t *prot = dhd->prot;
	bool more = TRUE;
	uint n = 0;

	/* Process all the messages - DTOH direction */
	while (TRUE) {
		uint8 *src_addr;
		uint16 src_len;

		if (dhd->hang_was_sent) {
			more = FALSE;
			break;
		}

		/* Store current read pointer */
		/* Read pointer will be updated in prot_early_upd_rxcpln_read_idx */
		prot_store_rxcpln_read_idx(dhd, prot->d2hring_rx_cpln);

		/* Get the message from ring */
		src_addr = prot_get_src_addr(dhd, prot->d2hring_rx_cpln, &src_len);
		if (src_addr == NULL) {
			more = FALSE;
			break;
		}

		/* Prefetch data to populate the cache */
		OSL_PREFETCH(src_addr);

		if (dhd_prot_process_msgtype(dhd, prot->d2hring_rx_cpln, src_addr,
			src_len) != BCME_OK) {
			prot_upd_read_idx(dhd, prot->d2hring_rx_cpln);
			DHD_ERROR(("%s: Error at  process rxpl msgbuf of len %d\n",
				__FUNCTION__, src_len));
		}

		/* After batch processing, check RX bound */
		n += src_len/RING_LEN_ITEMS(prot->d2hring_rx_cpln);
		if (n >= bound) {
			break;
		}
	}

	return more;
}

void
dhd_prot_update_txflowring(dhd_pub_t *dhd, uint16 flow_id, void *msgring_info)
{
	uint16 r_index = 0;
	msgbuf_ring_t *ring = (msgbuf_ring_t *)msgring_info;

	/* Update read pointer */
	if (DMA_INDX_ENAB(dhd->dma_d2h_ring_upd_support)) {
		r_index = dhd_get_dmaed_index(dhd, H2D_DMA_READINDX, ring->idx);
		ring->ringstate->r_offset = r_index;
	}

	DHD_TRACE(("flow %d, write %d read %d \n\n", flow_id, RING_WRITE_PTR(ring),
		RING_READ_PTR(ring)));

	/* Need more logic here, but for now use it directly */
	dhd_bus_schedule_queue(dhd->bus, flow_id, TRUE);
}


bool BCMFASTPATH
dhd_prot_process_msgbuf_txcpl(dhd_pub_t *dhd, uint bound)
{
	dhd_prot_t *prot = dhd->prot;
	bool more = TRUE;
	uint n = 0;

	/* Process all the messages - DTOH direction */
	while (TRUE) {
		uint8 *src_addr;
		uint16 src_len;

		if (dhd->hang_was_sent) {
			more = FALSE;
			break;
		}

		src_addr = prot_get_src_addr(dhd, prot->d2hring_tx_cpln, &src_len);
		if (src_addr == NULL) {
			more = FALSE;
			break;
		}

		/* Prefetch data to populate the cache */
		OSL_PREFETCH(src_addr);

		if (dhd_prot_process_msgtype(dhd, prot->d2hring_tx_cpln, src_addr,
			src_len) != BCME_OK) {
			DHD_ERROR(("%s: Error at  process txcmpl msgbuf of len %d\n",
				__FUNCTION__, src_len));
		}

		/* Write to dngl rd ptr */
		prot_upd_read_idx(dhd, prot->d2hring_tx_cpln);

		/* After batch processing, check bound */
		n += src_len/RING_LEN_ITEMS(prot->d2hring_tx_cpln);
		if (n >= bound) {
			break;
		}
	}

	return more;
}

int BCMFASTPATH
dhd_prot_process_ctrlbuf(dhd_pub_t * dhd)
{
	dhd_prot_t *prot = dhd->prot;

	/* Process all the messages - DTOH direction */
	while (TRUE) {
		uint8 *src_addr;
		uint16 src_len;

		if (dhd->hang_was_sent) {
			break;
		}

		src_addr = prot_get_src_addr(dhd, prot->d2hring_ctrl_cpln, &src_len);

		if (src_addr == NULL) {
			break;
		}

		/* Prefetch data to populate the cache */
		OSL_PREFETCH(src_addr);
		if (dhd_prot_process_msgtype(dhd, prot->d2hring_ctrl_cpln, src_addr,
			src_len) != BCME_OK) {
			DHD_ERROR(("%s: Error at  process ctrlmsgbuf of len %d\n",
				__FUNCTION__, src_len));
		}

		/* Write to dngl rd ptr */
		prot_upd_read_idx(dhd, prot->d2hring_ctrl_cpln);
	}

	return 0;
}

static int BCMFASTPATH
dhd_prot_process_msgtype(dhd_pub_t *dhd, msgbuf_ring_t *ring, uint8* buf, uint16 len)
{
	dhd_prot_t *prot = dhd->prot;
	uint32 cur_dma_len = 0;
	int ret = BCME_OK;

	DHD_INFO(("%s: process msgbuf of len %d\n", __FUNCTION__, len));

	while (len > 0) {
		ASSERT(len > (sizeof(cmn_msg_hdr_t) + prot->rx_dataoffset));

		if (dhd->hang_was_sent) {
			ret = BCME_ERROR;
			break;
		}

		if (prot->rx_dataoffset) {
			cur_dma_len = *(uint32 *) buf;
			ASSERT(cur_dma_len <= len);
			buf += prot->rx_dataoffset;
			len -= (uint16)prot->rx_dataoffset;
		}
		else {
			cur_dma_len = len;
		}
		if (dhd_process_msgtype(dhd, ring, buf, (uint16)cur_dma_len) != BCME_OK) {
			DHD_ERROR(("%s: Error at  process msg of dmalen %d\n",
				__FUNCTION__, cur_dma_len));
			ret = BCME_ERROR;
		}

		len -= (uint16)cur_dma_len;
		buf += cur_dma_len;
	}
	return ret;
}

static int BCMFASTPATH
dhd_process_msgtype(dhd_pub_t *dhd, msgbuf_ring_t *ring, uint8* buf, uint16 len)
{
	uint16 pktlen = len;
	uint16 msglen;
	uint8 msgtype;
	cmn_msg_hdr_t *msg = NULL;
	int ret = BCME_OK;

#if defined(PCIE_D2H_SYNC_BZERO)
	uint8 *buf_head = buf;
#endif /* PCIE_D2H_SYNC_BZERO */

	ASSERT(ring && ring->ringmem);
	msglen = RING_LEN_ITEMS(ring);
	if (msglen == 0) {
		DHD_ERROR(("%s: ringidx %d, msglen is %d, pktlen is %d \n",
			__FUNCTION__, ring->idx, msglen, pktlen));
		return BCME_ERROR;
	}

	while (pktlen > 0) {
		if (dhd->hang_was_sent) {
			ret = BCME_ERROR;
			goto done;
		}

		msg = (cmn_msg_hdr_t *)buf;

#if defined(PCIE_D2H_SYNC)
		/* Wait until DMA completes, then fetch msgtype */
		msgtype = dhd->prot->d2h_sync_cb(dhd, ring, msg, msglen);
#else
		msgtype = msg->msg_type;
#endif /* !PCIE_D2H_SYNC */

		DHD_INFO(("msgtype %d, msglen is %d, pktlen is %d \n",
			msgtype, msglen, pktlen));
		if (msgtype == MSG_TYPE_LOOPBACK) {
			bcm_print_bytes("LPBK RESP: ", (uint8 *)msg, msglen);
			DHD_ERROR((" MSG_TYPE_LOOPBACK, len %d\n", msglen));
		}


		if (msgtype >= DHD_PROT_FUNCS) {
			DHD_ERROR(("%s: msgtype %d, msglen is %d, pktlen is %d \n",
				__FUNCTION__, msgtype, msglen, pktlen));
			ret = BCME_ERROR;
			goto done;
		}

		if (table_lookup[msgtype]) {
			table_lookup[msgtype](dhd, buf, msglen);
		}

		if (pktlen < msglen) {
			ret = BCME_ERROR;
			goto done;
		}
		pktlen = pktlen - msglen;
		buf = buf + msglen;

		if (ring->idx == BCMPCIE_D2H_MSGRING_RX_COMPLETE) {
			prot_early_upd_rxcpln_read_idx(dhd, ring);
		}
	}
done:

#if defined(PCIE_D2H_SYNC_BZERO)
	OSL_CACHE_FLUSH(buf_head, len - pktlen); /* Flush the bzeroed msg */
#endif /* PCIE_D2H_SYNC_BZERO */

#ifdef DHD_RX_CHAINING
	dhd_rxchain_commit(dhd);
#endif

	return ret;
}

static void
dhd_prot_noop(dhd_pub_t *dhd, void * buf, uint16 msglen)
{
	return;
}

static void
dhd_prot_ringstatus_process(dhd_pub_t *dhd, void * buf, uint16 msglen)
{
	pcie_ring_status_t * ring_status = (pcie_ring_status_t *)buf;
	DHD_ERROR(("ring status: request_id %d, status 0x%04x, flow ring %d, w_offset %d \n",
		ring_status->cmn_hdr.request_id, ring_status->compl_hdr.status,
		ring_status->compl_hdr.flow_ring_id, ring_status->write_idx));
	/* How do we track this to pair it with ??? */
	return;
}

static void
dhd_prot_genstatus_process(dhd_pub_t *dhd, void * buf, uint16 msglen)
{
	pcie_gen_status_t * gen_status = (pcie_gen_status_t *)buf;
	DHD_ERROR(("gen status: request_id %d, status 0x%04x, flow ring %d \n",
		gen_status->cmn_hdr.request_id, gen_status->compl_hdr.status,
		gen_status->compl_hdr.flow_ring_id));

	/* How do we track this to pair it with ??? */
	return;
}

static void
dhd_prot_ioctack_process(dhd_pub_t *dhd, void * buf, uint16 msglen)
{
	ioctl_req_ack_msg_t * ioct_ack = (ioctl_req_ack_msg_t *)buf;
#if defined(DHD_PKTID_AUDIT_RING)
	uint32 pktid;
	pktid = ltoh32(ioct_ack->cmn_hdr.request_id);
	if (pktid != DHD_IOCTL_REQ_PKTID) {
		DHD_PKTID_AUDIT(dhd->prot->pktid_map_handle, pktid,
			DHD_TEST_IS_ALLOC);
	}
#endif /* DHD_PKTID_AUDIT_RING */

	DHD_CTL(("ioctl req ack: request_id %d, status 0x%04x, flow ring %d \n",
		ioct_ack->cmn_hdr.request_id, ioct_ack->compl_hdr.status,
		ioct_ack->compl_hdr.flow_ring_id));
	if (ioct_ack->compl_hdr.status != 0)  {
		DHD_ERROR(("got an error status for the ioctl request...need to handle that\n"));
	}

#if defined(PCIE_D2H_SYNC_BZERO)
	memset(buf, 0, msglen);
#endif /* PCIE_D2H_SYNC_BZERO */
}

static void
dhd_prot_ioctcmplt_process(dhd_pub_t *dhd, void * buf, uint16 msglen)
{
	uint16 status;
	uint32 resp_len = 0;
	uint32 pkt_id, xt_id;
	ioctl_comp_resp_msg_t * ioct_resp = (ioctl_comp_resp_msg_t *)buf;

	resp_len = ltoh16(ioct_resp->resp_len);
	xt_id = ltoh16(ioct_resp->trans_id);
	pkt_id = ltoh32(ioct_resp->cmn_hdr.request_id);
	status = ioct_resp->compl_hdr.status;

#if defined(PCIE_D2H_SYNC_BZERO)
	memset(buf, 0, msglen);
#endif /* PCIE_D2H_SYNC_BZERO */

#if defined(DHD_PKTID_AUDIT_RING)
	/* will be freed later */
	DHD_PKTID_AUDIT(dhd->prot->pktid_map_handle, pkt_id,
		DHD_TEST_IS_ALLOC);
#endif /* DHD_PKTID_AUDIT_RING */

	DHD_CTL(("IOCTL_COMPLETE: pktid %x xtid %d status %x resplen %d\n",
		pkt_id, xt_id, status, resp_len));

	dhd_bus_update_retlen(dhd->bus, sizeof(ioctl_comp_resp_msg_t), pkt_id, status, resp_len);
	dhd_os_ioctl_resp_wake(dhd);
}

static void BCMFASTPATH
dhd_prot_txstatus_process(dhd_pub_t *dhd, void * buf, uint16 msglen)
{
	dhd_prot_t *prot = dhd->prot;
	host_txbuf_cmpl_t * txstatus;
	unsigned long flags;
	uint32 pktid;
	void *pkt;

	/* locks required to protect circular buffer accesses */
	DHD_GENERAL_LOCK(dhd, flags);

	txstatus = (host_txbuf_cmpl_t *)buf;
	pktid = ltoh32(txstatus->cmn_hdr.request_id);
#if defined(DHD_PKTID_AUDIT_RING)
	DHD_PKTID_AUDIT(dhd->prot->pktid_map_handle, pktid,
		DHD_DUPLICATE_FREE);
#endif /* DHD_PKTID_AUDIT_RING */

	DHD_INFO(("txstatus for pktid 0x%04x\n", pktid));
	if (prot->active_tx_count) {
		prot->active_tx_count--;

		/* Release the Lock when no more tx packets are pending */
		if (prot->active_tx_count == 0)
			 DHD_TXFL_WAKE_UNLOCK(dhd);

	} else {
		DHD_ERROR(("Extra packets are freed\n"));
	}

	ASSERT(pktid != 0);
	pkt = dhd_prot_packet_get(dhd, pktid, BUFF_TYPE_DATA_TX);
	if (pkt) {
#if defined(BCMPCIE)
		dhd_txcomplete(dhd, pkt, true);
#endif 

#if DHD_DBG_SHOW_METADATA
		if (dhd->prot->tx_metadata_offset && txstatus->metadata_len) {
			uchar *ptr;
			/* The Ethernet header of TX frame was copied and removed.
			 * Here, move the data pointer forward by Ethernet header size.
			 */
			PKTPULL(dhd->osh, pkt, ETHER_HDR_LEN);
			ptr = PKTDATA(dhd->osh, pkt)  - (dhd->prot->tx_metadata_offset);
			bcm_print_bytes("txmetadata", ptr, txstatus->metadata_len);
			dhd_prot_print_metadata(dhd, ptr, txstatus->metadata_len);
		}
#endif /* DHD_DBG_SHOW_METADATA */
		PKTFREE(dhd->osh, pkt, TRUE);
	}

#if defined(PCIE_D2H_SYNC_BZERO)
	memset(buf, 0, msglen);
#endif /* PCIE_D2H_SYNC_BZERO */

	DHD_GENERAL_UNLOCK(dhd, flags);

	return;
}

static void
dhd_prot_event_process(dhd_pub_t *dhd, void* buf, uint16 len)
{
	wlevent_req_msg_t *evnt;
	uint32 pktid;
	uint16 buflen;
	int ifidx = 0;
	void* pkt;
	unsigned long flags;
	dhd_prot_t *prot = dhd->prot;
	int post_cnt = 0;
	bool zero_posted = FALSE;

	/* Event complete header */
	evnt = (wlevent_req_msg_t *)buf;
	pktid = ltoh32(evnt->cmn_hdr.request_id);

#if defined(DHD_PKTID_AUDIT_RING)
	DHD_PKTID_AUDIT(dhd->prot->pktid_map_handle, pktid,
		DHD_DUPLICATE_FREE);
#endif /* DHD_PKTID_AUDIT_RING */

	buflen = ltoh16(evnt->event_data_len);

	ifidx = BCMMSGBUF_API_IFIDX(&evnt->cmn_hdr);

	/* Post another rxbuf to the device */
	if (prot->cur_event_bufs_posted)
		prot->cur_event_bufs_posted--;
	else
		zero_posted = TRUE;


	post_cnt = dhd_msgbuf_rxbuf_post_event_bufs(dhd);
	if (zero_posted && (post_cnt <= 0)) {
		return;
	}

#if defined(PCIE_D2H_SYNC_BZERO)
	memset(buf, 0, len);
#endif /* PCIE_D2H_SYNC_BZERO */

	/* locks required to protect pktid_map */
	DHD_GENERAL_LOCK(dhd, flags);
	pkt = dhd_prot_packet_get(dhd, pktid, BUFF_TYPE_EVENT_RX);

	DHD_GENERAL_UNLOCK(dhd, flags);

	if (!pkt) {
		DHD_ERROR(("%s: pkt is NULL\n", __FUNCTION__));
		return;
	}

	/* DMA RX offset updated through shared area */
	if (dhd->prot->rx_dataoffset)
		PKTPULL(dhd->osh, pkt, dhd->prot->rx_dataoffset);

	PKTSETLEN(dhd->osh, pkt, buflen);

	dhd_bus_rx_frame(dhd->bus, pkt, ifidx, 1);
}

static void BCMFASTPATH
dhd_prot_rxcmplt_process(dhd_pub_t *dhd, void* buf, uint16 msglen)
{
	host_rxbuf_cmpl_t *rxcmplt_h;
	uint16 data_offset;             /* offset at which data starts */
	void * pkt;
	unsigned long flags;
	static uint8 current_phase = 0;
	uint ifidx;
	uint32 pktid;

	/* RXCMPLT HDR */
	rxcmplt_h = (host_rxbuf_cmpl_t *)buf;

	/* Post another set of rxbufs to the device */
	dhd_prot_return_rxbuf(dhd, 1);

	pktid = ltoh32(rxcmplt_h->cmn_hdr.request_id);

#if defined(DHD_PKTID_AUDIT_RING)
	DHD_PKTID_AUDIT(dhd->prot->pktid_map_handle, pktid,
		DHD_DUPLICATE_FREE);
#endif /* DHD_PKTID_AUDIT_RING */

	/* offset from which data starts is populated in rxstatus0 */
	data_offset = ltoh16(rxcmplt_h->data_offset);

	DHD_GENERAL_LOCK(dhd, flags);
	pkt = dhd_prot_packet_get(dhd, pktid, BUFF_TYPE_DATA_RX);
	DHD_GENERAL_UNLOCK(dhd, flags);

	if (!pkt) {
		return;
	}

	DHD_INFO(("id 0x%04x, offset %d, len %d, idx %d, phase 0x%02x, pktdata %p, metalen %d\n",
		pktid, data_offset, ltoh16(rxcmplt_h->data_len),
		rxcmplt_h->cmn_hdr.if_id, rxcmplt_h->cmn_hdr.flags, PKTDATA(dhd->osh, pkt),
		ltoh16(rxcmplt_h->metadata_len)));

#if DHD_DBG_SHOW_METADATA
	if (dhd->prot->rx_metadata_offset && rxcmplt_h->metadata_len) {
		uchar *ptr;
		ptr = PKTDATA(dhd->osh, pkt) - (dhd->prot->rx_metadata_offset);
		/* header followed by data */
		bcm_print_bytes("rxmetadata", ptr, rxcmplt_h->metadata_len);
		dhd_prot_print_metadata(dhd, ptr, rxcmplt_h->metadata_len);
	}
#endif /* DHD_DBG_SHOW_METADATA */

	if (current_phase !=  rxcmplt_h->cmn_hdr.flags) {
		current_phase = rxcmplt_h->cmn_hdr.flags;
	}
	if (rxcmplt_h->flags & BCMPCIE_PKT_FLAGS_FRAME_802_11)
		DHD_INFO(("D11 frame rxed \n"));
	/* data_offset from buf start */
	if (data_offset) {
		/* data offset given from dongle after split rx */
		PKTPULL(dhd->osh, pkt, data_offset); /* data offset */
	} else {
		/* DMA RX offset updated through shared area */
		if (dhd->prot->rx_dataoffset)
			PKTPULL(dhd->osh, pkt, dhd->prot->rx_dataoffset);
	}
	/* Actual length of the packet */
	PKTSETLEN(dhd->osh, pkt, ltoh16(rxcmplt_h->data_len));

	ifidx = rxcmplt_h->cmn_hdr.if_id;

#if defined(PCIE_D2H_SYNC_BZERO)
	memset(buf, 0, msglen);
#endif /* PCIE_D2H_SYNC_BZERO */

#ifdef DHD_RX_CHAINING
	/* Chain the packets */
	dhd_rxchain_frame(dhd, pkt, ifidx);
#else /* ! DHD_RX_CHAINING */
	/* offset from which data starts is populated in rxstatus0 */
	dhd_bus_rx_frame(dhd->bus, pkt, ifidx, 1);
#endif /* ! DHD_RX_CHAINING */

}

/* Stop protocol: sync w/dongle state. */
void dhd_prot_stop(dhd_pub_t *dhd)
{
	/* nothing to do for pcie */
}

/* Add any protocol-specific data header.
 * Caller must reserve prot_hdrlen prepend space.
 */
void BCMFASTPATH
dhd_prot_hdrpush(dhd_pub_t *dhd, int ifidx, void *PKTBUF)
{
	return;
}

uint
dhd_prot_hdrlen(dhd_pub_t *dhd, void *PKTBUF)
{
	return 0;
}


#define PKTBUF pktbuf

int BCMFASTPATH
dhd_prot_txdata(dhd_pub_t *dhd, void *PKTBUF, uint8 ifidx)
{
	unsigned long flags;
	dhd_prot_t *prot = dhd->prot;
	dhd_pktid_map_handle_t *map;
	host_txbuf_post_t *txdesc = NULL;
	dmaaddr_t physaddr, meta_physaddr;
	uint8 *pktdata;
	uint32 pktlen;
	uint32 pktid;
	uint8	prio;
	uint16 flowid = 0;
	uint16 alloced = 0;
	uint16	headroom;

	msgbuf_ring_t *msg_ring;

	if (!dhd->flow_ring_table) {
		return BCME_NORESOURCE;
	}

	map = dhd->prot->pktid_map_handle;

	if (!dhd_bus_is_txmode_push(dhd->bus)) {
		flow_ring_table_t *flow_ring_table;
		flow_ring_node_t *flow_ring_node;

		flowid = (uint16)DHD_PKTTAG_FLOWID((dhd_pkttag_fr_t*)PKTTAG(PKTBUF));

		flow_ring_table = (flow_ring_table_t *)dhd->flow_ring_table;
		flow_ring_node = (flow_ring_node_t *)&flow_ring_table[flowid];

		msg_ring = (msgbuf_ring_t *)flow_ring_node->prot_info;
	} else {
		msg_ring = prot->h2dring_txp_subn;
	}



	DHD_GENERAL_LOCK(dhd, flags);

	/* Create a unique 32-bit packet id */
	pktid = NATIVE_TO_PKTID_RSV(map, PKTBUF, BUFF_TYPE_DATA_TX);
	if (pktid == DHD_PKTID_INVALID) {
		DHD_ERROR(("Pktid pool depleted.\n"));
		/*
		 * If we return error here, the caller would queue the packet
		 * again. So we'll just free the skb allocated in DMA Zone.
		 * Since we have not freed the original SKB yet the caller would
		 * requeue the same.
		 */
		goto err_no_res_pktfree;
	}

	/* Reserve space in the circular buffer */
	txdesc = (host_txbuf_post_t *)dhd_alloc_ring_space(dhd,
		msg_ring, DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D, &alloced);
	if (txdesc == NULL) {
		DHD_INFO(("%s:%d: HTOD Msgbuf Not available TxCount = %d\n",
			__FUNCTION__, __LINE__, prot->active_tx_count));
		goto err_free_pktid;
	}

	/* Extract the data pointer and length information */
	pktdata = PKTDATA(dhd->osh, PKTBUF);
	pktlen  = PKTLEN(dhd->osh, PKTBUF);

	/* Ethernet header: Copy before we cache flush packet using DMA_MAP */
	bcopy(pktdata, txdesc->txhdr, ETHER_HDR_LEN);

	/* Extract the ethernet header and adjust the data pointer and length */
	pktdata = PKTPULL(dhd->osh, PKTBUF, ETHER_HDR_LEN);
	pktlen -= ETHER_HDR_LEN;

	/* Map the data pointer to a DMA-able address */
	physaddr = DMA_MAP(dhd->osh, PKTDATA(dhd->osh, PKTBUF), pktlen, DMA_TX, PKTBUF, 0);
	if (PHYSADDRISZERO(physaddr)) {
		DHD_ERROR(("%s: Something really bad, unless 0 is"
			" a valid physaddr\n", __FUNCTION__));
		ASSERT(0);
		goto err_rollback_idx;
	}

	/* No need to lock. Save the rest of the packet's metadata */
	NATIVE_TO_PKTID_SAVE(map, PKTBUF, pktid, physaddr, pktlen,
		DMA_TX, BUFF_TYPE_DATA_TX);

#ifdef TXP_FLUSH_NITEMS
	if (msg_ring->pend_items_count == 0)
		msg_ring->start_addr = (void *)txdesc;
	msg_ring->pend_items_count++;
#endif

	/* Form the Tx descriptor message buffer */

	/* Common message hdr */
	txdesc->cmn_hdr.msg_type = MSG_TYPE_TX_POST;
	txdesc->cmn_hdr.if_id = ifidx;
	txdesc->flags = BCMPCIE_PKT_FLAGS_FRAME_802_3;
	prio = (uint8)PKTPRIO(PKTBUF);


	txdesc->flags |= (prio & 0x7) << BCMPCIE_PKT_FLAGS_PRIO_SHIFT;
	txdesc->seg_cnt = 1;

	txdesc->data_len = htol16((uint16)pktlen);
	txdesc->data_buf_addr.high_addr = htol32(PHYSADDRHI(physaddr));
	txdesc->data_buf_addr.low_addr  = htol32(PHYSADDRLO(physaddr));

	/* Move data pointer to keep ether header in local PKTBUF for later reference */
	PKTPUSH(dhd->osh, PKTBUF, ETHER_HDR_LEN);

	/* Handle Tx metadata */
	headroom = (uint16)PKTHEADROOM(dhd->osh, PKTBUF);
	if (prot->tx_metadata_offset && (headroom < prot->tx_metadata_offset))
		DHD_ERROR(("No headroom for Metadata tx %d %d\n",
		prot->tx_metadata_offset, headroom));

	if (prot->tx_metadata_offset && (headroom >= prot->tx_metadata_offset)) {
		DHD_TRACE(("Metadata in tx %d\n", prot->tx_metadata_offset));

		/* Adjust the data pointer to account for meta data in DMA_MAP */
		PKTPUSH(dhd->osh, PKTBUF, prot->tx_metadata_offset);
		meta_physaddr = DMA_MAP(dhd->osh, PKTDATA(dhd->osh, PKTBUF),
			prot->tx_metadata_offset, DMA_RX, PKTBUF, 0);
		if (PHYSADDRISZERO(meta_physaddr)) {
			/* Unmap the data pointer to a DMA-able address */
			DMA_UNMAP(dhd->osh, meta_physaddr, prot->tx_metadata_offset, DMA_RX, 0, 0);
			DHD_ERROR(("%s: Something really bad, unless 0 is"
				" a valid meta_physaddr\n", __FUNCTION__));
			ASSERT(0);
			goto err_rollback_idx;
		}

		/* Adjust the data pointer back to original value */
		PKTPULL(dhd->osh, PKTBUF, prot->tx_metadata_offset);

		txdesc->metadata_buf_len = prot->tx_metadata_offset;
		txdesc->metadata_buf_addr.high_addr = htol32(PHYSADDRHI(meta_physaddr));
		txdesc->metadata_buf_addr.low_addr = htol32(PHYSADDRLO(meta_physaddr));
	}
	else {
		txdesc->metadata_buf_len = htol16(0);
		txdesc->metadata_buf_addr.high_addr = 0;
		txdesc->metadata_buf_addr.low_addr = 0;
	}

#if defined(DHD_PKTID_AUDIT_RING)
	DHD_PKTID_AUDIT(prot->pktid_map_handle, pktid,
		DHD_DUPLICATE_ALLOC);
#endif /* DHD_PKTID_AUDIT_RING */

	txdesc->cmn_hdr.request_id = htol32(pktid);

	DHD_TRACE(("txpost: data_len %d, pktid 0x%04x\n", txdesc->data_len,
		txdesc->cmn_hdr.request_id));

	/* Update the write pointer in TCM & ring bell */
#ifdef TXP_FLUSH_NITEMS
	/* Flush if we have either hit the txp_threshold or if this msg is */
	/* occupying the last slot in the flow_ring - before wrap around.  */
	if ((msg_ring->pend_items_count == prot->txp_threshold) ||
		((uint8 *) txdesc == (uint8 *) HOST_RING_END(msg_ring))) {
		dhd_prot_txdata_write_flush(dhd, flowid, TRUE);
	}
#else
	prot_ring_write_complete(dhd, msg_ring, txdesc, DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D);
#endif

	prot->active_tx_count++;

	/*
	 * Take a wake lock, do not sleep if we have atleast one packet
	 * to finish.
	 */
	if (prot->active_tx_count == 1)
		DHD_TXFL_WAKE_LOCK(dhd);

	DHD_GENERAL_UNLOCK(dhd, flags);

	return BCME_OK;

err_rollback_idx:
	/* roll back write pointer for unprocessed message */
	if (RING_WRITE_PTR(msg_ring) == 0) {
		RING_WRITE_PTR(msg_ring) = RING_MAX_ITEM(msg_ring) - 1;
	} else {
		RING_WRITE_PTR(msg_ring) -= 1;
	}

err_free_pktid:
	/* Free up the PKTID */
	PKTID_TO_NATIVE(dhd->prot->pktid_map_handle, pktid, physaddr,
		pktlen, BUFF_TYPE_NO_CHECK);

err_no_res_pktfree:



	DHD_GENERAL_UNLOCK(dhd, flags);
	return BCME_NORESOURCE;

}

/* called with a lock */
void BCMFASTPATH
dhd_prot_txdata_write_flush(dhd_pub_t *dhd, uint16 flowid, bool in_lock)
{
#ifdef TXP_FLUSH_NITEMS
	unsigned long flags = 0;
	flow_ring_table_t *flow_ring_table;
	flow_ring_node_t *flow_ring_node;
	msgbuf_ring_t *msg_ring;

	if (!dhd->flow_ring_table)
		return;

	if (!in_lock) {
		DHD_GENERAL_LOCK(dhd, flags);
	}

	flow_ring_table = (flow_ring_table_t *)dhd->flow_ring_table;
	flow_ring_node = (flow_ring_node_t *)&flow_ring_table[flowid];
	msg_ring = (msgbuf_ring_t *)flow_ring_node->prot_info;

	/* Update the write pointer in TCM & ring bell */
	if (msg_ring->pend_items_count) {
		prot_ring_write_complete(dhd, msg_ring, msg_ring->start_addr,
			msg_ring->pend_items_count);
		msg_ring->pend_items_count = 0;
		msg_ring->start_addr = NULL;
	}

	if (!in_lock) {
		DHD_GENERAL_UNLOCK(dhd, flags);
	}
#endif /* TXP_FLUSH_NITEMS */
}

#undef PKTBUF	/* Only defined in the above routine */
int BCMFASTPATH
dhd_prot_hdrpull(dhd_pub_t *dhd, int *ifidx, void *pkt, uchar *buf, uint *len)
{
	return 0;
}

static void BCMFASTPATH
dhd_prot_return_rxbuf(dhd_pub_t *dhd, uint16 rxcnt)
{
	dhd_prot_t *prot = dhd->prot;

	if (prot->rxbufpost >= rxcnt) {
		prot->rxbufpost -= rxcnt;
	} else {
		/* ASSERT(0); */
		prot->rxbufpost = 0;
	}

	if (prot->rxbufpost <= (prot->max_rxbufpost - RXBUFPOST_THRESHOLD))
		dhd_msgbuf_rxbuf_post(dhd);

	return;
}


#if defined(CUSTOMER_HW4) && defined(CONFIG_CONTROL_PM)
extern bool g_pm_control;
#endif /* CUSTOMER_HW4 & CONFIG_CONTROL_PM */

/* Use protocol to issue ioctl to dongle */
int dhd_prot_ioctl(dhd_pub_t *dhd, int ifidx, wl_ioctl_t * ioc, void * buf, int len)
{
	dhd_prot_t *prot = dhd->prot;
	int ret = -1;
	uint8 action;

	if ((dhd->busstate == DHD_BUS_DOWN) || dhd->hang_was_sent) {
		DHD_ERROR(("%s : bus is down. we have nothing to do\n", __FUNCTION__));
		goto done;
	}

#ifdef DHD_USE_IDLECOUNT
	bus_wake(dhd->bus);
#endif /* DHD_USE_IDLECOUNT */

	if (dhd->busstate == DHD_BUS_SUSPEND) {
		DHD_ERROR(("%s : bus is suspended\n", __FUNCTION__));
		goto done;
	}

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));
#ifdef CUSTOMER_HW4
	if (ioc->cmd == WLC_SET_PM) {
#ifdef CONFIG_CONTROL_PM
		if (g_pm_control == TRUE) {
			DHD_ERROR(("%s: SET PM ignored!(Requested:%d)\n",
				__FUNCTION__, *(char *)buf));
			goto done;
		}
#endif /* CONFIG_CONTROL_PM */
		DHD_INFO(("%s: SET PM to %d\n", __FUNCTION__, *(char *)buf));
	}
#endif /* CUSTOMER_HW4 */

	ASSERT(len <= WLC_IOCTL_MAXLEN);

	if (len > WLC_IOCTL_MAXLEN)
		goto done;

	/* All sanity done ... we either go ahead and run the IOCTL or
	 * wait for our turn based on the availability of the mutex.
	 */
	mutex_lock(&prot->ioctl_mutex);

	if (prot->pending == TRUE) {
		DHD_ERROR(("packet is pending!!!! cmd=0x%x (%lu) lastcmd=0x%x (%lu)\n",
			ioc->cmd, (unsigned long)ioc->cmd, prot->lastcmd,
			(unsigned long)prot->lastcmd));
		if ((ioc->cmd == WLC_SET_VAR) || (ioc->cmd == WLC_GET_VAR)) {
			DHD_TRACE(("iovar cmd=%s\n", (char*)buf));
		}
		/* With mutex in place we should never see this. That is
		 * we make prot->pending = TRUE only after grabbing the
		 * mutex, and release the mutex only after making pending
		 * as FALSE. So there won't be a scenario where we get
		 * the mutex and find prot->pending as TRUE. Neverthless
		 * handling this scenario.
		 */
		mutex_unlock(&prot->ioctl_mutex);
		goto done;
	}

	prot->pending = TRUE;
	prot->lastcmd = ioc->cmd;
	action = ioc->set;


	if (action & WL_IOCTL_ACTION_SET) {
		ret = dhd_msgbuf_set_ioctl(dhd, ifidx, ioc->cmd, buf, len, action);
	} else {
		ret = dhdmsgbuf_query_ioctl(dhd, ifidx, ioc->cmd, buf, len, action);
		if (ret > 0)
			ioc->used = ret;
	}
	/* Too many programs assume ioctl() returns 0 on success */
	if (ret >= 0) {
		ret = 0;
	} else {
		dhd->dongle_error = ret;
	}

	/* Intercept the wme_dp ioctl here */
	if ((!ret) && (ioc->cmd == WLC_SET_VAR) && (!strcmp(buf, "wme_dp"))) {
		int slen, val = 0;

		slen = strlen("wme_dp") + 1;
		if (len >= (int)(slen + sizeof(int)))
			bcopy(((char *)buf + slen), &val, sizeof(int));
		dhd->wme_dp = (uint8) ltoh32(val);
	}


	prot->pending = FALSE;

	/* Release the lock if there are other contexts waiting to fire an
	 * IOCTL, they'll grab this lock and proceed.
	 */
	mutex_unlock(&prot->ioctl_mutex);
done:
	return ret;

}

int
dhdmsgbuf_lpbk_req(dhd_pub_t *dhd, uint len)
{
	unsigned long flags;
	dhd_prot_t *prot = dhd->prot;
	uint16 alloced = 0;

	ioct_reqst_hdr_t *ioct_rqst;

	uint16 hdrlen = sizeof(ioct_reqst_hdr_t);
	uint16 msglen = len + hdrlen;


	if (msglen  > MSGBUF_MAX_MSG_SIZE)
		msglen = MSGBUF_MAX_MSG_SIZE;

	msglen = align(msglen, DMA_ALIGN_LEN);

	DHD_GENERAL_LOCK(dhd, flags);
	ioct_rqst = (ioct_reqst_hdr_t *)dhd_alloc_ring_space(dhd,
		prot->h2dring_ctrl_subn, DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D, &alloced);

	if (ioct_rqst == NULL) {
		DHD_GENERAL_UNLOCK(dhd, flags);
		return 0;
	}

	{
		uint8 *ptr;
		uint16 i;

		ptr = (uint8 *)ioct_rqst;
		for (i = 0; i < msglen; i++) {
			ptr[i] = i % 256;
		}
	}


	/* Common msg buf hdr */
	ioct_rqst->msg.msg_type = MSG_TYPE_LOOPBACK;
	ioct_rqst->msg.if_id = 0;

	bcm_print_bytes("LPBK REQ: ", (uint8 *)ioct_rqst, msglen);

	/* Update the write pointer in TCM & ring bell */
	prot_ring_write_complete(dhd, prot->h2dring_ctrl_subn, ioct_rqst,
		DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D);
	DHD_GENERAL_UNLOCK(dhd, flags);

	return 0;
}

void dmaxfer_free_dmaaddr(dhd_pub_t *dhd, dhd_dmaxfer_t *dma)
{
	if (dma == NULL)
		return;

	if (dma->srcmem.va) {
		DMA_FREE_CONSISTENT(dhd->osh, dma->srcmem.va,
			dma->len, dma->srcmem.pa, dma->srcmem.dmah);
		dma->srcmem.va = NULL;
	}
	if (dma->destmem.va) {
		DMA_FREE_CONSISTENT(dhd->osh, dma->destmem.va,
			dma->len + 8, dma->destmem.pa, dma->destmem.dmah);
		dma->destmem.va = NULL;
	}
}

int dmaxfer_prepare_dmaaddr(dhd_pub_t *dhd, uint len,
	uint srcdelay, uint destdelay, dhd_dmaxfer_t *dma)
{
	uint i;

	if (!dma)
		return BCME_ERROR;

	/* First free up exisiting buffers */
	dmaxfer_free_dmaaddr(dhd, dma);

	dma->srcmem.va = DMA_ALLOC_CONSISTENT(dhd->osh, len, DMA_ALIGN_LEN,
	&i, &dma->srcmem.pa, &dma->srcmem.dmah);
	if (dma->srcmem.va ==  NULL) {
		return BCME_NOMEM;
	}

	/* Populate source with a pattern */
	for (i = 0; i < len; i++) {
		((uint8*)dma->srcmem.va)[i] = i % 256;
	}
	OSL_CACHE_FLUSH(dma->srcmem.va, len);

	dma->destmem.va = DMA_ALLOC_CONSISTENT(dhd->osh, len + 8, DMA_ALIGN_LEN,
	&i, &dma->destmem.pa, &dma->destmem.dmah);
	if (dma->destmem.va ==  NULL) {
		DMA_FREE_CONSISTENT(dhd->osh, dma->srcmem.va,
			dma->len, dma->srcmem.pa, dma->srcmem.dmah);
		dma->srcmem.va = NULL;
		return BCME_NOMEM;
	}


	/* Clear the destination buffer */
	bzero(dma->destmem.va, len +8);
	OSL_CACHE_FLUSH(dma->destmem.va, len+8);

	dma->len = len;
	dma->srcdelay = srcdelay;
	dma->destdelay = destdelay;

	return BCME_OK;
}

static void
dhdmsgbuf_dmaxfer_compare(dhd_pub_t *dhd, void * buf, uint16 msglen)
{
	dhd_prot_t *prot = dhd->prot;

	OSL_CACHE_INV(prot->dmaxfer.destmem.va, prot->dmaxfer.len);
	if (prot->dmaxfer.srcmem.va && prot->dmaxfer.destmem.va) {
		if (memcmp(prot->dmaxfer.srcmem.va,
			prot->dmaxfer.destmem.va,
			prot->dmaxfer.len)) {
			bcm_print_bytes("XFER SRC: ",
				prot->dmaxfer.srcmem.va, prot->dmaxfer.len);
			bcm_print_bytes("XFER DEST: ",
				prot->dmaxfer.destmem.va, prot->dmaxfer.len);
		}
		else {
			DHD_INFO(("DMA successful\n"));
		}
	}
	dmaxfer_free_dmaaddr(dhd, &prot->dmaxfer);
	dhd->prot->dmaxfer_in_progress = FALSE;
}

int
dhdmsgbuf_dmaxfer_req(dhd_pub_t *dhd, uint len, uint srcdelay, uint destdelay)
{
	unsigned long flags;
	int ret = BCME_OK;
	dhd_prot_t *prot = dhd->prot;
	pcie_dma_xfer_params_t *dmap;
	uint32 xferlen = len > DMA_XFER_LEN_LIMIT ? DMA_XFER_LEN_LIMIT : len;
	uint16 msglen = sizeof(pcie_dma_xfer_params_t);
	uint16 alloced = 0;

	if (prot->dmaxfer_in_progress) {
		DHD_ERROR(("DMA is in progress...\n"));
		return ret;
	}
	prot->dmaxfer_in_progress = TRUE;
	if ((ret = dmaxfer_prepare_dmaaddr(dhd, xferlen, srcdelay, destdelay,
		&prot->dmaxfer)) != BCME_OK) {
		prot->dmaxfer_in_progress = FALSE;
		return ret;
	}


	if (msglen  > MSGBUF_MAX_MSG_SIZE)
		msglen = MSGBUF_MAX_MSG_SIZE;

	msglen = align(msglen, DMA_ALIGN_LEN);

	DHD_GENERAL_LOCK(dhd, flags);
	dmap = (pcie_dma_xfer_params_t *)dhd_alloc_ring_space(dhd,
		prot->h2dring_ctrl_subn, DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D, &alloced);

	if (dmap == NULL) {
		dmaxfer_free_dmaaddr(dhd, &prot->dmaxfer);
		prot->dmaxfer_in_progress = FALSE;
		DHD_GENERAL_UNLOCK(dhd, flags);
		return BCME_NOMEM;
	}

	/* Common msg buf hdr */
	dmap->cmn_hdr.msg_type = MSG_TYPE_LPBK_DMAXFER;
	dmap->cmn_hdr.request_id = 0x1234;

	dmap->host_input_buf_addr.high = htol32(PHYSADDRHI(prot->dmaxfer.srcmem.pa));
	dmap->host_input_buf_addr.low = htol32(PHYSADDRLO(prot->dmaxfer.srcmem.pa));
	dmap->host_ouput_buf_addr.high = htol32(PHYSADDRHI(prot->dmaxfer.destmem.pa));
	dmap->host_ouput_buf_addr.low = htol32(PHYSADDRLO(prot->dmaxfer.destmem.pa));
	dmap->xfer_len = htol32(prot->dmaxfer.len);
	dmap->srcdelay = htol32(prot->dmaxfer.srcdelay);
	dmap->destdelay = htol32(prot->dmaxfer.destdelay);

	/* Update the write pointer in TCM & ring bell */
	prot_ring_write_complete(dhd, prot->h2dring_ctrl_subn, dmap,
		DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D);
	DHD_GENERAL_UNLOCK(dhd, flags);

	DHD_ERROR(("DMA Started...\n"));

	return BCME_OK;
}

static int
dhdmsgbuf_query_ioctl(dhd_pub_t *dhd, int ifidx, uint cmd, void *buf, uint len, uint8 action)
{
	dhd_prot_t *prot = dhd->prot;

	int ret = 0;
	uint copylen = 0;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (cmd == WLC_GET_VAR && buf)
	{
		if (!len || !*(uint8 *)buf) {
			DHD_ERROR(("%s(): Zero length bailing\n", __FUNCTION__));
			ret = BCME_BADARG;
			goto done;
		}

		/* Respond "bcmerror" and "bcmerrorstr" with local cache */
		copylen = MIN(len, BCME_STRLEN);

		if ((len >= strlen("bcmerrorstr")) &&
			(!strcmp((char *)buf, "bcmerrorstr"))) {

			strncpy((char *)buf, bcmerrorstr(dhd->dongle_error), copylen);
			*(uint8 *)((uint8 *)buf + (copylen - 1)) = '\0';

			goto done;
		} else if ((len >= strlen("bcmerror")) &&
			!strcmp((char *)buf, "bcmerror")) {

			*(uint32 *)(uint32 *)buf = dhd->dongle_error;

			goto done;
		}
	}

	ret = dhd_fillup_ioct_reqst_ptrbased(dhd, (uint16)len, cmd, buf, ifidx);
	if (ret < 0) {
		DHD_ERROR(("%s : dhd_fillup_ioct_reqst_ptrbased error : %d\n", __FUNCTION__, ret));
		return ret;
	}

	DHD_INFO(("ACTION %d ifdix %d cmd %d len %d \n",
		action, ifidx, cmd, len));

	/* wait for interrupt and get first fragment */
	ret = dhdmsgbuf_cmplt(dhd, prot->reqid, len, buf, prot->retbuf.va);

done:
	return ret;
}

static int
dhdmsgbuf_cmplt(dhd_pub_t *dhd, uint32 id, uint32 len, void* buf, void* retbuf)
{
	dhd_prot_t *prot = dhd->prot;
	ioctl_comp_resp_msg_t  ioct_resp;
	void* pkt = NULL;
	int retlen;
	int msgbuf_len = 0;
	int post_cnt = 0;
	unsigned long flags;
	bool zero_posted = FALSE;
	uint32 pktid;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));
	if (dhd->busstate == DHD_BUS_DOWN) {
		DHD_ERROR(("%s: bus is already down.\n", __FUNCTION__));
		return -1;
	}

	if (prot->cur_ioctlresp_bufs_posted)
		prot->cur_ioctlresp_bufs_posted--;
	else
		zero_posted = TRUE;

	post_cnt = dhd_msgbuf_rxbuf_post_ioctlresp_bufs(dhd);
	if (zero_posted && (post_cnt <= 0)) {
		return -1;
	}

	memset(&ioct_resp, 0, sizeof(ioctl_comp_resp_msg_t));

	retlen = dhd_bus_rxctl(dhd->bus, (uchar*)&ioct_resp, msgbuf_len);
	if (retlen <= 0) {
		DHD_ERROR(("IOCTL request failed with error code %d\n", retlen));
		return retlen;
	}

	pktid = ioct_resp.cmn_hdr.request_id; /* no need for ltoh32 */

#if defined(DHD_PKTID_AUDIT_RING)
	DHD_PKTID_AUDIT(prot->pktid_map_handle, pktid, DHD_DUPLICATE_FREE);
#endif /* DHD_PKTID_AUDIT_RING */

	DHD_INFO(("ioctl resp retlen %d status %d, resp_len %d, pktid %d\n",
		retlen, ioct_resp.compl_hdr.status, ioct_resp.resp_len, pktid));
	if (ioct_resp.resp_len != 0) {
		DHD_GENERAL_LOCK(dhd, flags);
		pkt = dhd_prot_packet_get(dhd, pktid, BUFF_TYPE_IOCTL_RX);
		DHD_GENERAL_UNLOCK(dhd, flags);

		DHD_INFO(("ioctl ret buf %p retlen %d status %x \n", pkt, retlen,
			ioct_resp.compl_hdr.status));
		/* get ret buf */
		if ((buf) && (pkt)) {
			/* bcopy(PKTDATA(dhd->osh, pkt), buf, ioct_resp.resp_len); */
			/* ioct_resp.resp_len could have been changed to make it > 8 bytes */
			bcopy(PKTDATA(dhd->osh, pkt), buf, len);
		}
		if (pkt) {
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_CTRLBUF)
			PKTFREE_STATIC(dhd->osh, pkt, FALSE);
#else
			PKTFREE(dhd->osh, pkt, FALSE);
#endif /* CONFIG_DHD_USE_STATIC_BUF && DHD_USE_STATIC_CTRLBUF */
		}
	} else {
		DHD_GENERAL_LOCK(dhd, flags);
		dhd_prot_packet_free(dhd, pktid, BUFF_TYPE_IOCTL_RX);
		DHD_GENERAL_UNLOCK(dhd, flags);
	}

	return (int)(ioct_resp.compl_hdr.status);
}
static int
dhd_msgbuf_set_ioctl(dhd_pub_t *dhd, int ifidx, uint cmd, void *buf, uint len, uint8 action)
{
	dhd_prot_t *prot = dhd->prot;

	int ret = 0;

	DHD_TRACE(("%s: Enter \n", __FUNCTION__));
	DHD_TRACE(("%s: cmd %d len %d\n", __FUNCTION__, cmd, len));

	if (dhd->busstate == DHD_BUS_DOWN) {
		DHD_ERROR(("%s : bus is down. we have nothing to do\n", __FUNCTION__));
		return -EIO;
	}

	/* don't talk to the dongle if fw is about to be reloaded */
	if (dhd->hang_was_sent) {
		DHD_ERROR(("%s: HANG was sent up earlier. Not talking to the chip\n",
			__FUNCTION__));
		return -EIO;
	}

	/* Fill up msgbuf for ioctl req */
	ret = dhd_fillup_ioct_reqst_ptrbased(dhd, (uint16)len, cmd, buf, ifidx);
	if (ret < 0) {
		DHD_ERROR(("%s : dhd_fillup_ioct_reqst_ptrbased error : %d\n", __FUNCTION__, ret));
		return ret;
	}

	DHD_INFO(("ACTIOn %d ifdix %d cmd %d len %d \n",
		action, ifidx, cmd, len));

	ret = dhdmsgbuf_cmplt(dhd, prot->reqid, len, buf, prot->retbuf.va);

	return ret;
}
/* Handles a protocol control response asynchronously */
int dhd_prot_ctl_complete(dhd_pub_t *dhd)
{
	return 0;
}

/* Check for and handle local prot-specific iovar commands */
int dhd_prot_iovar_op(dhd_pub_t *dhd, const char *name,
                             void *params, int plen, void *arg, int len, bool set)
{
	return BCME_UNSUPPORTED;
}

/* Add prot dump output to a buffer */
void dhd_prot_dump(dhd_pub_t *dhd, struct bcmstrbuf *strbuf)
{
#if defined(PCIE_D2H_SYNC)
	if (dhd->d2h_sync_mode & PCIE_SHARED_D2H_SYNC_SEQNUM)
		bcm_bprintf(strbuf, "\nd2h_sync: SEQNUM:");
	else if (dhd->d2h_sync_mode & PCIE_SHARED_D2H_SYNC_XORCSUM)
		bcm_bprintf(strbuf, "\nd2h_sync: XORCSUM:");
	else
		bcm_bprintf(strbuf, "\nd2h_sync: NONE:");
	bcm_bprintf(strbuf, " d2h_sync_wait max<%lu> tot<%lu>\n",
	            dhd->prot->d2h_sync_wait_max, dhd->prot->d2h_sync_wait_tot);
#endif  /* PCIE_D2H_SYNC */
}

#ifdef DHD_DEBUG_PAGEALLOC
static void
dump_ring(dhd_pub_t *dhd, msgbuf_ring_t *ring)
{
	void* ret_addr = NULL;
	cmn_msg_hdr_t *msg = NULL;
	uint16 msglen;
	uint16 r_ptr, w_ptr, depth;
	uint16 read_cnt, tot;
	int i;
	ioctl_resp_evt_buf_post_msg_t *rxbuf_post;
	host_rxbuf_post_t *rxbuf_data;

	dhd_bus_cmn_readshared(dhd->bus, &r_ptr, RING_READ_PTR, ring->idx);
	dhd_bus_cmn_readshared(dhd->bus, &w_ptr, RING_WRITE_PTR, ring->idx);
	read_cnt = READ_AVAIL_SPACE(w_ptr, r_ptr, RING_MAX_ITEM(ring));

	tot = read_cnt;
	msglen = RING_LEN_ITEMS(ring);
	depth = ring->ringmem->max_item;

	DHD_ERROR(("%s:%s r:%d w:%d depth:%d msglen:%d tot:%d\n",
		__FUNCTION__, ring->name, r_ptr, w_ptr, depth, msglen, tot));
	for (i = 0; i < tot; i++) {
		/* if space available, calculate address to be read */
		ret_addr = (char*)ring->ring_base.va + (r_ptr * ring->ringmem->len_items);

		/* Cache invalidate */
		OSL_CACHE_INV((void *) ret_addr, ring->ringmem->len_items);

		msg = (cmn_msg_hdr_t *)ret_addr;
		if ((msg->msg_type == MSG_TYPE_EVENT_BUF_POST) ||
			(msg->msg_type == MSG_TYPE_IOCTLRESP_BUF_POST)) {

			rxbuf_post = (ioctl_resp_evt_buf_post_msg_t *)msg+1;
			DHD_ERROR(("%d,%d) msgtype:0x%x request_id:0x%x if_id:0x%x"
				" addr:0x%x:0x%x len:%d\n",
				i, r_ptr, msg->msg_type, msg->request_id, msg->if_id,
				rxbuf_post->host_buf_addr.high_addr,
				rxbuf_post->host_buf_addr.low_addr,
				rxbuf_post->host_buf_len));
		} else if (msg->msg_type == MSG_TYPE_RXBUF_POST) {
			rxbuf_data = (host_rxbuf_post_t *)msg+1;
			DHD_ERROR(("%d,%d) msgtype:0x%x request_id:0x%x if_id:0x%x"
				" addr:0x%x:%x len:%d meta:0x%x:%x len:%d\n",
				i, r_ptr, msg->msg_type, msg->request_id, msg->if_id,
				rxbuf_data->data_buf_addr.high_addr,
				rxbuf_data->data_buf_addr.low_addr,
				rxbuf_data->data_buf_len,
				rxbuf_data->metadata_buf_addr.high_addr,
				rxbuf_data->metadata_buf_addr.low_addr,
				rxbuf_data->metadata_buf_len));
		} else {
			DHD_ERROR(("%d,%d) msgtype:0x%x request_id:0x%x if_id:0x%x\n",
				i, r_ptr, msg->msg_type, msg->request_id, msg->if_id));
		}

		r_ptr = (r_ptr + 1) % depth;
	}
}

void
dhd_prot_dump_kernel_crash(dhd_pub_t *dhd)
{
	dhd_prot_t *prot = dhd->prot;

#if defined(PCIE_D2H_SYNC)
	if (dhd->d2h_sync_mode & PCIE_SHARED_D2H_SYNC_SEQNUM)
		DHD_ERROR(("\nd2h_sync: SEQNUM:"));
	else if (dhd->d2h_sync_mode & PCIE_SHARED_D2H_SYNC_XORCSUM)
		DHD_ERROR(("\nd2h_sync: XORCSUM:"));
	else
		DHD_ERROR(("\nd2h_sync: NONE:"));
	DHD_ERROR((" d2h_sync_wait max<%lu> tot<%lu>\n",
		dhd->prot->d2h_sync_wait_max, dhd->prot->d2h_sync_wait_tot));
#endif	/* PCIE_D2H_SYNC */

	/* Dump Ctrl */
	if (prot->h2dring_ctrl_subn)
		dump_ring(dhd, prot->h2dring_ctrl_subn);
	if (prot->d2hring_ctrl_cpln)
		dump_ring(dhd, prot->d2hring_ctrl_cpln);

	/* Dump TX */
	if (prot->h2dring_txp_subn)
		dump_ring(dhd, prot->h2dring_txp_subn);
	if (prot->d2hring_tx_cpln)
		dump_ring(dhd, prot->d2hring_tx_cpln);

	/* Dump RX */
	if (prot->h2dring_rxp_subn)
		dump_ring(dhd, prot->h2dring_rxp_subn);
	if (prot->d2hring_rx_cpln)
		dump_ring(dhd, prot->d2hring_rx_cpln);

}
#endif /* DHD_DEBUG_PAGEALLOC */

/* Update local copy of dongle statistics */
void dhd_prot_dstats(dhd_pub_t *dhd)
{
		return;
}

int dhd_process_pkt_reorder_info(dhd_pub_t *dhd, uchar *reorder_info_buf,
	uint reorder_info_len, void **pkt, uint32 *free_buf_count)
{
	return 0;
}
/* post a dummy message to interrupt dongle */
/* used to process cons commands */
int
dhd_post_dummy_msg(dhd_pub_t *dhd)
{
	unsigned long flags;
	hostevent_hdr_t *hevent = NULL;
	uint16 alloced = 0;

	dhd_prot_t *prot = dhd->prot;

	DHD_GENERAL_LOCK(dhd, flags);
	hevent = (hostevent_hdr_t *)dhd_alloc_ring_space(dhd,
		prot->h2dring_ctrl_subn, DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D, &alloced);

	if (hevent == NULL) {
		DHD_GENERAL_UNLOCK(dhd, flags);
		return -1;
	}

	/* CMN msg header */
	hevent->msg.msg_type = MSG_TYPE_HOST_EVNT;
	hevent->msg.if_id = 0;

	/* Event payload */
	hevent->evnt_pyld = htol32(HOST_EVENT_CONS_CMD);

	/* Since, we are filling the data directly into the bufptr obtained
	 * from the msgbuf, we can directly call the write_complete
	 */
	prot_ring_write_complete(dhd, prot->h2dring_ctrl_subn, hevent,
		DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D);
	DHD_GENERAL_UNLOCK(dhd, flags);

	return 0;
}

static void * BCMFASTPATH
dhd_alloc_ring_space(dhd_pub_t *dhd, msgbuf_ring_t *ring, uint16 nitems, uint16 * alloced)
{
	void * ret_buf;
	uint16 r_index = 0;

	/* Alloc space for nitems in the ring */
	ret_buf = prot_get_ring_space(ring, nitems, alloced);

	if (ret_buf == NULL) {
		/* if alloc failed , invalidate cached read ptr */
		if (DMA_INDX_ENAB(dhd->dma_d2h_ring_upd_support)) {
			r_index = dhd_get_dmaed_index(dhd, H2D_DMA_READINDX, ring->idx);
			ring->ringstate->r_offset = r_index;
		} else
			dhd_bus_cmn_readshared(dhd->bus, &(RING_READ_PTR(ring)),
				RING_READ_PTR, ring->idx);

		/* Try allocating once more */
		ret_buf = prot_get_ring_space(ring, nitems, alloced);

		if (ret_buf == NULL) {
			DHD_INFO(("RING space not available on ring %s for %d items \n",
				ring->name, nitems));
			DHD_INFO(("write %d read %d \n\n", RING_WRITE_PTR(ring),
				RING_READ_PTR(ring)));
			return NULL;
		}
	}

	/* Return alloced space */
	return ret_buf;
}

/* Non inline ioct request */
/* Form a ioctl request first as per ioctptr_reqst_hdr_t header in the circular buffer */
/* Form a separate request buffer where a 4 byte cmn header is added in the front */
/* buf contents from parent function is copied to remaining section of this buffer */
static int
dhd_fillup_ioct_reqst_ptrbased(dhd_pub_t *dhd, uint16 len, uint cmd, void* buf, int ifidx)
{
	dhd_prot_t *prot = dhd->prot;
	ioctl_req_msg_t *ioct_rqst;
	void * ioct_buf;	/* For ioctl payload */
	uint16  rqstlen, resplen;
	unsigned long flags;
	uint16 alloced = 0;

	rqstlen = len;
	resplen = len;

	/* Limit ioct request to MSGBUF_MAX_MSG_SIZE bytes including hdrs */
	/* 8K allocation of dongle buffer fails */
	/* dhd doesnt give separate input & output buf lens */
	/* so making the assumption that input length can never be more than 1.5k */
	rqstlen = MIN(rqstlen, MSGBUF_MAX_MSG_SIZE);

	DHD_GENERAL_LOCK(dhd, flags);
	/* Request for cbuf space */
	ioct_rqst = (ioctl_req_msg_t*)dhd_alloc_ring_space(dhd, prot->h2dring_ctrl_subn,
		DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D,	&alloced);
	if (ioct_rqst == NULL) {
		DHD_ERROR(("couldn't allocate space on msgring to send ioctl request\n"));
		DHD_GENERAL_UNLOCK(dhd, flags);
		return -1;
	}

	/* Common msg buf hdr */
	ioct_rqst->cmn_hdr.msg_type = MSG_TYPE_IOCTLPTR_REQ;
	ioct_rqst->cmn_hdr.if_id = (uint8)ifidx;
	ioct_rqst->cmn_hdr.flags = 0;
	ioct_rqst->cmn_hdr.request_id = DHD_IOCTL_REQ_PKTID;

	ioct_rqst->cmd = htol32(cmd);
	ioct_rqst->output_buf_len = htol16(resplen);
	ioct_rqst->trans_id = prot->ioctl_trans_id ++;

	/* populate ioctl buffer info */
	ioct_rqst->input_buf_len = htol16(rqstlen);
	ioct_rqst->host_input_buf_addr.high = htol32(PHYSADDRHI(prot->ioctbuf.pa));
	ioct_rqst->host_input_buf_addr.low = htol32(PHYSADDRLO(prot->ioctbuf.pa));

	/* copy ioct payload */
	ioct_buf = (void *) prot->ioctbuf.va;

	if (buf)
		memcpy(ioct_buf, buf, len);

	OSL_CACHE_FLUSH((void *) prot->ioctbuf.va, len);

	if ((ulong)ioct_buf % DMA_ALIGN_LEN)
		DHD_ERROR(("host ioct address unaligned !!!!! \n"));

	DHD_CTL(("submitted IOCTL request request_id %d, cmd %d, output_buf_len %d, tx_id %d\n",
		ioct_rqst->cmn_hdr.request_id, cmd, ioct_rqst->output_buf_len,
		ioct_rqst->trans_id));

	/* upd wrt ptr and raise interrupt */
	prot_ring_write_complete(dhd, prot->h2dring_ctrl_subn, ioct_rqst,
		DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D);
	DHD_GENERAL_UNLOCK(dhd, flags);

	return 0;
}

/* Packet to PacketID mapper */
typedef struct {
	ulong native;
	dmaaddr_t pa;
	uint32 pa_len;
	uchar dma;
} pktid_t;

typedef struct {
	void	*osh;
	void	*mwbmap_hdl;
	pktid_t *pktid_list;
	uint32	count;
} pktid_map_t;


void *pktid_map_init(void *osh, uint32 count)
{
	pktid_map_t *handle;

	handle = (pktid_map_t *) MALLOC(osh, sizeof(pktid_map_t));
	if (handle == NULL) {
		printf("%s:%d: MALLOC failed for size %d\n",
			__FUNCTION__, __LINE__, (uint32) sizeof(pktid_map_t));
		return NULL;
	}
	handle->osh = osh;
	handle->count = count;
	handle->mwbmap_hdl = bcm_mwbmap_init(osh, count);
	if (handle->mwbmap_hdl == NULL) {
		printf("%s:%d: bcm_mwbmap_init failed for count %d\n",
			__FUNCTION__, __LINE__, count);
		MFREE(osh, handle, sizeof(pktid_map_t));
		return NULL;
	}

	handle->pktid_list = (pktid_t *) MALLOC(osh, sizeof(pktid_t) * (count+1));
	if (handle->pktid_list == NULL) {
		printf("%s:%d: MALLOC failed for count %d / total = %d\n",
			__FUNCTION__, __LINE__, count, (uint32) sizeof(pktid_t) * count);
		bcm_mwbmap_fini(osh, handle->mwbmap_hdl);
		MFREE(osh, handle, sizeof(pktid_map_t));
		return NULL;
	}

	return handle;
}

void
pktid_map_uninit(void *pktid_map_handle)
{
	pktid_map_t *handle = (pktid_map_t *) pktid_map_handle;
	uint32 ix;

	if (handle != NULL) {
		void *osh = handle->osh;
		for (ix = 0; ix < MAX_PKTID_ITEMS; ix++)
		{
			if (!bcm_mwbmap_isfree(handle->mwbmap_hdl, ix)) {
				/* Mark the slot as free */
				bcm_mwbmap_free(handle->mwbmap_hdl, ix);
				/*
				Here we can do dma unmapping for 32 bit also.
				Since this in removal path, it will not affect performance
				*/
				DMA_UNMAP(osh, handle->pktid_list[ix+1].pa,
					(uint) handle->pktid_list[ix+1].pa_len,
					handle->pktid_list[ix+1].dma, 0, 0);
				PKTFREE(osh, (unsigned long*)handle->pktid_list[ix+1].native, TRUE);
			}
		}
		bcm_mwbmap_fini(osh, handle->mwbmap_hdl);
		MFREE(osh, handle->pktid_list, sizeof(pktid_t) * (handle->count+1));
		MFREE(osh, handle, sizeof(pktid_map_t));
	}
	return;
}

uint32 BCMFASTPATH
pktid_map_unique(void *pktid_map_handle, void *pkt, dmaaddr_t physaddr, uint32 physlen, uint32 dma)
{
	uint32 id;
	pktid_map_t *handle = (pktid_map_t *) pktid_map_handle;

	if (handle == NULL) {
		printf("%s:%d: Error !!! pktid_map_unique called without initing pktid_map\n",
			__FUNCTION__, __LINE__);
		return 0;
	}
	id = bcm_mwbmap_alloc(handle->mwbmap_hdl);
	if (id == BCM_MWBMAP_INVALID_IDX) {
		printf("%s:%d: bcm_mwbmap_alloc failed. Free Count = %d\n",
			__FUNCTION__, __LINE__, bcm_mwbmap_free_cnt(handle->mwbmap_hdl));
		return 0;
	}

	/* id=0 is invalid as we use this for error checking in the dongle */
	id += 1;
	handle->pktid_list[id].native = (ulong) pkt;
	handle->pktid_list[id].pa     = physaddr;
	handle->pktid_list[id].pa_len = (uint32) physlen;
	handle->pktid_list[id].dma = (uchar)dma;

	return id;
}

void * BCMFASTPATH
pktid_get_packet(void *pktid_map_handle, uint32 id, dmaaddr_t *physaddr, uint32 *physlen)
{
	void *native = NULL;
	pktid_map_t *handle = (pktid_map_t *) pktid_map_handle;
	if (handle == NULL) {
		printf("%s:%d: Error !!! pktid_get_packet called without initing pktid_map\n",
			__FUNCTION__, __LINE__);
		return NULL;
	}

	/* Debug check */
	if (bcm_mwbmap_isfree(handle->mwbmap_hdl, (id-1))) {
		printf("%s:%d: Error !!!. slot (%d/0x%04x) free but the app is using it.\n",
			__FUNCTION__, __LINE__, (id-1), (id-1));
		return NULL;
	}

	native = (void *) handle->pktid_list[id].native;
	*physaddr = handle->pktid_list[id].pa;
	*physlen  = (uint32) handle->pktid_list[id].pa_len;

	/* Mark the slot as free */
	bcm_mwbmap_free(handle->mwbmap_hdl, (id-1));

	return native;
}
static msgbuf_ring_t*
prot_ring_attach(dhd_prot_t * prot, char* name, uint16 max_item, uint16 len_item, uint16 ringid)
{
	uint alloced = 0;
	msgbuf_ring_t *ring;
	dmaaddr_t physaddr;
	uint16 size;

	ASSERT(name);
	BCM_REFERENCE(physaddr);

	/* allocate ring info */
	ring = MALLOC(prot->osh, sizeof(msgbuf_ring_t));
	if (ring == NULL) {
		ASSERT(0);
		return NULL;
	}
	bzero(ring, sizeof(*ring));

	/* Init name */
	strncpy(ring->name, name, sizeof(ring->name) - 1);

	/* Ringid in the order given in bcmpcie.h */
	ring->idx = ringid;

	/* init ringmem */
	ring->ringmem = MALLOC(prot->osh, sizeof(ring_mem_t));
	if (ring->ringmem == NULL)
		goto fail;
	bzero(ring->ringmem, sizeof(*ring->ringmem));

	ring->ringmem->max_item = max_item;
	ring->ringmem->len_items = len_item;
	size = max_item * len_item;

	/* Ring Memmory allocation */
	ring->ring_base.va = DMA_ALLOC_CONSISTENT(prot->osh, size, DMA_ALIGN_LEN,
		&alloced, &ring->ring_base.pa, &ring->ring_base.dmah);

	if (ring->ring_base.va == NULL)
		goto fail;
	ring->ringmem->base_addr.high_addr = htol32(PHYSADDRHI(ring->ring_base.pa));
	ring->ringmem->base_addr.low_addr = htol32(PHYSADDRLO(ring->ring_base.pa));

	ASSERT(MODX((unsigned long)ring->ring_base.va, DMA_ALIGN_LEN) == 0);
	bzero(ring->ring_base.va, size);

	OSL_CACHE_FLUSH((void *) ring->ring_base.va, size);

	/* Ring state init */
	ring->ringstate	= MALLOC(prot->osh, sizeof(ring_state_t));
	if (ring->ringstate == NULL)
		goto fail;
	bzero(ring->ringstate, sizeof(*ring->ringstate));

	DHD_INFO(("RING_ATTACH : %s Max item %d len item %d total size %d "
		"ring start %p buf phys addr  %x:%x \n",
		ring->name, ring->ringmem->max_item, ring->ringmem->len_items,
		size, ring->ring_base.va, ring->ringmem->base_addr.high_addr,
		ring->ringmem->base_addr.low_addr));
	return ring;
fail:
	if (ring->ring_base.va && ring->ringmem) {
		PHYSADDRHISET(physaddr, ring->ringmem->base_addr.high_addr);
		PHYSADDRLOSET(physaddr, ring->ringmem->base_addr.low_addr);
		size = ring->ringmem->max_item * ring->ringmem->len_items;
		DMA_FREE_CONSISTENT(prot->osh, ring->ring_base.va, size, ring->ring_base.pa, NULL);
		ring->ring_base.va = NULL;
	}
	if (ring->ringmem)
		MFREE(prot->osh, ring->ringmem, sizeof(ring_mem_t));
	MFREE(prot->osh, ring, sizeof(msgbuf_ring_t));
	ASSERT(0);
	return NULL;
}
static void
dhd_ring_init(dhd_pub_t *dhd, msgbuf_ring_t *ring)
{
	/* update buffer address of ring */
	dhd_bus_cmn_writeshared(dhd->bus, &ring->ringmem->base_addr,
		sizeof(ring->ringmem->base_addr), RING_BUF_ADDR, ring->idx);

	/* Update max items possible in ring */
	dhd_bus_cmn_writeshared(dhd->bus, &ring->ringmem->max_item,
		sizeof(ring->ringmem->max_item), RING_MAX_ITEM, ring->idx);

	/* Update length of each item in the ring */
	dhd_bus_cmn_writeshared(dhd->bus, &ring->ringmem->len_items,
		sizeof(ring->ringmem->len_items), RING_LEN_ITEMS, ring->idx);

	/* ring inited */
	ring->inited = TRUE;
}
static void
dhd_prot_ring_detach(dhd_pub_t *dhd, msgbuf_ring_t * ring)
{
	dmaaddr_t phyaddr;
	uint16 size;
	dhd_prot_t *prot = dhd->prot;

	BCM_REFERENCE(phyaddr);

	if (ring == NULL)
		return;


	if (ring->ringmem == NULL) {
		DHD_ERROR(("%s: ring->ringmem is NULL\n", __FUNCTION__));
			return;
	}

	ring->inited = FALSE;

	PHYSADDRHISET(phyaddr, ring->ringmem->base_addr.high_addr);
	PHYSADDRLOSET(phyaddr, ring->ringmem->base_addr.low_addr);
	size = ring->ringmem->max_item * ring->ringmem->len_items;
	/* Free up ring */
	if (ring->ring_base.va) {
		DMA_FREE_CONSISTENT(prot->osh, ring->ring_base.va, size, ring->ring_base.pa,
			ring->ring_base.dmah);
		ring->ring_base.va = NULL;
	}

	/* Free up ring mem space */
	if (ring->ringmem) {
		MFREE(prot->osh, ring->ringmem, sizeof(ring_mem_t));
		ring->ringmem = NULL;
	}

	/* Free up ring state info */
	if (ring->ringstate) {
		MFREE(prot->osh, ring->ringstate, sizeof(ring_state_t));
		ring->ringstate = NULL;
	}

	/* free up ring info */
	MFREE(prot->osh, ring, sizeof(msgbuf_ring_t));
}
/* Assumes only one index is updated ata time */
static void *BCMFASTPATH
prot_get_ring_space(msgbuf_ring_t *ring, uint16 nitems, uint16 * alloced)
{
	void *ret_ptr = NULL;
	uint16 ring_avail_cnt;

	ASSERT(nitems <= RING_MAX_ITEM(ring));

	ring_avail_cnt = CHECK_WRITE_SPACE(RING_READ_PTR(ring), RING_WRITE_PTR(ring),
		RING_MAX_ITEM(ring));

	if (ring_avail_cnt == 0) {
		return NULL;
	}
	*alloced = MIN(nitems, ring_avail_cnt);

	/* Return next available space */
	ret_ptr = (char*)HOST_RING_BASE(ring) + (RING_WRITE_PTR(ring) * RING_LEN_ITEMS(ring));

	/* Update write pointer */
	if ((RING_WRITE_PTR(ring) + *alloced) == RING_MAX_ITEM(ring))
		RING_WRITE_PTR(ring) = 0;
	else if ((RING_WRITE_PTR(ring) + *alloced) < RING_MAX_ITEM(ring))
		RING_WRITE_PTR(ring) += *alloced;
	else {
		/* Should never hit this */
		ASSERT(0);
		return NULL;
	}

	return ret_ptr;
}

static void BCMFASTPATH
prot_ring_write_complete(dhd_pub_t *dhd, msgbuf_ring_t * ring, void* p, uint16 nitems)
{
	dhd_prot_t *prot = dhd->prot;

	/* cache flush */
	OSL_CACHE_FLUSH(p, RING_LEN_ITEMS(ring) * nitems);

	/* update write pointer */
	/* If dma'ing h2d indices are supported
	 * update the values in the host memory
	 * o/w update the values in TCM
	 */
	if (DMA_INDX_ENAB(dhd->dma_h2d_ring_upd_support))
		dhd_set_dmaed_index(dhd, H2D_DMA_WRITEINDX,
			ring->idx, (uint16)RING_WRITE_PTR(ring));
	else
		dhd_bus_cmn_writeshared(dhd->bus, &(RING_WRITE_PTR(ring)),
			sizeof(uint16), RING_WRITE_PTR, ring->idx);

	/* raise h2d interrupt */
	prot->mb_ring_fn(dhd->bus, RING_WRITE_PTR(ring));
}

/* If dma'ing h2d indices are supported
 * this function updates the indices in
 * the host memory
 */
static void
dhd_set_dmaed_index(dhd_pub_t *dhd, uint8 type, uint16 ringid, uint16 new_index)
{
	dhd_prot_t *prot = dhd->prot;

	uint32 *ptr = NULL;
	uint16 offset = 0;

	switch (type) {
		case H2D_DMA_WRITEINDX:
			ptr = (uint32 *)(prot->h2d_dma_writeindx_buf.va);

			/* Flow-Rings start at Id BCMPCIE_COMMON_MSGRINGS
			 * but in host memory their indices start
			 * after H2D Common Rings
			 */
			if (ringid >= BCMPCIE_COMMON_MSGRINGS)
				offset = ringid - BCMPCIE_COMMON_MSGRINGS +
					BCMPCIE_H2D_COMMON_MSGRINGS;
			else
				offset = ringid;
			ptr += offset;

			*ptr = htol16(new_index);

			/* cache flush */
			OSL_CACHE_FLUSH((void *)prot->h2d_dma_writeindx_buf.va,
				prot->h2d_dma_writeindx_buf_len);

			break;

		case D2H_DMA_READINDX:
			ptr = (uint32 *)(prot->d2h_dma_readindx_buf.va);

			/* H2D Common Righs start at Id BCMPCIE_H2D_COMMON_MSGRINGS */
			offset = ringid - BCMPCIE_H2D_COMMON_MSGRINGS;
			ptr += offset;

			*ptr = htol16(new_index);
			/* cache flush */
			OSL_CACHE_FLUSH((void *)prot->d2h_dma_readindx_buf.va,
				prot->d2h_dma_readindx_buf_len);

			break;

		default:
			DHD_ERROR(("%s: Invalid option for DMAing read/write index\n",
				__FUNCTION__));

			break;
	}
	DHD_TRACE(("%s: Data 0x%p, ringId %d, new_index %d\n",
		__FUNCTION__, ptr, ringid, new_index));
}


static uint16
dhd_get_dmaed_index(dhd_pub_t *dhd, uint8 type, uint16 ringid)
{
	uint32 *ptr = NULL;
	uint16 data = 0;
	uint16 offset = 0;

	switch (type) {
		case H2D_DMA_WRITEINDX:
			OSL_CACHE_INV((void *)dhd->prot->h2d_dma_writeindx_buf.va,
				dhd->prot->h2d_dma_writeindx_buf_len);
			ptr = (uint32 *)(dhd->prot->h2d_dma_writeindx_buf.va);

			/* Flow-Rings start at Id BCMPCIE_COMMON_MSGRINGS
			 * but in host memory their indices start
			 * after H2D Common Rings
			 */
			if (ringid >= BCMPCIE_COMMON_MSGRINGS)
				offset = ringid - BCMPCIE_COMMON_MSGRINGS +
					BCMPCIE_H2D_COMMON_MSGRINGS;
			else
				offset = ringid;
			ptr += offset;

			data = LTOH16((uint16)*ptr);
			break;

		case H2D_DMA_READINDX:
			OSL_CACHE_INV((void *)dhd->prot->h2d_dma_readindx_buf.va,
				dhd->prot->h2d_dma_readindx_buf_len);
			ptr = (uint32 *)(dhd->prot->h2d_dma_readindx_buf.va);

			/* Flow-Rings start at Id BCMPCIE_COMMON_MSGRINGS
			 * but in host memory their indices start
			 * after H2D Common Rings
			 */
			if (ringid >= BCMPCIE_COMMON_MSGRINGS)
				offset = ringid - BCMPCIE_COMMON_MSGRINGS +
					BCMPCIE_H2D_COMMON_MSGRINGS;
			else
				offset = ringid;
			ptr += offset;

			data = LTOH16((uint16)*ptr);
			break;

		case D2H_DMA_WRITEINDX:
			OSL_CACHE_INV((void *)dhd->prot->d2h_dma_writeindx_buf.va,
				dhd->prot->d2h_dma_writeindx_buf_len);
			ptr = (uint32 *)(dhd->prot->d2h_dma_writeindx_buf.va);

			/* H2D Common Righs start at Id BCMPCIE_H2D_COMMON_MSGRINGS */
			offset = ringid - BCMPCIE_H2D_COMMON_MSGRINGS;
			ptr += offset;

			data = LTOH16((uint16)*ptr);
			break;

		case D2H_DMA_READINDX:
			OSL_CACHE_INV((void *)dhd->prot->d2h_dma_readindx_buf.va,
				dhd->prot->d2h_dma_readindx_buf_len);
			ptr = (uint32 *)(dhd->prot->d2h_dma_readindx_buf.va);

			/* H2D Common Righs start at Id BCMPCIE_H2D_COMMON_MSGRINGS */
			offset = ringid - BCMPCIE_H2D_COMMON_MSGRINGS;
			ptr += offset;

			data = LTOH16((uint16)*ptr);
			break;

		default:
			DHD_ERROR(("%s: Invalid option for DMAing read/write index\n",
				__FUNCTION__));

			break;
	}
	DHD_TRACE(("%s: Data 0x%p, data %d\n", __FUNCTION__, ptr, data));
	return (data);
}

/* D2H dircetion: get next space to read from */
static uint8*
prot_get_src_addr(dhd_pub_t *dhd, msgbuf_ring_t * ring, uint16* available_len)
{
	uint16 w_ptr;
	uint16 r_ptr;
	uint16 depth;
	void* ret_addr = NULL;
	uint16 d2h_w_index = 0;

	DHD_TRACE(("%s: h2d_dma_readindx_buf %p, d2h_dma_writeindx_buf %p\n",
		__FUNCTION__, (uint32 *)(dhd->prot->h2d_dma_readindx_buf.va),
		(uint32 *)(dhd->prot->d2h_dma_writeindx_buf.va)));

	/* update write pointer */
	if (DMA_INDX_ENAB(dhd->dma_d2h_ring_upd_support)) {
		/* DMAing write/read indices supported */
		d2h_w_index = dhd_get_dmaed_index(dhd, D2H_DMA_WRITEINDX, ring->idx);
		ring->ringstate->w_offset = d2h_w_index;
	} else
		dhd_bus_cmn_readshared(dhd->bus,
			&(RING_WRITE_PTR(ring)), RING_WRITE_PTR, ring->idx);

	w_ptr = ring->ringstate->w_offset;
	r_ptr = ring->ringstate->r_offset;
	depth = ring->ringmem->max_item;

	*available_len = READ_AVAIL_SPACE(w_ptr, r_ptr, depth);
	if (*available_len == 0)
		return NULL;

	if (*available_len > ring->ringmem->max_item) {
		DHD_ERROR(("\r\n======================= \r\n"));
		DHD_ERROR(("%s(): ring %p, ring->name %s, ring->max_item %d\r\n",
			__FUNCTION__, ring, ring->name, ring->ringmem->max_item));
		DHD_ERROR(("wr: %d,  rd: %d,  depth: %d  \r\n", w_ptr, r_ptr, depth));
		DHD_ERROR(("dhd->busstate %d bus->wait_for_d3_ack %d \r\n",
			dhd->busstate, dhd->bus->wait_for_d3_ack));
		DHD_ERROR(("\r\n======================= \r\n"));
#ifdef SUPPORT_LINKDOWN_RECOVERY
		if (w_ptr >= ring->ringmem->max_item) {
			dhd->bus->read_shm_fail = true;
		}
#else
#ifdef DHD_FW_COREDUMP
	if (dhd->memdump_enabled) {
		/* collect core dump */
		dhd->memdump_type = DUMP_TYPE_RESUMED_ON_INVALID_RING_RDWR;
		dhd_bus_mem_dump(dhd);
	}
#endif /* DHD_FW_COREDUMP */
#endif /* SUPPORT_LINKDOWN_RECOVERY */

		return NULL;
	}

	/* if space available, calculate address to be read */
	ret_addr = (char*)ring->ring_base.va + (r_ptr * ring->ringmem->len_items);

	/* update read pointer */
	if ((ring->ringstate->r_offset + *available_len) >= ring->ringmem->max_item)
		ring->ringstate->r_offset = 0;
	else
		ring->ringstate->r_offset += *available_len;

	ASSERT(ring->ringstate->r_offset < ring->ringmem->max_item);

	/* convert index to bytes */
	*available_len = *available_len * ring->ringmem->len_items;

	/* Cache invalidate */
	OSL_CACHE_INV((void *) ret_addr, *available_len);

	/* return read address */
	return ret_addr;
}
static void
prot_upd_read_idx(dhd_pub_t *dhd, msgbuf_ring_t * ring)
{
	/* update read index */
	/* If dma'ing h2d indices supported
	 * update the r -indices in the
	 * host memory o/w in TCM
	 */
	if (DMA_INDX_ENAB(dhd->dma_h2d_ring_upd_support))
		dhd_set_dmaed_index(dhd, D2H_DMA_READINDX,
			ring->idx, (uint16)RING_READ_PTR(ring));
	else
		dhd_bus_cmn_writeshared(dhd->bus, &(RING_READ_PTR(ring)),
			sizeof(uint16), RING_READ_PTR, ring->idx);
}

static void
prot_store_rxcpln_read_idx(dhd_pub_t *dhd, msgbuf_ring_t * ring)
{
	dhd_prot_t *prot;

	if (!dhd || !dhd->prot)
		return;

	prot = dhd->prot;
	prot->rx_cpln_early_upd_idx = RING_READ_PTR(ring);
}

static void
prot_early_upd_rxcpln_read_idx(dhd_pub_t *dhd, msgbuf_ring_t * ring)
{
	dhd_prot_t *prot;

	if (!dhd || !dhd->prot)
		return;

	prot = dhd->prot;

	if (prot->rx_cpln_early_upd_idx == RING_READ_PTR(ring))
		return;

	if (++prot->rx_cpln_early_upd_idx >= RING_MAX_ITEM(ring))
		prot->rx_cpln_early_upd_idx = 0;

	if (DMA_INDX_ENAB(dhd->dma_h2d_ring_upd_support))
		dhd_set_dmaed_index(dhd, D2H_DMA_READINDX,
			ring->idx, (uint16)prot->rx_cpln_early_upd_idx);
	else
		dhd_bus_cmn_writeshared(dhd->bus, &(prot->rx_cpln_early_upd_idx),
			sizeof(uint16), RING_READ_PTR, ring->idx);
}

int
dhd_prot_flow_ring_create(dhd_pub_t *dhd, flow_ring_node_t *flow_ring_node)
{
	tx_flowring_create_request_t *flow_create_rqst;
	msgbuf_ring_t *msgbuf_flow_info;
	dhd_prot_t *prot = dhd->prot;
	uint16 hdrlen = sizeof(tx_flowring_create_request_t);
	uint16 msglen = hdrlen;
	unsigned long flags;
	uint16 alloced = 0;

	if (!(msgbuf_flow_info = prot_ring_attach(prot, "h2dflr",
		H2DRING_TXPOST_MAX_ITEM, H2DRING_TXPOST_ITEMSIZE,
		BCMPCIE_H2D_TXFLOWRINGID +
		(flow_ring_node->flowid - BCMPCIE_H2D_COMMON_MSGRINGS)))) {
		DHD_ERROR(("%s: kmalloc for H2D TX Flow ring failed\n", __FUNCTION__));
		return BCME_NOMEM;
	}
	/* Clear write pointer of the ring */
	flow_ring_node->prot_info = (void *)msgbuf_flow_info;

	/* align it to 4 bytes, so that all start addr form cbuf is 4 byte aligned */
	msglen = align(msglen, DMA_ALIGN_LEN);


	DHD_GENERAL_LOCK(dhd, flags);
	/* Request for ring buffer space */
	flow_create_rqst = (tx_flowring_create_request_t *)dhd_alloc_ring_space(dhd,
		prot->h2dring_ctrl_subn, DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D, &alloced);

	if (flow_create_rqst == NULL) {
		DHD_ERROR(("%s: No space in control ring for Flow create req\n", __FUNCTION__));
		DHD_GENERAL_UNLOCK(dhd, flags);
		return BCME_NOMEM;
	}
	msgbuf_flow_info->inited = TRUE;

	/* Common msg buf hdr */
	flow_create_rqst->msg.msg_type = MSG_TYPE_FLOW_RING_CREATE;
	flow_create_rqst->msg.if_id = (uint8)flow_ring_node->flow_info.ifindex;
	flow_create_rqst->msg.request_id = htol16(0); /* TBD */

	/* Update flow create message */
	flow_create_rqst->tid = flow_ring_node->flow_info.tid;
	flow_create_rqst->flow_ring_id = htol16((uint16)flow_ring_node->flowid);
	memcpy(flow_create_rqst->sa, flow_ring_node->flow_info.sa, sizeof(flow_create_rqst->sa));
	memcpy(flow_create_rqst->da, flow_ring_node->flow_info.da, sizeof(flow_create_rqst->da));
	flow_create_rqst->flow_ring_ptr.low_addr = msgbuf_flow_info->ringmem->base_addr.low_addr;
	flow_create_rqst->flow_ring_ptr.high_addr = msgbuf_flow_info->ringmem->base_addr.high_addr;
	flow_create_rqst->max_items = htol16(H2DRING_TXPOST_MAX_ITEM);
	flow_create_rqst->len_item = htol16(H2DRING_TXPOST_ITEMSIZE);

	DHD_ERROR(("%s Send Flow create Req msglen flow ID %d for peer " MACDBG
		" prio %d ifindex %d\n", __FUNCTION__, flow_ring_node->flowid,
		MAC2STRDBG(flow_ring_node->flow_info.da), flow_ring_node->flow_info.tid,
		flow_ring_node->flow_info.ifindex));

	/* upd wrt ptr and raise interrupt */
	prot_ring_write_complete(dhd, prot->h2dring_ctrl_subn, flow_create_rqst,
		DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D);

	/* If dma'ing indices supported
	 * update the w-index in host memory o/w in TCM
	 */
	if (DMA_INDX_ENAB(dhd->dma_h2d_ring_upd_support))
		dhd_set_dmaed_index(dhd, H2D_DMA_WRITEINDX,
			msgbuf_flow_info->idx, (uint16)RING_WRITE_PTR(msgbuf_flow_info));
	else
		dhd_bus_cmn_writeshared(dhd->bus, &(RING_WRITE_PTR(msgbuf_flow_info)),
			sizeof(uint16), RING_WRITE_PTR, msgbuf_flow_info->idx);
	DHD_GENERAL_UNLOCK(dhd, flags);

	return BCME_OK;
}

static void
dhd_prot_process_flow_ring_create_response(dhd_pub_t *dhd, void* buf, uint16 msglen)
{
	tx_flowring_create_response_t *flow_create_resp = (tx_flowring_create_response_t *)buf;

	DHD_ERROR(("%s Flow create Response status = %d Flow %d\n", __FUNCTION__,
		flow_create_resp->cmplt.status, flow_create_resp->cmplt.flow_ring_id));

	dhd_bus_flow_ring_create_response(dhd->bus, flow_create_resp->cmplt.flow_ring_id,
		flow_create_resp->cmplt.status);
}

void dhd_prot_clean_flow_ring(dhd_pub_t *dhd, void *msgbuf_flow_info)
{
	msgbuf_ring_t *flow_ring = (msgbuf_ring_t *)msgbuf_flow_info;
	dhd_prot_ring_detach(dhd, flow_ring);
	DHD_INFO(("%s Cleaning up Flow \n", __FUNCTION__));
}

void dhd_prot_print_flow_ring(dhd_pub_t *dhd, void *msgbuf_flow_info,
	struct bcmstrbuf *strbuf, const char *fmt)
{
	const char *default_fmt = "RD %d WR %d BASE(VA) %p BASE(PA) %x:%x SIZE %d\n";
	msgbuf_ring_t *flow_ring = (msgbuf_ring_t *)msgbuf_flow_info;
	uint16 rd, wr;
	uint32 dma_buf_len = RING_MAX_ITEM(flow_ring) * RING_LEN_ITEMS(flow_ring);

	if (fmt == NULL) {
		fmt = default_fmt;
	}
	dhd_bus_cmn_readshared(dhd->bus, &rd, RING_READ_PTR, flow_ring->idx);
	dhd_bus_cmn_readshared(dhd->bus, &wr, RING_WRITE_PTR, flow_ring->idx);
	bcm_bprintf(strbuf, fmt, rd, wr, HOST_RING_BASE(flow_ring),
		(uint32)ltoh32(PHYSADDRHI(HOST_RING_BASE_PHY(flow_ring))),
		(uint32)ltoh32(PHYSADDRLO(HOST_RING_BASE_PHY(flow_ring))),
		dma_buf_len);
}

void dhd_prot_print_info(dhd_pub_t *dhd, struct bcmstrbuf *strbuf)
{
	dhd_prot_t *prot = dhd->prot;
	bcm_bprintf(strbuf,
		"%8s %4s %4s %5s %17s %17s %7s\n",
		"Type", "RBP", "RD", "WR", "BASE(VA)", "BASE(PA)", "SIZE");
	bcm_bprintf(strbuf, "%8s %4s", "CtrlPost", "NA");
	dhd_prot_print_flow_ring(dhd, prot->h2dring_ctrl_subn, strbuf,
		"%5d %5d %17p %8x:%8x %7d\n");
	bcm_bprintf(strbuf, "%8s %4s", "CtrlCpl", "NA");
	dhd_prot_print_flow_ring(dhd, prot->d2hring_ctrl_cpln, strbuf,
		"%5d %5d %17p %8x:%8x %7d\n");
	bcm_bprintf(strbuf, "%8s %4d", "RxPost", prot->rxbufpost);
	dhd_prot_print_flow_ring(dhd, prot->h2dring_rxp_subn, strbuf,
		"%5d %5d %17p %8x:%8x %7d\n");
	bcm_bprintf(strbuf, "%8s %4s", "RxCpl", "NA");
	dhd_prot_print_flow_ring(dhd, prot->d2hring_rx_cpln, strbuf,
		"%5d %5d %17p %8x:%8x %7d\n");
	if (dhd_bus_is_txmode_push(dhd->bus)) {
		bcm_bprintf(strbuf, "TxPost: ");
		dhd_prot_print_flow_ring(dhd, prot->h2dring_txp_subn, strbuf, NULL);
	}
	bcm_bprintf(strbuf, "%8s %4s", "TxCpl", "NA");
	dhd_prot_print_flow_ring(dhd, prot->d2hring_tx_cpln, strbuf,
		"%5d %5d %17p %8x:%8x %7d\n");
	bcm_bprintf(strbuf, "active_tx_count %d	 pktidmap_avail %d\n",
		dhd->prot->active_tx_count,
		dhd_pktid_map_avail_cnt(dhd->prot->pktid_map_handle));
}

int
dhd_prot_flow_ring_delete(dhd_pub_t *dhd, flow_ring_node_t *flow_ring_node)
{
	tx_flowring_delete_request_t *flow_delete_rqst;
	dhd_prot_t *prot = dhd->prot;
	uint16 msglen = sizeof(tx_flowring_delete_request_t);
	unsigned long flags;
	uint16 alloced = 0;

	/* align it to 4 bytes, so that all start addr form cbuf is 4 byte aligned */
	msglen = align(msglen, DMA_ALIGN_LEN);

	/* Request for ring buffer space */
	DHD_GENERAL_LOCK(dhd, flags);
	flow_delete_rqst = (tx_flowring_delete_request_t *)dhd_alloc_ring_space(dhd,
		prot->h2dring_ctrl_subn, DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D, &alloced);

	if (flow_delete_rqst == NULL) {
		DHD_GENERAL_UNLOCK(dhd, flags);
		DHD_ERROR(("%s Flow Delete req failure no ring mem %d \n", __FUNCTION__, msglen));
		return BCME_NOMEM;
	}

	/* Common msg buf hdr */
	flow_delete_rqst->msg.msg_type = MSG_TYPE_FLOW_RING_DELETE;
	flow_delete_rqst->msg.if_id = (uint8)flow_ring_node->flow_info.ifindex;
	flow_delete_rqst->msg.request_id = htol16(0); /* TBD */

	/* Update Delete info */
	flow_delete_rqst->flow_ring_id = htol16((uint16)flow_ring_node->flowid);
	flow_delete_rqst->reason = htol16(BCME_OK);

	DHD_ERROR(("%s sending FLOW RING ID %d for peer " MACDBG " prio %d ifindex %d"
		" Delete req msglen %d\n", __FUNCTION__, flow_ring_node->flowid,
		MAC2STRDBG(flow_ring_node->flow_info.da), flow_ring_node->flow_info.tid,
		flow_ring_node->flow_info.ifindex, msglen));

	/* upd wrt ptr and raise interrupt */
	prot_ring_write_complete(dhd, prot->h2dring_ctrl_subn, flow_delete_rqst,
		DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D);
	DHD_GENERAL_UNLOCK(dhd, flags);

	return BCME_OK;
}

static void
dhd_prot_process_flow_ring_delete_response(dhd_pub_t *dhd, void* buf, uint16 msglen)
{
	tx_flowring_delete_response_t *flow_delete_resp = (tx_flowring_delete_response_t *)buf;

	DHD_INFO(("%s Flow Delete Response status = %d \n", __FUNCTION__,
		flow_delete_resp->cmplt.status));

	dhd_bus_flow_ring_delete_response(dhd->bus, flow_delete_resp->cmplt.flow_ring_id,
		flow_delete_resp->cmplt.status);
}

int
dhd_prot_flow_ring_flush(dhd_pub_t *dhd, flow_ring_node_t *flow_ring_node)
{
	tx_flowring_flush_request_t *flow_flush_rqst;
	dhd_prot_t *prot = dhd->prot;
	uint16 msglen = sizeof(tx_flowring_flush_request_t);
	unsigned long flags;
	uint16 alloced = 0;

	/* align it to 4 bytes, so that all start addr form cbuf is 4 byte aligned */
	msglen = align(msglen, DMA_ALIGN_LEN);

	/* Request for ring buffer space */
	DHD_GENERAL_LOCK(dhd, flags);
	flow_flush_rqst = (tx_flowring_flush_request_t *)dhd_alloc_ring_space(dhd,
		prot->h2dring_ctrl_subn, DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D, &alloced);
	if (flow_flush_rqst == NULL) {
		DHD_GENERAL_UNLOCK(dhd, flags);
		DHD_ERROR(("%s Flow Flush req failure no ring mem %d \n", __FUNCTION__, msglen));
		return BCME_NOMEM;
	}

	/* Common msg buf hdr */
	flow_flush_rqst->msg.msg_type = MSG_TYPE_FLOW_RING_FLUSH;
	flow_flush_rqst->msg.if_id = (uint8)flow_ring_node->flow_info.ifindex;
	flow_flush_rqst->msg.request_id = htol16(0); /* TBD */

	flow_flush_rqst->flow_ring_id = htol16((uint16)flow_ring_node->flowid);
	flow_flush_rqst->reason = htol16(BCME_OK);

	DHD_INFO(("%s sending FLOW RING Flush req msglen %d \n", __FUNCTION__, msglen));

	/* upd wrt ptr and raise interrupt */
	prot_ring_write_complete(dhd, prot->h2dring_ctrl_subn, flow_flush_rqst,
		DHD_FLOWRING_DEFAULT_NITEMS_POSTED_H2D);
	DHD_GENERAL_UNLOCK(dhd, flags);

	return BCME_OK;
}

static void
dhd_prot_process_flow_ring_flush_response(dhd_pub_t *dhd, void* buf, uint16 msglen)
{
	tx_flowring_flush_response_t *flow_flush_resp = (tx_flowring_flush_response_t *)buf;

	DHD_INFO(("%s Flow Flush Response status = %d \n", __FUNCTION__,
		flow_flush_resp->cmplt.status));

	dhd_bus_flow_ring_flush_response(dhd->bus, flow_flush_resp->cmplt.flow_ring_id,
		flow_flush_resp->cmplt.status);
}

int
dhd_prot_debug_info_print(dhd_pub_t *dhd)
{
	dhd_prot_t *prot = dhd->prot;
	msgbuf_ring_t *ring;
	uint16 rd, wr;
	uint32 intstatus = 0;
	uint32 intmask = 0;
	uint32 mbintstatus = 0;
	uint32 d2h_mb_data = 0;
	uint32 dma_buf_len;

	DHD_ERROR(("\n ------- DUMPING IOCTL RING RD WR Pointers ------- \r\n"));

	ring = prot->h2dring_ctrl_subn;
	dma_buf_len = RING_MAX_ITEM(ring) * RING_LEN_ITEMS(ring);
	DHD_ERROR(("CtrlPost: Mem Info: BASE(VA) %p BASE(PA) %x:%x SIZE %d \r\n",
		HOST_RING_BASE(ring), (uint32)ltoh32(PHYSADDRHI(HOST_RING_BASE_PHY(ring))),
		(uint32)ltoh32(PHYSADDRLO(HOST_RING_BASE_PHY(ring))), dma_buf_len));
	DHD_ERROR(("CtrlPost: From Host mem: RD: %d WR %d \r\n",
		RING_READ_PTR(ring), RING_WRITE_PTR(ring)));
	dhd_bus_cmn_readshared(dhd->bus, &rd, RING_READ_PTR, ring->idx);
	dhd_bus_cmn_readshared(dhd->bus, &wr, RING_WRITE_PTR, ring->idx);
	DHD_ERROR(("CtrlPost: From Shared Mem: RD: %d WR %d \r\n", rd, wr));

	ring = prot->d2hring_ctrl_cpln;
	dma_buf_len = RING_MAX_ITEM(ring) * RING_LEN_ITEMS(ring);
	DHD_ERROR(("CtrlCpl: Mem Info: BASE(VA) %p BASE(PA) %x:%x SIZE %d \r\n",
		HOST_RING_BASE(ring), (uint32)ltoh32(PHYSADDRHI(HOST_RING_BASE_PHY(ring))),
		(uint32)ltoh32(PHYSADDRLO(HOST_RING_BASE_PHY(ring))), dma_buf_len));
	DHD_ERROR(("CtrlCpl: From Host mem: RD: %d WR %d \r\n",
		RING_READ_PTR(ring), RING_WRITE_PTR(ring)));
	dhd_bus_cmn_readshared(dhd->bus, &rd, RING_READ_PTR, ring->idx);
	dhd_bus_cmn_readshared(dhd->bus, &wr, RING_WRITE_PTR, ring->idx);
	DHD_ERROR(("CtrlCpl: From Shared Mem: RD: %d WR %d \r\n", rd, wr));
	DHD_ERROR(("CtrlCpl: Expected seq num: %d \r\n", ring->seqnum));

	intstatus = si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, PCIMailBoxInt, 0, 0);
	intmask = si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, PCIMailBoxMask, 0, 0);
	mbintstatus = si_corereg(dhd->bus->sih, dhd->bus->sih->buscoreidx, PCID2H_MailBox, 0, 0);
	dhd_bus_cmn_readshared(dhd->bus, &d2h_mb_data, DTOH_MB_DATA, 0);

	DHD_ERROR(("\n ------- DUMPING INTR Status and Masks ------- \r\n"));
	DHD_ERROR(("intstatus=0x%x intmask=0x%x mbintstatus=0x%x \r\n",
		intstatus, intmask, mbintstatus));
	DHD_ERROR(("d2h_mb_data=0x%x def_intmask=0x%x \r\n", d2h_mb_data, dhd->bus->def_intmask));

	return 0;
}

int
dhd_prot_ringupd_dump(dhd_pub_t *dhd, struct bcmstrbuf *b)
{
	uint32 *ptr;
	uint32 value;
	uint32 i;
	uint8 txpush = 0;
	uint32 max_h2d_queues = dhd_bus_max_h2d_queues(dhd->bus, &txpush);

	OSL_CACHE_INV((void *)dhd->prot->d2h_dma_writeindx_buf.va,
		dhd->prot->d2h_dma_writeindx_buf_len);

	ptr = (uint32 *)(dhd->prot->d2h_dma_writeindx_buf.va);

	bcm_bprintf(b, "\n max_tx_queues %d, txpush mode %d\n", max_h2d_queues, txpush);

	bcm_bprintf(b, "\nRPTR block H2D common rings, 0x%04x\n", ptr);
	value = ltoh32(*ptr);
	bcm_bprintf(b, "\tH2D CTRL: value 0x%04x\n", value);
	ptr++;
	value = ltoh32(*ptr);
	bcm_bprintf(b, "\tH2D RXPOST: value 0x%04x\n", value);

	if (txpush) {
		ptr++;
		value = ltoh32(*ptr);
		bcm_bprintf(b, "\tH2D TXPOST value 0x%04x\n", value);
	}
	else {
		ptr++;
		bcm_bprintf(b, "RPTR block Flow rings , 0x%04x\n", ptr);
		for (i = BCMPCIE_H2D_COMMON_MSGRINGS; i < max_h2d_queues; i++) {
			value = ltoh32(*ptr);
			bcm_bprintf(b, "\tflowring ID %d: value 0x%04x\n", i, value);
			ptr++;
		}
	}

	OSL_CACHE_INV((void *)dhd->prot->h2d_dma_readindx_buf.va,
		dhd->prot->h2d_dma_readindx_buf_len);

	ptr = (uint32 *)(dhd->prot->h2d_dma_readindx_buf.va);

	bcm_bprintf(b, "\nWPTR block D2H common rings, 0x%04x\n", ptr);
	value = ltoh32(*ptr);
	bcm_bprintf(b, "\tD2H CTRLCPLT: value 0x%04x\n", value);
	ptr++;
	value = ltoh32(*ptr);
	bcm_bprintf(b, "\tD2H TXCPLT: value 0x%04x\n", value);
	ptr++;
	value = ltoh32(*ptr);
	bcm_bprintf(b, "\tD2H RXCPLT: value 0x%04x\n", value);

	return 0;
}

uint32
dhd_prot_metadatalen_set(dhd_pub_t *dhd, uint32 val, bool rx)
{
	dhd_prot_t *prot = dhd->prot;
	if (rx)
		prot->rx_metadata_offset = (uint16)val;
	else
		prot->tx_metadata_offset = (uint16)val;
	return dhd_prot_metadatalen_get(dhd, rx);
}

uint32
dhd_prot_metadatalen_get(dhd_pub_t *dhd, bool rx)
{
	dhd_prot_t *prot = dhd->prot;
	if (rx)
		return prot->rx_metadata_offset;
	else
		return prot->tx_metadata_offset;
}

uint32
dhd_prot_txp_threshold(dhd_pub_t *dhd, bool set, uint32 val)
{
	dhd_prot_t *prot = dhd->prot;
	if (set)
		prot->txp_threshold = (uint16)val;
	val = prot->txp_threshold;
	return val;
}

#ifdef DHD_RX_CHAINING
static INLINE void BCMFASTPATH
dhd_rxchain_reset(rxchain_info_t *rxchain)
{
	rxchain->pkt_count = 0;
}

static void BCMFASTPATH
dhd_rxchain_frame(dhd_pub_t *dhd, void *pkt, uint ifidx)
{
	uint8 *eh;
	uint8 prio;
	dhd_prot_t *prot = dhd->prot;
	rxchain_info_t *rxchain = &prot->rxchain;

	eh = PKTDATA(dhd->osh, pkt);
	prio = IP_TOS46(eh + ETHER_HDR_LEN) >> IPV4_TOS_PREC_SHIFT;

	/* For routers, with HNDCTF, link the packets using PKTSETCLINK, */
	/* so that the chain can be handed off to CTF bridge as is. */
	if (rxchain->pkt_count == 0) {
		/* First packet in chain */
		rxchain->pkthead = rxchain->pkttail = pkt;

		/* Keep a copy of ptr to ether_da, ether_sa and prio */
		rxchain->h_da = ((struct ether_header *)eh)->ether_dhost;
		rxchain->h_sa = ((struct ether_header *)eh)->ether_shost;
		rxchain->h_prio = prio;
		rxchain->ifidx = ifidx;
		rxchain->pkt_count++;
	} else {
		if (PKT_CTF_CHAINABLE(dhd, ifidx, eh, prio, rxchain->h_sa,
			rxchain->h_da, rxchain->h_prio)) {
			/* Same flow - keep chaining */
			PKTSETCLINK(rxchain->pkttail, pkt);
			rxchain->pkttail = pkt;
			rxchain->pkt_count++;
		} else {
			/* Different flow - First release the existing chain */
			dhd_rxchain_commit(dhd);

			/* Create a new chain */
			rxchain->pkthead = rxchain->pkttail = pkt;

			/* Keep a copy of ptr to ether_da, ether_sa and prio */
			rxchain->h_da = ((struct ether_header *)eh)->ether_dhost;
			rxchain->h_sa = ((struct ether_header *)eh)->ether_shost;
			rxchain->h_prio = prio;
			rxchain->ifidx = ifidx;
			rxchain->pkt_count++;
		}
	}

	if ((!ETHER_ISMULTI(rxchain->h_da)) &&
		((((struct ether_header *)eh)->ether_type == HTON16(ETHER_TYPE_IP)) ||
		(((struct ether_header *)eh)->ether_type == HTON16(ETHER_TYPE_IPV6)))) {
		PKTSETCHAINED(dhd->osh, pkt);
		PKTCINCRCNT(rxchain->pkthead);
		PKTCADDLEN(rxchain->pkthead, PKTLEN(dhd->osh, pkt));
	} else {
		dhd_rxchain_commit(dhd);
		return;
	}

	/* If we have hit the max chain length, dispatch the chain and reset */
	if (rxchain->pkt_count >= DHD_PKT_CTF_MAX_CHAIN_LEN) {
		dhd_rxchain_commit(dhd);
	}
}

static void BCMFASTPATH
dhd_rxchain_commit(dhd_pub_t *dhd)
{
	dhd_prot_t *prot = dhd->prot;
	rxchain_info_t *rxchain = &prot->rxchain;

	if (rxchain->pkt_count == 0)
		return;

	/* Release the packets to dhd_linux */
	dhd_bus_rx_frame(dhd->bus, rxchain->pkthead, rxchain->ifidx, rxchain->pkt_count);

	/* Reset the chain */
	dhd_rxchain_reset(rxchain);
}
#endif /* DHD_RX_CHAINING */

static void
dhd_prot_ring_clear(msgbuf_ring_t* ring)
{
	uint16 size;

	DHD_TRACE(("%s\n", __FUNCTION__));

	size = ring->ringmem->max_item * ring->ringmem->len_items;
	ASSERT(MODX((unsigned long)ring->ring_base.va, DMA_ALIGN_LEN) == 0);
	OSL_CACHE_INV((void *) ring->ring_base.va, size);
	bzero(ring->ring_base.va, size);

	OSL_CACHE_FLUSH((void *) ring->ring_base.va, size);

	bzero(ring->ringstate, sizeof(*ring->ringstate));
}

void
dhd_prot_clear(dhd_pub_t *dhd)
{
	struct dhd_prot *prot = dhd->prot;

	DHD_TRACE(("%s\n", __FUNCTION__));

	if (prot == NULL)
		return;

	if (prot->h2dring_txp_subn)
		dhd_prot_ring_clear(prot->h2dring_txp_subn);
	if (prot->h2dring_rxp_subn)
		dhd_prot_ring_clear(prot->h2dring_rxp_subn);
	if (prot->h2dring_ctrl_subn)
		dhd_prot_ring_clear(prot->h2dring_ctrl_subn);
	if (prot->d2hring_tx_cpln)
		dhd_prot_ring_clear(prot->d2hring_tx_cpln);
	if (prot->d2hring_rx_cpln)
		dhd_prot_ring_clear(prot->d2hring_rx_cpln);
	if (prot->d2hring_ctrl_cpln)
		dhd_prot_ring_clear(prot->d2hring_ctrl_cpln);

	if (prot->retbuf.va) {
		OSL_CACHE_INV((void *) prot->retbuf.va, IOCT_RETBUF_SIZE);
		bzero(prot->retbuf.va, IOCT_RETBUF_SIZE);
		OSL_CACHE_FLUSH((void *) prot->retbuf.va, IOCT_RETBUF_SIZE);
	}

	if (prot->ioctbuf.va) {
		OSL_CACHE_INV((void *) prot->ioctbuf.va, IOCT_RETBUF_SIZE);
		bzero(prot->ioctbuf.va, IOCT_RETBUF_SIZE);
		OSL_CACHE_FLUSH((void *) prot->ioctbuf.va, IOCT_RETBUF_SIZE);
	}

	if (prot->d2h_dma_scratch_buf.va) {
		OSL_CACHE_INV((void *)prot->d2h_dma_scratch_buf.va, DMA_D2H_SCRATCH_BUF_LEN);
		bzero(prot->d2h_dma_scratch_buf.va, DMA_D2H_SCRATCH_BUF_LEN);
		OSL_CACHE_FLUSH((void *)prot->d2h_dma_scratch_buf.va, DMA_D2H_SCRATCH_BUF_LEN);
	}

	if (prot->h2d_dma_readindx_buf.va) {
		OSL_CACHE_INV((void *)prot->h2d_dma_readindx_buf.va,
			prot->h2d_dma_readindx_buf_len);
		bzero(prot->h2d_dma_readindx_buf.va,
			prot->h2d_dma_readindx_buf_len);
		OSL_CACHE_FLUSH((void *)prot->h2d_dma_readindx_buf.va,
			prot->h2d_dma_readindx_buf_len);
	}

	if (prot->h2d_dma_writeindx_buf.va) {
		OSL_CACHE_INV((void *)prot->h2d_dma_writeindx_buf.va,
			prot->h2d_dma_writeindx_buf_len);
		bzero(prot->h2d_dma_writeindx_buf.va, prot->h2d_dma_writeindx_buf_len);
		OSL_CACHE_FLUSH((void *)prot->h2d_dma_writeindx_buf.va,
			prot->h2d_dma_writeindx_buf_len);
	}

	if (prot->d2h_dma_readindx_buf.va) {
		OSL_CACHE_INV((void *)prot->d2h_dma_readindx_buf.va,
			prot->d2h_dma_readindx_buf_len);
		bzero(prot->d2h_dma_readindx_buf.va, prot->d2h_dma_readindx_buf_len);
		OSL_CACHE_FLUSH((void *)prot->d2h_dma_readindx_buf.va,
			prot->d2h_dma_readindx_buf_len);
	}

	if (prot->d2h_dma_writeindx_buf.va) {
		OSL_CACHE_INV((void *)prot->d2h_dma_writeindx_buf.va,
			prot->d2h_dma_writeindx_buf_len);
		bzero(prot->d2h_dma_writeindx_buf.va, prot->d2h_dma_writeindx_buf_len);
		OSL_CACHE_FLUSH((void *)prot->d2h_dma_writeindx_buf.va,
			prot->d2h_dma_writeindx_buf_len);
	}

	prot->rx_metadata_offset = 0;
	prot->tx_metadata_offset = 0;

	prot->rxbufpost = 0;
	prot->cur_event_bufs_posted = 0;
	prot->cur_ioctlresp_bufs_posted = 0;

	prot->active_tx_count = 0;
	prot->data_seq_no = 0;
	prot->ioctl_seq_no = 0;
	prot->pending = 0;
	prot->lastcmd = 0;

	prot->ioctl_trans_id = 1;

	/* dhd_flow_rings_init is located at dhd_bus_start,
	 *  so when stopping bus, flowrings shall be deleted
	 */
	dhd_flow_rings_deinit(dhd);
	NATIVE_TO_PKTID_CLEAR(prot->pktid_map_handle);
}
