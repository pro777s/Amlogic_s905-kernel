/*
 * drivers/amlogic/amports/vavs.c
 *
 * Copyright (C) 2015 Amlogic, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
*/
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/kfifo.h>
#include <linux/platform_device.h>
#include <linux/amlogic/amports/amstream.h>
#include <linux/amlogic/amports/ptsserv.h>
#include <linux/amlogic/canvas/canvas.h>
#include <linux/amlogic/amports/vframe.h>
#include <linux/amlogic/amports/vframe_provider.h>
#include <linux/amlogic/amports/vframe_receiver.h>
#include <linux/amlogic/amports/vformat.h>
#include "vdec_reg.h"
#include "streambuf_reg.h"
#include "amvdec.h"
#include "arch/register.h"
#include "amports_priv.h"
#include <linux/dma-mapping.h>
#include <linux/amlogic/codec_mm/codec_mm.h>
#include <linux/slab.h>
#include "avs.h"

#define DRIVER_NAME "amvdec_avs"
#define MODULE_NAME "amvdec_avs"

#if 1/* MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON6 */
#define NV21
#endif

#define USE_AVS_SEQ_INFO
#define HANDLE_AVS_IRQ
#define DEBUG_PTS

#define I_PICTURE   0
#define P_PICTURE   1
#define B_PICTURE   2

/* #define ORI_BUFFER_START_ADDR   0x81000000 */
#define ORI_BUFFER_START_ADDR   0x80000000

#define INTERLACE_FLAG          0x80
#define TOP_FIELD_FIRST_FLAG 0x40

/* protocol registers */
#define AVS_PIC_RATIO       AV_SCRATCH_0
#define AVS_PIC_WIDTH      AV_SCRATCH_1
#define AVS_PIC_HEIGHT     AV_SCRATCH_2
#define AVS_FRAME_RATE     AV_SCRATCH_3

#define AVS_ERROR_COUNT    AV_SCRATCH_6
#define AVS_SOS_COUNT     AV_SCRATCH_7
#define AVS_BUFFERIN       AV_SCRATCH_8
#define AVS_BUFFEROUT      AV_SCRATCH_9
#define AVS_REPEAT_COUNT    AV_SCRATCH_A
#define AVS_TIME_STAMP      AV_SCRATCH_B
#define AVS_OFFSET_REG      AV_SCRATCH_C
#define MEM_OFFSET_REG      AV_SCRATCH_F
#define AVS_ERROR_RECOVERY_MODE   AV_SCRATCH_G

#define VF_POOL_SIZE        32
#define PUT_INTERVAL        (HZ/100)

#if 1 /*MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8*/
#define INT_AMVENCODER INT_DOS_MAILBOX_1
#else
/* #define AMVENC_DEV_VERSION "AML-MT" */
#define INT_AMVENCODER INT_MAILBOX_1A
#endif

#define VPP_VD1_POSTBLEND       (1 << 10)

static int debug_flag;

static int firmware_sel; /* 0, normal; 1, old ucode */

int avs_get_debug_flag(void)
{
	return debug_flag;
}

static struct vframe_s *vavs_vf_peek(void *);
static struct vframe_s *vavs_vf_get(void *);
static void vavs_vf_put(struct vframe_s *, void *);
static int vavs_vf_states(struct vframe_states *states, void *);

static const char vavs_dec_id[] = "vavs-dev";

#define PROVIDER_NAME   "decoder.avs"
static DEFINE_SPINLOCK(lock);

static const struct vframe_operations_s vavs_vf_provider = {
	.peek = vavs_vf_peek,
	.get = vavs_vf_get,
	.put = vavs_vf_put,
	.vf_states = vavs_vf_states,
};

static struct vframe_provider_s vavs_vf_prov;

#define  VF_BUF_NUM_MAX 16

/*static u32 vf_buf_num = 4*/
static u32 vf_buf_num = 4;
static u32 vf_buf_num_used;
static u32 canvas_base = 128;
#ifdef NV21
	int	canvas_num = 2; /*NV21*/
#else
	int	canvas_num = 3;
#endif
	static u32 work_buf_size;

static struct vframe_s vfpool[VF_POOL_SIZE];
static s32 vfbuf_use[VF_BUF_NUM_MAX];
static u32 saved_resolution;
static u32 frame_width, frame_height, frame_dur, frame_prog;
static struct timer_list recycle_timer;
static u32 stat;
static unsigned long buf_start;
static u32 buf_size, buf_offset;
static u32 avi_flag;
static u32 vavs_ratio;
static u32 pic_type;
static u32 pts_by_offset = 1;
static u32 total_frame;
static u32 next_pts;
static unsigned char throw_pb_flag;
#ifdef DEBUG_PTS
static u32 pts_hit, pts_missed, pts_i_hit, pts_i_missed;
#endif

static u32 radr, rval;
static struct dec_sysinfo vavs_amstream_dec_info;

#ifdef AVSP_LONG_CABAC
static struct work_struct long_cabac_wd_work;
void *es_write_addr_virt;
dma_addr_t es_write_addr_phy;

void *bitstream_read_tmp;
dma_addr_t bitstream_read_tmp_phy;
void *avsp_heap_adr;

#endif

static DECLARE_KFIFO(newframe_q, struct vframe_s *, VF_POOL_SIZE);
static DECLARE_KFIFO(display_q, struct vframe_s *, VF_POOL_SIZE);
static DECLARE_KFIFO(recycle_q, struct vframe_s *, VF_POOL_SIZE);

static inline u32 index2canvas(u32 index)
{
	const u32 canvas_tab[VF_BUF_NUM_MAX] = {
		0x010100, 0x030302, 0x050504, 0x070706,
		0x090908, 0x0b0b0a, 0x0d0d0c, 0x0f0f0e,
		0x111110, 0x131312, 0x151514, 0x171716,
		0x191918, 0x1b1b1a, 0x1d1d1c, 0x1f1f1e,
	};
	const u32 canvas_tab_3[4] = {
		0x010100, 0x040403, 0x070706, 0x0a0a09
	};

	if (canvas_num == 2)
		return canvas_tab[index] + (canvas_base << 16)
		+ (canvas_base << 8) + canvas_base;

	return canvas_tab_3[index] + (canvas_base << 16)
		+ (canvas_base << 8) + canvas_base;
}

static const u32 frame_rate_tab[16] = {
	96000 / 30,		/* forbidden */
	96000 / 24,		/* 24000/1001 (23.967) */
	96000 / 24,
	96000 / 25,
	96000 / 30,		/* 30000/1001 (29.97) */
	96000 / 30,
	96000 / 50,
	96000 / 60,		/* 60000/1001 (59.94) */
	96000 / 60,
	/* > 8 reserved, use 24 */
	96000 / 24, 96000 / 24, 96000 / 24, 96000 / 24,
	96000 / 24, 96000 / 24, 96000 / 24
};

static void set_frame_info(struct vframe_s *vf, unsigned *duration)
{
	int ar = 0;

	unsigned pixel_ratio = READ_VREG(AVS_PIC_RATIO);
#ifndef USE_AVS_SEQ_INFO
	if (vavs_amstream_dec_info.width > 0
		&& vavs_amstream_dec_info.height > 0) {
		vf->width = vavs_amstream_dec_info.width;
		vf->height = vavs_amstream_dec_info.height;
	} else
#endif
	{
		vf->width = READ_VREG(AVS_PIC_WIDTH);
		vf->height = READ_VREG(AVS_PIC_HEIGHT);
		frame_width = vf->width;
		frame_height = vf->height;
		/* pr_info("%s: (%d,%d)\n", __func__,vf->width, vf->height);*/
	}

#ifndef USE_AVS_SEQ_INFO
	if (vavs_amstream_dec_info.rate > 0)
		*duration = vavs_amstream_dec_info.rate;
	else
#endif
	{
		*duration = frame_rate_tab[READ_VREG(AVS_FRAME_RATE) & 0xf];
		/* pr_info("%s: duration = %d\n", __func__, *duration); */
		frame_dur = *duration;
	}

	if (vavs_ratio == 0) {
		/* always stretch to 16:9 */
		vf->ratio_control |= (0x90 <<
				DISP_RATIO_ASPECT_RATIO_BIT);
	} else {
		switch (pixel_ratio) {
		case 1:
			ar = (vf->height * vavs_ratio) / vf->width;
			break;
		case 2:
			ar = (vf->height * 3 * vavs_ratio) / (vf->width * 4);
			break;
		case 3:
			ar = (vf->height * 9 * vavs_ratio) / (vf->width * 16);
			break;
		case 4:
			ar = (vf->height * 100 * vavs_ratio) / (vf->width *
					221);
			break;
		default:
			ar = (vf->height * vavs_ratio) / vf->width;
			break;
		}
	}

	ar = min(ar, DISP_RATIO_ASPECT_RATIO_MAX);

	vf->ratio_control = (ar << DISP_RATIO_ASPECT_RATIO_BIT);
	/*vf->ratio_control |= DISP_RATIO_FORCECONFIG | DISP_RATIO_KEEPRATIO; */

	vf->flag = 0;
}

#ifdef HANDLE_AVS_IRQ
static irqreturn_t vavs_isr(int irq, void *dev_id)
#else
static void vavs_isr(void)
#endif
{
	u32 reg;
	struct vframe_s *vf;
	u32 dur;
	u32 repeat_count;
	u32 picture_type;
	u32 buffer_index;
	u32 picture_struct;
	unsigned int pts, pts_valid = 0, offset;
	if (debug_flag & AVS_DEBUG_UCODE) {
		if (READ_VREG(AV_SCRATCH_E) != 0) {
			pr_info("dbg%x: %x\n", READ_VREG(AV_SCRATCH_E),
				   READ_VREG(AV_SCRATCH_D));
			WRITE_VREG(AV_SCRATCH_E, 0);
		}
	}
#ifdef AVSP_LONG_CABAC
	if (firmware_sel == 0 && READ_VREG(LONG_CABAC_REQ)) {
#ifdef PERFORMANCE_DEBUG
		pr_info("%s:schedule long_cabac_wd_work\r\n", __func__);
#endif
		schedule_work(&long_cabac_wd_work);
	}
#endif
	reg = READ_VREG(AVS_BUFFEROUT);

	if (reg) {
		picture_struct = READ_VREG(AV_SCRATCH_5);
		if (debug_flag & AVS_DEBUG_PRINT)
			pr_info("AVS_BUFFEROUT=%x, picture_struct is 0x%x\n",
				reg, picture_struct);
		if (pts_by_offset) {
			offset = READ_VREG(AVS_OFFSET_REG);
			if (debug_flag & AVS_DEBUG_PRINT)
				pr_info("AVS OFFSET=%x\n", offset);
			if (pts_lookup_offset(PTS_TYPE_VIDEO, offset, &pts, 0)
				== 0) {
				pts_valid = 1;
#ifdef DEBUG_PTS
				pts_hit++;
#endif
			} else {
#ifdef DEBUG_PTS
				pts_missed++;
#endif
			}
		}

		repeat_count = READ_VREG(AVS_REPEAT_COUNT);
		if (firmware_sel == 0)
			buffer_index =
				((reg & 0x7) +
				(((reg >> 8) & 0x3) << 3) - 1) & 0x1f;
		else
			buffer_index =
				((reg & 0x7) - 1) & 3;

		picture_type = (reg >> 3) & 7;
#ifdef DEBUG_PTS
		if (picture_type == I_PICTURE) {
			/* pr_info("I offset 0x%x, pts_valid %d\n",
			   offset, pts_valid); */
			if (!pts_valid)
				pts_i_missed++;
			else
				pts_i_hit++;
		}
#endif

		if (throw_pb_flag && picture_type != I_PICTURE) {

			if (debug_flag & AVS_DEBUG_PRINT) {
				pr_info("picture type %d throwed\n",
					   picture_type);
			}
			WRITE_VREG(AVS_BUFFERIN, ~(1 << buffer_index));
		} else if (reg & INTERLACE_FLAG) {	/* interlace */
			throw_pb_flag = 0;

			if (debug_flag & AVS_DEBUG_PRINT) {
				pr_info("interlace, picture type %d\n",
					   picture_type);
			}

			if (kfifo_get(&newframe_q, &vf) == 0) {
				pr_info
				("fatal error, no available buffer slot.");
				return IRQ_HANDLED;
			}
			set_frame_info(vf, &dur);
			vf->bufWidth = 1920;
			pic_type = 2;
			if ((I_PICTURE == picture_type) && pts_valid) {
				vf->pts = pts;
				if ((repeat_count > 1) && avi_flag) {
					/* next_pts = pts +
					   (vavs_amstream_dec_info.rate *
					   repeat_count >> 1)*15/16; */
					next_pts =
						pts +
						(dur * repeat_count >> 1) *
						15 / 16;
				} else
					next_pts = 0;
			} else {
				vf->pts = next_pts;
				if ((repeat_count > 1) && avi_flag) {
					/* vf->duration =
					   vavs_amstream_dec_info.rate *
					   repeat_count >> 1; */
					vf->duration = dur * repeat_count >> 1;
					if (next_pts != 0) {
						next_pts +=
							((vf->duration) -
							 ((vf->duration) >> 4));
					}
				} else {
					/* vf->duration =
					   vavs_amstream_dec_info.rate >> 1; */
					vf->duration = dur >> 1;
					next_pts = 0;
				}
			}
			vf->signal_type = 0;
			vf->index = buffer_index;
			vf->duration_pulldown = 0;
			vf->type =
				(reg & TOP_FIELD_FIRST_FLAG)
				? VIDTYPE_INTERLACE_TOP
				: VIDTYPE_INTERLACE_BOTTOM;
#ifdef NV21
			vf->type |= VIDTYPE_VIU_NV21;
#endif
			vf->canvas0Addr = vf->canvas1Addr =
				index2canvas(buffer_index);
			vf->type_original = vf->type;

			if (debug_flag & AVS_DEBUG_PRINT) {
				pr_info("buffer_index %d, canvas addr %x\n",
					   buffer_index, vf->canvas0Addr);
			}

			vfbuf_use[buffer_index]++;

			kfifo_put(&display_q,
					  (const struct vframe_s *)vf);
			vf_notify_receiver(PROVIDER_NAME,
					VFRAME_EVENT_PROVIDER_VFRAME_READY,
					NULL);

			if (kfifo_get(&newframe_q, &vf) == 0) {
				pr_info("fatal error, no available buffer slot.");
				return IRQ_HANDLED;
						}
			set_frame_info(vf, &dur);
			vf->bufWidth = 1920;

			vf->pts = next_pts;
			if ((repeat_count > 1) && avi_flag) {
				/* vf->duration = vavs_amstream_dec_info.rate *
				   repeat_count >> 1; */
				vf->duration = dur * repeat_count >> 1;
				if (next_pts != 0) {
					next_pts +=
						((vf->duration) -
						 ((vf->duration) >> 4));
				}
			} else {
				/* vf->duration = vavs_amstream_dec_info.rate
				   >> 1; */
				vf->duration = dur >> 1;
				next_pts = 0;
			}
			vf->signal_type = 0;
			vf->index = buffer_index;
			vf->duration_pulldown = 0;
			vf->type =
				(reg & TOP_FIELD_FIRST_FLAG) ?
				VIDTYPE_INTERLACE_BOTTOM :
				VIDTYPE_INTERLACE_TOP;
#ifdef NV21
			vf->type |= VIDTYPE_VIU_NV21;
#endif
			vf->canvas0Addr = vf->canvas1Addr =
				index2canvas(buffer_index);
			vf->type_original = vf->type;
			vfbuf_use[buffer_index]++;

			kfifo_put(&display_q,
					  (const struct vframe_s *)vf);
			vf_notify_receiver(PROVIDER_NAME,
					VFRAME_EVENT_PROVIDER_VFRAME_READY,
					NULL);
			total_frame++;
		} else {	/* progressive */
			throw_pb_flag = 0;

			if (debug_flag & AVS_DEBUG_PRINT) {
				pr_info("progressive picture type %d\n",
					   picture_type);
			}
			if (kfifo_get(&newframe_q, &vf) == 0) {
				pr_info
				("fatal error, no available buffer slot.");
				return IRQ_HANDLED;
			}
			set_frame_info(vf, &dur);
			vf->bufWidth = 1920;
			pic_type = 1;

			if ((I_PICTURE == picture_type) && pts_valid) {
				vf->pts = pts;
				if ((repeat_count > 1) && avi_flag) {
					/* next_pts = pts +
					   (vavs_amstream_dec_info.rate *
					   repeat_count)*15/16; */
					next_pts =
						pts +
						(dur * repeat_count) * 15 / 16;
				} else
					next_pts = 0;
			} else {
				vf->pts = next_pts;
				if ((repeat_count > 1) && avi_flag) {
					/* vf->duration =
					   vavs_amstream_dec_info.rate *
					   repeat_count; */
					vf->duration = dur * repeat_count;
					if (next_pts != 0) {
						next_pts +=
							((vf->duration) -
							 ((vf->duration) >> 4));
					}
				} else {
					/* vf->duration =
					   vavs_amstream_dec_info.rate; */
					vf->duration = dur;
					next_pts = 0;
				}
			}
			vf->signal_type = 0;
			vf->index = buffer_index;
			vf->duration_pulldown = 0;
			vf->type = VIDTYPE_PROGRESSIVE | VIDTYPE_VIU_FIELD;
#ifdef NV21
			vf->type |= VIDTYPE_VIU_NV21;
#endif
			vf->canvas0Addr = vf->canvas1Addr =
				index2canvas(buffer_index);
			vf->type_original = vf->type;
			if (debug_flag & AVS_DEBUG_PRINT) {
				pr_info("buffer_index %d, canvas addr %x\n",
					   buffer_index, vf->canvas0Addr);
			}

			vfbuf_use[buffer_index]++;
			kfifo_put(&display_q,
					  (const struct vframe_s *)vf);
			vf_notify_receiver(PROVIDER_NAME,
					VFRAME_EVENT_PROVIDER_VFRAME_READY,
					NULL);
			total_frame++;
		}

		/* pr_info("PicType = %d, PTS = 0x%x\n",
		   picture_type, vf->pts); */
		WRITE_VREG(AVS_BUFFEROUT, 0);
	}

	WRITE_VREG(ASSIST_MBOX1_CLR_REG, 1);

#ifdef HANDLE_AVS_IRQ
	return IRQ_HANDLED;
#else
	return;
#endif
}

static int run_flag = 1;
static int step_flag;
static int error_recovery_mode;   /*0: blocky  1: mosaic*/

static struct vframe_s *vavs_vf_peek(void *op_arg)
{
	struct vframe_s *vf;

	if (kfifo_peek(&display_q, &vf))
		return vf;

	return NULL;

}

static struct vframe_s *vavs_vf_get(void *op_arg)
{
	struct vframe_s *vf;

	if (kfifo_get(&display_q, &vf))
		return vf;

	return NULL;

}

static void vavs_vf_put(struct vframe_s *vf, void *op_arg)
{
	kfifo_put(&recycle_q, (const struct vframe_s *)vf);

}

int vavs_dec_status(struct vdec_status *vstatus)
{
	vstatus->width = frame_width;	/* vavs_amstream_dec_info.width; */
	vstatus->height = frame_height;	/* vavs_amstream_dec_info.height; */
	if (0 != frame_dur /*vavs_amstream_dec_info.rate */)
		vstatus->fps = 96000 / frame_dur;
	else
		vstatus->fps = 96000;
	vstatus->error_count = READ_VREG(AVS_ERROR_COUNT);
	vstatus->status = stat;

	return 0;
}

/****************************************/
static void vavs_canvas_init(void)
{
	int i;
	u32 canvas_width, canvas_height;
	u32 decbuf_size, decbuf_y_size, decbuf_uv_size;
	u32 disp_addr = 0xffffffff;
	int vf_buf_num_avail = 0;
	vf_buf_num_used = vf_buf_num;
	if (buf_size <= 0x00400000) {
		/* SD only */
		canvas_width = 768;
		canvas_height = 576;
		decbuf_y_size = 0x80000;
		decbuf_uv_size = 0x20000;
		decbuf_size = 0x100000;
		vf_buf_num_avail =
		((buf_size - work_buf_size) / decbuf_size) - 1;
		pr_info
		("avs(SD):buf_start %p, size %x, offset %x avail %d\n",
		 (void *)buf_start, buf_size, buf_offset,
		 vf_buf_num_avail);
	} else {
		/* HD & SD */
		canvas_width = 1920;
		canvas_height = 1088;
		decbuf_y_size = 0x200000;
		decbuf_uv_size = 0x80000;
		decbuf_size = 0x300000;
		vf_buf_num_avail =
		((buf_size - work_buf_size) / decbuf_size) - 1;
		pr_info("avs: buf_start %p, buf_size %x, buf_offset %x buf avail %d\n",
			   (void *)buf_start, buf_size, buf_offset,
			   vf_buf_num_avail);
	}
	if (vf_buf_num_used > vf_buf_num_avail)
		vf_buf_num_used = vf_buf_num_avail;

	buf_offset = buf_offset + ((vf_buf_num_used + 1) * decbuf_size);
	if (READ_MPEG_REG(VPP_MISC) & VPP_VD1_POSTBLEND) {
		struct canvas_s cur_canvas;

		canvas_read((READ_MPEG_REG(VD1_IF0_CANVAS0) & 0xff),
					&cur_canvas);
		disp_addr = (cur_canvas.addr + 7) >> 3;
	}

	for (i = 0; i < vf_buf_num_used; i++) {
		if (((buf_start + i * decbuf_size + 7) >> 3) == disp_addr) {
#ifdef NV21
			canvas_config(canvas_base + canvas_num * i + 0,
					buf_start +
					vf_buf_num_used * decbuf_size,
					canvas_width, canvas_height,
					CANVAS_ADDR_NOWRAP,
					CANVAS_BLKMODE_32X32);
			canvas_config(canvas_base + canvas_num * i + 1,
					buf_start +
					vf_buf_num_used * decbuf_size +
					decbuf_y_size, canvas_width,
					canvas_height / 2,
					CANVAS_ADDR_NOWRAP,
					CANVAS_BLKMODE_32X32);
#else
			canvas_config(canvas_num * i + 0,
					buf_start + 4 * decbuf_size,
					canvas_width, canvas_height,
					CANVAS_ADDR_NOWRAP,
					CANVAS_BLKMODE_32X32);
			canvas_config(canvas_num * i + 1,
					buf_start + 4 * decbuf_size +
					decbuf_y_size, canvas_width / 2,
					canvas_height / 2,
					CANVAS_ADDR_NOWRAP,
						  CANVAS_BLKMODE_32X32);
			canvas_config(canvas_num * i + 2,
					buf_start + 4 * decbuf_size +
					decbuf_y_size + decbuf_uv_size,
					canvas_width / 2, canvas_height / 2,
					CANVAS_ADDR_NOWRAP,
					CANVAS_BLKMODE_32X32);
#endif
			if (debug_flag & AVS_DEBUG_PRINT) {
				pr_info("canvas config %d, addr %p\n",
					vf_buf_num_used,
					   (void *)(buf_start +
					   vf_buf_num_used * decbuf_size));
			}

		} else {
#ifdef NV21
			canvas_config(canvas_base + canvas_num * i + 0,
					buf_start + i * decbuf_size,
					canvas_width, canvas_height,
					CANVAS_ADDR_NOWRAP,
					CANVAS_BLKMODE_32X32);
			canvas_config(canvas_base + canvas_num * i + 1,
					buf_start + i * decbuf_size +
					decbuf_y_size, canvas_width,
					canvas_height / 2,
					CANVAS_ADDR_NOWRAP,
					CANVAS_BLKMODE_32X32);
#else
			canvas_config(canvas_num * i + 0,
					buf_start + i * decbuf_size,
					canvas_width, canvas_height,
					CANVAS_ADDR_NOWRAP,
					CANVAS_BLKMODE_32X32);
			canvas_config(canvas_num * i + 1,
					buf_start + i * decbuf_size +
					decbuf_y_size, canvas_width / 2,
					canvas_height / 2,
					CANVAS_ADDR_NOWRAP,
					CANVAS_BLKMODE_32X32);
			canvas_config(canvas_num * i + 2,
					buf_start + i * decbuf_size +
					decbuf_y_size + decbuf_uv_size,
					canvas_width / 2, canvas_height / 2,
					CANVAS_ADDR_NOWRAP,
					CANVAS_BLKMODE_32X32);
#endif
			if (debug_flag & AVS_DEBUG_PRINT) {
				pr_info("canvas config %d, addr %p\n", i,
					   (void *)(buf_start +
					   i * decbuf_size));
			}
		}
	}
}

static void vavs_prot_init(void)
{
#if 1 /* MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON6 */
	WRITE_VREG(DOS_SW_RESET0, (1 << 7) | (1 << 6) | (1 << 4));
	WRITE_VREG(DOS_SW_RESET0, 0);

	READ_VREG(DOS_SW_RESET0);

	WRITE_VREG(DOS_SW_RESET0, (1 << 7) | (1 << 6) | (1 << 4));
	WRITE_VREG(DOS_SW_RESET0, 0);

	WRITE_VREG(DOS_SW_RESET0, (1 << 9) | (1 << 8));
	WRITE_VREG(DOS_SW_RESET0, 0);

#else
	WRITE_MPEG_REG(RESET0_REGISTER,
				   RESET_IQIDCT | RESET_MC | RESET_VLD_PART);
	READ_MPEG_REG(RESET0_REGISTER);
	WRITE_MPEG_REG(RESET0_REGISTER,
				   RESET_IQIDCT | RESET_MC | RESET_VLD_PART);

	WRITE_MPEG_REG(RESET2_REGISTER, RESET_PIC_DC | RESET_DBLK);
#endif

	WRITE_VREG(POWER_CTL_VLD, 0x10);
	WRITE_VREG_BITS(VLD_MEM_VIFIFO_CONTROL, 2, MEM_FIFO_CNT_BIT, 2);
	WRITE_VREG_BITS(VLD_MEM_VIFIFO_CONTROL, 8, MEM_LEVEL_CNT_BIT, 6);

	vavs_canvas_init();
	if (firmware_sel == 0)
		WRITE_VREG(AV_SCRATCH_5, 0);
#ifdef NV21
		if (firmware_sel == 0) {
			/* fixed canvas index */
			WRITE_VREG(AV_SCRATCH_0, canvas_base);
			WRITE_VREG(AV_SCRATCH_1, vf_buf_num_used);
		} else {
			int ii;
			for (ii = 0; ii < 4; ii++) {
				WRITE_VREG(AV_SCRATCH_0 + ii,
					(canvas_base + canvas_num * ii) |
					((canvas_base + canvas_num * ii + 1)
						<< 8) |
					((canvas_base + canvas_num * ii + 1)
						<< 16)
				);
			}
			/*
			WRITE_VREG(AV_SCRATCH_0, 0x010100);
			WRITE_VREG(AV_SCRATCH_1, 0x040403);
			WRITE_VREG(AV_SCRATCH_2, 0x070706);
			WRITE_VREG(AV_SCRATCH_3, 0x0a0a09);
			*/
		}
#else
	/* index v << 16 | u << 8 | y */
	WRITE_VREG(AV_SCRATCH_0, 0x020100);
	WRITE_VREG(AV_SCRATCH_1, 0x050403);
	WRITE_VREG(AV_SCRATCH_2, 0x080706);
	WRITE_VREG(AV_SCRATCH_3, 0x0b0a09);
#endif
	/* notify ucode the buffer offset */
	WRITE_VREG(AV_SCRATCH_F, buf_offset);

	/* disable PSCALE for hardware sharing */
	WRITE_VREG(PSCALE_CTRL, 0);

	WRITE_VREG(AVS_SOS_COUNT, 0);
	WRITE_VREG(AVS_BUFFERIN, 0);
	WRITE_VREG(AVS_BUFFEROUT, 0);
	if (error_recovery_mode)
		WRITE_VREG(AVS_ERROR_RECOVERY_MODE, 0);
	else
		WRITE_VREG(AVS_ERROR_RECOVERY_MODE, 1);
	/* clear mailbox interrupt */
	WRITE_VREG(ASSIST_MBOX1_CLR_REG, 1);

	/* enable mailbox interrupt */
	WRITE_VREG(ASSIST_MBOX1_MASK, 1);
#if 1				/* def DEBUG_UCODE */
	WRITE_VREG(AV_SCRATCH_D, 0);
#endif

#ifdef NV21
	SET_VREG_MASK(MDEC_PIC_DC_CTRL, 1 << 17);
#endif

#ifdef PIC_DC_NEED_CLEAR
	CLEAR_VREG_MASK(MDEC_PIC_DC_CTRL, 1 << 31);
#endif

#ifdef AVSP_LONG_CABAC
	if (firmware_sel == 0) {
		WRITE_VREG(LONG_CABAC_DES_ADDR, es_write_addr_phy);
		WRITE_VREG(LONG_CABAC_REQ, 0);
		WRITE_VREG(LONG_CABAC_PIC_SIZE, 0);
		WRITE_VREG(LONG_CABAC_SRC_ADDR, 0);
	}
#endif
}

#ifdef AVSP_LONG_CABAC
static unsigned char es_write_addr[MAX_CODED_FRAME_SIZE]  __aligned(64);
#endif
static void vavs_local_init(void)
{
	int i;

	vavs_ratio = vavs_amstream_dec_info.ratio;

	avi_flag = (unsigned long) vavs_amstream_dec_info.param;

	frame_width = frame_height = frame_dur = frame_prog = 0;

	throw_pb_flag = 1;

	total_frame = 0;
	saved_resolution = 0;
	next_pts = 0;

#ifdef DEBUG_PTS
	pts_hit = pts_missed = pts_i_hit = pts_i_missed = 0;
#endif
	INIT_KFIFO(display_q);
	INIT_KFIFO(recycle_q);
	INIT_KFIFO(newframe_q);

	for (i = 0; i < VF_POOL_SIZE; i++) {
		const struct vframe_s *vf = &vfpool[i];
		vfpool[i].index = vf_buf_num;
		vfpool[i].bufWidth = 1920;
		kfifo_put(&newframe_q, vf);
	}
	for (i = 0; i < vf_buf_num; i++)
		vfbuf_use[i] = 0;

}

static int vavs_vf_states(struct vframe_states *states, void *op_arg)
{
	unsigned long flags;
	spin_lock_irqsave(&lock, flags);
	states->vf_pool_size = VF_POOL_SIZE;
	states->buf_free_num = kfifo_len(&newframe_q);
	states->buf_avail_num = kfifo_len(&display_q);
	states->buf_recycle_num = kfifo_len(&recycle_q);
	spin_unlock_irqrestore(&lock, flags);
	return 0;
}

#ifdef CONFIG_POST_PROCESS_MANAGER
static void vavs_ppmgr_reset(void)
{
	vf_notify_receiver(PROVIDER_NAME, VFRAME_EVENT_PROVIDER_RESET, NULL);

	vavs_local_init();

	pr_info("vavs: vf_ppmgr_reset\n");
}
#endif


static void vavs_put_timer_func(unsigned long arg)
{
	struct timer_list *timer = (struct timer_list *)arg;

#ifndef HANDLE_AVS_IRQ
	vavs_isr();
#endif

#if 1
	if (!error_recovery_mode && READ_VREG(AVS_SOS_COUNT)) {
		pr_info("vavs fatal error reset !\n");
		amvdec_stop();
#ifdef CONFIG_POST_PROCESS_MANAGER
		vavs_ppmgr_reset();
#else
		vf_light_unreg_provider(&vavs_vf_prov);
		vavs_local_init();
		vf_reg_provider(&vavs_vf_prov);
#endif
		vavs_prot_init();
		amvdec_start();
	}
#endif
	if (radr != 0) {
		if (rval != 0) {
			WRITE_VREG(radr, rval);
			pr_info("WRITE_VREG(%x,%x)\n", radr, rval);
		} else
			pr_info("READ_VREG(%x)=%x\n", radr, READ_VREG(radr));
		rval = 0;
		radr = 0;
	}

	if (!kfifo_is_empty(&recycle_q) && (READ_VREG(AVS_BUFFERIN) == 0)) {
		struct vframe_s *vf;
		if (kfifo_get(&recycle_q, &vf)) {
			if ((vf->index < vf_buf_num) &&
			 (--vfbuf_use[vf->index] == 0)) {
				WRITE_VREG(AVS_BUFFERIN, ~(1 << vf->index));
				vf->index = vf_buf_num;
			}
				kfifo_put(&newframe_q,
						  (const struct vframe_s *)vf);
		}

	}
	if (frame_dur > 0 && saved_resolution !=
		frame_width * frame_height * (96000 / frame_dur)) {
		int fps = 96000 / frame_dur;
		saved_resolution = frame_width * frame_height * fps;
		if (firmware_sel == 0 &&
			(debug_flag & AVS_DEBUG_USE_FULL_SPEED)) {
			vdec_source_changed(VFORMAT_AVS,
				4096, 2048, 60);
		} else {
			vdec_source_changed(VFORMAT_AVS,
			frame_width, frame_height, fps);
		}

	}

	timer->expires = jiffies + PUT_INTERVAL;

	add_timer(timer);
}

#ifdef AVSP_LONG_CABAC

static void long_cabac_do_work(struct work_struct *work)
{
#ifdef PERFORMANCE_DEBUG
	pr_info("enter %s buf level (new %d, display %d, recycle %d)\r\n",
		__func__,
		kfifo_len(&newframe_q),
		kfifo_len(&display_q),
		kfifo_len(&recycle_q)
		);
#endif
	while (READ_VREG(LONG_CABAC_REQ))
		process_long_cabac();
#ifdef PERFORMANCE_DEBUG
	pr_info("exit %s buf level (new %d, display %d, recycle %d)\r\n",
		__func__,
		kfifo_len(&newframe_q),
		kfifo_len(&display_q),
		kfifo_len(&recycle_q)
		);
#endif

}
#endif

#ifdef AVSP_LONG_CABAC
static void init_avsp_long_cabac_buf(void)
{
#if 0
	es_write_addr_phy = (unsigned long)codec_mm_alloc_for_dma(
		"vavs",
		PAGE_ALIGN(MAX_CODED_FRAME_SIZE)/PAGE_SIZE,
		0, CODEC_MM_FLAGS_DMA_CPU);
	es_write_addr_virt = codec_mm_phys_to_virt(es_write_addr_phy);

#elif 0
	es_write_addr_virt =
		(void *)dma_alloc_coherent(amports_get_dma_device(),
		 MAX_CODED_FRAME_SIZE, &es_write_addr_phy,
		GFP_KERNEL);
#else
	/*es_write_addr_virt = kmalloc(MAX_CODED_FRAME_SIZE, GFP_KERNEL);
		es_write_addr_virt = (void *)__get_free_pages(GFP_KERNEL,
		get_order(MAX_CODED_FRAME_SIZE));
	*/
	es_write_addr_virt = &es_write_addr[0];
	if (es_write_addr_virt == NULL) {
		pr_err("%s: failed to alloc es_write_addr_virt buffer\n",
			__func__);
		return;
	}

	es_write_addr_phy = dma_map_single(amports_get_dma_device(),
			es_write_addr_virt,
			MAX_CODED_FRAME_SIZE, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(amports_get_dma_device(),
			es_write_addr_phy)) {
		pr_err("%s: failed to map es_write_addr_virt buffer\n",
			__func__);
		/*kfree(es_write_addr_virt);*/
		es_write_addr_virt = NULL;
		return;
	}
#endif


#ifdef BITSTREAM_READ_TMP_NO_CACHE
	bitstream_read_tmp =
		(void *)dma_alloc_coherent(amports_get_dma_device(),
			SVA_STREAM_BUF_SIZE, &bitstream_read_tmp_phy,
			 GFP_KERNEL);

#else

	bitstream_read_tmp = kmalloc(SVA_STREAM_BUF_SIZE, GFP_KERNEL);
		/*bitstream_read_tmp = (void *)__get_free_pages(GFP_KERNEL,
		get_order(MAX_CODED_FRAME_SIZE));
		*/
	if (bitstream_read_tmp == NULL) {
		pr_err("%s: failed to alloc bitstream_read_tmp buffer\n",
			__func__);
		return;
	}

	bitstream_read_tmp_phy = dma_map_single(amports_get_dma_device(),
			bitstream_read_tmp,
			SVA_STREAM_BUF_SIZE, DMA_FROM_DEVICE);
	if (dma_mapping_error(amports_get_dma_device(),
			bitstream_read_tmp_phy)) {
		pr_err("%s: failed to map rpm buffer\n", __func__);
		kfree(bitstream_read_tmp);
		bitstream_read_tmp = NULL;
		return;
	}
#endif
}
#endif


static s32 vavs_init(void)
{
	pr_info("vavs_init\n");
	init_timer(&recycle_timer);

	stat |= STAT_TIMER_INIT;

	amvdec_enable();

	vavs_local_init();

#ifdef AVSP_LONG_CABAC
	if (firmware_sel == 0)
		init_avsp_long_cabac_buf();
#endif
	if (debug_flag & AVS_DEBUG_UCODE) {
		if (amvdec_loadmc_ex(VFORMAT_AVS, "vavs_mc_debug", NULL) < 0) {
			amvdec_disable();
			pr_info("failed\n");
			return -EBUSY;
		}
		pr_info("debug ucode loaded\r\n");
	} else if (firmware_sel == 1) {
		/* old ucode */
		if (amvdec_loadmc_ex(VFORMAT_AVS, "vavs_mc_old", NULL) < 0) {
			amvdec_disable();
			pr_info("failed\n");
			return -EBUSY;
		}
		pr_info("old ucode loaded\r\n");
	} else {
		if (amvdec_loadmc_ex(VFORMAT_AVS, "vavs_mc", NULL) < 0) {
			amvdec_disable();
			pr_info("failed\n");
			return -EBUSY;
		}
		pr_info("ucode loaded\r\n");
	}

	stat |= STAT_MC_LOAD;

	/* enable AMRISC side protocol */
	vavs_prot_init();

#ifdef HANDLE_AVS_IRQ
	if (vdec_request_irq(VDEC_IRQ_1, vavs_isr,
			"vavs-irq", (void *)vavs_dec_id)) {
		amvdec_disable();
		pr_info("vavs irq register error.\n");
		return -ENOENT;
	}
#endif

	stat |= STAT_ISR_REG;

#ifdef CONFIG_POST_PROCESS_MANAGER
	vf_provider_init(&vavs_vf_prov, PROVIDER_NAME, &vavs_vf_provider, NULL);
	vf_reg_provider(&vavs_vf_prov);
	vf_notify_receiver(PROVIDER_NAME, VFRAME_EVENT_PROVIDER_START, NULL);
#else
	vf_provider_init(&vavs_vf_prov, PROVIDER_NAME, &vavs_vf_provider, NULL);
	vf_reg_provider(&vavs_vf_prov);
#endif

	vf_notify_receiver(PROVIDER_NAME, VFRAME_EVENT_PROVIDER_FR_HINT,
					   (void *)((unsigned long)
					   vavs_amstream_dec_info.rate));

	stat |= STAT_VF_HOOK;

	recycle_timer.data = (ulong)(&recycle_timer);
	recycle_timer.function = vavs_put_timer_func;
	recycle_timer.expires = jiffies + PUT_INTERVAL;

	add_timer(&recycle_timer);

	stat |= STAT_TIMER_ARM;

#ifdef AVSP_LONG_CABAC
	if (firmware_sel == 0)
		INIT_WORK(&long_cabac_wd_work, long_cabac_do_work);
#endif

	amvdec_start();

	stat |= STAT_VDEC_RUN;

	set_vdec_func(&vavs_dec_status);

	return 0;
}

static int amvdec_avs_probe(struct platform_device *pdev)
{
	struct vdec_dev_reg_s *pdata =
		(struct vdec_dev_reg_s *)pdev->dev.platform_data;

	if (pdata == NULL) {
		pr_info("amvdec_avs memory resource undefined.\n");
		return -EFAULT;
	}
	if (firmware_sel == 1) {
		vf_buf_num = 4;
		canvas_base = 0;
		canvas_num = 3;
	} else {
		/*if(vf_buf_num <= 4)
			canvas_base = 0;
		else */
		canvas_base = 128;
		canvas_num = 2; /*NV21*/
	}

#ifdef AVSP_LONG_CABAC
	buf_start = pdata->mem_start;
	buf_size = pdata->mem_end - pdata->mem_start + 1
		- (MAX_CODED_FRAME_SIZE * 2)
		- LOCAL_HEAP_SIZE;
	avsp_heap_adr = codec_mm_phys_to_virt(
		pdata->mem_start + buf_size);
#else
	buf_start = pdata->mem_start;
	buf_size = pdata->mem_end - pdata->mem_start + 1;
#endif

	if (buf_start > ORI_BUFFER_START_ADDR)
		buf_offset = buf_start - ORI_BUFFER_START_ADDR;
	else
		buf_offset = buf_start;

	if (pdata->sys_info)
		vavs_amstream_dec_info = *pdata->sys_info;

	pr_info("%s (%d,%d) %d\n", __func__, vavs_amstream_dec_info.width,
		   vavs_amstream_dec_info.height, vavs_amstream_dec_info.rate);
	if (vavs_init() < 0) {
		pr_info("amvdec_avs init failed.\n");

		return -ENODEV;
	}

	return 0;
}

static int amvdec_avs_remove(struct platform_device *pdev)
{
	if (stat & STAT_VDEC_RUN) {
		amvdec_stop();
		stat &= ~STAT_VDEC_RUN;
	}

	if (stat & STAT_ISR_REG) {
		vdec_free_irq(VDEC_IRQ_1, (void *)vavs_dec_id);
		stat &= ~STAT_ISR_REG;
	}

	if (stat & STAT_TIMER_ARM) {
		del_timer_sync(&recycle_timer);
		stat &= ~STAT_TIMER_ARM;
	}
#ifdef AVSP_LONG_CABAC
	if (firmware_sel == 0) {
		cancel_work_sync(&long_cabac_wd_work);

		if (es_write_addr_virt) {
#if 0
			codec_mm_free_for_dma("vavs", es_write_addr_phy);
#else
			dma_unmap_single(amports_get_dma_device(),
				es_write_addr_phy,
				MAX_CODED_FRAME_SIZE, DMA_FROM_DEVICE);
			/*kfree(es_write_addr_virt);*/
			es_write_addr_virt = NULL;
#endif
		}

#ifdef BITSTREAM_READ_TMP_NO_CACHE
		if (bitstream_read_tmp) {
			dma_free_coherent(amports_get_dma_device(),
				SVA_STREAM_BUF_SIZE, bitstream_read_tmp,
				bitstream_read_tmp_phy);
			bitstream_read_tmp = NULL;
		}
#else
		if (bitstream_read_tmp) {
			dma_unmap_single(amports_get_dma_device(),
				bitstream_read_tmp_phy,
				SVA_STREAM_BUF_SIZE, DMA_FROM_DEVICE);
			kfree(bitstream_read_tmp);
			bitstream_read_tmp = NULL;
		}
#endif
	}
#endif
	if (stat & STAT_VF_HOOK) {
		vf_notify_receiver(PROVIDER_NAME,
				VFRAME_EVENT_PROVIDER_FR_END_HINT, NULL);

		vf_unreg_provider(&vavs_vf_prov);
		stat &= ~STAT_VF_HOOK;
	}

	amvdec_disable();

	pic_type = 0;
#ifdef DEBUG_PTS
	pr_info("pts hit %d, pts missed %d, i hit %d, missed %d\n", pts_hit,
		   pts_missed, pts_i_hit, pts_i_missed);
	pr_info("total frame %d, avi_flag %d, rate %d\n", total_frame, avi_flag,
		   vavs_amstream_dec_info.rate);
#endif

	return 0;
}

/****************************************/

static struct platform_driver amvdec_avs_driver = {
	.probe = amvdec_avs_probe,
	.remove = amvdec_avs_remove,
	.driver = {
		.name = DRIVER_NAME,
	}
};

static struct codec_profile_t amvdec_avs_profile = {
	.name = "avs",
	.profile = ""
};

static int __init amvdec_avs_driver_init_module(void)
{
	pr_debug("amvdec_avs module init\n");

	if (platform_driver_register(&amvdec_avs_driver)) {
		pr_info("failed to register amvdec_avs driver\n");
		return -ENODEV;
	}

	if (get_cpu_type() >= MESON_CPU_MAJOR_ID_GXBB)
		amvdec_avs_profile.profile = "avs+";

	vcodec_profile_register(&amvdec_avs_profile);

	return 0;
}

static void __exit amvdec_avs_driver_remove_module(void)
{
	pr_debug("amvdec_avs module remove.\n");

	platform_driver_unregister(&amvdec_avs_driver);
}

/****************************************/

module_param(stat, uint, 0664);
MODULE_PARM_DESC(stat, "\n amvdec_avs stat\n");

module_param(run_flag, uint, 0664);
MODULE_PARM_DESC(run_flag, "\n run_flag\n");

module_param(step_flag, uint, 0664);
MODULE_PARM_DESC(step_flag, "\n step_flag\n");

module_param(debug_flag, uint, 0664);
MODULE_PARM_DESC(debug_flag, "\n debug_flag\n");

module_param(error_recovery_mode, uint, 0664);
MODULE_PARM_DESC(error_recovery_mode, "\n error_recovery_mode\n");


module_param(pic_type, uint, 0444);
MODULE_PARM_DESC(pic_type, "\n amdec_vas picture type\n");

module_param(radr, uint, 0664);
MODULE_PARM_DESC(radr, "\nradr\n");

module_param(rval, uint, 0664);
MODULE_PARM_DESC(rval, "\nrval\n");

module_param(vf_buf_num, uint, 0664);
MODULE_PARM_DESC(vf_buf_num, "\nvf_buf_num\n");

module_param(vf_buf_num_used, uint, 0664);
MODULE_PARM_DESC(vf_buf_num_used, "\nvf_buf_num_used\n");

module_param(canvas_base, uint, 0664);
MODULE_PARM_DESC(canvas_base, "\ncanvas_base\n");

module_param(work_buf_size, uint, 0664);
MODULE_PARM_DESC(work_buf_size, "\nwork_buf_size\n");

module_param(firmware_sel, uint, 0664);
MODULE_PARM_DESC(firmware_sel, "\firmware_sel\n");


module_init(amvdec_avs_driver_init_module);
module_exit(amvdec_avs_driver_remove_module);

MODULE_DESCRIPTION("AMLOGIC AVS Video Decoder Driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Qi Wang <qi.wang@amlogic.com>");
