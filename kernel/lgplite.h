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

#define LGP_DMA_SIZE 0x8000
#define LGP_MAX_FRAME (1920 * 1080 * 2)

struct lgp_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct lgp_signal {
	u32 width, height, fps, control, audio_rate;
};

struct lgp {
	struct usb_device *usb;
	struct v4l2_device v4l2;
	struct video_device video;
	struct vb2_queue queue;
	struct v4l2_ctrl_handler controls;
	struct v4l2_ctrl *quality;
	struct mutex lock;
	spinlock_t buffers_lock;
	struct list_head buffers;
	struct task_struct *worker;
	bool disconnected;
	u32 width, height, fps, sequence;
	unsigned int frame_length;
	u32 frame_pts, last_pts;
	u64 timestamp_base, pts_ticks;
	bool pts_started;
	u8 command[64];
	u8 reply[64];
	u8 *dma;
	u8 *frame;
	bool running;
};

int lgp_hw_start(struct lgp *dev);
int lgp_hw_stop(struct lgp *dev);
/* Returns 1 for a frame, 0 if no message was pending, or a negative errno. */
int lgp_hw_frame(struct lgp *dev, unsigned int *length);

#endif
