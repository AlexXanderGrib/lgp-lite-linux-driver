// SPDX-License-Identifier: GPL-2.0-only
/* AVerMedia GL310/C835 protocol. See docs/PROTOCOL.md for provenance. */
#include <crypto/sha2.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

#include "lgplite.h"

/* Bulk endpoint numbers. */
#define EP_DMA_IN 1
#define EP_DMA_OUT 2
#define EP_REPLY 3
#define EP_COMMAND 4

/* USB command opcodes. */
#define OP_REGISTER 0x01
#define OP_MEMORY 0x02
#define OP_ARM_RUN 0x07
#define OP_DMA 0x09
#define OP_SWI2C_READ 0x0c

#define I2C_OK 0x08
#define MCU_ADDRESS 0x15

/* Board MCU registers describing the HDMI input. */
#define MCU_UNSTABLE 0x1b
#define MCU_STATUS 0x1c
#define MCU_STATUS_SYNC BIT(2)
#define MCU_TIMING 0x1d
#define MCU_POLARITY 0x26

/* SoC registers (byte addresses). */
#define REG_CONTROL 0x000
#define REG_OUTPUT_SWITCH 0x050
#define REG_GPIO_DIRECTION 0x610
#define REG_GPIO_VALUE 0x614
#define REG_GPIO_INPUT 0x618
#define GPIO_MCU_READY BIT(13)
#define REG_HOST_MESSAGE 0x6b0	/* ARM to host: code, five parameters, status */
#define REG_HOST_STATUS 0x6c8
#define REG_ARM_STATUS 0x6cc	/* bit 0: host to ARM message pending */
#define REG_ARM_PARAM1 0x6f8	/* further parameters at descending addresses */
#define REG_ARM_MESSAGE 0x6fc
#define REG_IRQ 0x800
#define IRQ_ARM_MESSAGE BIT(16)
#define IRQ_PENDING_MASK 0x70000

#define STATUS_BUSY BIT(0)
#define STATUS_WANTS_ACK BIT(8)

/* Mailbox messages, host to ARM. Bits 16..31 carry the firmware task. */
#define MSG_VERSION 0x00
#define MSG_START 0x01
#define MSG_STOP 0x02
#define MSG_MISC 0x10
#define MSG_ENCODER_MODE 0x11
#define MSG_BUFFER_DONE 0x30
#define MSG_ACK_ENCODER 0x31
#define MSG_ACK_SYSTEM 0xa2
#define MSG_SYSTEM_OPEN 0xf1
#define MSG_SYSTEM_LINK 0xf2

#define MISC_JPEG_QUALITY 11
#define MISC_JPEG_BUFFERING 12
#define MISC_SOFTWARE_PTS 0x17

/* Mailbox messages, ARM to host. */
#define MSG_DATA 0x40
#define MSG_LAST_DATA 0x41
#define MSG_STATE 0x50
#define MSG_ERROR 0x51
#define STATE_STOPPED 2

#define BUFFER_STREAM 0x83
#define SOC_MEMORY_WORDS 0x1000000	/* 64 MiB */

/* Firmware load addresses and boot handshake words in SoC memory. */
#define AUDIO_ADDRESS 0x100000
#define AUDIO_REGION 0x70624
#define VIDEO_ADDRESS 0
#define MEM_INTERRUPT_MODE 0x2f1090
#define MEM_FIXED_MODE 0x2f1094
#define MEM_QSOS_STATE 0x2f2004

struct lgp_reg {
	u32 addr, value;
};

struct lgp_signal {
	u32 width, height, fps, control;
};

/* An interrupted command can leave a reply or DMA payload in an IN FIFO.
 * Drain those before beginning a new session, using only DMA-safe storage.
 */
static int lgp_drain(struct lgp *d, unsigned int ep, unsigned int size)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(500);
	int actual, ret;

	do {
		actual = 0;
		ret = usb_bulk_msg(d->usb, usb_rcvbulkpipe(d->usb, ep),
				   d->dma, size, &actual, 20);
		if (ret && ret != -ETIMEDOUT)
			return ret;
		if (ret == -ETIMEDOUT && !actual)
			return 0;
	} while (time_before(jiffies, deadline));
	return -ETIMEDOUT;
}

static int lgp_bulk(struct lgp *d, bool in, unsigned int ep, void *data, int len)
{
	int actual, ret;
	unsigned int pipe;

	if (READ_ONCE(d->disconnected))
		return -ENODEV;
	pipe = in ? usb_rcvbulkpipe(d->usb, ep) : usb_sndbulkpipe(d->usb, ep);
	ret = usb_bulk_msg(d->usb, pipe, data, len, &actual, 1000);
	if (ret)
		return ret;
	return actual == len ? 0 : -EIO;
}

/* Commands are `u8 opcode, u8 flag, le16 count, le32 address, le32 params[]`. */
static int lgp_command(struct lgp *d, u8 op, u8 flag, u16 count, u32 addr,
		       const u32 *params, unsigned int nparams, unsigned int reply)
{
	unsigned int i;
	int ret;

	if (WARN_ON(8 + nparams * 4 > LGP_COMMAND_SIZE || reply > LGP_COMMAND_SIZE))
		return -EINVAL;
	d->command[0] = op;
	d->command[1] = flag;
	put_unaligned_le16(count, d->command + 2);
	put_unaligned_le32(addr, d->command + 4);
	for (i = 0; i < nparams; i++)
		put_unaligned_le32(params[i], d->command + 8 + i * 4);
	ret = lgp_bulk(d, false, EP_COMMAND, d->command, 8 + nparams * 4);
	if (!ret && reply)
		ret = lgp_bulk(d, true, EP_REPLY, d->reply, reply);
	return ret;
}

static int lgp_read_regs(struct lgp *d, u32 addr, u32 *values, unsigned int n)
{
	unsigned int i;
	int ret = lgp_command(d, OP_REGISTER, 0, n, addr, NULL, 0, n * 4);

	if (ret)
		return ret;
	for (i = 0; i < n; i++)
		values[i] = get_unaligned_le32(d->reply + i * 4);
	return 0;
}

static int lgp_write(struct lgp *d, u32 addr, u32 value)
{
	return lgp_command(d, OP_REGISTER, 1, 1, addr, &value, 1, 0);
}

static int lgp_write_table(struct lgp *d, const struct lgp_reg *regs,
			   unsigned int n)
{
	unsigned int i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = lgp_write(d, regs[i].addr, regs[i].value);
		if (ret)
			return ret;
	}
	return 0;
}

static int lgp_update(struct lgp *d, u32 addr, u32 clear, u32 set)
{
	u32 value;
	int ret = lgp_read_regs(d, addr, &value, 1);

	if (ret)
		return ret;
	return lgp_write(d, addr, (value & ~clear) | set);
}

static int lgp_memory_write(struct lgp *d, u32 addr, u32 value)
{
	u32 p[] = { addr >> 2, value };

	return lgp_command(d, OP_MEMORY, 1, 4, addr >> 2, p, ARRAY_SIZE(p), 0);
}

/* Transfers `length` bytes between SoC memory and d->dma. The device takes
 * word addresses and counts; `addr` and `length` are multiples of four.
 */
static int lgp_dma(struct lgp *d, bool write, u32 addr, unsigned int length)
{
	u32 p[] = { addr >> 2, length >> 2 };
	int ret;

	ret = lgp_command(d, OP_DMA, write, 8, 0, p, ARRAY_SIZE(p), 1);
	if (ret)
		return ret;
	return lgp_bulk(d, !write, write ? EP_DMA_OUT : EP_DMA_IN, d->dma, length);
}

static int lgp_arm(struct lgp *d, bool run)
{
	int ret = lgp_command(d, OP_ARM_RUN, run, 0, 0, NULL, 0, 0);

	if (ret)
		return ret;
	return lgp_write(d, REG_ARM_STATUS, 0);
}

/* Waits for the firmware to consume the previous host message. */
static int lgp_mailbox_wait(struct lgp *d)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(500);
	u32 status;
	int ret;

	do {
		ret = lgp_read_regs(d, REG_ARM_STATUS, &status, 1);
		if (ret || !(status & STATUS_BUSY))
			return ret;
		usleep_range(1000, 1500);
	} while (time_before(jiffies, deadline));
	return -ETIMEDOUT;
}

static int lgp_send(struct lgp *d, u32 code, const u32 *p, unsigned int n,
		    u32 status)
{
	unsigned int i;
	int ret = lgp_mailbox_wait(d);

	dev_dbg(&d->usb->dev, "Mailbox command %#x, ready=%d\n", code, ret);
	if (ret)
		return ret;
	for (i = 0; i < n; i++) {
		ret = lgp_write(d, REG_ARM_PARAM1 - i * 4, p[i]);
		if (ret)
			return ret;
	}
	/* The upper status half echoes the message ID on acknowledgments. */
	ret = lgp_write(d, REG_ARM_STATUS, (status & 0xffff0000) | STATUS_BUSY);
	if (ret)
		return ret;
	return lgp_write(d, REG_ARM_MESSAGE, code);
}

/* Returns 1 with a message in m[0..6], 0 if none is pending, or an errno. */
static int lgp_receive(struct lgp *d, u32 *m)
{
	u32 irq, ack;
	int ret = lgp_read_regs(d, REG_IRQ, &irq, 1);

	if (ret || !(irq & IRQ_ARM_MESSAGE))
		return ret;
	/* IRQ is the publication barrier. HOST_STATUS becomes busy too early. */
	ret = lgp_read_regs(d, REG_HOST_MESSAGE, m, 7);
	if (ret)
		return ret;
	ret = lgp_write(d, REG_IRQ, (irq & ~IRQ_PENDING_MASK) | IRQ_ARM_MESSAGE);
	if (ret || !(m[6] & STATUS_BUSY))
		return ret;
	if (m[6] & STATUS_WANTS_ACK) {
		ack = (m[0] & 0xffff) < 0x80 ? MSG_ACK_ENCODER : MSG_ACK_SYSTEM;
		ret = lgp_send(d, (m[0] & 0xffff0000) | ack, NULL, 0, m[6]);
		if (ret)
			return ret;
	}
	ret = lgp_write(d, REG_HOST_STATUS, m[6] & ~STATUS_BUSY);
	return ret ? ret : 1;
}

/* Hands a stream buffer back to the firmware after `words` were consumed. */
static int lgp_return_buffer(struct lgp *d, const u32 *m, u32 words)
{
	u32 p[] = { m[1] & 0xffff, words, m[5] & 0x7fffffff,
		    m[5] >> 31, 0, m[1] >> 24 };

	return lgp_send(d, (m[0] & 0xffff0000) | MSG_BUFFER_DONE, p,
			ARRAY_SIZE(p), m[6]);
}

/* Reads board MCU registers over the SoC's software I2C bus. */
static int lgp_mcu(struct lgp *d, u8 reg, u8 *values, unsigned int length)
{
	u32 gpio;
	unsigned int i;
	int ret;

	/* Like the vendor driver, give a busy MCU three chances, then try. */
	for (i = 0; i < 3; i++) {
		ret = lgp_read_regs(d, REG_GPIO_INPUT, &gpio, 1);
		if (ret)
			return ret;
		if (gpio & GPIO_MCU_READY)
			break;
		msleep(30);
	}
	d->command[0] = OP_SWI2C_READ;
	d->command[1] = 1;
	put_unaligned_le16(length, d->command + 2);
	put_unaligned_le32(MCU_ADDRESS, d->command + 4);
	d->command[8] = reg;
	ret = lgp_bulk(d, false, EP_COMMAND, d->command, 9);
	if (ret)
		return ret;
	ret = lgp_bulk(d, true, EP_REPLY, d->reply, length + 1);
	if (ret)
		return ret;
	if (d->reply[length] != I2C_OK)
		return -EIO;
	memcpy(values, d->reply, length);
	return 0;
}

struct lgp_mode {
	u16 width, height, htotal, vtotal;
	u32 clock, fps, control;
};

/* Progressive HDMI timings and the encoder input-control word for each. */
static const struct lgp_mode modes[] = {
	{ 1920, 1080, 2750, 1125, 74000, 24, 0x0c580609 },
	{ 1920, 1080, 2640, 1125, 74000, 25, 0x0cd90609 },
	{ 1920, 1080, 2200, 1125, 74000, 30, 0x0f5e0609 },
	{ 1920, 1080, 2640, 1125, 148000, 50, 0x0cf20609 },
	{ 1920, 1080, 2200, 1125, 148500, 60, 0x0f7c0609 },
	{ 1280, 720, 1650, 750, 74000, 60, 0x3e7c0609 },
	{ 1280, 720, 1980, 750, 74000, 50, 0x39720609 },
};

static int lgp_signal(struct lgp *d, struct lgp_signal *s)
{
	u8 unstable, status, b[7], polarity;
	u32 ht, vt, clock;
	unsigned int i;
	int ret;

	for (i = 0; i < 10; i++) {
		ret = lgp_mcu(d, MCU_UNSTABLE, &unstable, 1);
		if (ret)
			return ret;
		if (!unstable)
			break;
		msleep(30);
	}
	if (unstable)
		return -ENOLINK;
	usleep_range(5000, 6000);
	ret = lgp_mcu(d, MCU_STATUS, &status, 1);
	if (ret)
		return ret;
	if (!(status & MCU_STATUS_SYNC))
		return -ENOLINK;
	ret = lgp_mcu(d, MCU_TIMING, b, sizeof(b));
	if (ret)
		return ret;
	ret = lgp_mcu(d, MCU_POLARITY, &polarity, 1);
	if (ret)
		return ret;
	ht = b[0] | (b[1] & 0x0f) << 8;
	s->width = b[2] | (b[1] & 0xf0) << 4;
	vt = b[3] | (b[4] & 0x0f) << 8;
	s->height = b[5] | (b[4] & 0xf0) << 4;
	clock = b[6] ? 3456000 / b[6] : 0;
	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		const struct lgp_mode *m = &modes[i];

		/* Tolerances are those of the vendor driver. */
		if (m->width == s->width && m->height == s->height &&
		    abs((int)m->htotal - (int)ht) < 16 &&
		    abs((int)m->vtotal - (int)vt) < 31 &&
		    abs((int)m->clock - (int)clock) * 100 < m->clock * 6 &&
		    (polarity & 3) == 3) {
			s->fps = m->fps;
			s->control = m->control;
			return 0;
		}
	}
	dev_err(&d->usb->dev,
		"Unsupported HDMI timing %ux%u, totals %ux%u, %u kHz, polarity %#x\n",
		s->width, s->height, ht, vt, clock, polarity);
	return -ERANGE;
}

static int lgp_request_firmware(struct lgp *d, const struct firmware **fw,
				const char *name, size_t size, const u8 *hash)
{
	u8 digest[SHA256_DIGEST_SIZE];
	int ret = request_firmware(fw, name, &d->usb->dev);

	if (ret) {
		dev_err(&d->usb->dev,
			"Firmware %s is missing; see the README (tools/get-firmware.sh)\n",
			name);
		return ret;
	}
	/* The boot sequence below is only known to be right for these images. */
	if ((*fw)->size == size) {
		sha256((*fw)->data, size, digest);
		if (!memcmp(digest, hash, sizeof(digest)))
			return 0;
	}
	dev_err(&d->usb->dev, "Firmware %s is not a supported image\n", name);
	release_firmware(*fw);
	return -EINVAL;
}

/* Writes an image to SoC memory and compares what the device reads back. */
static int lgp_upload(struct lgp *d, const struct firmware *fw, u32 addr)
{
	unsigned int off, n;
	int ret;

	for (off = 0; off < fw->size; off += n) {
		n = min_t(size_t, LGP_DMA_SIZE, fw->size - off);
		memcpy(d->dma, fw->data + off, n);
		ret = lgp_dma(d, true, addr + off, n);
		if (ret)
			return ret;
		usleep_range(1000, 1500);
		ret = lgp_dma(d, false, addr + off, n);
		if (ret)
			return ret;
		if (memcmp(d->dma, fw->data + off, n))
			return -EILSEQ;
	}
	return 0;
}

static int lgp_boot_images(struct lgp *d, const struct firmware *video,
			   const struct firmware *audio)
{
	/* Three 1 MiB host windows onto SoC memory: start, end, offset. */
	static const struct lgp_reg windows[] = {
		{ 0x81c, 0x004000 }, { 0x820, 0x103fff }, { 0x824, 0x000000 },
		{ 0x828, 0x104000 }, { 0x82c, 0x203fff }, { 0x830, 0x100000 },
		{ 0x834, 0x204000 }, { 0x838, 0x303fff }, { 0x83c, 0x200000 },
		{ 0x840, 0x70003124 }, { 0x840, 0x90003124 },
	};
	static const struct lgp_reg gpio_defaults[] = {
		{ REG_GPIO_DIRECTION, 0 }, { REG_GPIO_VALUE, 0 },
	};
	u32 off, n;
	int ret;

	ret = lgp_arm(d, false);
	if (ret)
		return ret;
	ret = lgp_write_table(d, windows, ARRAY_SIZE(windows));
	if (ret)
		return ret;
	/* Bit 1 clear enables the audio output, bit 2 disables video output. */
	ret = lgp_update(d, REG_OUTPUT_SWITCH, BIT(1), BIT(2));
	if (ret)
		return ret;
	ret = lgp_write_table(d, gpio_defaults, ARRAY_SIZE(gpio_defaults));
	if (ret)
		return ret;
	msleep(50);
	ret = lgp_update(d, REG_CONTROL, BIT(13), 0);
	if (ret)
		return ret;

	/* The audio image, with the rest of its region cleared first. */
	for (off = audio->size; off < AUDIO_REGION; off += n) {
		n = min_t(u32, LGP_DMA_SIZE, AUDIO_REGION - off);
		memset(d->dma, 0, n);
		ret = lgp_dma(d, true, AUDIO_ADDRESS + off, n);
		if (ret)
			return ret;
	}
	ret = lgp_upload(d, audio, AUDIO_ADDRESS);
	if (ret)
		return ret;

	/* The video image is the ARM program itself. */
	ret = lgp_memory_write(d, MEM_FIXED_MODE, 1);
	if (ret)
		return ret;
	ret = lgp_memory_write(d, MEM_INTERRUPT_MODE, 0);
	if (ret)
		return ret;
	ret = lgp_memory_write(d, MEM_QSOS_STATE, 0);
	if (ret)
		return ret;
	ret = lgp_write(d, REG_ARM_STATUS, 0);
	if (ret)
		return ret;
	ret = lgp_upload(d, video, VIDEO_ADDRESS);
	if (ret)
		return ret;
	usleep_range(1000, 1500);
	ret = lgp_arm(d, true);
	if (ret)
		return ret;
	msleep(150);

	/* Only a consumed mailbox message proves that the ARM core runs. */
	ret = lgp_send(d, MSG_VERSION, NULL, 0, 0);
	if (ret)
		return ret;
	return lgp_mailbox_wait(d);
}

/* The SoC keeps no codec firmware in flash; every session uploads it. */
static int lgp_boot(struct lgp *d)
{
	static const u8 video_hash[SHA256_DIGEST_SIZE] = {
		0x8d, 0xa3, 0x0f, 0x6d, 0xc2, 0xd4, 0xfe, 0xe5,
		0xae, 0xb3, 0xe2, 0xe0, 0x6d, 0xf5, 0x58, 0x1d,
		0xb4, 0xc0, 0x1d, 0x14, 0x6c, 0x50, 0xc2, 0x1f,
		0x6a, 0x5d, 0x48, 0x24, 0x02, 0x5a, 0x47, 0xd1,
	};
	static const u8 audio_hash[SHA256_DIGEST_SIZE] = {
		0x0e, 0x8e, 0xa6, 0x6b, 0x6a, 0x3d, 0xdb, 0x68,
		0xf9, 0xa2, 0x83, 0xf1, 0x01, 0x55, 0xdf, 0x90,
		0xce, 0x8d, 0x6f, 0x46, 0x59, 0x9e, 0xd0, 0xec,
		0xf6, 0xf3, 0x08, 0xd6, 0xc9, 0x02, 0x10, 0xf4,
	};
	const struct firmware *video, *audio;
	int ret;

	ret = lgp_request_firmware(d, &video, LGP_VIDEO_FIRMWARE, 454064,
				   video_hash);
	if (ret)
		return ret;
	ret = lgp_request_firmware(d, &audio, LGP_AUDIO_FIRMWARE, 363832,
				   audio_hash);
	if (!ret) {
		ret = lgp_boot_images(d, video, audio);
		release_firmware(audio);
	}
	release_firmware(video);
	return ret;
}

/* Writes the encoder configuration; see "Encoder start" in the docs. */
static int lgp_configure(struct lgp *d, const struct lgp_signal *s)
{
	const struct lgp_reg bank[] = {
		{ 0x6f8, 0x2101c20e },	/* system control: MJPEG video */
		{ 0x6f4, s->height << 16 | s->width },	/* input size */
		/* Input control, with the output frame rate in bits 23..28. */
		{ 0x6f0, (s->control & ~(0x3f << 23)) | d->fps << 23 },
		{ 0x6ec, 0x00781f40 },	/* rate control */
		{ 0x6e8, 0x1f4007d0 },	/* VBR limits */
		{ 0x6e4, 0x2000 },	/* filter control */
		{ 0x6e0, 0xf1990000 | d->fps },	/* GOP and loop filter */
		{ 0x6d8, 0x10 },	/* block transfer size */
		{ 0x6dc, d->height << 16 | d->width },	/* output size */
		{ 0x6d4, 0x21161080 },	/* audio control */
		{ 0x6d0, 0x520840f4 },	/* AAC extension */
	};
	int ret = lgp_mailbox_wait(d);

	if (ret)
		return ret;
	return lgp_write_table(d, bank, ARRAY_SIZE(bank));
}

int lgp_hw_start(struct lgp *d)
{
	const struct {
		u32 code, count, params[3];
	} setup[] = {
		{ MSG_SYSTEM_OPEN, 1, { 0x80000002 } },	/* MJPEG, no audio */
		{ MSG_SYSTEM_LINK, 1, { 0x100 } },	/* video in, host out */
		{ MSG_ENCODER_MODE, 3, { 0, 0, 0 } },
		/* The firmware takes a JPEG quality index of 0 through 15. */
		{ MSG_MISC, 2, { MISC_JPEG_QUALITY,
				 v4l2_ctrl_g_ctrl(d->quality) * 15 / 100 } },
		{ MSG_MISC, 2, { MISC_JPEG_BUFFERING, 0 } },
		{ MSG_MISC, 2, { MISC_SOFTWARE_PTS, 1 } },
	};
	struct lgp_signal s;
	unsigned int i;
	int ret;

	ret = lgp_drain(d, EP_REPLY, LGP_COMMAND_SIZE);
	if (ret)
		return ret;
	ret = lgp_drain(d, EP_DMA_IN, LGP_DMA_SIZE);
	if (ret)
		return ret;
	ret = lgp_signal(d, &s);
	if (ret)
		return ret;
	/* The encoder scales down and drops frames, but does neither upward. */
	if (d->width > s.width || d->height > s.height || d->fps > s.fps) {
		dev_err(&d->usb->dev, "HDMI input is only %ux%u at %u fps\n",
			s.width, s.height, s.fps);
		return -ERANGE;
	}
	d->frame_length = 0;
	ret = lgp_boot(d);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(setup); i++) {
		ret = lgp_send(d, setup[i].code, setup[i].params,
			       setup[i].count, 0);
		if (ret)
			return ret;
	}
	ret = lgp_configure(d, &s);
	if (ret)
		return ret;
	/* From here on lgp_hw_stop() has an encoder to stop. */
	d->running = true;
	return lgp_send(d, MSG_START, NULL, 0, 0);
}

/* Returns the JPEG's length without alignment padding, or 0 if it has not
 * ended yet. Only the buffer that completes a frame is padded.
 */
static unsigned int lgp_jpeg_end(const u8 *frame, unsigned int length)
{
	unsigned int pad, end;

	for (pad = 0; pad < 8 && length >= pad + 4; pad++) {
		end = length - pad;
		if (frame[end - 2] == 0xff && frame[end - 1] == 0xd9)
			return end;
	}
	return 0;
}

int lgp_hw_frame(struct lgp *d, unsigned int *length)
{
	u32 m[7], code, addr, bytes, off, i, n;
	u8 *out;
	int ret;

	ret = lgp_receive(d, m);
	if (ret <= 0)
		return ret;
	code = m[0] & 0xffff;
	if (code == MSG_ERROR) {
		dev_err(&d->usb->dev, "Encoder reported error %#x\n", m[1]);
		return -EIO;
	}
	if (code != MSG_DATA && code != MSG_LAST_DATA)
		return 0;
	if ((m[1] & 0xffff) != BUFFER_STREAM || m[4] > LGP_MAX_FRAME / 4 ||
	    m[2] >= SOC_MEMORY_WORDS || m[4] > SOC_MEMORY_WORDS - m[2]) {
		dev_err(&d->usb->dev,
			"Invalid stream buffer: type=%#x addr=%#x words=%u\n",
			m[1], m[2], m[4]);
		lgp_return_buffer(d, m, 0);
		return -EPROTO;
	}
	addr = m[2] * 4;
	bytes = m[4] * 4;
	if (bytes > LGP_MAX_FRAME - d->frame_length) {
		/* The end of the pending frame was lost; start over. */
		dev_warn_ratelimited(&d->usb->dev, "Dropping oversized frame\n");
		d->frame_length = 0;
		d->sequence++;
	}
	if (!d->frame_length)
		d->frame_pts = m[5];
	out = d->frame + d->frame_length;
	for (off = 0; off < bytes; off += n) {
		n = min_t(u32, LGP_DMA_SIZE, bytes - off);
		ret = lgp_dma(d, false, addr + off, n);
		if (ret)
			return ret;
		/* The encoder stores stream bytes reversed in each word. */
		for (i = 0; i < n; i += 4)
			put_unaligned_be32(get_unaligned_le32(d->dma + i),
					   out + off + i);
	}
	/* The firmware splits a JPEG when its circular stream buffer wraps.
	 * Return each buffer immediately, but retain the partial JPEG until a
	 * continuation supplies the end marker.
	 */
	ret = lgp_return_buffer(d, m, m[4]);
	if (ret)
		return ret;
	if (d->frame_length && bytes &&
	    out[0] == 0xff && out[1] == 0xd8 && out[2] == 0xff) {
		/* A new JPEG begins although the pending one never ended. */
		dev_warn_ratelimited(&d->usb->dev, "Dropping unfinished frame\n");
		memmove(d->frame, out, bytes);
		d->frame_length = 0;
		d->frame_pts = m[5];
		d->sequence++;
	}
	bytes += d->frame_length;
	if (!bytes)
		return 0;
	if (d->frame[0] != 0xff || d->frame[1] != 0xd8) {
		/* Not the start of a JPEG; resynchronize on the next buffer. */
		dev_warn_ratelimited(&d->usb->dev, "Dropping damaged frame\n");
		d->frame_length = 0;
		d->sequence++;
		return 0;
	}
	*length = lgp_jpeg_end(d->frame, bytes);
	d->frame_length = *length ? 0 : bytes;
	if (!*length)
		return 0;
	dev_dbg(&d->usb->dev, "JPEG pts=%#x size=%u\n", d->frame_pts, *length);
	return 1;
}

int lgp_hw_stop(struct lgp *d)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(1500);
	u32 stop[] = { 0 }, m[7], code;
	int ret;

	if (!d->running)
		return 0;
	d->running = false;
	if (READ_ONCE(d->disconnected))
		return -ENODEV;
	ret = lgp_send(d, MSG_STOP, stop, ARRAY_SIZE(stop), 0);
	if (ret)
		return ret;
	/* Give back buffers still in flight until the encoder confirms. */
	while (time_before(jiffies, deadline)) {
		ret = lgp_receive(d, m);
		if (ret < 0)
			return ret;
		if (!ret) {
			usleep_range(1000, 1500);
			continue;
		}
		code = m[0] & 0xffff;
		if (code == MSG_STATE && (m[1] & 0x7fffffff) == STATE_STOPPED)
			return lgp_mailbox_wait(d);
		if (code == MSG_DATA || code == MSG_LAST_DATA) {
			ret = lgp_return_buffer(d, m, 0);
			if (ret)
				return ret;
		}
	}
	return -ETIMEDOUT;
}
