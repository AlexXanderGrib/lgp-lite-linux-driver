/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LGPLITE_H
#define LGPLITE_H

#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-v4l2.h>

#define LGP_VIDEO_FIRMWARE "avermedia/qpvidfwusb.bin"
#define LGP_AUDIO_FIRMWARE "avermedia/qpaudfwusb.bin"

#define LGP_COMMAND_SIZE 64		/* largest command or reply */
#define LGP_DMA_SIZE 0x8000		/* largest bulk memory transfer */
#define LGP_MAX_FRAME (1920 * 1080 * 2)	/* upper bound for one JPEG */

struct lgp_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct lgp {
	struct usb_device *usb;
	struct v4l2_device v4l2;
	struct video_device video;
	struct vb2_queue queue;
	struct v4l2_ctrl_handler controls;
	struct v4l2_ctrl *quality;
	/* Serializes ioctls, the buffer queue and stream start/stop. */
	struct mutex lock;
	/* Protects the list of queued buffers. */
	spinlock_t buffers_lock;
	struct list_head buffers;
	/* Polls the encoder while streaming; sole user of the hardware then. */
	struct task_struct *worker;
	bool disconnected;
	bool running;			/* encoder start was sent */

	/* Selected output format. */
	u32 width, height, fps;

	/* Frame being assembled from firmware stream buffers. */
	u8 *frame;
	unsigned int frame_length;
	u32 frame_pts;
	u32 sequence;

	/* Encoder clock, anchored to the monotonic clock at the first frame. */
	bool pts_started;
	u32 last_pts;
	u64 pts_ticks;
	u64 timestamp_base;

	/* USB transfer buffers. Allocated separately so each is DMA-safe. */
	u8 *command;
	u8 *reply;
	u8 *dma;
};

int lgp_hw_start(struct lgp *dev);
int lgp_hw_stop(struct lgp *dev);
/* Returns 1 for a complete frame, 0 if none is ready, or a negative errno. */
int lgp_hw_frame(struct lgp *dev, unsigned int *length);

#endif
