// SPDX-License-Identifier: GPL-2.0-only
/* Native MJPEG webcam interface for AVerMedia Live Gamer Portable Lite. */
#include <linux/delay.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-vmalloc.h>

#include "lgplite.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)
#define min_queued_buffers min_buffers_needed
#endif

/* The encoder scales its input down. All sizes keep the 16:9 aspect of the
 * supported HDMI modes; a 4:3 size would be stretched, not cropped.
 */
static const struct v4l2_frmsize_discrete sizes[] = {
	{ 640, 360 }, { 960, 540 }, { 1280, 720 }, { 1920, 1080 },
};

static const unsigned int rates[] = { 30, 15 };

/* Driver-private control; IDs only need to be unique within the device. */
#define LGP_CID_LIGHT (V4L2_CID_USER_BASE + 0x1f00)

/* Shows the pattern that the Button Light control selects. Callers hold
 * d->lock, which also guards d->running, or run before registration.
 */
static int lgp_show_light(struct lgp *d, s32 light)
{
	if (light == LGP_LIGHT_AUTO)
		light = d->running ? LGP_LIGHT_SOLID : LGP_LIGHT_CLOCKWISE;
	return lgp_hw_light(d, light);
}

/* Follows a capture start or stop while the light is automatic. */
static void lgp_follow_capture(struct lgp *d)
{
	int ret;

	if (v4l2_ctrl_g_ctrl(d->light) != LGP_LIGHT_AUTO)
		return;
	ret = lgp_show_light(d, LGP_LIGHT_AUTO);
	if (ret)
		dev_warn(&d->usb->dev, "Cannot set the button light: %d\n", ret);
}

static int lgp_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct lgp *d = container_of(ctrl->handler, struct lgp, controls);

	/* Only the light control has these ops. */
	return lgp_show_light(d, ctrl->val);
}

static const struct v4l2_ctrl_ops lgp_ctrl_ops = {
	.s_ctrl = lgp_s_ctrl,
};

static const char * const lgp_light_menu[] = {
	[LGP_LIGHT_AUTO] = "Automatic",
	[LGP_LIGHT_CLOCKWISE] = "Clockwise",
	[LGP_LIGHT_SOLID] = "Solid",
	[LGP_LIGHT_FADE] = "Fade",
	[LGP_LIGHT_FAST] = "Fast Clockwise",
	NULL,
};

static const struct v4l2_ctrl_config lgp_light_control = {
	.ops = &lgp_ctrl_ops,
	.id = LGP_CID_LIGHT,
	.name = "Button Light",
	.type = V4L2_CTRL_TYPE_MENU,
	.max = LGP_LIGHT_FAST,
	.def = LGP_LIGHT_AUTO,
	.qmenu = lgp_light_menu,
	/* Writing the current value again restarts the animation. */
	.flags = V4L2_CTRL_FLAG_EXECUTE_ON_WRITE,
};

static void lgp_return_buffers(struct lgp *d, enum vb2_buffer_state state)
{
	struct lgp_buffer *buf;
	unsigned long flags;

	spin_lock_irqsave(&d->buffers_lock, flags);
	while (!list_empty(&d->buffers)) {
		buf = list_first_entry(&d->buffers, struct lgp_buffer, list);
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&d->buffers_lock, flags);
}

static u64 lgp_timestamp(struct lgp *d)
{
	u32 pts = d->frame_pts & 0x7fffffff;

	if (!d->pts_started) {
		d->timestamp_base = ktime_get_ns();
		d->pts_ticks = 0;
		d->pts_started = true;
	} else {
		/* The mailbox holds PTS >> 2, i.e. 22.5 kHz modulo 2^31.
		 * MJPEG advances this counter even with its validity flag clear.
		 * USB arrival times include scheduling/fragmentation jitter.
		 */
		d->pts_ticks += (pts - d->last_pts) & 0x7fffffff;
	}
	d->last_pts = pts;
	return d->timestamp_base + mul_u64_u32_div(d->pts_ticks, NSEC_PER_SEC, 22500);
}

static int lgp_capture(void *data)
{
	struct lgp *d = data;
	unsigned long last_frame = jiffies, flags;
	struct lgp_buffer *buf;
	unsigned int length;
	u32 sequence;
	int ret;

	while (!kthread_should_stop()) {
		ret = lgp_hw_frame(d, &length);
		if (!ret && time_after(jiffies, last_frame + 5 * HZ))
			ret = -ETIMEDOUT;
		if (ret < 0) {
			if (!READ_ONCE(d->disconnected))
				dev_err(&d->usb->dev, "Capture failed: %d\n", ret);
			vb2_queue_error(&d->queue);
			lgp_return_buffers(d, VB2_BUF_STATE_ERROR);
			/* Keep the task alive until stop_streaming joins it. */
			while (!kthread_should_stop())
				schedule_timeout_interruptible(HZ);
			break;
		}
		if (!ret) {
			usleep_range(1000, 1500);
			continue;
		}
		last_frame = jiffies;
		sequence = d->sequence++;
		spin_lock_irqsave(&d->buffers_lock, flags);
		buf = list_first_entry_or_null(&d->buffers, struct lgp_buffer, list);
		if (buf)
			list_del(&buf->list);
		spin_unlock_irqrestore(&d->buffers_lock, flags);
		if (!buf)
			continue;
		memcpy(vb2_plane_vaddr(&buf->vb.vb2_buf, 0), d->frame, length);
		vb2_set_plane_payload(&buf->vb.vb2_buf, 0, length);
		buf->vb.field = V4L2_FIELD_NONE;
		buf->vb.sequence = sequence;
		buf->vb.vb2_buf.timestamp = lgp_timestamp(d);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
	}
	return 0;
}

static int lgp_queue_setup(struct vb2_queue *q, unsigned int *count,
			   unsigned int *planes, unsigned int bytes[],
			   struct device *alloc_devs[])
{
	if (*planes)
		return *planes == 1 && bytes[0] >= LGP_MAX_FRAME ? 0 : -EINVAL;
	*planes = 1;
	bytes[0] = LGP_MAX_FRAME;
	return 0;
}

static int lgp_buffer_prepare(struct vb2_buffer *vb)
{
	struct lgp *d = vb2_get_drv_priv(vb->vb2_queue);

	if (READ_ONCE(d->disconnected))
		return -ENODEV;
	if (vb2_plane_size(vb, 0) < LGP_MAX_FRAME)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, 0);
	return 0;
}

static void lgp_buffer_queue(struct vb2_buffer *vb)
{
	struct lgp *d = vb2_get_drv_priv(vb->vb2_queue);
	struct lgp_buffer *buf = container_of(to_vb2_v4l2_buffer(vb),
					     struct lgp_buffer, vb);
	unsigned long flags;

	if (READ_ONCE(d->disconnected)) {
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
		return;
	}
	spin_lock_irqsave(&d->buffers_lock, flags);
	list_add_tail(&buf->list, &d->buffers);
	spin_unlock_irqrestore(&d->buffers_lock, flags);
}

static int lgp_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct lgp *d = vb2_get_drv_priv(q);
	int ret;

	if (READ_ONCE(d->disconnected)) {
		ret = -ENODEV;
		goto failed;
	}
	d->sequence = 0;
	d->pts_started = false;
	v4l2_ctrl_grab(d->quality, true);
	ret = lgp_hw_start(d);
	if (ret)
		goto stop;
	d->worker = kthread_run(lgp_capture, d, "lgplite-capture");
	if (IS_ERR(d->worker)) {
		ret = PTR_ERR(d->worker);
		d->worker = NULL;
		goto stop;
	}
	lgp_follow_capture(d);
	return 0;
stop:
	lgp_hw_stop(d);
	v4l2_ctrl_grab(d->quality, false);
	dev_err(&d->usb->dev, "Cannot start %ux%u@%u capture: %d\n",
		d->width, d->height, d->fps, ret);
failed:
	lgp_return_buffers(d, VB2_BUF_STATE_QUEUED);
	return ret;
}

/* Called with d->lock held; the worker never takes that mutex. */
static void lgp_stop(struct lgp *d)
{
	int ret;

	if (d->worker) {
		kthread_stop(d->worker);
		d->worker = NULL;
	}
	ret = lgp_hw_stop(d);
	if (ret && !READ_ONCE(d->disconnected))
		dev_warn(&d->usb->dev, "Encoder stop failed: %d\n", ret);
	lgp_return_buffers(d, VB2_BUF_STATE_ERROR);
	v4l2_ctrl_grab(d->quality, false);
}

static void lgp_stop_streaming(struct vb2_queue *q)
{
	struct lgp *d = vb2_get_drv_priv(q);

	lgp_stop(d);
	if (!READ_ONCE(d->disconnected))
		lgp_follow_capture(d);
}

static const struct vb2_ops lgp_queue_ops = {
	.queue_setup = lgp_queue_setup,
	.buf_prepare = lgp_buffer_prepare,
	.buf_queue = lgp_buffer_queue,
	.start_streaming = lgp_start_streaming,
	.stop_streaming = lgp_stop_streaming,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
	/* Since 6.13 vb2 releases the queue lock itself while waiting. */
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
#endif
};

static int lgp_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	struct lgp *d = video_drvdata(file);

	strscpy(cap->driver, "lgplite", sizeof(cap->driver));
	strscpy(cap->card, d->video.name, sizeof(cap->card));
	usb_make_path(d->usb, cap->bus_info, sizeof(cap->bus_info));
	return 0;
}

static int lgp_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *fmt)
{
	if (fmt->index)
		return -EINVAL;
	fmt->pixelformat = V4L2_PIX_FMT_MJPEG;
	return 0;
}

static void lgp_format(struct v4l2_pix_format *pix, unsigned int width,
		       unsigned int height)
{
	memset(pix, 0, sizeof(*pix));
	pix->width = width;
	pix->height = height;
	pix->pixelformat = V4L2_PIX_FMT_MJPEG;
	pix->field = V4L2_FIELD_NONE;
	pix->sizeimage = LGP_MAX_FRAME;
	pix->colorspace = V4L2_COLORSPACE_JPEG;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
	pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_SRGB;
}

static int lgp_get_fmt(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct lgp *d = video_drvdata(file);

	lgp_format(&fmt->fmt.pix, d->width, d->height);
	return 0;
}

static int lgp_try_fmt(struct file *file, void *fh, struct v4l2_format *fmt)
{
	const struct v4l2_frmsize_discrete *size;

	size = v4l2_find_nearest_size(sizes, ARRAY_SIZE(sizes), width, height,
				      fmt->fmt.pix.width, fmt->fmt.pix.height);
	lgp_format(&fmt->fmt.pix, size->width, size->height);
	return 0;
}

static int lgp_set_fmt(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct lgp *d = video_drvdata(file);

	if (vb2_is_busy(&d->queue))
		return -EBUSY;
	lgp_try_fmt(file, fh, fmt);
	d->width = fmt->fmt.pix.width;
	d->height = fmt->fmt.pix.height;
	return 0;
}

static int lgp_enum_sizes(struct file *file, void *fh, struct v4l2_frmsizeenum *f)
{
	if (f->pixel_format != V4L2_PIX_FMT_MJPEG || f->index >= ARRAY_SIZE(sizes))
		return -EINVAL;
	f->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	f->discrete = sizes[f->index];
	return 0;
}

static int lgp_enum_intervals(struct file *file, void *fh,
			      struct v4l2_frmivalenum *f)
{
	unsigned int i;

	if (f->pixel_format != V4L2_PIX_FMT_MJPEG || f->index >= ARRAY_SIZE(rates))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(sizes); i++)
		if (f->width == sizes[i].width && f->height == sizes[i].height)
			break;
	if (i == ARRAY_SIZE(sizes))
		return -EINVAL;
	f->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	f->discrete.numerator = 1;
	f->discrete.denominator = rates[f->index];
	return 0;
}

static int lgp_get_parm(struct file *file, void *fh, struct v4l2_streamparm *p)
{
	struct lgp *d = video_drvdata(file);

	if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	memset(&p->parm, 0, sizeof(p->parm));
	p->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	p->parm.capture.timeperframe.numerator = 1;
	p->parm.capture.timeperframe.denominator = d->fps;
	p->parm.capture.readbuffers = 2;
	return 0;
}

static int lgp_set_parm(struct file *file, void *fh, struct v4l2_streamparm *p)
{
	struct lgp *d = video_drvdata(file);
	struct v4l2_fract t = p->parm.capture.timeperframe;
	/* A zero in either field asks for the default rate. */
	unsigned int fps = t.numerator && t.denominator ?
			   t.denominator / t.numerator : 30;
	unsigned int best = 30, distance = UINT_MAX, i, delta;

	if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	if (vb2_is_streaming(&d->queue))
		return -EBUSY;
	for (i = 0; i < ARRAY_SIZE(rates); i++) {
		delta = abs((s64)fps - rates[i]);
		if (delta < distance) {
			distance = delta;
			best = rates[i];
		}
	}
	d->fps = best;
	return lgp_get_parm(file, fh, p);
}

static int lgp_enum_input(struct file *file, void *fh, struct v4l2_input *input)
{
	if (input->index)
		return -EINVAL;
	strscpy(input->name, "HDMI", sizeof(input->name));
	input->type = V4L2_INPUT_TYPE_CAMERA;
	input->capabilities = 0;
	return 0;
}

static int lgp_get_input(struct file *file, void *fh, unsigned int *input)
{
	*input = 0;
	return 0;
}

static int lgp_set_input(struct file *file, void *fh, unsigned int input)
{
	return input ? -EINVAL : 0;
}

static const struct v4l2_ioctl_ops lgp_ioctl_ops = {
	.vidioc_querycap = lgp_querycap,
	.vidioc_enum_fmt_vid_cap = lgp_enum_fmt,
	.vidioc_g_fmt_vid_cap = lgp_get_fmt,
	.vidioc_s_fmt_vid_cap = lgp_set_fmt,
	.vidioc_try_fmt_vid_cap = lgp_try_fmt,
	.vidioc_enum_framesizes = lgp_enum_sizes,
	.vidioc_enum_frameintervals = lgp_enum_intervals,
	.vidioc_g_parm = lgp_get_parm,
	.vidioc_s_parm = lgp_set_parm,
	.vidioc_enum_input = lgp_enum_input,
	.vidioc_g_input = lgp_get_input,
	.vidioc_s_input = lgp_set_input,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
	.vidioc_log_status = v4l2_ctrl_log_status,
};

static const struct v4l2_file_operations lgp_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

static void lgp_free(struct lgp *d)
{
	vfree(d->frame);
	kfree(d->dma);
	kfree(d->reply);
	kfree(d->command);
	kfree(d);
}

/* Runs when the last reference goes: after disconnect and the final close. */
static void lgp_release(struct v4l2_device *v4l2)
{
	struct lgp *d = container_of(v4l2, struct lgp, v4l2);

	vb2_queue_release(&d->queue);
	v4l2_ctrl_handler_free(&d->controls);
	v4l2_device_unregister(v4l2);
	usb_put_dev(d->usb);
	lgp_free(d);
}

/* Only the verified layout is supported: four bulk endpoints. */
static bool lgp_endpoints_ok(const struct usb_host_interface *alt)
{
	static const u8 wanted[] = { 0x81, 0x02, 0x83, 0x04 };
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(wanted); i++) {
		for (j = 0; j < alt->desc.bNumEndpoints; j++)
			if (alt->endpoint[j].desc.bEndpointAddress == wanted[i] &&
			    usb_endpoint_xfer_bulk(&alt->endpoint[j].desc))
				break;
		if (j == alt->desc.bNumEndpoints)
			return false;
	}
	return true;
}

static int lgp_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct lgp *d;
	int ret;

	if (intf->cur_altsetting->desc.bInterfaceNumber != 0 ||
	    !lgp_endpoints_ok(intf->cur_altsetting))
		return -ENODEV;
	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->command = kmalloc(LGP_COMMAND_SIZE, GFP_KERNEL);
	d->reply = kmalloc(LGP_COMMAND_SIZE, GFP_KERNEL);
	d->dma = kmalloc(LGP_DMA_SIZE, GFP_KERNEL);
	d->frame = vmalloc(LGP_MAX_FRAME);
	if (!d->command || !d->reply || !d->dma || !d->frame) {
		ret = -ENOMEM;
		goto free;
	}
	d->usb = usb_get_dev(interface_to_usbdev(intf));
	mutex_init(&d->lock);
	mutex_init(&d->io_lock);
	spin_lock_init(&d->buffers_lock);
	INIT_LIST_HEAD(&d->buffers);
	d->width = 1280;
	d->height = 720;
	d->fps = 30;
	d->queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	d->queue.io_modes = VB2_MMAP | VB2_READ | VB2_DMABUF;
	d->queue.drv_priv = d;
	d->queue.buf_struct_size = sizeof(struct lgp_buffer);
	d->queue.ops = &lgp_queue_ops;
	d->queue.mem_ops = &vb2_vmalloc_memops;
	d->queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	d->queue.min_queued_buffers = 2;
	d->queue.lock = &d->lock;
	d->queue.dev = &d->usb->dev;
	ret = vb2_queue_init(&d->queue);
	if (ret)
		goto put_usb;
	d->v4l2.release = lgp_release;
	ret = v4l2_device_register(&intf->dev, &d->v4l2);
	if (ret)
		goto release_queue;
	v4l2_ctrl_handler_init(&d->controls, 2);
	/* Controls take their own mutex inside ioctls already holding d->lock. */
	d->quality = v4l2_ctrl_new_std(&d->controls, NULL,
				       V4L2_CID_JPEG_COMPRESSION_QUALITY,
				       1, 100, 1, 75);
	d->light = v4l2_ctrl_new_custom(&d->controls, &lgp_light_control, NULL);
	if (d->controls.error) {
		ret = d->controls.error;
		goto put_v4l2;
	}
	d->v4l2.ctrl_handler = &d->controls;
	strscpy(d->video.name, "AVerMedia GL310", sizeof(d->video.name));
	d->video.v4l2_dev = &d->v4l2;
	d->video.fops = &lgp_fops;
	d->video.ioctl_ops = &lgp_ioctl_ops;
	d->video.release = video_device_release_empty;
	d->video.lock = &d->lock;
	d->video.queue = &d->queue;
	d->video.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			       V4L2_CAP_READWRITE;
	video_set_drvdata(&d->video, d);
	ret = video_register_device(&d->video, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto put_v4l2;
	usb_set_intfdata(intf, &d->v4l2);
	dev_info(&intf->dev, "Native MJPEG webcam registered as %s\n",
		 video_device_node_name(&d->video));
	/* The webcam is still useful without the button. */
	ret = lgp_button_register(d, intf);
	if (ret)
		dev_warn(&intf->dev, "Button unavailable: %d\n", ret);
	/* Replace a pattern left by a session that did not end cleanly. */
	ret = lgp_show_light(d, LGP_LIGHT_AUTO);
	if (ret)
		dev_warn(&intf->dev, "Cannot reset the button light: %d\n", ret);
	return 0;
put_v4l2:
	v4l2_device_put(&d->v4l2);
	return ret;
release_queue:
	vb2_queue_release(&d->queue);
put_usb:
	usb_put_dev(d->usb);
free:
	lgp_free(d);
	return ret;
}

static void lgp_disconnect(struct usb_interface *intf)
{
	struct v4l2_device *v4l2 = usb_get_intfdata(intf);
	struct lgp *d = container_of(v4l2, struct lgp, v4l2);

	/* Stops the button poll while transfers still work. */
	lgp_button_unregister(d);
	mutex_lock(&d->lock);
	/* On driver unbind the USB device is still present. Finish in-flight
	 * command/reply pairs and stop the encoder before disabling transfers.
	 * Physical removal already makes usb_bulk_msg return -ENODEV.
	 */
	if (READ_ONCE(d->usb->state) == USB_STATE_NOTATTACHED)
		WRITE_ONCE(d->disconnected, true);
	lgp_stop(d);
	/* Leave the power-on pattern behind, whatever the control selects. */
	if (!READ_ONCE(d->disconnected))
		lgp_hw_light(d, LGP_LIGHT_CLOCKWISE);
	WRITE_ONCE(d->disconnected, true);
	vb2_queue_error(&d->queue);
	v4l2_device_disconnect(v4l2);
	video_unregister_device(&d->video);
	usb_set_intfdata(intf, NULL);
	mutex_unlock(&d->lock);
	/* Open file handles keep v4l2 and the USB device allocation alive. */
	v4l2_device_put(v4l2);
}

static const struct usb_device_id lgp_ids[] = {
	{ USB_DEVICE(0x07ca, 0xc835) },
	{ }
};
MODULE_DEVICE_TABLE(usb, lgp_ids);

static struct usb_driver lgp_usb_driver = {
	.name = "lgplite",
	.probe = lgp_probe,
	.disconnect = lgp_disconnect,
	.id_table = lgp_ids,
	/* Keep transfers usable until disconnect has joined the worker and
	 * returned pending firmware buffers on a sysfs driver unbind.
	 */
	.soft_unbind = 1,
};
module_usb_driver(lgp_usb_driver);

MODULE_DESCRIPTION("AVerMedia Live Gamer Portable Lite GL310 native MJPEG webcam, button and light");
MODULE_AUTHOR("lgp-lite-linux-driver contributors");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1.0");
MODULE_FIRMWARE(LGP_VIDEO_FIRMWARE);
MODULE_FIRMWARE(LGP_AUDIO_FIRMWARE);
