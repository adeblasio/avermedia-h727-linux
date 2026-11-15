// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * avertm62.c - Linux V4L2 PCIe driver for AVerMedia H727 / C027
 *              (Trident Microsystems TM6200 / TM6202 PCIe AV Bridge, 1a0a:6202)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/version.h>
#include <linux/videodev2.h>
#include <media/v4l2-common.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>
#include <media/dvb_frontend.h>
#include <media/dvbdev.h>
#include "kheaders/drxk.h"
#include "kheaders/tda18271.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
#define avertm62_timer_del_sync(t) timer_delete_sync(t)
#else
#define avertm62_timer_del_sync(t) del_timer_sync(t)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
#define avertm62_from_timer(var, callback_timer, timer_fieldname) \
	timer_container_of(var, callback_timer, timer_fieldname)
#else
#define avertm62_from_timer(var, callback_timer, timer_fieldname) \
	from_timer(var, callback_timer, timer_fieldname)
#endif

#define DRV_NAME	"avertm62"
#define DRV_VERSION	"0.5.0"

/*
 * Internal colorbar generator (Windows 0x52f2c): 0x0188[3]=1 enables it,
 * 0x0188[2:0] selects the pattern, and 0x0760[18:16] is cleared. While it is
 * on, Windows reports the source as 720x288 per field (576i), so capture
 * with a 720x576 format. -1 = off (live video).
 */
static int test_pattern = -1;
module_param(test_pattern, int, 0644);
MODULE_PARM_DESC(test_pattern,
		 "Internal colorbar generator pattern 0-7 (-1 = off, live input). Use a 720x576 format.");

/*
 * Override for 0x0760[18:16] (capture clock/source select) at stream start.
 * Live video normally uses 1 and the colorbar generator uses 0. With the
 * generator, DMA delivers fields; with live HDMI and 1, it never starts.
 * -1 = default for the mode.
 */
static int clk_src = -1;
module_param(clk_src, int, 0644);
MODULE_PARM_DESC(clk_src, "Force 0x0760[18:16] at stream start (0-7, -1 = default)");

/* 1 = old behaviour (set 0x019c[21:20,17:16]); default clears them like the Windows video path */
static int str_route;
module_param(str_route, int, 0644);
MODULE_PARM_DESC(str_route, "Set 0x019c[21:20,17:16] TS/audio route bits (0 = clear, Windows video path; 1 = set)");

#define AVERTM62_INPUT_HDMI		0
#define AVERTM62_INPUT_COMPOSITE	1	/* breakout socket, TM6202 internal decoder */
#define AVERTM62_NUM_INPUTS		2

/* Composite is PAL only for now: 720x576, two 288-line fields per frame */
#define AVERTM62_PAL_WIDTH		720
#define AVERTM62_PAL_HEIGHT		576

/*
 * Digital TV (DVB-T/C): Micronas DRX-K demodulator (I2C0 0x52, reset on
 * GPIO5) + NXP TDA18271C2 tuner (I2C0 0xc0, directly on the bus, not behind
 * the DRX-K I2C gate). Board settings from the Windows INF. Off by default
 * while the transport-stream DMA path is still being worked out.
 */
static int dvb;
module_param(dvb, int, 0444);
MODULE_PARM_DESC(dvb, "Register the DVB-T/C frontend (DRX-K + TDA18271): 1 = on");

static char *drxk_fw;
module_param(drxk_fw, charp, 0444);
MODULE_PARM_DESC(drxk_fw, "DRX-K microcode file to load (default: none, use the chip's ROM code)");

DVB_DEFINE_MOD_OPT_ADAPTER_NR(adapter_nr);

static int drxk_parallel;
module_param(drxk_parallel, int, 0444);
MODULE_PARM_DESC(drxk_parallel, "DRX-K transport stream output: 0 = serial (default), 1 = parallel");

/*
 * Debug: read-only sysfs files exposing the DMA buffers, so register-level
 * experiments (e.g. finding the TS DMA route) can be run from user space
 * with tm62reg. Off by default.
 */
static int debug_sysfs;
module_param(debug_sysfs, int, 0444);
MODULE_PARM_DESC(debug_sysfs, "Expose dbg_pt / dbg_hwbuf sysfs files for DMA experiments");

#define PCI_VENDOR_ID_TRIDENT_TM62	0x1a0a
#define PCI_DEVICE_ID_TRIDENT_TM6200	0x6200
#define PCI_DEVICE_ID_TRIDENT_TM6202	0x6202

/* BAR0 MMIO Register Offsets */
#define TM62_REG_AUD_CLK_A		0x002c
#define TM62_REG_AUD_CLK_B		0x0030
#define TM62_REG_SYS_RST		0x003c	/* [24] PLL/sys config unlock */

#define TM62_REG_PLL_CTRL0		0x0160
#define TM62_REG_PLL_CTRL1		0x0164
#define TM62_REG_PLL_CTRL2		0x0168

/* Channel 0 (Analog) Video Capture */
#define TM62_REG_CH0_CROP_H		0x0170
#define TM62_REG_CH0_CROP_V		0x0174
#define TM62_REG_CH0_VID_CTRL		0x0178
#define TM62_REG_CH0_VID_TIMING		0x017c

/* Channel 1 (Digital/HDMI) Video Capture */
#define TM62_REG_CH1_CROP_H		0x0180	/* [10:0] src_h, [26:16] src_w, [28] prog */
#define TM62_REG_CH1_CROP_V		0x0184	/* [10:0] dst_h, [26:16] dst_w */
#define TM62_REG_CH1_VID_CTRL		0x0188	/* [2:0] mode, [3] en, [5] int, [30] en2, [31] go */
#define TM62_REG_CH1_VID_TIMING		0x018c	/* [11:0] h, [27:16] w, [28] de, [31] lock_status */
#define TM62_REG_VID_ROUTE		0x0190	/* [0] hdmi_en, [1] vsync_inv, [2] hsync_inv, [5] comp_en, [6] ch1_cap_en */
#define TM62_REG_VID_TIMING_REF		0x0194	/* [10:0] h_ref, [26:16] v_ref */
#define TM62_REG_VID_TIMING_SYNC	0x0198	/* [10:0] sync_ref, [26:16] de_ref */
#define TM62_REG_STREAM_ROUTE		0x019c	/* [7:0] timing_ctl, [15:8] clk_ctl, [20] str2_en, [21] str2_sel */
#define TM62_REG_CH1_VID_TIMING_CFG	0x01a0	/* [11:0] h_offset, [27:16] active_w */

#define TM62_REG_DMA_GLB_CFG		0x0280	/* [9] dma_clk_dis */
#define TM62_REG_AUD_PATH_A		0x0290
#define TM62_REG_AUD_PATH_B		0x0294

/* Channel 0 Video DMA Engine (Base: 0x0300, FIFO: 0x0500) */
#define TM62_REG_CH0_DMA_CFG		0x0300
#define TM62_REG_CH0_SLOT0_STS		0x0304
#define TM62_REG_CH0_SLOT0_PTA		0x0318
#define TM62_REG_CH0_FRAME_BYTES	0x0390
#define TM62_REG_CH0_FMT_CFG		0x0394
#define TM62_REG_CH0_DMA_TIMER		0x03a0
#define TM62_REG_CH0_DMA_THRESH		0x03a8	/* [30:16] 0x7fff, [31] 1 */
#define TM62_REG_CH0_DMA_MISC		0x03b4	/* [23] clk_en */
#define TM62_REG_CH0_SCALE_H		0x03c0
#define TM62_REG_CH1_SCALE_H		0x03d0	/* [10:0] dst_w, [27:16] src_w */
#define TM62_REG_CH0_DESC_CTRL		0x0500
#define TM62_REG_CH0_DESC_PTA		0x0504
#define TM62_REG_CH0_FIFO_CNT		0x0508
#define TM62_REG_CH0_DMA_START		0x050c

/* Channel 1 Video DMA Engine (Base: 0x0400, FIFO: 0x0600) */
#define TM62_REG_CH1_DMA_CFG		0x0400	/* [11:0] stride, [24] rst, [31] enable */
#define TM62_REG_CH1_SLOT0_STS		0x0404	/* [21:0] bytes, [22] done, [31:24] count */
#define TM62_REG_CH1_SLOT1_STS		0x0408
#define TM62_REG_CH1_SLOT2_STS		0x040c
#define TM62_REG_CH1_SLOT3_STS		0x0410
#define TM62_REG_CH1_SLOT4_STS		0x0414
#define TM62_REG_CH1_SLOT0_PTA		0x0418	/* Completed pt_phys >> 12 */
#define TM62_REG_CH1_SLOT1_PTA		0x041c
#define TM62_REG_CH1_SLOT2_PTA		0x0420
#define TM62_REG_CH1_SLOT3_PTA		0x0424
#define TM62_REG_CH1_SLOT4_PTA		0x0428
#define TM62_REG_CH1_FRAME_BYTES	0x0490	/* Frame byte count */
#define TM62_REG_CH1_FMT_CFG		0x0494	/* [15:0] pkts, [19:18] ch, [20] prog, [22] ch1, [25:24] pixfmt, [31] stop */
#define TM62_REG_CH1_TIMESTAMP		0x0498
#define TM62_REG_CH1_DMA_TIMER		0x04a0	/* [29:0] period (0x7698000/fps), [30] en, [31] ack */
#define TM62_REG_CH1_DMA_MISC		0x04b4	/* [23] clk_en */

#define TM62_REG_CH1_DESC_CTRL		0x0600	/* [22:0] buf_bytes, [30] fifo_run, [31] push */
#define TM62_REG_CH1_DESC_PTA		0x0604	/* [19:0] pt_phys >> 12, [31:20] page_off */
#define TM62_REG_CH1_FIFO_CNT		0x0608	/* [3:0] occupancy (0..8) */
#define TM62_REG_DMA_START		0x060c	/* [0] 1 = FIFO reset pulse */

/* VBI DMA Engines */
#define TM62_REG_CH0_VBI_DMA_CFG	0x0330
#define TM62_REG_CH0_VBI_DESC_CTRL	0x0530
#define TM62_REG_CH1_VBI_DMA_CFG	0x0430
#define TM62_REG_CH1_VBI_DESC_CTRL	0x0630

/* Hardware I2C Masters */
#define TM62_REG_I2C0_TX		0x0700
#define TM62_REG_I2C0_RX		0x0704
#define TM62_REG_I2C0_CTRL		0x0708
#define TM62_REG_I2C0_STATUS		0x070c

#define TM62_REG_I2C1_TX		0x0710
#define TM62_REG_I2C1_RX		0x0714
#define TM62_REG_I2C1_CTRL		0x0718
#define TM62_REG_I2C1_STATUS		0x071c

/* GPIO & Clock/Power & Interrupts */
#define TM62_REG_GPIO_DIR		0x0750	/* [7:0] 1=output, 0=input */
#define TM62_REG_GPIO_OUT		0x0754	/* [7:0] output value */
#define TM62_REG_GPIO_IN		0x0758	/* [7:0] input value */
#define TM62_REG_CLK_CFG		0x0760
#define TM62_REG_CLK_GATE		0x0764	/* [15:0] module clk enable, [31] global en */
#define TM62_REG_GPIO_INT		0x0768	/* [15:8] gpio irq en */

#define TM62_REG_IRQ_MASK		0x0780	/* 0=enabled, 1=masked; [31]=global_dis */
#define TM62_REG_IRQ_ACK		0x0784	/* Write 1 to clear pending IRQ bit */
#define TM62_REG_IRQ_STATUS		0x0788	/* Pending IRQ bits */

#define TM62_IRQ_CH0_VID_DMA		BIT(0)	/* Internal IRQ 0x14: Ch0 Video DMA slot */
#define TM62_IRQ_CH1_VID_DMA		BIT(1)	/* Internal IRQ 0x13: Ch1 Video DMA slot */
#define TM62_IRQ_I2C0_DONE		BIT(6)	/* Internal IRQ: I2C Bus 0 done */
#define TM62_IRQ_I2C1_DONE		BIT(7)	/* Internal IRQ: I2C Bus 1 done */
#define TM62_IRQ_CH0_DMA		BIT(16)	/* Internal IRQ 0x0c: Ch0 DMA timer (0x03a0) */
#define TM62_IRQ_CH1_DMA		BIT(17)	/* Internal IRQ 0x0b: Ch1 DMA timer (0x04a0) */
#define TM62_IRQ_FRAME			BIT(20)	/* Internal IRQ 0x08: Video Frame / VSYNC */
#define TM62_IRQ_GLOBAL_DIS		BIT(31)
#define TM62_IRQ_MASK_ALL		0x001fffffU

/* Number of pre-allocated 4MB hardware scatter-gather page-table buffers */
#define AVERTM62_HW_BUFS		8
#define AVERTM62_BUFS_PER_CH		4
#define AVERTM62_MAX_WIDTH		1920
#define AVERTM62_MAX_HEIGHT		1080
#define AVERTM62_MAX_FRAME_SIZE		(AVERTM62_MAX_WIDTH * AVERTM62_MAX_HEIGHT * 2)
#define AVERTM62_PT_PAGES		1024	/* 1024 * 4KB = 4MB per HW buffer */

enum avertm62_hdmi_chip {
	HDMI_CHIP_NONE = 0,
	HDMI_CHIP_SII9011,
	HDMI_CHIP_IT6603,
};

struct avertm62_hw_buf {
	u32			*pt_cpu;	/* 4KB page table virtual addr */
	dma_addr_t		pt_dma;		/* 4KB page table physical addr */
	void			*page_cpu[AVERTM62_PT_PAGES];
	dma_addr_t		page_dma[AVERTM62_PT_PAGES];
	u32			num_pages;
};

struct avertm62_vb2_buf {
	struct vb2_v4l2_buffer	vb;
	struct list_head	list;
};

struct avertm62_i2c_bus {
	struct avertm62_dev	*dev;
	int			bus_id;
	u32			reg_base;
	struct i2c_adapter	adap;
};

struct avertm62_dev {
	struct pci_dev		*pdev;
	void __iomem		*mmio;
	struct v4l2_device	v4l2_dev;
	struct video_device	vdev;
	struct vb2_queue	vb2_q;
	struct mutex		lock;
	struct mutex		i2c_lock;
	spinlock_t		qlock;

	struct avertm62_i2c_bus	i2c_bus[2];
	enum avertm62_hdmi_chip	hdmi_chip;
	u8			eeprom_id;
	u32			sii_vid_mode;	/* [dev+0x1b0] from 0x16646 */

	/* Video format & state */
	u32			width;
	u32			height;
	u32			pixelformat;
	u32			bytesperline;
	u32			sizeimage;
	bool			interlaced;
	u32			fps;
	u32			input;		/* AVERTM62_INPUT_* */
	u32			field_count;	/* composite: fields woven into the current frame */
	int			active_pattern;	/* test_pattern latched at stream start, -1 = live */

	/* Streaming & DMA state */
	bool			streaming;
	u32			sequence;
	u32			irq_count;
	u32			last_irq_pending;
	u32			dma_frame_count;
	u32			cur_frame_bytes;
	u32			cur_slice_mask;
	u32			last_s0_pta[2];
	int			active_dma_ch;
	struct list_head	buf_list;
	struct avertm62_hw_buf	hw_bufs[AVERTM62_HW_BUFS];
	u32			hw_push_idx;
	struct timer_list	fallback_timer;

	/* Digital TV */
	struct dvb_adapter	dvb_adap;
	struct dvb_frontend	*fe;
	bool			dvb_adap_registered;
	bool			dvb_fe_registered;
};

/* MMIO helpers */
static inline u32 tm62_read32(struct avertm62_dev *dev, u32 reg)
{
	return readl(dev->mmio + reg);
}

static inline void tm62_write32(struct avertm62_dev *dev, u32 reg, u32 val)
{
	writel(val, dev->mmio + reg);
}

static inline void tm62_write_bf(struct avertm62_dev *dev, u32 reg,
				 u8 lsb, u8 width, u32 val)
{
	u32 mask = (width >= 32) ? 0xffffffffU : (((1U << width) - 1U) << lsb);
	u32 cur = tm62_read32(dev, reg);

	cur = (cur & ~mask) | ((val << lsb) & mask);
	tm62_write32(dev, reg, cur);
}

static inline u32 tm62_read_bf(struct avertm62_dev *dev, u32 reg,
			       u8 lsb, u8 width)
{
	u32 mask = (width >= 32) ? 0xffffffffU : ((1U << width) - 1U);

	return (tm62_read32(dev, reg) >> lsb) & mask;
}

/* GPIO Controller */
static void tm62_gpio_dir(struct avertm62_dev *dev, u8 pin, bool output)
{
	u32 val;

	if (pin > 7)
		return;
	val = tm62_read32(dev, TM62_REG_GPIO_DIR);
	if (output)
		val |= BIT(pin);
	else
		val &= ~BIT(pin);
	tm62_write32(dev, TM62_REG_GPIO_DIR, val);
}

static void tm62_gpio_set(struct avertm62_dev *dev, u8 pin, bool high)
{
	u32 val;

	if (pin > 7)
		return;
	val = tm62_read32(dev, TM62_REG_GPIO_OUT);
	if (high)
		val |= BIT(pin);
	else
		val &= ~BIT(pin);
	tm62_write32(dev, TM62_REG_GPIO_OUT, val);
	if (high)
		udelay(10);
}

/* Hardware I2C Master Engine */
static int tm62_i2c_check_hw_status(struct avertm62_dev *dev, u32 base)
{
	u32 state;

	if (tm62_read_bf(dev, base + 0x0c, 11, 1) != 0)
		return 4; /* Busy */
	if (tm62_read_bf(dev, base + 0x0c, 9, 1) != 0)
		return 4; /* Busy */

	state = tm62_read_bf(dev, base + 0x0c, 12, 3);
	if (state == 0 || state == 1 || state == 7)
		return 1; /* Success (ACK) */
	return 0; /* Error / NACK */
}

static void tm62_i2c_reset_bus(struct avertm62_dev *dev, int bus)
{
	u32 base = (bus == 1) ? TM62_REG_I2C1_TX : TM62_REG_I2C0_TX;

	tm62_write_bf(dev, base + 0x08, 19, 1, 1);
	tm62_write_bf(dev, 0x07a0, (bus == 1) ? 9 : 8, 1, 1);
	udelay(50);
	tm62_write_bf(dev, base + 0x08, 19, 1, 0);
	tm62_write_bf(dev, 0x07a0, (bus == 1) ? 9 : 8, 1, 0);
	tm62_write_bf(dev, base + 0x0c, 16, 10, 0x46);
}

static int tm62_i2c_wait_complete(struct avertm62_dev *dev, int bus, u32 base)
{
	u32 done_bit = (bus == 1) ? TM62_IRQ_I2C1_DONE : TM62_IRQ_I2C0_DONE;
	int i, st = 4;

	udelay(50);
	for (i = 0; i < 200; i++) {
		u32 irq_sts = tm62_read32(dev, TM62_REG_IRQ_STATUS);

		if (irq_sts & done_bit) {
			tm62_write32(dev, TM62_REG_IRQ_ACK, done_bit);
			break;
		}
		udelay(50);
	}
	if (i == 200)
		return 2; /* Timeout */

	for (i = 0; i < 20; i++) {
		st = tm62_i2c_check_hw_status(dev, base);
		if (st != 4)
			break;
		udelay(50);
	}
	udelay(40);
	return st;
}

static int tm62_i2c_write_chunk4(struct avertm62_dev *dev, int bus,
				 u8 addr_8bit, const u8 *buf,
				 u32 flags, int chunk_len)
{
	u32 base = (bus == 1) ? TM62_REG_I2C1_TX : TM62_REG_I2C0_TX;
	u32 done_bit = (bus == 1) ? TM62_IRQ_I2C1_DONE : TM62_IRQ_I2C0_DONE;
	u32 word = 0, ctrl = 0;
	int b, st;

	for (b = 0; b < chunk_len && b < 4; b++)
		word |= (u32)buf[b] << (b * 8);

	tm62_write32(dev, base + 0x00, word);

	ctrl = (u32)addr_8bit & 0xffff;
	ctrl |= ((u32)(chunk_len & 0x7)) << 29;
	if ((flags >> 3) & 1)
		ctrl |= BIT(18);
	if ((flags >> 1) & 1)
		ctrl |= BIT(20);
	if ((flags >> 0) & 1)
		ctrl |= BIT(21);
	if ((flags >> 2) & 1)
		ctrl |= BIT(25);
	ctrl |= BIT(22);

	tm62_write32(dev, TM62_REG_IRQ_ACK, done_bit);

	if (dev->pdev->revision == 0)
		tm62_write_bf(dev, 0x0774, 16, 1, 1);

	tm62_write32(dev, base + 0x08, ctrl | BIT(16));

	if (dev->pdev->revision == 0) {
		tm62_write_bf(dev, 0x0774, 16, 1, 0);
		tm62_write_bf(dev, 0x0774, 16, 1, 1);
		tm62_write_bf(dev, 0x0774, 16, 1, 0);
	}

	st = tm62_i2c_wait_complete(dev, bus, base);
	if (st != 1 && st != 0) {
		tm62_i2c_reset_bus(dev, bus);
		return -ENXIO;
	}
	if (flags & 0x10)
		udelay(200);

	return 0;
}

static int tm62_i2c_read_chunk4(struct avertm62_dev *dev, int bus,
				u8 addr_8bit, u8 *buf,
				u32 flags, int chunk_len)
{
	u32 base = (bus == 1) ? TM62_REG_I2C1_TX : TM62_REG_I2C0_TX;
	u32 done_bit = (bus == 1) ? TM62_IRQ_I2C1_DONE : TM62_IRQ_I2C0_DONE;
	u32 word, ctrl = 0;
	int b, st;

	ctrl = (u32)addr_8bit & 0xffff;
	ctrl |= ((u32)(chunk_len & 0x7)) << 26;
	if ((flags >> 3) & 1)
		ctrl |= BIT(18);
	if ((flags >> 1) & 1)
		ctrl |= BIT(20);
	if ((flags >> 0) & 1)
		ctrl |= BIT(21);
	if ((flags >> 2) & 1)
		ctrl |= BIT(24);
	ctrl |= BIT(22);
	ctrl |= BIT(23);

	tm62_write32(dev, TM62_REG_IRQ_ACK, done_bit);

	if (dev->pdev->revision == 0)
		tm62_write_bf(dev, 0x0774, 16, 1, 1);

	tm62_write32(dev, base + 0x08, ctrl | BIT(16));

	if (dev->pdev->revision == 0) {
		tm62_write_bf(dev, 0x0774, 16, 1, 0);
		tm62_write_bf(dev, 0x0774, 16, 1, 1);
		tm62_write_bf(dev, 0x0774, 16, 1, 0);
	}

	st = tm62_i2c_wait_complete(dev, bus, base);
	if (st != 1 && st != 0) {
		tm62_i2c_reset_bus(dev, bus);
		return -ENXIO;
	}

	word = tm62_read32(dev, base + 0x04);
	for (b = 0; b < chunk_len && b < 4; b++)
		buf[b] = (word >> (b * 8)) & 0xff;

	return 0;
}

static int tm62_i2c_write_bytes(struct avertm62_dev *dev, int bus,
				u8 addr_8bit, const u8 *buf, int len)
{
	u32 base = (bus == 1) ? TM62_REG_I2C1_TX : TM62_REG_I2C0_TX;
	u32 flags = 0x09; /* START | ADDR */
	int i = 0, ret;

	if (len <= 0)
		return 0;

	tm62_write_bf(dev, base + 0x0c, 16, 10, 0x46);

	while (len - i > 4) {
		ret = tm62_i2c_write_chunk4(dev, bus, addr_8bit,
					    buf + i, flags | 0x0c, 4);
		if (ret < 0)
			return ret;
		flags = 0;
		i += 4;
	}

	return tm62_i2c_write_chunk4(dev, bus, addr_8bit,
				     buf + i, flags | 0x12, len - i);
}

static int tm62_i2c_read_regs(struct avertm62_dev *dev, int bus,
			      u8 addr_8bit, const u8 *sub, int sub_len,
			      u8 *buf, int len)
{
	u32 base = (bus == 1) ? TM62_REG_I2C1_TX : TM62_REG_I2C0_TX;
	u32 flags;
	int i = 0, ret;

	tm62_write_bf(dev, base + 0x0c, 16, 10, 0x46);

	if (sub && sub_len > 0) {
		if (sub_len == 1) {
			ret = tm62_i2c_write_chunk4(dev, bus, addr_8bit,
						    sub, 0x09, 1);
			if (ret < 0)
				return ret;
		} else {
			flags = 0x0d;
			while (sub_len - i > 4) {
				ret = tm62_i2c_write_chunk4(dev, bus, addr_8bit,
							    sub + i, flags, 4);
				if (ret < 0)
					return ret;
				flags = 0;
				i += 4;
			}
			ret = tm62_i2c_write_chunk4(dev, bus, addr_8bit,
						    sub + i, flags & ~0x04U,
						    sub_len - i);
			if (ret < 0)
				return ret;
		}
	}

	i = 0;
	flags = 0x09; /* START | ADDR */
	while (len - i > 4) {
		ret = tm62_i2c_read_chunk4(dev, bus, addr_8bit,
					   buf + i, flags | 0x0c, 4);
		if (ret < 0)
			return ret;
		flags = 0;
		i += 4;
	}

	return tm62_i2c_read_chunk4(dev, bus, addr_8bit,
				    buf + i, flags | 0x02, len - i);
}

static int tm62_i2c_write_reg8(struct avertm62_dev *dev, int bus,
			       u8 addr_8bit, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };

	return tm62_i2c_write_bytes(dev, bus, addr_8bit, buf, 2);
}

static int tm62_i2c_read_reg8(struct avertm62_dev *dev, int bus,
			      u8 addr_8bit, u8 reg, u8 *val)
{
	return tm62_i2c_read_regs(dev, bus, addr_8bit, &reg, 1, val, 1);
}

/* Linux i2c_adapter algorithm wrapper */
static int avertm62_i2c_xfer(struct i2c_adapter *adap,
			     struct i2c_msg *msgs, int num)
{
	struct avertm62_i2c_bus *ibus = i2c_get_adapdata(adap);
	struct avertm62_dev *dev = ibus->dev;
	int ret = 0, i;

	mutex_lock(&dev->i2c_lock);
	for (i = 0; i < num; i++) {
		u8 addr8 = msgs[i].addr << 1;

		if (i + 1 < num &&
		    !(msgs[i].flags & I2C_M_RD) &&
		    (msgs[i + 1].flags & I2C_M_RD) &&
		    msgs[i].addr == msgs[i + 1].addr) {
			ret = tm62_i2c_read_regs(dev, ibus->bus_id, addr8,
						 msgs[i].buf, msgs[i].len,
						 msgs[i + 1].buf,
						 msgs[i + 1].len);
			if (ret < 0)
				break;
			i++;
		} else if (msgs[i].flags & I2C_M_RD) {
			ret = tm62_i2c_read_regs(dev, ibus->bus_id, addr8,
						 NULL, 0,
						 msgs[i].buf, msgs[i].len);
			if (ret < 0)
				break;
		} else {
			ret = tm62_i2c_write_bytes(dev, ibus->bus_id, addr8,
						   msgs[i].buf, msgs[i].len);
			if (ret < 0)
				break;
		}
	}
	mutex_unlock(&dev->i2c_lock);

	return (ret < 0) ? ret : num;
}

static u32 avertm62_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm avertm62_i2c_algo = {
	.master_xfer   = avertm62_i2c_xfer,
	.functionality = avertm62_i2c_func,
};

/* TM6202 Global Power, Clock & PLL Initialization */
static void tm62_hw_power_and_pll_init(struct avertm62_dev *dev)
{
	tm62_write32(dev, TM62_REG_IRQ_MASK, TM62_IRQ_MASK_ALL);
	tm62_write32(dev, TM62_REG_IRQ_ACK, 0x001fffff);
	tm62_write_bf(dev, TM62_REG_GPIO_INT, 8, 8, 0);

	/* 1. Unlock system/PLL configuration */
	tm62_write_bf(dev, TM62_REG_SYS_RST, 24, 1, 1);
	udelay(200);

	/* 2. Bring up main PLL */
	tm62_write32(dev, TM62_REG_CLK_GATE, 0x80000001);
	tm62_write_bf(dev, TM62_REG_PLL_CTRL1, 22, 2, 1);
	tm62_write_bf(dev, TM62_REG_PLL_CTRL1, 20, 2, 1);
	tm62_write_bf(dev, TM62_REG_PLL_CTRL0, 0, 1, 1);
	tm62_write_bf(dev, TM62_REG_CLK_GATE, 0, 1, 0);
	udelay(250);

	tm62_write_bf(dev, TM62_REG_PLL_CTRL1, 20, 2, 0);
	tm62_write_bf(dev, TM62_REG_PLL_CTRL1, 22, 2, 2);
	tm62_write_bf(dev, TM62_REG_CLK_GATE, 0, 1, 1);

	tm62_write_bf(dev, TM62_REG_CLK_CFG, 27, 4, 0x0b);
	tm62_write_bf(dev, TM62_REG_PLL_CTRL0, 0, 1, 0);
	tm62_write_bf(dev, TM62_REG_PLL_CTRL0, 2, 1, 1);
	tm62_write_bf(dev, TM62_REG_PLL_CTRL0, 13, 3, 3);
	tm62_write_bf(dev, TM62_REG_PLL_CTRL0, 8, 2, 3);
	tm62_write_bf(dev, TM62_REG_PLL_CTRL0, 21, 3, 2);
	tm62_write_bf(dev, TM62_REG_PLL_CTRL0, 16, 3, 1);
	tm62_write_bf(dev, TM62_REG_CLK_CFG, 16, 3, 1);

	/* Lock system/PLL configuration */
	tm62_write_bf(dev, TM62_REG_SYS_RST, 24, 1, 0);
	udelay(200);

	/* 3. Enable functional module clocks & DMA clocks (D0 active state = 0x8000ffff) */
	tm62_write32(dev, TM62_REG_CLK_GATE, 0x8000ffff);
	tm62_write_bf(dev, TM62_REG_DMA_GLB_CFG, 9, 1, 0);
	tm62_write_bf(dev, TM62_REG_CH0_DMA_MISC, 23, 1, 1);
	tm62_write_bf(dev, TM62_REG_CH1_DMA_MISC, 23, 1, 1);

	tm62_write_bf(dev, TM62_REG_DMA_GLB_CFG, 16, 5, 0x15);
	tm62_write_bf(dev, TM62_REG_DMA_GLB_CFG, 24, 5, 0x15);

	/* 4. Enable Audio & Video path clocks */
	tm62_write_bf(dev, TM62_REG_AUD_PATH_A, 19, 1, 0);
	tm62_write_bf(dev, TM62_REG_AUD_PATH_A, 18, 1, 0);
	tm62_write_bf(dev, TM62_REG_AUD_PATH_B, 17, 1, 0);
	tm62_write_bf(dev, TM62_REG_AUD_PATH_B, 18, 1, 0);
	tm62_write_bf(dev, TM62_REG_AUD_PATH_B, 19, 1, 0);
	tm62_write_bf(dev, TM62_REG_AUD_PATH_B, 21, 1, 0);
	tm62_write_bf(dev, TM62_REG_AUD_PATH_B, 22, 1, 0);
	tm62_write_bf(dev, TM62_REG_AUD_PATH_B, 23, 1, 0);

	/* 5. AVerMedia H727 D0 wake sequence */
	tm62_i2c_write_reg8(dev, 1, 0x80, 0x00, 0x5a);
	tm62_i2c_write_reg8(dev, 1, 0x80, 0x01, 0x00);
	tm62_i2c_write_reg8(dev, 1, 0x80, 0x00, 0x00);

	tm62_gpio_dir(dev, 4, true);
	tm62_gpio_set(dev, 4, false);
	tm62_gpio_dir(dev, 5, true);
	tm62_gpio_set(dev, 5, false);

	tm62_gpio_dir(dev, 3, true);
	tm62_gpio_dir(dev, 6, true);
	tm62_gpio_dir(dev, 7, true);
}

/*
 * TM6202 PCIe Bus Master Controller Setup
 * Exact 0x6202 table from 0x14678..0x14c9e in AVerTM62_x64.sys
 */
static void tm62_bus_master_init(struct avertm62_dev *dev)
{
	tm62_write_bf(dev, TM62_REG_SYS_RST, 24, 1, 1);
	udelay(100);

	/* 0x14699..0x146b9: Enable global BAR0+0x0000 bits [3:1], [4], [6:5] */
	tm62_write_bf(dev, 0x0000, 0, 1, 1);
	tm62_write_bf(dev, 0x0000, 1, 3, 1);
	tm62_write_bf(dev, 0x0000, 4, 1, 1);
	tm62_write_bf(dev, 0x0000, 5, 2, 1);
	tm62_write_bf(dev, 0x0000, 8, 1, 0);
	tm62_write_bf(dev, 0x0000, 10, 2, 2);
	tm62_write_bf(dev, 0x0000, 20, 2, 0);
	tm62_write_bf(dev, 0x0000, 22, 1, 0);
	tm62_write_bf(dev, 0x0000, 23, 1, 1);
	tm62_write_bf(dev, 0x0000, 24, 3, 0);

	tm62_write32(dev, 0x0004, 0x010e32dd);
	tm62_write32(dev, 0x0008, 0x00a81f7e);
	tm62_write32(dev, 0x000c, 0x200000b4);
	tm62_write32(dev, 0x0010, 0x8206b90a);
	tm62_write32(dev, 0x0014, 0x8b746440);
	/*
	 * Analog decoder (Trident TM6010-family core; byte n of BAR0 is the
	 * TM6010's register Rn). 0x0018 = R18..R1B CHROMA_DTO_INCREMENT,
	 * 0x2a098acb = 4.43361875 MHz (PAL) at the 27 MHz decoder clock, stored
	 * R18 in the low byte. Windows (0x147d4..0x14814) writes it as the
	 * fields [5:0]=0x2a [15:8]=0x09 [23:16]=0x8a [31:24]=0xcb. An earlier
	 * transcription had the bytes shifted by one (0x00cb8a09 plus 0x2a in
	 * R23), which put the colour oscillator at ~1 MHz.
	 */
	tm62_write32(dev, 0x0018, 0xcb8a092a);
	/*
	 * R23 HSYNC_PLL_END_TIME (0x0020[31:24]). Neither Windows nor tm6000
	 * program it, but the old shifted DTO write left 0x2a here, and the
	 * register survives module reloads (and possibly warm reboots). Values
	 * 0x08-0x30 stop the decoder seeing the colour burst (colour killed,
	 * hue rotating ~90 deg/line when forced); 0x00 and 0x38-0xf8 all give a
	 * locked PAL burst on a live signal. Use the middle of the good range.
	 */
	tm62_write_bf(dev, 0x0020, 24, 8, 0x80);
	tm62_write32(dev, 0x0028, 0x4ed64e22);
	tm62_write32(dev, 0x002c, 0x50804d3a);
	tm62_write32(dev, 0x0030, 0x0074c12c);
	tm62_write32(dev, 0x0034, 0x207f906c);
	/* R38 VSYNC_THRESHOLD / R39 VSYNC_TIME_CONSTANT / R3A, and R3C/R3D, from
	 * the Windows PAL table (0x14678); previously never written */
	tm62_write_bf(dev, 0x0038, 0, 6, 0x36);
	tm62_write_bf(dev, 0x0038, 6, 2, 2);
	tm62_write_bf(dev, 0x0038, 15, 1, 1);
	tm62_write_bf(dev, 0x0038, 21, 3, 4);
	tm62_write_bf(dev, 0x003c, 8, 4, 8);
	tm62_write_bf(dev, 0x003c, 12, 1, 1);
	tm62_write_bf(dev, 0x003c, 13, 3, 0);

	tm62_write_bf(dev, 0x0200, 1, 3, 1);
	tm62_write_bf(dev, 0x0200, 4, 1, 1);
	tm62_write_bf(dev, 0x0200, 5, 2, 1);
	tm62_write_bf(dev, 0x0200, 8, 1, 0);
	tm62_write_bf(dev, 0x0200, 16, 4, 2);
	tm62_write_bf(dev, 0x0200, 24, 3, 0);
	tm62_write_bf(dev, 0x0200, 28, 3, 1);
	tm62_write_bf(dev, 0x0204, 16, 2, 0);
	tm62_write_bf(dev, 0x0204, 20, 2, 0);
	tm62_write_bf(dev, 0x0214, 8, 5, 0x10);
	tm62_write_bf(dev, 0x0214, 24, 2, 2);
	tm62_write_bf(dev, 0x0214, 26, 2, 2);
	tm62_write_bf(dev, 0x0218, 24, 8, 1);
	tm62_write_bf(dev, 0x021c, 28, 3, 4);
	tm62_write_bf(dev, 0x0228, 4, 4, 1);
	tm62_write_bf(dev, 0x0228, 8, 8, 8);
	tm62_write_bf(dev, 0x0264, 24, 8, 0x15);

	/* 0x14920..0x14c89: TM6202 (0x6202) PCIe DMA & Bus Master Arbiter */
	tm62_write32(dev, 0x0080, 0x8386b015);
	tm62_write32(dev, 0x0088, 0x00ea2000);
	tm62_write32(dev, 0x00a8, 0x04000000);
	tm62_write32(dev, 0x00ac, 0x3f033b39);
	tm62_write32(dev, 0x00b0, 0xc1000000);
	tm62_write32(dev, 0x00b4, 0x10648e10);
	tm62_write32(dev, 0x00b8, 0x1006e2df);
	tm62_write32(dev, 0x00c0, 0xf040a000);
	tm62_write32(dev, 0x00c4, 0x00000000);
	tm62_write32(dev, 0x00c8, 0x0a050000);
	tm62_write32(dev, 0x00cc, 0xffff04f8);
	tm62_write32(dev, 0x00d0, 0x1a232801);
	tm62_write32(dev, 0x00d4, 0x00030810);
	tm62_write32(dev, 0x00d8, 0x66210100);
	tm62_write32(dev, 0x00dc, 0x8010049c);
	tm62_write32(dev, 0x00e0, 0x00000000);
	tm62_write32(dev, 0x00e8, 0x00001400);
	if (dev->pdev->device == PCI_DEVICE_ID_TRIDENT_TM6202)
		tm62_write32(dev, 0x00ec, 0x082100d6);
	else
		tm62_write32(dev, 0x00ec, 0x082178d6);
	tm62_write32(dev, 0x00f0, 0x0021220f);
	tm62_write32(dev, 0x00f4, 0x00201110);
	tm62_write32(dev, 0x00f8, 0x80015200);
	tm62_write32(dev, 0x00fc, 0xff02ff01);
	tm62_write32(dev, 0x0100, 0x1810ff03);
	tm62_write32(dev, 0x0104, 0x58302820);
	tm62_write32(dev, 0x0108, 0xf0218088);
	tm62_write32(dev, 0x010c, 0xff041f7c);
	tm62_write32(dev, 0x0118, 0xec400198);
	tm62_write32(dev, 0x011c, 0x000000c0);
	tm62_write32(dev, 0x0120, 0x3a000000);
	tm62_write32(dev, 0x0124, 0x5233554d);
	tm62_write32(dev, 0x012c, 0xd5003300);
	tm62_write32(dev, 0x0130, 0xf9fe3502);
	if (dev->pdev->device == PCI_DEVICE_ID_TRIDENT_TM6202)
		tm62_write32(dev, 0x0134, 0xffffffa0);
	tm62_write32(dev, 0x016c, 0x0000001d);

	tm62_write_bf(dev, TM62_REG_SYS_RST, 24, 1, 0);
	udelay(100);
}

/*
 * SiI9011 page 0x68 register 0x3e: the low three bits are active-low power
 * controls for the output side. 0xd7 drives the video output bus; 0xd0
 * powers it down, so the TM6202 capture path gets no pixel clock or data.
 * (Verified live: 0xd0 = no fields; 0xd7 = full 1920x540 fields.)
 */
#define SII9011_PD_RUN		0xd7
#define SII9011_PD_IDLE		0xd0

static void sii9011_configure_regs(struct avertm62_dev *dev)
{
	u8 val = 0, r48 = 0;

	tm62_i2c_write_reg8(dev, 0, 0x60, 0x05, 0x10);
	tm62_i2c_write_reg8(dev, 0, 0x60, 0x09, 0x91);
	tm62_i2c_write_reg8(dev, 0, 0x68, 0x18, 0x5c);
	tm62_i2c_write_reg8(dev, 0, 0x68, 0x14, 0x20);
	tm62_i2c_write_reg8(dev, 0, 0x60, 0x08, 0x07);
	tm62_i2c_write_reg8(dev, 0, 0x68, 0x26, 0x40);
	tm62_i2c_write_reg8(dev, 0, 0x68, 0x35, 0x06);
	tm62_i2c_write_reg8(dev, 0, 0x68, 0x38, 0x0c);
	tm62_i2c_write_reg8(dev, 0, 0x60, 0x7c, 0x01);
	tm62_i2c_write_reg8(dev, 0, 0x60, 0x79, 0x06);
	tm62_i2c_write_reg8(dev, 0, 0x68, 0x37, 0x00);
	tm62_i2c_write_reg8(dev, 0, 0x68, 0x34, 0x03);
	tm62_i2c_write_reg8(dev, 0, 0x60, 0x88, 0x88);
	tm62_i2c_write_reg8(dev, 0, 0x60, 0x89, 0x16);
	tm62_i2c_write_reg8(dev, 0, 0x68, 0x3e, SII9011_PD_RUN);
	tm62_i2c_write_reg8(dev, 0, 0x60, 0x5f, 0xc0);
	tm62_i2c_write_reg8(dev, 0, 0x60, 0x4a, 0x20);
	tm62_i2c_write_reg8(dev, 0, 0x60, 0xb5, 0x25);

	/* H727 sys 0x16878..0x168bd: if (r55 & 3) == 0, write 0x48 = 0xc0; else leave 0x48 alone */
	(void)r48;
	if (tm62_i2c_read_reg8(dev, 0, 0x60, 0x55, &val) == 0 && !(val & 0x03))
		tm62_i2c_write_reg8(dev, 0, 0x60, 0x48, 0xc0);

	if (tm62_i2c_read_reg8(dev, 0, 0x60, 0x78, &val) == 0)
		tm62_i2c_write_reg8(dev, 0, 0x60, 0x78, val | 0x01);
}

static int sii9011_hw_init(struct avertm62_dev *dev)
{
	u8 val = 0;
	int ret;

	tm62_gpio_set(dev, 3, false);
	tm62_gpio_set(dev, 7, true);
	msleep(10);
	tm62_gpio_set(dev, 7, false);
	msleep(10);
	tm62_gpio_set(dev, 7, true);
	msleep(20);

	ret = tm62_i2c_read_reg8(dev, 0, 0x60, 0x02, &val);
	if (ret < 0)
		return ret;
	dev_info(&dev->pdev->dev, "SiI9011 detected! Device ID 0x02 = 0x%02x\n", val);

	tm62_i2c_write_reg8(dev, 0, 0x60, 0x08, 0x01);
	sii9011_configure_regs(dev);
	tm62_gpio_set(dev, 3, true);
	return 0;
}

static int it6603_hw_init(struct avertm62_dev *dev)
{
	static const struct { u8 reg; u8 val; } init_tbl[] = {
		{ 0x06, 0x00 }, { 0x05, 0x10 }, { 0x05, 0xa1 },
		{ 0x07, 0x0c }, { 0x1d, 0x30 }, { 0x75, 0x60 },
		{ 0x3d, 0x40 }, { 0x1b, 0x20 }, { 0x1c, 0x00 },
		{ 0x78, 0xc1 }, { 0x05, 0x20 }, { 0x05, 0x21 },
		{ 0x05, 0x20 },
	};
	u8 val = 0;
	int i, ret;

	tm62_gpio_set(dev, 7, false);
	msleep(100);
	tm62_gpio_set(dev, 7, true);
	msleep(200);

	ret = tm62_i2c_read_reg8(dev, 0, 0x90, 0x00, &val);
	if (ret < 0)
		return ret;

	for (i = 0; i < ARRAY_SIZE(init_tbl); i++) {
		tm62_i2c_write_reg8(dev, 0, 0x90,
				    init_tbl[i].reg, init_tbl[i].val);
		udelay(1000);
	}

	if (tm62_i2c_read_reg8(dev, 0, 0x90, 0x89, &val) == 0)
		tm62_i2c_write_reg8(dev, 0, 0x90, 0x89, val & 0x9e);
	if (tm62_i2c_read_reg8(dev, 0, 0x90, 0x73, &val) == 0)
		tm62_i2c_write_reg8(dev, 0, 0x90, 0x73, val & 0x0f);
	tm62_i2c_write_reg8(dev, 0, 0x90, 0x06, 0x00);
	tm62_gpio_set(dev, 3, true);
	return 0;
}

static bool avertm62_detect_hdmi_timings(struct avertm62_dev *dev,
					 u32 *w_out, u32 *h_out,
					 bool *interlaced_out)
{
	u8 st = 0;

	if (dev->hdmi_chip == HDMI_CHIP_SII9011) {
		u8 r06 = 0, r3a = 0, r3b = 0, r3c = 0, r3d = 0;
		u8 r4e = 0, r4f = 0, r50 = 0, r51 = 0, r55 = 0;
		u8 r48 = 0, r48_sync = 0, r4a = 0, r45 = 0, r74 = 0;
		u32 w, h, h_tot, v_tot;
		bool inter;

		if (tm62_i2c_read_reg8(dev, 0, 0x60, 0x06, &r06) < 0)
			return false;

		if ((r06 & 0x0b) != 0x0b && (r06 & 0x09) != 0x09 && (r06 & 0x08) != 0x08)
			return false;

		/* H727 sys 0x162a5..0x16302: only when (r55 & 3) == 0 force 0x48 = 0xc0 */
		(void)r48_sync;
		if (tm62_i2c_read_reg8(dev, 0, 0x60, 0x55, &r55) == 0 && !(r55 & 0x03)) {
			if (tm62_i2c_read_reg8(dev, 0, 0x60, 0x48, &r48) == 0 &&
			    r48 != 0xc0)
				tm62_i2c_write_reg8(dev, 0, 0x60, 0x48, 0xc0);
		}

		tm62_i2c_read_reg8(dev, 0, 0x60, 0x3b, &r3b);
		tm62_i2c_read_reg8(dev, 0, 0x60, 0x3a, &r3a);
		h_tot = ((u32)(r3b & 0x1f) << 8) | r3a;

		tm62_i2c_read_reg8(dev, 0, 0x60, 0x3d, &r3d);
		tm62_i2c_read_reg8(dev, 0, 0x60, 0x3c, &r3c);
		v_tot = ((u32)(r3d & 0x1f) << 8) | r3c;

		tm62_i2c_read_reg8(dev, 0, 0x60, 0x4f, &r4f);
		tm62_i2c_read_reg8(dev, 0, 0x60, 0x4e, &r4e);
		w = ((u32)(r4f & 0x1f) << 8) | r4e;

		tm62_i2c_read_reg8(dev, 0, 0x60, 0x51, &r51);
		tm62_i2c_read_reg8(dev, 0, 0x60, 0x50, &r50);
		h = ((u32)(r51 & 0x1f) << 8) | r50;

		inter = !!(r55 & 0x04);
		if (inter && h >= 480 && h < 600)
			h <<= 1;

		/*
		 * Exact video mode calculation from 0x165a6..0x16646 in AVerTM62_x64.sys:
		 * Determines [dev+0x1b0] (written to 0x0188[2:0] in 0x52fb8).
		 */
		tm62_i2c_read_reg8(dev, 0, 0x60, 0x4a, &r4a);
		if (r4a & 0x08) {
			tm62_i2c_read_reg8(dev, 0, 0x60, 0x48, &r48);
			if (r48 & 0x10)
				dev->sii_vid_mode = ((r48 & 0x01) << 1) | 0x01;
			else
				dev->sii_vid_mode = (r48 & 0x01) << 1;
		} else {
			tm62_i2c_read_reg8(dev, 0, 0x68, 0x45, &r45);
			dev->sii_vid_mode = ((r45 & 0x20) | 0x10) >> 4;
		}

		if (tm62_i2c_read_reg8(dev, 0, 0x60, 0x74, &r74) == 0 && (r74 & 0x01)) {
			int retry;

			tm62_i2c_write_reg8(dev, 0, 0x60, 0x74, r74);
			tm62_i2c_write_reg8(dev, 0, 0x60, 0x05, 0x12);
			msleep(10);
			tm62_i2c_write_reg8(dev, 0, 0x60, 0x05, 0x10);
			msleep(10);
			for (retry = 0; retry < 20; retry++) {
				if (tm62_i2c_read_reg8(dev, 0, 0x60, 0x74, &r74) < 0 ||
				    !(r74 & 0x01))
					break;
				tm62_i2c_write_reg8(dev, 0, 0x60, 0x74, r74);
			}
		}

		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 0, 1, 1);
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 1, 1, 0);
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 2, 1, 0);
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 5, 1, 0);
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 30, 1, 0);
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 31, 1, 0);

		if (w < 640 || w > 1920) {
			if (h_tot >= 2150 && h_tot <= 2700)
				w = 1920;
			else if (h_tot >= 1600 && h_tot <= 2000)
				w = 1280;
			else if (h_tot >= 800 && h_tot <= 900)
				w = 720;
			else
				w = 1920;
		}
		if (h < 480 || h > 1080) {
			if (w == 1920)
				h = 1080;
			else if (w == 1280)
				h = 720;
			else
				h = (v_tot >= 600) ? 576 : 480;
		}

		if (w_out)
			*w_out = w;
		if (h_out)
			*h_out = h;
		if (interlaced_out)
			*interlaced_out = inter;

		return true;
	} else if (dev->hdmi_chip == HDMI_CHIP_IT6603) {
		u8 pol = 0;
		u8 t[8] = { 0 };
		u32 w, h;
		bool inter;

		if (tm62_i2c_read_reg8(dev, 0, 0x90, 0x10, &st) < 0)
			return false;
		if ((st & 0x07) != 0x07 && (st & 0x0e) != 0x0e)
			return false;

		if (tm62_i2c_read_reg8(dev, 0, 0x90, 0x65, &pol) == 0) {
			tm62_write_bf(dev, TM62_REG_VID_ROUTE, 1, 1, !(pol & 0x02));
			tm62_write_bf(dev, TM62_REG_VID_ROUTE, 2, 1, !(pol & 0x01));
		}

		if (tm62_i2c_read_regs(dev, 0, 0x90, (const u8[]){ 0x59 }, 1, t, 8) == 0) {
			w = t[1] | ((t[2] & 0x0f) << 8);
			h = t[5] | ((t[6] & 0x0f) << 8);
			inter = !!(pol & 0x04);
			if (inter)
				h <<= 1;
			if (w >= 640 && w <= 1920 && h >= 480 && h <= 1080) {
				if (w_out)
					*w_out = w;
				if (h_out)
					*h_out = h;
				if (interlaced_out)
					*interlaced_out = inter;
			}
		}
		return true;
	}

	return tm62_read_bf(dev, TM62_REG_CH1_VID_TIMING, 31, 1) != 0;
}

static void avertm62_free_hw_bufs(struct avertm62_dev *dev)
{
	int i, p;

	for (i = 0; i < AVERTM62_HW_BUFS; i++) {
		struct avertm62_hw_buf *hb = &dev->hw_bufs[i];

		for (p = 0; p < hb->num_pages; p++) {
			if (hb->page_cpu[p]) {
				dma_free_coherent(&dev->pdev->dev, PAGE_SIZE,
						  hb->page_cpu[p], hb->page_dma[p]);
				hb->page_cpu[p] = NULL;
			}
		}
		hb->num_pages = 0;
		if (hb->pt_cpu) {
			dma_free_coherent(&dev->pdev->dev, PAGE_SIZE,
					  hb->pt_cpu, hb->pt_dma);
			hb->pt_cpu = NULL;
		}
	}
}

static int avertm62_alloc_hw_bufs(struct avertm62_dev *dev)
{
	int i, p;

	for (i = 0; i < AVERTM62_HW_BUFS; i++) {
		struct avertm62_hw_buf *hb = &dev->hw_bufs[i];

		hb->pt_cpu = dma_alloc_coherent(&dev->pdev->dev, PAGE_SIZE,
						&hb->pt_dma, GFP_KERNEL);
		if (!hb->pt_cpu)
			goto err_free;

		for (p = 0; p < AVERTM62_PT_PAGES; p++) {
			void *vaddr;
			dma_addr_t daddr;

			vaddr = dma_alloc_coherent(&dev->pdev->dev, PAGE_SIZE,
						   &daddr, GFP_KERNEL);
			if (!vaddr)
				goto err_free;

			hb->page_cpu[p] = vaddr;
			hb->page_dma[p] = daddr;
			hb->pt_cpu[p] = cpu_to_le32((u32)(daddr & ~0xfffULL));
			hb->num_pages = p + 1;
		}
	}
	wmb();
	return 0;

err_free:
	avertm62_free_hw_bufs(dev);
	return -ENOMEM;
}

static const u32 tm62_vid_ch_base[2] = { 0x0300, 0x0400 };
#define TM62_NUM_VID_CH	2

static void avertm62_push_hw_buf_ch(struct avertm62_dev *dev, u32 idx, int ch)
{
	struct avertm62_hw_buf *hb = &dev->hw_bufs[idx % AVERTM62_HW_BUFS];
	u32 pt_pfn = (u32)(hb->pt_dma >> 12) & 0xfffffU;
	/*
	 * Always size the buffer for a whole frame. On HDMI the TM6202 weaves
	 * both fields into it; on composite it still closes the buffer at the
	 * end of each field, but a buffer sized exactly one field long loses
	 * every other field (27 instead of 50 fields/s).
	 */
	u32 len = (dev->bytesperline * dev->height) & 0x007fffffU;
	u32 fifo = tm62_vid_ch_base[ch] + 0x200;

	if (tm62_read_bf(dev, fifo + 0x08, 0, 4) >= 8)
		return;

	tm62_write_bf(dev, fifo, 0, 23, len);
	tm62_write_bf(dev, fifo + 0x04, 0, 20, pt_pfn);
	tm62_write_bf(dev, fifo + 0x04, 20, 12, 0);
	tm62_write_bf(dev, fifo, 31, 1, 1);
}

static void avertm62_copy_hw_to_vb2(struct avertm62_dev *dev,
				    struct avertm62_hw_buf *hb,
				    void *dst, u32 bytes)
{
	u32 rem = min_t(u32, bytes, AVERTM62_PT_PAGES * PAGE_SIZE);
	u32 p = 0;
	u8 *out = dst;

	while (rem > 0 && p < hb->num_pages) {
		u32 chunk = min_t(u32, rem, PAGE_SIZE);

		memcpy(out, hb->page_cpu[p], chunk);
		out += chunk;
		rem -= chunk;
		p++;
	}
}

/* Copy one field into every other line of the frame (field 0 = even lines) */
static void avertm62_copy_hw_field(struct avertm62_dev *dev,
				   struct avertm62_hw_buf *hb,
				   u8 *dst, u32 field)
{
	u32 bpl = dev->bytesperline;
	u32 lines = dev->height / 2;
	u32 src_off = 0, y;

	for (y = 0; y < lines; y++) {
		u8 *line_dst = dst + (y * 2 + (field & 1)) * bpl;
		u32 rem = bpl;

		while (rem > 0) {
			u32 p = src_off >> PAGE_SHIFT;
			u32 pg_off = src_off & (PAGE_SIZE - 1);
			u32 chunk = min_t(u32, rem, PAGE_SIZE - pg_off);

			if (p >= hb->num_pages)
				return;
			memcpy(line_dst, (u8 *)hb->page_cpu[p] + pg_off, chunk);
			line_dst += chunk;
			src_off += chunk;
			rem -= chunk;
		}
	}
}

static void avertm62_dma_fifo_reset(struct avertm62_dev *dev)
{
	tm62_write_bf(dev, TM62_REG_CH0_DMA_START, 0, 1, 1);
	tm62_write_bf(dev, TM62_REG_DMA_START, 0, 1, 1);
	udelay(10);
	tm62_write_bf(dev, TM62_REG_CH0_DMA_START, 0, 1, 0);
	tm62_write_bf(dev, TM62_REG_DMA_START, 0, 1, 0);
}

static void avertm62_hw_start_streaming(struct avertm62_dev *dev)
{
	u32 w = dev->width;
	u32 h = dev->height;
	u32 field_h = dev->interlaced ? (h >> 1) : h;
	u32 timer_period;
	u32 fmt_cfg;
	int i;

	/* 1. Stream Router setup (matching 0x5dd84, 0x5388c, 0x52f2c) */
	tm62_write_bf(dev, 0x0000, 0, 1, 1);
	tm62_write_bf(dev, 0x0290, 26, 2, 3);
	tm62_write_bf(dev, TM62_REG_AUD_PATH_A, 20, 1, 1);
	tm62_write_bf(dev, TM62_REG_AUD_PATH_B, 2, 2, 2);
	tm62_write_bf(dev, TM62_REG_AUD_PATH_B, 19, 1, 1);
	tm62_write_bf(dev, 0x0298, 16, 4, 6);
	tm62_write_bf(dev, 0x0298, 28, 1, 0);
	tm62_write_bf(dev, 0x029c, 10, 2, 2);
	tm62_write_bf(dev, 0x029c, 0, 2, 1);
	tm62_write_bf(dev, TM62_REG_DMA_GLB_CFG, 3, 1, 0);
	tm62_write_bf(dev, TM62_REG_DMA_GLB_CFG, 4, 4, 6);
	tm62_write_bf(dev, TM62_REG_DMA_GLB_CFG, 21, 1, 0);
	tm62_write_bf(dev, TM62_REG_DMA_GLB_CFG, 29, 1, 0);

	/*
	 * 0x019c[21:20] / [17:16]: Windows writes these only at 0x4ff5f..0x4ffae,
	 * and only for stream types 2 and 3 (TS/audio, value from the registry).
	 * The video path never sets them. They may switch the shared input pins
	 * to a TS port, so clear them for video unless str_route=1.
	 */
	{
		u32 v = (str_route == 1) ? 1 : 0;

		tm62_write_bf(dev, TM62_REG_STREAM_ROUTE, 16, 1, v);
		tm62_write_bf(dev, TM62_REG_STREAM_ROUTE, 17, 1, v);
		tm62_write_bf(dev, TM62_REG_STREAM_ROUTE, 20, 1, v);
		tm62_write_bf(dev, TM62_REG_STREAM_ROUTE, 21, 1, v);
	}

	tm62_write_bf(dev, TM62_REG_PLL_CTRL0, 7, 1, 1);
	/* 0x4e3d6 (edx=8, live video): 0x0760[18:16] = 1 (0 is for colorbar generator) */
	tm62_write_bf(dev, TM62_REG_CLK_CFG, 16, 3,
		      (clk_src >= 0 && clk_src <= 7) ? clk_src :
		      (dev->active_pattern >= 0 ? 0 : 1));

	if (dev->input == AVERTM62_INPUT_HDMI ||
	    dev->input == AVERTM62_INPUT_COMPOSITE) {
		/* RMW only (Windows uses bitfield writes): keep power-on default bits 0x00060200 */
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 0, 1, 1);
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 1, 1, 0);
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 2, 1, 0);
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 5, 1, 0);
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 24, 4, 0);
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 30, 1, 0);
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 31, 1, 0);

		/* 0x17a70: 0x019c and 0x01a0 are 0 for SiI9011 (only set on ADV7441A) */
		tm62_write_bf(dev, TM62_REG_STREAM_ROUTE, 0, 16, 0);
		tm62_write32(dev, TM62_REG_CH1_VID_TIMING_CFG, 0);

		if (dev->input == AVERTM62_INPUT_HDMI &&
		    dev->hdmi_chip == HDMI_CHIP_SII9011) {
			sii9011_configure_regs(dev);
			tm62_i2c_write_reg8(dev, 0, 0x60, 0x08, 0x07);
			tm62_i2c_write_reg8(dev, 0, 0x68, 0x3e, SII9011_PD_RUN);
		}
	} else {
		u32 vr;

		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 0, 1, 0);
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 5, 1, 1);
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 30, 1, 1);
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 31, 1, 0);

		vr = tm62_read32(dev, TM62_REG_VID_ROUTE);
		tm62_write32(dev, TM62_REG_VID_ROUTE, vr & ~0x60U);
		udelay(10);
		tm62_write32(dev, TM62_REG_VID_ROUTE, vr | 0x60U);
		udelay(10);
	}

	/*
	 * 2. Configure Source & Destination Size (0x65aa4: 0x65ade..0x65ba3)
	 *    and Timing Reference / Sync (0x17863..0x17924):
	 *   0x0180[28]    = ~interlaced
	 *   0x0180[26:16] = src_w (w)
	 *   0x0180[10:0]  = src_h (field_h) -- 0x65b90 writes src_h here, NOT 0!
	 *   0x0184[26:16] = dst_w (w)
	 *   0x0184[10:0]  = dst_h (field_h)
	 */
	if (field_h == 540) {
		tm62_write_bf(dev, TM62_REG_VID_TIMING_REF, 0, 11, 0x71);
		tm62_write_bf(dev, TM62_REG_VID_TIMING_SYNC, 0, 11, 0x13);
	} else if (field_h == 720) {
		tm62_write_bf(dev, TM62_REG_VID_TIMING_REF, 0, 11, 0x12a);
		tm62_write_bf(dev, TM62_REG_VID_TIMING_SYNC, 0, 11, 0x18);
	} else if (field_h == 480) {
		tm62_write_bf(dev, TM62_REG_VID_TIMING_REF, 0, 11, 0x4b);
		tm62_write_bf(dev, TM62_REG_VID_TIMING_SYNC, 0, 11, 0x26);
	} else if (field_h == 576) {
		tm62_write_bf(dev, TM62_REG_VID_TIMING_REF, 0, 11, 0x82);
		tm62_write_bf(dev, TM62_REG_VID_TIMING_SYNC, 0, 11, 0x2c);
	}

	tm62_write_bf(dev, TM62_REG_CH1_CROP_H, 28, 1, dev->interlaced ? 0 : 1);
	tm62_write_bf(dev, TM62_REG_CH1_CROP_H, 16, 11, w);
	tm62_write_bf(dev, TM62_REG_CH1_CROP_H, 0, 11, field_h);
	tm62_write_bf(dev, TM62_REG_CH1_CROP_V, 16, 11, w);
	tm62_write_bf(dev, TM62_REG_CH1_CROP_V, 0, 11, field_h);

	tm62_write_bf(dev, TM62_REG_CH1_SCALE_H, 0, 11, w);
	tm62_write_bf(dev, TM62_REG_CH1_SCALE_H, 16, 12, (w <= 720) ? 1440 : w);

	/*
	 * 3. Configure Video Control (0x0188, matching 0x618e8, 0x4e39c, 0x4efc4, 0x4f010):
	 *   0x618e8 initializes shadow of 0x0188 to 0x00000000
	 *   [2:0]   = 0 (test pattern index when [3]=1; 0 for edx=8 live video)
	 *   [3]     = 0 (0x4e3ba: 0 = live external video, 1 = internal colorbar generator)
	 *   [28:24] = 5 (0x4f010)
	 *   [29]    = 0 (0x4efc4)
	 *   [30]    = 0 (0x4efc4)
	 *   [31]    = 0 (never set in AVerTM62_x64.sys; 0x0190[6] is the CH1 capture gate)
	 */
	tm62_write32(dev, TM62_REG_CH1_VID_CTRL, 0x05000000);
	if (dev->active_pattern >= 0) {
		/* Windows 0x52fb8 / 0x52fcd: pattern index, then generator enable */
		tm62_write_bf(dev, TM62_REG_CH1_VID_CTRL, 0, 3, dev->active_pattern);
		tm62_write_bf(dev, TM62_REG_CH1_VID_CTRL, 3, 1, 1);
	}

	/*
	 * 4. Configure DMA Format Engine on Channel 1 (0x0490 / 0x0494)
	 *    matching 0x5b4ce..0x5b533 (0x5c064) and 0x50358..0x5037b:
	 *    - 0x0490 = 0 and 0x0494[15:0] = 0 for video capture (only set when edx==2 MPEG-TS)
	 *    - 0x0494[17:16] = 2 for live unmuted video (0x50358; 3 = muted)
	 *    - 0x0494[19:18] = 1 for CH1 HDMI/S-Video (0x5c0f3)
	 *    - 0x0494[20]    = !interlaced (0x5c0b6, +0xab8 in 0x60dd0)
	 *    - 0x0494[22]    = 1 (0x5c0e1, +0xaa0 in 0x60dd0)
	 *    - 0x0494[25:24] = pixfmt (0x5c0cc: 0=YUYV, 1=UYVY)
	 *    - 0x0494[30]    = 1 (power-on default 0x40470000)
	 */
	tm62_write32(dev, TM62_REG_CH0_DMA_CFG, 0);
	tm62_write_bf(dev, TM62_REG_CH0_DESC_CTRL, 30, 1, 0);
	tm62_write32(dev, TM62_REG_CH0_FMT_CFG, BIT(31) | BIT(30) | (3U << 16));

	tm62_write32(dev, TM62_REG_CH1_FRAME_BYTES, 0);
	fmt_cfg = BIT(30) |
		  (((dev->pixelformat == V4L2_PIX_FMT_UYVY) ? 1U : 0U) << 24) |
		  BIT(22) |
		  /* [20]=1 hands every field to DMA separately; composite needs it,
		   * otherwise the engine drops every other field */
		  ((dev->interlaced && dev->input != AVERTM62_INPUT_COMPOSITE) ?
		   0U : 1U) << 20 |
		  ((dev->input == AVERTM62_INPUT_HDMI) ? BIT(18) : 0U) |
		  (2U << 16);
	tm62_write32(dev, TM62_REG_CH1_FMT_CFG, fmt_cfg);

	/* Disable VBI DMA */
	tm62_write32(dev, TM62_REG_CH0_VBI_DMA_CFG, 0);
	tm62_write32(dev, TM62_REG_CH1_VBI_DMA_CFG, 0);
	tm62_write_bf(dev, TM62_REG_CH0_VBI_DESC_CTRL, 30, 1, 0);
	tm62_write_bf(dev, TM62_REG_CH1_VBI_DESC_CTRL, 30, 1, 0);

	/* Ensure DMA clocks are enabled */
	tm62_write_bf(dev, TM62_REG_DMA_GLB_CFG, 9, 1, 0);
	tm62_write_bf(dev, TM62_REG_CH1_DMA_MISC, 23, 1, 1);

	/* 5. Program stride on Channel 1 (0x0400), enable FIFO run bit 30, clear reset, set enable */
	tm62_write32(dev, TM62_REG_CH1_DMA_CFG, dev->bytesperline & 0xfffU);
	tm62_write_bf(dev, TM62_REG_CH1_DESC_CTRL, 30, 1, 1);

	msleep(10);

	tm62_write_bf(dev, TM62_REG_CH1_DMA_CFG, 24, 1, 0);
	tm62_write_bf(dev, TM62_REG_CH1_DMA_CFG, 31, 1, 1);

	/* Push initial scatter-gather descriptors into Channel 1 FIFO */
	dev->active_dma_ch = 1;
	dev->last_s0_pta[0] = 0;
	dev->last_s0_pta[1] = 0;
	for (i = 0; i < AVERTM62_BUFS_PER_CH; i++)
		avertm62_push_hw_buf_ch(dev, AVERTM62_BUFS_PER_CH + i, 1);

	/* 6. Enable Global DMA Threshold (0x03a8, 0x60076) & Frame Interrupt Timer on Channel 1 */
	tm62_write_bf(dev, TM62_REG_CH0_DMA_THRESH, 16, 15, 0x7fff);
	tm62_write_bf(dev, TM62_REG_CH0_DMA_THRESH, 31, 1, 1);

	timer_period = 0x07698000U / (dev->fps ? dev->fps : 60);
	tm62_write_bf(dev, TM62_REG_CH1_DMA_TIMER, 0, 30, timer_period);
	tm62_write_bf(dev, TM62_REG_CH1_DMA_TIMER, 30, 1, 1);

	/* 7. Unmask Video DMA Slot IRQs & DMA Timer IRQs & Frame VSYNC IRQ */
	tm62_write32(dev, TM62_REG_IRQ_ACK,
		     TM62_IRQ_CH1_VID_DMA | TM62_IRQ_CH0_VID_DMA |
		     TM62_IRQ_CH1_DMA | TM62_IRQ_CH0_DMA | TM62_IRQ_FRAME);
	tm62_write_bf(dev, TM62_REG_IRQ_MASK, 20, 1, 0);
	tm62_write_bf(dev, TM62_REG_IRQ_MASK, 17, 1, 0);
	tm62_write_bf(dev, TM62_REG_IRQ_MASK, 1, 1, 0);

	/* 8. Start Video Capture Gate on Channel 1 (0x4f57f: 0x0190[6]=1) */
	if (dev->input != AVERTM62_INPUT_HDMI &&
	    dev->input != AVERTM62_INPUT_COMPOSITE) {
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 5, 2, 0);
		udelay(10);
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 5, 2, 3);
	} else {
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 5, 1, 0);
		tm62_write_bf(dev, TM62_REG_VID_ROUTE, 6, 1, 1);
	}

	if (dev->input == AVERTM62_INPUT_COMPOSITE) {
		/*
		 * Route the breakout socket's composite input through the TM6202
		 * internal decoder to DMA engine 1 (found by live testing; see
		 * HARDWARE.md). 0x0494[19:18] is already 0 from fmt_cfg above.
		 */
		tm62_write_bf(dev, 0x0290, 26, 2, 1);	/* analog mux into decoder */
		/*
		 * Windows' decoder start routine (0x11740) sets these. Without them
		 * the decoder never sees the input (status frozen, flat grey picture).
		 */
		tm62_write_bf(dev, 0x029c, 7, 1, 1);
		tm62_write_bf(dev, 0x029c, 6, 1, 1);
		tm62_write_bf(dev, 0x0000, 0, 1, 0);	/* Windows 0x537fc, analog ports */
		/*
		 * Decoder input select: Windows' PAL table value (1). Value 4 also
		 * produces a picture but with a strong echo 7 px to the right and
		 * the chroma demodulator pinned to the rails.
		 */
		tm62_write_bf(dev, 0x0000, 1, 3, 1);
		/* Contrast: Windows' table default (0x7e) gives standard 16-235 levels */
		tm62_write_bf(dev, 0x0008, 0, 8, 0x7e);
		tm62_write32(dev, TM62_REG_CH0_CROP_H, 0x02d00120);	/* 720x288 */
		tm62_write32(dev, TM62_REG_CH0_CROP_V, 0x02d00120);
	}

	dev_info(&dev->pdev->dev,
		 "Stream started: pattern=%d CLK_CFG=0x%08x VID_TIMING=0x%08x\n",
		 dev->active_pattern,
		 tm62_read32(dev, TM62_REG_CLK_CFG),
		 tm62_read32(dev, TM62_REG_CH1_VID_TIMING));
	dev_info(&dev->pdev->dev,
		 "Stream started: sii_mode=%u FIFO_CNT(E1)=%u FMT(E1)=0x%08x BYTES=%u PKTS=%u CROP_H=0x%08x CROP_V=0x%08x VID_CTRL=0x%08x VID_ROUTE=0x%08x SCALE_H=0x%08x DMA_CFG=0x%08x\n",
		 dev->sii_vid_mode,
		 tm62_read_bf(dev, TM62_REG_CH1_FIFO_CNT, 0, 4),
		 tm62_read32(dev, TM62_REG_CH1_FMT_CFG),
		 tm62_read32(dev, TM62_REG_CH1_FRAME_BYTES),
		 tm62_read32(dev, TM62_REG_CH1_FMT_CFG) & 0xffffU,
		 tm62_read32(dev, TM62_REG_CH1_CROP_H),
		 tm62_read32(dev, TM62_REG_CH1_CROP_V),
		 tm62_read32(dev, TM62_REG_CH1_VID_CTRL),
		 tm62_read32(dev, TM62_REG_VID_ROUTE),
		 tm62_read32(dev, TM62_REG_CH1_SCALE_H),
		 tm62_read32(dev, TM62_REG_CH1_DMA_CFG));
}

static void avertm62_hw_stop_streaming(struct avertm62_dev *dev)
{
	tm62_write_bf(dev, TM62_REG_IRQ_MASK, 20, 1, 1);
	tm62_write_bf(dev, TM62_REG_IRQ_MASK, 17, 1, 1);
	tm62_write_bf(dev, TM62_REG_IRQ_MASK, 16, 1, 1);
	tm62_write_bf(dev, TM62_REG_IRQ_MASK, 1, 1, 1);
	tm62_write_bf(dev, TM62_REG_IRQ_MASK, 0, 1, 1);

	tm62_write_bf(dev, TM62_REG_VID_ROUTE, 6, 1, 0);
	tm62_write_bf(dev, TM62_REG_PLL_CTRL0, 7, 1, 0);

	/* Turn the colorbar generator off and restore the live clock source */
	tm62_write_bf(dev, TM62_REG_CH1_VID_CTRL, 3, 1, 0);
	tm62_write_bf(dev, TM62_REG_CLK_CFG, 16, 3, 1);

	if (dev->hdmi_chip == HDMI_CHIP_SII9011)
		tm62_i2c_write_reg8(dev, 0, 0x68, 0x3e, SII9011_PD_IDLE);

	tm62_write_bf(dev, TM62_REG_CH0_DMA_TIMER, 30, 1, 0);
	tm62_write_bf(dev, TM62_REG_CH1_DMA_TIMER, 30, 1, 0);
	tm62_write32(dev, TM62_REG_CH0_DMA_CFG, 0);
	tm62_write32(dev, TM62_REG_CH0_VBI_DMA_CFG, 0);
	tm62_write32(dev, TM62_REG_CH1_DMA_CFG, 0);
	tm62_write32(dev, TM62_REG_CH1_VBI_DMA_CFG, 0);
	tm62_write_bf(dev, TM62_REG_CH0_DESC_CTRL, 30, 1, 0);
	tm62_write_bf(dev, TM62_REG_CH0_VBI_DESC_CTRL, 30, 1, 0);
	tm62_write_bf(dev, TM62_REG_CH1_DESC_CTRL, 30, 1, 0);
	tm62_write_bf(dev, TM62_REG_CH1_VBI_DESC_CTRL, 30, 1, 0);
	tm62_write_bf(dev, TM62_REG_CH0_FMT_CFG, 16, 2, 3);
	tm62_write_bf(dev, TM62_REG_CH0_FMT_CFG, 31, 1, 1);
	tm62_write_bf(dev, TM62_REG_CH1_FMT_CFG, 16, 2, 3);
	tm62_write_bf(dev, TM62_REG_CH1_FMT_CFG, 31, 1, 1);

	avertm62_dma_fifo_reset(dev);
	udelay(100);
}

static bool avertm62_poll_dma_slots(struct avertm62_dev *dev);

static irqreturn_t avertm62_irq(int irq, void *dev_id)
{
	struct avertm62_dev *dev = dev_id;
	u32 mask_save, status, pending;

	mask_save = tm62_read32(dev, TM62_REG_IRQ_MASK);
	if (mask_save == 0xffffffffU)
		return IRQ_NONE;

	tm62_write32(dev, TM62_REG_IRQ_MASK, mask_save | TM62_IRQ_GLOBAL_DIS);

	status = tm62_read32(dev, TM62_REG_IRQ_STATUS);
	pending = status & (~mask_save) & 0x001fffffU;
	if (!pending) {
		tm62_write32(dev, TM62_REG_IRQ_MASK, mask_save);
		return IRQ_NONE;
	}

	tm62_write32(dev, TM62_REG_IRQ_ACK, pending);
	dev->irq_count++;
	dev->last_irq_pending |= pending;

	if (pending & (TM62_IRQ_CH1_VID_DMA | TM62_IRQ_CH0_VID_DMA |
		       TM62_IRQ_CH1_DMA | TM62_IRQ_CH0_DMA | TM62_IRQ_FRAME)) {
		if (pending & TM62_IRQ_CH1_DMA)
			tm62_write_bf(dev, TM62_REG_CH1_DMA_TIMER, 31, 1, 1);
		if (pending & TM62_IRQ_CH0_DMA)
			tm62_write_bf(dev, TM62_REG_CH0_DMA_TIMER, 31, 1, 1);

		spin_lock(&dev->qlock);
		if (dev->streaming)
			avertm62_poll_dma_slots(dev);
		spin_unlock(&dev->qlock);
	}

	tm62_write32(dev, TM62_REG_IRQ_MASK, mask_save);
	return IRQ_HANDLED;
}

static bool avertm62_poll_dma_slots(struct avertm62_dev *dev)
{
	int chan = 1, slot, k;
	bool any_completed = false;
	u32 base_st = tm62_vid_ch_base[chan] + 0x04;
	u32 base_pta = tm62_vid_ch_base[chan] + 0x18;
	u32 s0_pta = tm62_read32(dev, base_pta) & 0xfffffU;

	if (s0_pta == 0 || s0_pta == dev->last_s0_pta[chan])
		return false;

	for (k = 0; k < 5; k++) {
		u32 pta_k = tm62_read32(dev, base_pta + k * 4) & 0xfffffU;

		if (pta_k == 0 || pta_k == dev->last_s0_pta[chan])
			break;
	}
	dev->last_s0_pta[chan] = s0_pta;

	for (slot = k - 1; slot >= 0; slot--) {
		u32 slot_sts = tm62_read32(dev, base_st + slot * 4);
		u32 bytes = slot_sts & 0x3fffffU;
		u32 pta = tm62_read32(dev, base_pta + slot * 4) & 0xfffffU;
		int b_idx;
		u8 *p0;

		for (b_idx = 0; b_idx < AVERTM62_HW_BUFS; b_idx++) {
			if (((u32)(dev->hw_bufs[b_idx].pt_dma >> 12) & 0xfffffU) == pta)
				break;
		}
		if (b_idx >= AVERTM62_HW_BUFS)
			b_idx = chan * AVERTM62_BUFS_PER_CH + (slot % AVERTM62_BUFS_PER_CH);

		p0 = dev->hw_bufs[b_idx].page_cpu[0];
		pr_info_ratelimited("avertm62: slot done: ch%d s%d buf=%d sts=0x%08x done=%u pta=0x%08x bytes=%u hdr=%16ph\n",
				    chan, slot, b_idx, slot_sts,
				    !!(slot_sts & BIT(22)), pta, bytes, p0);

		/*
		 * The TM6202 weaves both fields of an interlaced frame into one
		 * buffer (descriptor sized for a whole frame, like Windows'
		 * 0x465000), so every completed slot is one complete frame. The
		 * hardware may stop a line short (1079 of 1080 lines for 1080i);
		 * the missing tail is filled by repeating the last line.
		 */
		if (bytes > 0) {
			any_completed = true;
			if (!list_empty(&dev->buf_list)) {
				struct avertm62_vb2_buf *vbuf;
				u8 *vaddr;
				u32 copy = min_t(u32, bytes, dev->sizeimage);

				vbuf = list_first_entry(&dev->buf_list,
							struct avertm62_vb2_buf, list);
				vaddr = vb2_plane_vaddr(&vbuf->vb.vb2_buf, 0);

				if (dev->input == AVERTM62_INPUT_COMPOSITE) {
					/* One field per slot: weave pairs into a frame */
					if (vaddr)
						avertm62_copy_hw_field(dev, &dev->hw_bufs[b_idx],
								       vaddr, dev->field_count);
					if (++dev->field_count < 2)
						goto requeue;
					dev->field_count = 0;
				} else if (vaddr) {
					u32 bpl = dev->bytesperline;
					u32 off;

					avertm62_copy_hw_to_vb2(dev, &dev->hw_bufs[b_idx],
								vaddr, copy);
					copy -= copy % bpl;
					for (off = copy; copy >= bpl &&
					     off + bpl <= dev->sizeimage; off += bpl)
						memcpy(vaddr + off, vaddr + copy - bpl, bpl);
				}

				list_del(&vbuf->list);
				vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0,
						      dev->sizeimage);
				vbuf->vb.vb2_buf.timestamp = ktime_get_ns();
				vbuf->vb.sequence = dev->sequence++;
				vbuf->vb.field = dev->interlaced ? V4L2_FIELD_INTERLACED :
								   V4L2_FIELD_NONE;
				vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);
				dev->dma_frame_count++;
			}
		}
requeue:

		avertm62_push_hw_buf_ch(dev, b_idx, chan);
	}
	return any_completed;
}

static void avertm62_fill_colorbars(struct avertm62_dev *dev, u8 *dst)
{
	static const u8 yuyv_bars[8][4] = {
		{ 235, 128, 235, 128 }, /* White */
		{ 210,  16, 210, 146 }, /* Yellow */
		{ 170, 166, 170,  16 }, /* Cyan */
		{ 145,  54, 145,  34 }, /* Green */
		{ 106, 202, 106, 222 }, /* Magenta */
		{  81,  90,  81, 240 }, /* Red */
		{  41, 240,  41, 110 }, /* Blue */
		{  16, 128,  16, 128 }, /* Black */
	};
	u32 w = dev->width;
	u32 h = dev->height;
	u32 stride = dev->bytesperline;
	u32 x, y;
	u8 *line0 = dst;

	for (x = 0; x < w; x += 2) {
		int bar = (x * 8) / w;
		const u8 *c = yuyv_bars[bar & 7];

		if (dev->pixelformat == V4L2_PIX_FMT_UYVY) {
			line0[x * 2 + 0] = c[1];
			line0[x * 2 + 1] = c[0];
			line0[x * 2 + 2] = c[3];
			line0[x * 2 + 3] = c[2];
		} else {
			line0[x * 2 + 0] = c[0];
			line0[x * 2 + 1] = c[1];
			line0[x * 2 + 2] = c[2];
			line0[x * 2 + 3] = c[3];
		}
	}
	for (y = 1; y < h; y++) {
		u8 *row = dst + y * stride;

		if (y == ((dev->sequence * 4) % h))
			memset(row, 0xff, stride);
		else
			memcpy(row, line0, stride);
	}
}

static void avertm62_fallback_timer_fn(struct timer_list *t)
{
	struct avertm62_dev *dev = avertm62_from_timer(dev, t, fallback_timer);
	unsigned long flags;
	bool completed;

	spin_lock_irqsave(&dev->qlock, flags);
	if (!dev->streaming) {
		spin_unlock_irqrestore(&dev->qlock, flags);
		return;
	}

	completed = avertm62_poll_dma_slots(dev);

	/* Never fake frames in pattern mode: only real DMA output counts */
	if (!completed && dev->active_pattern < 0 &&
	    dev->dma_frame_count == 0 && !list_empty(&dev->buf_list)) {
		u32 timing = tm62_read32(dev, TM62_REG_CH1_VID_TIMING);
		bool hdmi_locked = (timing & BIT(31)) != 0;

		if (!hdmi_locked) {
			struct avertm62_vb2_buf *vbuf;
			void *vaddr;

			vbuf = list_first_entry(&dev->buf_list,
						struct avertm62_vb2_buf, list);
			list_del(&vbuf->list);

			vaddr = vb2_plane_vaddr(&vbuf->vb.vb2_buf, 0);
			if (vaddr)
				avertm62_fill_colorbars(dev, vaddr);

			vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, dev->sizeimage);
			vbuf->vb.vb2_buf.timestamp = ktime_get_ns();
			vbuf->vb.sequence = dev->sequence++;
			vbuf->vb.field = V4L2_FIELD_NONE;
			vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);
		}
	}

	mod_timer(&dev->fallback_timer, jiffies + msecs_to_jiffies(16));
	spin_unlock_irqrestore(&dev->qlock, flags);
}

/* Videobuf2 Queue Operations */
static int avertm62_queue_setup(struct vb2_queue *vq,
				unsigned int *nbuffers, unsigned int *nplanes,
				unsigned int sizes[], struct device *alloc_devs[])
{
	struct avertm62_dev *dev = vb2_get_drv_priv(vq);

	if (*nbuffers < 2)
		*nbuffers = 2;

	if (*nplanes)
		return sizes[0] < dev->sizeimage ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = dev->sizeimage;
	return 0;
}

static int avertm62_buf_prepare(struct vb2_buffer *vb)
{
	struct avertm62_dev *dev = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < dev->sizeimage)
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, dev->sizeimage);
	return 0;
}

static void avertm62_buf_queue(struct vb2_buffer *vb)
{
	struct avertm62_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct avertm62_vb2_buf *buf =
		container_of(vbuf, struct avertm62_vb2_buf, vb);
	unsigned long flags;

	spin_lock_irqsave(&dev->qlock, flags);
	list_add_tail(&buf->list, &dev->buf_list);
	spin_unlock_irqrestore(&dev->qlock, flags);
}

static int avertm62_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct avertm62_dev *dev = vb2_get_drv_priv(vq);
	unsigned long flags;
	u32 det_w = 0, det_h = 0;
	bool det_inter = false;

	dev->active_pattern = (test_pattern >= 0 && test_pattern <= 7) ?
			      test_pattern : -1;

	if (dev->active_pattern >= 0) {
		/* Generator output is 720x288 per field, interlaced (Windows 0x52ff7) */
		dev->interlaced = true;
		dev_info(&dev->pdev->dev,
			 "start_streaming: TEST PATTERN %d (fmt=%ux%u, generator is 720x576i)\n",
			 dev->active_pattern, dev->width, dev->height);
		if (dev->width != 720 || dev->height != 576)
			dev_warn(&dev->pdev->dev,
				 "test_pattern: format is %ux%u, expected 720x576\n",
				 dev->width, dev->height);
	} else if (dev->input == AVERTM62_INPUT_COMPOSITE) {
		dev->interlaced = true;
	} else if (dev->input == AVERTM62_INPUT_HDMI) {
		bool locked = avertm62_detect_hdmi_timings(dev, &det_w, &det_h,
							   &det_inter);
		dev_info(&dev->pdev->dev,
			 "start_streaming: HDMI lock=%d detected=%ux%u%s (fmt=%ux%u) sii_mode=%u\n",
			 locked, det_w, det_h, det_inter ? "i" : "p",
			 dev->width, dev->height, dev->sii_vid_mode);
		if (locked)
			dev->interlaced = det_inter;
	}

	dev->last_irq_pending = 0;
	avertm62_hw_start_streaming(dev);

	spin_lock_irqsave(&dev->qlock, flags);
	dev->sequence = 0;
	dev->field_count = 0;
	dev->cur_frame_bytes = 0;
	dev->cur_slice_mask = 0;
	dev->streaming = true;
	mod_timer(&dev->fallback_timer, jiffies + msecs_to_jiffies(50));
	spin_unlock_irqrestore(&dev->qlock, flags);

	return 0;
}

static void avertm62_stop_streaming(struct vb2_queue *vq)
{
	struct avertm62_dev *dev = vb2_get_drv_priv(vq);
	struct avertm62_vb2_buf *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&dev->qlock, flags);
	dev->streaming = false;
	list_for_each_entry_safe(buf, tmp, &dev->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&dev->qlock, flags);

	avertm62_timer_del_sync(&dev->fallback_timer);
	avertm62_hw_stop_streaming(dev);
}

static const struct vb2_ops avertm62_vb2_ops = {
	.queue_setup     = avertm62_queue_setup,
	.buf_prepare     = avertm62_buf_prepare,
	.buf_queue       = avertm62_buf_queue,
	.start_streaming = avertm62_start_streaming,
	.stop_streaming  = avertm62_stop_streaming,
	.wait_prepare    = vb2_ops_wait_prepare,
	.wait_finish     = vb2_ops_wait_finish,
};

/* V4L2 IOCTL Operations */
static int avertm62_querycap(struct file *file, void *priv,
			     struct v4l2_capability *cap)
{
	struct avertm62_dev *dev = video_drvdata(file);

	strscpy(cap->driver, DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "AVerMedia H727 PCIe HDMI Capture", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI:%s",
		 pci_name(dev->pdev));
	return 0;
}

static int avertm62_enum_fmt_vid_cap(struct file *file, void *priv,
				     struct v4l2_fmtdesc *f)
{
	switch (f->index) {
	case 0:
		f->pixelformat = V4L2_PIX_FMT_YUYV;
		return 0;
	case 1:
		f->pixelformat = V4L2_PIX_FMT_UYVY;
		return 0;
	default:
		return -EINVAL;
	}
}

static int avertm62_g_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct avertm62_dev *dev = video_drvdata(file);

	f->fmt.pix.width        = dev->width;
	f->fmt.pix.height       = dev->height;
	f->fmt.pix.field        = (dev->input == AVERTM62_INPUT_COMPOSITE) ?
				  V4L2_FIELD_INTERLACED : V4L2_FIELD_NONE;
	f->fmt.pix.pixelformat  = dev->pixelformat;
	f->fmt.pix.bytesperline = dev->bytesperline;
	f->fmt.pix.sizeimage    = dev->sizeimage;
	f->fmt.pix.colorspace   = V4L2_COLORSPACE_REC709;
	return 0;
}

static int avertm62_try_fmt_vid_cap(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	struct avertm62_dev *dev = video_drvdata(file);
	u32 w = f->fmt.pix.width;
	u32 h = f->fmt.pix.height;

	if (f->fmt.pix.pixelformat != V4L2_PIX_FMT_UYVY)
		f->fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;

	if (dev->input == AVERTM62_INPUT_COMPOSITE) {
		/* The decoder output size is fixed (no scaler support yet) */
		w = AVERTM62_PAL_WIDTH;
		h = AVERTM62_PAL_HEIGHT;
	} else {
		w = clamp_t(u32, ALIGN(w, 2), 640, AVERTM62_MAX_WIDTH);
		h = clamp_t(u32, ALIGN(h, 2), 480, AVERTM62_MAX_HEIGHT);
	}

	f->fmt.pix.width        = w;
	f->fmt.pix.height       = h;
	f->fmt.pix.field        = (dev->input == AVERTM62_INPUT_COMPOSITE) ?
				  V4L2_FIELD_INTERLACED : V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = w * 2;
	f->fmt.pix.sizeimage    = w * h * 2;
	f->fmt.pix.colorspace   = V4L2_COLORSPACE_REC709;
	return 0;
}

static int avertm62_s_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct avertm62_dev *dev = video_drvdata(file);
	int ret;

	if (vb2_is_busy(&dev->vb2_q))
		return -EBUSY;

	ret = avertm62_try_fmt_vid_cap(file, priv, f);
	if (ret < 0)
		return ret;

	dev->width        = f->fmt.pix.width;
	dev->height       = f->fmt.pix.height;
	dev->pixelformat  = f->fmt.pix.pixelformat;
	dev->bytesperline = f->fmt.pix.bytesperline;
	dev->sizeimage    = f->fmt.pix.sizeimage;

	if (dev->input == 0) {
		u32 dw = 0, dh = 0;
		bool dinter = false;

		if (avertm62_detect_hdmi_timings(dev, &dw, &dh, &dinter))
			dev->interlaced = dinter;
	}
	return 0;
}

static int avertm62_enum_input(struct file *file, void *priv,
			       struct v4l2_input *inp)
{
	struct avertm62_dev *dev = video_drvdata(file);
	u32 w = 0, h = 0;
	bool inter = false;

	switch (inp->index) {
	case 0:
		strscpy(inp->name, "HDMI", sizeof(inp->name));
		inp->type = V4L2_INPUT_TYPE_CAMERA;
		if (!avertm62_detect_hdmi_timings(dev, &w, &h, &inter))
			inp->status = V4L2_IN_ST_NO_SIGNAL | V4L2_IN_ST_NO_SYNC;
		return 0;
	case AVERTM62_INPUT_COMPOSITE:
		strscpy(inp->name, "Composite", sizeof(inp->name));
		inp->type = V4L2_INPUT_TYPE_CAMERA;
		inp->std = V4L2_STD_PAL;
		return 0;
	default:
		return -EINVAL;
	}
}

static int avertm62_g_input(struct file *file, void *priv, unsigned int *i)
{
	struct avertm62_dev *dev = video_drvdata(file);

	*i = dev->input;
	return 0;
}

static int avertm62_s_input(struct file *file, void *priv, unsigned int i)
{
	struct avertm62_dev *dev = video_drvdata(file);

	if (i >= AVERTM62_NUM_INPUTS)
		return -EINVAL;
	if (i == dev->input)
		return 0;
	if (vb2_is_busy(&dev->vb2_q))
		return -EBUSY;

	dev->input = i;
	if (i == AVERTM62_INPUT_COMPOSITE) {
		dev->width = AVERTM62_PAL_WIDTH;
		dev->height = AVERTM62_PAL_HEIGHT;
		dev->interlaced = true;
	} else {
		dev->width = AVERTM62_MAX_WIDTH;
		dev->height = AVERTM62_MAX_HEIGHT;
	}
	dev->bytesperline = dev->width * 2;
	dev->sizeimage = dev->bytesperline * dev->height;
	return 0;
}

static int avertm62_enum_framesizes(struct file *file, void *priv,
				    struct v4l2_frmsizeenum *fsize)
{
	struct avertm62_dev *dev = video_drvdata(file);
	static const struct { u32 w; u32 h; } sizes[] = {
		{ 1920, 1080 },
		{ 1280,  720 },
		{  720,  576 },
		{  720,  480 },
		{  640,  480 },
	};

	if (fsize->pixel_format != V4L2_PIX_FMT_YUYV &&
	    fsize->pixel_format != V4L2_PIX_FMT_UYVY)
		return -EINVAL;
	if (dev->input == AVERTM62_INPUT_COMPOSITE) {
		if (fsize->index > 0)
			return -EINVAL;
		fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		fsize->discrete.width = AVERTM62_PAL_WIDTH;
		fsize->discrete.height = AVERTM62_PAL_HEIGHT;
		return 0;
	}
	if (fsize->index >= ARRAY_SIZE(sizes))
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = sizes[fsize->index].w;
	fsize->discrete.height = sizes[fsize->index].h;
	return 0;
}

static int avertm62_enum_frameintervals(struct file *file, void *priv,
					struct v4l2_frmivalenum *fival)
{
	if (fival->index > 0)
		return -EINVAL;
	if (fival->pixel_format != V4L2_PIX_FMT_YUYV &&
	    fival->pixel_format != V4L2_PIX_FMT_UYVY)
		return -EINVAL;

	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fival->discrete.numerator = 1;
	fival->discrete.denominator =
		(((struct avertm62_dev *)video_drvdata(file))->input ==
		 AVERTM62_INPUT_COMPOSITE) ? 25 : 30;
	return 0;
}

static int avertm62_g_parm(struct file *file, void *priv,
			   struct v4l2_streamparm *sp)
{
	if (sp->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	sp->parm.capture.capability   = V4L2_CAP_TIMEPERFRAME;
	sp->parm.capture.readbuffers  = 4;
	sp->parm.capture.timeperframe.numerator   = 1;
	sp->parm.capture.timeperframe.denominator =
		(((struct avertm62_dev *)video_drvdata(file))->input ==
		 AVERTM62_INPUT_COMPOSITE) ? 25 : 30;
	return 0;
}

static int avertm62_log_status(struct file *file, void *priv)
{
	struct avertm62_dev *dev = video_drvdata(file);
	u32 w = 0, h = 0;
	bool inter = false;
	bool locked = avertm62_detect_hdmi_timings(dev, &w, &h, &inter);

	if (locked)
		dev->interlaced = inter;

	dev_info(&dev->pdev->dev, "=== AVerMedia H727 (TM6202) Status ===\n");
	dev_info(&dev->pdev->dev,
		 "  HDMILock=%s (%ux%u%s) sii_mode=%u IRQs=%u (seen_bits=0x%06x) Frames=%u\n",
		 locked ? "LOCKED" : "NO SIGNAL", w, h, inter ? "i" : "p",
		 dev->sii_vid_mode, dev->irq_count, dev->last_irq_pending,
		 dev->dma_frame_count);
	if (dev->hdmi_chip == HDMI_CHIP_SII9011) {
		u8 r06 = 0, r08 = 0, r48 = 0, r4a = 0, r45 = 0, r55 = 0, r78 = 0, r3e = 0;
		u8 r3a = 0, r3b = 0, r3c = 0, r3d = 0;
		u8 r4e = 0, r4f = 0, r50 = 0, r51 = 0;

		tm62_i2c_read_reg8(dev, 0, 0x60, 0x06, &r06);
		tm62_i2c_read_reg8(dev, 0, 0x60, 0x08, &r08);
		tm62_i2c_read_reg8(dev, 0, 0x60, 0x48, &r48);
		tm62_i2c_read_reg8(dev, 0, 0x60, 0x4a, &r4a);
		tm62_i2c_read_reg8(dev, 0, 0x68, 0x45, &r45);
		tm62_i2c_read_reg8(dev, 0, 0x60, 0x55, &r55);
		tm62_i2c_read_reg8(dev, 0, 0x60, 0x78, &r78);
		tm62_i2c_read_reg8(dev, 0, 0x68, 0x3e, &r3e);
		tm62_i2c_read_reg8(dev, 0, 0x60, 0x3a, &r3a);
		tm62_i2c_read_reg8(dev, 0, 0x60, 0x3b, &r3b);
		tm62_i2c_read_reg8(dev, 0, 0x60, 0x3c, &r3c);
		tm62_i2c_read_reg8(dev, 0, 0x60, 0x3d, &r3d);
		tm62_i2c_read_reg8(dev, 0, 0x60, 0x4e, &r4e);
		tm62_i2c_read_reg8(dev, 0, 0x60, 0x4f, &r4f);
		tm62_i2c_read_reg8(dev, 0, 0x60, 0x50, &r50);
		tm62_i2c_read_reg8(dev, 0, 0x60, 0x51, &r51);
		dev_info(&dev->pdev->dev,
			 "  SiI9011: r06=%02x r08=%02x r48=%02x r4a=%02x r45=%02x r55=%02x r78=%02x r3e=%02x DE_PIX=%u DE_LIN=%u H_TOT=%u V_TOT=%u\n",
			 r06, r08, r48, r4a, r45, r55, r78, r3e,
			 ((r4f & 0x1f) << 8) | r4e, ((r51 & 0x1f) << 8) | r50,
			 ((r3b & 0x1f) << 8) | r3a, ((r3d & 0x1f) << 8) | r3c);
	}
	dev_info(&dev->pdev->dev,
		 "  CH1: VID_CTRL=%08x VID_TIMING=%08x VID_ROUTE=%08x CROP_H=%08x CROP_V=%08x\n",
		 tm62_read32(dev, TM62_REG_CH1_VID_CTRL),
		 tm62_read32(dev, TM62_REG_CH1_VID_TIMING),
		 tm62_read32(dev, TM62_REG_VID_ROUTE),
		 tm62_read32(dev, TM62_REG_CH1_CROP_H),
		 tm62_read32(dev, TM62_REG_CH1_CROP_V));
	dev_info(&dev->pdev->dev,
		 "  DMA: CH0_CFG=%08x CH1_CFG=%08x FIFO_CNT(E0/E1)=%u/%u FMT(E0/E1)=%08x/%08x\n",
		 tm62_read32(dev, TM62_REG_CH0_DMA_CFG),
		 tm62_read32(dev, TM62_REG_CH1_DMA_CFG),
		 tm62_read_bf(dev, TM62_REG_CH0_FIFO_CNT, 0, 4),
		 tm62_read_bf(dev, TM62_REG_CH1_FIFO_CNT, 0, 4),
		 tm62_read32(dev, TM62_REG_CH0_FMT_CFG),
		 tm62_read32(dev, TM62_REG_CH1_FMT_CFG));
	dev_info(&dev->pdev->dev,
		 "  CLK_CFG=%08x (src[18:16]=%u) FMT1[29]=%u PLL_CTRL0=%08x STREAM_ROUTE=%08x\n",
		 tm62_read32(dev, TM62_REG_CLK_CFG),
		 tm62_read_bf(dev, TM62_REG_CLK_CFG, 16, 3),
		 tm62_read_bf(dev, TM62_REG_CH1_FMT_CFG, 29, 1),
		 tm62_read32(dev, TM62_REG_PLL_CTRL0),
		 tm62_read32(dev, TM62_REG_STREAM_ROUTE));
	dev_info(&dev->pdev->dev,
		 "  CH1 SLOTS: S0=%08x/%08x S1=%08x/%08x S2=%08x/%08x S3=%08x/%08x\n",
		 tm62_read32(dev, TM62_REG_CH1_SLOT0_STS),
		 tm62_read32(dev, TM62_REG_CH1_SLOT0_PTA),
		 tm62_read32(dev, TM62_REG_CH1_SLOT1_STS),
		 tm62_read32(dev, TM62_REG_CH1_SLOT1_PTA),
		 tm62_read32(dev, TM62_REG_CH1_SLOT2_STS),
		 tm62_read32(dev, TM62_REG_CH1_SLOT2_PTA),
		 tm62_read32(dev, TM62_REG_CH1_SLOT3_STS),
		 tm62_read32(dev, TM62_REG_CH1_SLOT3_PTA));
	return 0;
}

static const struct v4l2_ioctl_ops avertm62_ioctl_ops = {
	.vidioc_querycap            = avertm62_querycap,
	.vidioc_enum_fmt_vid_cap    = avertm62_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap       = avertm62_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap     = avertm62_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap       = avertm62_s_fmt_vid_cap,
	.vidioc_enum_framesizes     = avertm62_enum_framesizes,
	.vidioc_enum_frameintervals = avertm62_enum_frameintervals,
	.vidioc_g_parm              = avertm62_g_parm,
	.vidioc_s_parm              = avertm62_g_parm,
	.vidioc_enum_input          = avertm62_enum_input,
	.vidioc_g_input             = avertm62_g_input,
	.vidioc_s_input             = avertm62_s_input,
	.vidioc_reqbufs             = vb2_ioctl_reqbufs,
	.vidioc_create_bufs         = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf         = vb2_ioctl_prepare_buf,
	.vidioc_querybuf            = vb2_ioctl_querybuf,
	.vidioc_qbuf                = vb2_ioctl_qbuf,
	.vidioc_dqbuf               = vb2_ioctl_dqbuf,
	.vidioc_expbuf              = vb2_ioctl_expbuf,
	.vidioc_streamon            = vb2_ioctl_streamon,
	.vidioc_streamoff           = vb2_ioctl_streamoff,
	.vidioc_log_status          = avertm62_log_status,
};

static const struct v4l2_file_operations avertm62_fops = {
	.owner          = THIS_MODULE,
	.open           = v4l2_fh_open,
	.release        = vb2_fop_release,
	.read           = vb2_fop_read,
	.poll           = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap           = vb2_fop_mmap,
};

/* --- debug sysfs: DMA buffer page tables and contents --- */
static ssize_t dbg_pt_show(struct device *d, struct device_attribute *attr, char *buf)
{
	struct avertm62_dev *dev = pci_get_drvdata(to_pci_dev(d));
	ssize_t n = 0;
	int i;

	for (i = 0; i < AVERTM62_HW_BUFS; i++)
		n += sysfs_emit_at(buf, n, "%d 0x%05llx %u\n", i,
				   (unsigned long long)(dev->hw_bufs[i].pt_dma >> 12),
				   dev->hw_bufs[i].num_pages);
	return n;
}
static DEVICE_ATTR_RO(dbg_pt);

static ssize_t dbg_hwbuf_read(struct file *f, struct kobject *kobj,
			      const struct bin_attribute *attr, char *buf,
			      loff_t off, size_t count)
{
	struct avertm62_dev *dev = pci_get_drvdata(to_pci_dev(kobj_to_dev(kobj)));
	const loff_t per_buf = (loff_t)AVERTM62_PT_PAGES * PAGE_SIZE;
	size_t done = 0;

	while (done < count) {
		loff_t pos = off + done;
		int b = pos / per_buf;
		loff_t in = pos % per_buf;
		int pg = in >> PAGE_SHIFT;
		size_t po = in & (PAGE_SIZE - 1);
		size_t chunk = min_t(size_t, count - done, PAGE_SIZE - po);

		if (b >= AVERTM62_HW_BUFS || pg >= dev->hw_bufs[b].num_pages)
			break;
		memcpy(buf + done, (u8 *)dev->hw_bufs[b].page_cpu[pg] + po, chunk);
		done += chunk;
	}
	return done;
}
static const struct bin_attribute bin_attr_dbg_hwbuf = {
	.attr = { .name = "dbg_hwbuf", .mode = 0444 },
	.size = (loff_t)AVERTM62_HW_BUFS * AVERTM62_PT_PAGES * PAGE_SIZE,
	.read_new = dbg_hwbuf_read,
};

static void avertm62_dvb_exit(struct avertm62_dev *dev)
{
	if (dev->dvb_fe_registered)
		dvb_unregister_frontend(dev->fe);
	if (dev->fe)
		dvb_frontend_detach(dev->fe);
	if (dev->dvb_adap_registered)
		dvb_unregister_adapter(&dev->dvb_adap);
	dev->fe = NULL;
	dev->dvb_fe_registered = false;
	dev->dvb_adap_registered = false;
}

static int avertm62_dvb_init(struct avertm62_dev *dev)
{
	struct drxk_config drxk_cfg = {
		.adr = 0x29,		/* 8-bit 0x52 */
		.no_i2c_bridge = true,	/* tuner answers with the DRX-K held in reset */
		.chunk_size = 56,
		.microcode_name = drxk_fw,
		.parallel_ts = !!drxk_parallel,
	};
	static struct tda18271_config tda_cfg = {
		.gate = TDA18271_GATE_AUTO,
	};
	struct i2c_adapter *i2c = &dev->i2c_bus[0].adap;
	int ret;

	/* Pulse the DRX-K reset (INF: "GPIO Reset Demodulator" = 5) */
	tm62_gpio_dir(dev, 5, true);
	tm62_gpio_set(dev, 5, false);
	msleep(20);
	tm62_gpio_set(dev, 5, true);
	msleep(100);

	ret = dvb_register_adapter(&dev->dvb_adap, "AVerMedia H727", THIS_MODULE,
				   &dev->pdev->dev, adapter_nr);
	if (ret < 0)
		return ret;
	dev->dvb_adap_registered = true;

	dev->fe = dvb_attach(drxk_attach, &drxk_cfg, i2c);
	if (!dev->fe) {
		dev_err(&dev->pdev->dev, "DRX-K demodulator attach failed\n");
		ret = -ENODEV;
		goto err;
	}
	if (!dvb_attach(tda18271_attach, dev->fe, 0x60, i2c, &tda_cfg)) {
		dev_err(&dev->pdev->dev, "TDA18271 tuner attach failed\n");
		ret = -ENODEV;
		goto err;
	}
	ret = dvb_register_frontend(&dev->dvb_adap, dev->fe);
	if (ret < 0)
		goto err;
	dev->dvb_fe_registered = true;
	dev_info(&dev->pdev->dev, "DVB frontend registered (DRX-K + TDA18271)\n");
	return 0;

err:
	avertm62_dvb_exit(dev);
	return ret;
}

static void avertm62_probe_i2c_and_hdmi(struct avertm62_dev *dev)
{
	u8 val = 0;

	tm62_gpio_set(dev, 7, false);
	msleep(50);
	tm62_gpio_set(dev, 7, true);
	msleep(100);

	if (tm62_i2c_read_reg8(dev, 0, 0xa0, 0x30, &val) == 0)
		dev->eeprom_id = val;

	if (dev->eeprom_id == 0x0b && it6603_hw_init(dev) == 0) {
		dev->hdmi_chip = HDMI_CHIP_IT6603;
		return;
	}

	if (sii9011_hw_init(dev) == 0)
		dev->hdmi_chip = HDMI_CHIP_SII9011;
	else if (it6603_hw_init(dev) == 0)
		dev->hdmi_chip = HDMI_CHIP_IT6603;
	else
		dev_warn(&dev->pdev->dev, "No HDMI receiver responded on I2C0\n");
}

static int avertm62_pci_probe(struct pci_dev *pdev,
			      const struct pci_device_id *ent)
{
	struct avertm62_dev *dev;
	struct vb2_queue *q;
	struct video_device *vdev;
	int ret, i;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->pdev = pdev;
	mutex_init(&dev->lock);
	mutex_init(&dev->i2c_lock);
	spin_lock_init(&dev->qlock);
	INIT_LIST_HEAD(&dev->buf_list);
	timer_setup(&dev->fallback_timer, avertm62_fallback_timer_fn, 0);

	dev->width        = 1920;
	dev->height       = 1080;
	dev->pixelformat  = V4L2_PIX_FMT_YUYV;
	dev->bytesperline = dev->width * 2;
	dev->sizeimage    = dev->width * dev->height * 2;
	dev->fps          = 60;
	dev->input        = 0;
	dev->active_pattern = -1;

	ret = pci_enable_device(pdev);
	if (ret < 0)
		return ret;

	pci_set_master(pdev);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret < 0)
		goto err_disable_pci;

	ret = pci_request_regions(pdev, DRV_NAME);
	if (ret < 0)
		goto err_disable_pci;

	dev->mmio = pci_iomap(pdev, 0, 0);
	if (!dev->mmio) {
		ret = -ENOMEM;
		goto err_release_regions;
	}

	ret = avertm62_alloc_hw_bufs(dev);
	if (ret < 0)
		goto err_iounmap;

	tm62_hw_power_and_pll_init(dev);
	tm62_bus_master_init(dev);

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_INTX);
	if (ret < 0)
		goto err_free_hw_bufs;

	ret = request_irq(pci_irq_vector(pdev, 0), avertm62_irq,
			  IRQF_SHARED, DRV_NAME, dev);
	if (ret < 0) {
		pci_free_irq_vectors(pdev);
		goto err_free_hw_bufs;
	}

	for (i = 0; i < 2; i++) {
		struct avertm62_i2c_bus *ibus = &dev->i2c_bus[i];

		ibus->dev = dev;
		ibus->bus_id = i;
		ibus->reg_base = (i == 0) ? TM62_REG_I2C0_TX : TM62_REG_I2C1_TX;
		ibus->adap.owner = THIS_MODULE;
		ibus->adap.algo = &avertm62_i2c_algo;
		ibus->adap.dev.parent = &pdev->dev;
		snprintf(ibus->adap.name, sizeof(ibus->adap.name),
			 "avertm62-i2c%d", i);
		i2c_set_adapdata(&ibus->adap, ibus);
		ret = i2c_add_adapter(&ibus->adap);
		if (ret < 0) {
			if (i == 1)
				i2c_del_adapter(&dev->i2c_bus[0].adap);
			goto err_free_irq;
		}
	}

	avertm62_probe_i2c_and_hdmi(dev);

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret < 0)
		goto err_del_i2c;

	q = &dev->vb2_q;
	q->type            = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes        = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
	q->drv_priv        = dev;
	q->buf_struct_size = sizeof(struct avertm62_vb2_buf);
	q->ops             = &avertm62_vb2_ops;
	q->mem_ops         = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_queued_buffers = 2;
	q->lock            = &dev->lock;
	q->dev             = &pdev->dev;

	ret = vb2_queue_init(q);
	if (ret < 0)
		goto err_unreg_v4l2;

	vdev = &dev->vdev;
	strscpy(vdev->name, "AVerMedia H727 HDMI Capture", sizeof(vdev->name));
	vdev->fops        = &avertm62_fops;
	vdev->ioctl_ops   = &avertm62_ioctl_ops;
	vdev->v4l2_dev    = &dev->v4l2_dev;
	vdev->queue       = q;
	vdev->lock        = &dev->lock;
	vdev->release     = video_device_release_empty;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			    V4L2_CAP_READWRITE;
	video_set_drvdata(vdev, dev);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret < 0)
		goto err_unreg_v4l2;

	pci_set_drvdata(pdev, dev);
	dev_info(&pdev->dev, "Registered V4L2 capture device %s\n",
		 video_device_node_name(vdev));

	if (debug_sysfs) {
		if (device_create_file(&pdev->dev, &dev_attr_dbg_pt) ||
		    device_create_bin_file(&pdev->dev, &bin_attr_dbg_hwbuf))
			dev_warn(&pdev->dev, "debug sysfs files not created\n");
	}

	if (dvb && avertm62_dvb_init(dev) < 0)
		dev_warn(&pdev->dev, "DVB frontend not available; analog capture still works\n");
	return 0;

err_unreg_v4l2:
	v4l2_device_unregister(&dev->v4l2_dev);
err_del_i2c:
	i2c_del_adapter(&dev->i2c_bus[1].adap);
	i2c_del_adapter(&dev->i2c_bus[0].adap);
err_free_irq:
	free_irq(pci_irq_vector(pdev, 0), dev);
	pci_free_irq_vectors(pdev);
err_free_hw_bufs:
	avertm62_free_hw_bufs(dev);
err_iounmap:
	pci_iounmap(pdev, dev->mmio);
err_release_regions:
	pci_release_regions(pdev);
err_disable_pci:
	pci_disable_device(pdev);
	return ret;
}

static void avertm62_pci_remove(struct pci_dev *pdev)
{
	struct avertm62_dev *dev = pci_get_drvdata(pdev);

	avertm62_timer_del_sync(&dev->fallback_timer);
	avertm62_hw_stop_streaming(dev);
	tm62_write32(dev, TM62_REG_IRQ_MASK, TM62_IRQ_MASK_ALL);

	if (debug_sysfs) {
		device_remove_bin_file(&pdev->dev, &bin_attr_dbg_hwbuf);
		device_remove_file(&pdev->dev, &dev_attr_dbg_pt);
	}
	avertm62_dvb_exit(dev);
	video_unregister_device(&dev->vdev);
	v4l2_device_unregister(&dev->v4l2_dev);
	free_irq(pci_irq_vector(pdev, 0), dev);
	pci_free_irq_vectors(pdev);
	i2c_del_adapter(&dev->i2c_bus[1].adap);
	i2c_del_adapter(&dev->i2c_bus[0].adap);
	avertm62_free_hw_bufs(dev);
	pci_iounmap(pdev, dev->mmio);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static const struct pci_device_id avertm62_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_TRIDENT_TM62, PCI_DEVICE_ID_TRIDENT_TM6202) },
	{ PCI_DEVICE(PCI_VENDOR_ID_TRIDENT_TM62, PCI_DEVICE_ID_TRIDENT_TM6200) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, avertm62_pci_tbl);

static struct pci_driver avertm62_pci_driver = {
	.name     = DRV_NAME,
	.id_table = avertm62_pci_tbl,
	.probe    = avertm62_pci_probe,
	.remove   = avertm62_pci_remove,
};

module_pci_driver(avertm62_pci_driver);

MODULE_DESCRIPTION("AVerMedia H727 / C027 (Trident TM6202) PCIe HDMI Capture V4L2 Driver");
MODULE_AUTHOR("Antigravity Reverse Engineering");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
