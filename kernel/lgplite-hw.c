// SPDX-License-Identifier: GPL-2.0-only
/* AVerMedia GL310/C835 protocol. See docs/PROTOCOL.md for provenance. */
#include <crypto/hash.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "lgplite.h"

#define ARM_STATUS 0x6cc
#define ARM_MESSAGE 0x6fc
#define HOST_MESSAGE 0x6b0
#define HOST_STATUS 0x6c8
#define IRQ_STATUS 0x800

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
	return ret ? ret : actual == len ? 0 : -EIO;
}

static int lgp_command(struct lgp *d, u8 op, u8 flag, u16 count, u32 addr,
		       const u32 *params, unsigned int nparams, unsigned int reply)
{
	unsigned int i;
	int ret;

	d->command[0] = op;
	d->command[1] = flag;
	put_unaligned_le16(count, d->command + 2);
	put_unaligned_le32(addr, d->command + 4);
	for (i = 0; i < nparams; i++)
		put_unaligned_le32(params[i], d->command + 8 + i * 4);
	ret = lgp_bulk(d, false, 4, d->command, 8 + nparams * 4);
	if (!ret && reply)
		ret = lgp_bulk(d, true, 3, d->reply, reply);
	return ret;
}

static int lgp_read_regs(struct lgp *d, u32 addr, u32 *values, unsigned int n)
{
	unsigned int i;
	int ret = lgp_command(d, 1, 0, n, addr, NULL, 0, n * 4);

	if (!ret)
		for (i = 0; i < n; i++)
			values[i] = get_unaligned_le32(d->reply + i * 4);
	return ret;
}

static int lgp_write(struct lgp *d, u32 addr, u32 value)
{
	return lgp_command(d, 1, 1, 1, addr, &value, 1, 0);
}

static int lgp_memory_write(struct lgp *d, u32 addr, u32 value)
{
	u32 p[] = { addr >> 2, value };

	return lgp_command(d, 2, 1, 4, addr >> 2, p, ARRAY_SIZE(p), 0);
}

/* DMA operates on the kmalloc buffer, never firmware/vmalloc/stack memory. */
static int lgp_dma(struct lgp *d, bool write, u32 addr, unsigned int length)
{
	u32 p[] = { addr >> 2, length >> 2 };
	int ret;

	ret = lgp_command(d, 9, write, 8, 0, p, ARRAY_SIZE(p), 1);
	if (ret)
		return ret;
	return lgp_bulk(d, !write, write ? 2 : 1, d->dma, length);
}

static int lgp_arm(struct lgp *d, bool run)
{
	int ret = lgp_command(d, 7, run, 0, 0, NULL, 0, 0);

	return ret ? ret : lgp_write(d, ARM_STATUS, 0);
}

static int lgp_mailbox_wait(struct lgp *d)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(500);
	u32 status;
	int ret;

	do {
		ret = lgp_read_regs(d, ARM_STATUS, &status, 1);
		if (ret || !(status & 1))
			return ret;
		usleep_range(1000, 1500);
	} while (time_before(jiffies, deadline));
	return -ETIMEDOUT;
}

static int lgp_send(struct lgp *d, u32 code, const u32 *p, unsigned int n, u32 status)
{
	unsigned int i;
	int ret = lgp_mailbox_wait(d);

	dev_dbg(&d->usb->dev, "Mailbox command %#x, ready=%d\n", code, ret);

	for (i = 0; !ret && i < n; i++)
		ret = lgp_write(d, 0x6f8 - i * 4, p[i]);
	if (!ret)
		ret = lgp_write(d, ARM_STATUS, (status & 0xffff0000) | 1);
	return ret ? ret : lgp_write(d, ARM_MESSAGE, code);
}

static int lgp_receive(struct lgp *d, u32 *m)
{
	u32 irq, ack;
	int ret = lgp_read_regs(d, IRQ_STATUS, &irq, 1);

	if (ret || !(irq & BIT(16)))
		return ret;
	/* IRQ is the publication barrier. HOST_STATUS becomes busy too early. */
	ret = lgp_read_regs(d, HOST_MESSAGE, m, 7);
	if (ret)
		return ret;
	ret = lgp_write(d, IRQ_STATUS, (irq & ~0x70000) | BIT(16));
	if (ret || !(m[6] & 1))
		return ret;
	if (m[6] & BIT(8)) {
		ack = (m[0] & 0xffff0000) | ((m[0] & 0xffff) < 0x80 ? 0x31 : 0xa2);
		ret = lgp_send(d, ack, NULL, 0, m[6]);
	}
	if (!ret)
		ret = lgp_write(d, HOST_STATUS, m[6] & ~1);
	return ret ? ret : 1;
}

static int lgp_return_buffer(struct lgp *d, const u32 *m, u32 words)
{
	u32 p[] = { m[1] & 0xffff, words, m[5] & 0x7fffffff,
		    m[5] >> 31, 0, m[1] >> 24 };

	return lgp_send(d, (m[0] & 0xffff0000) | 0x30, p, ARRAY_SIZE(p), m[6]);
}

static int lgp_mcu(struct lgp *d, u8 reg, u8 *values, unsigned int length)
{
	u32 ready;
	unsigned int i;
	int ret;

	for (i = 0; i < 3; i++) {
		ret = lgp_read_regs(d, 0x618, &ready, 1);
		if (ret)
			return ret;
		if (ready & BIT(13))
			break;
		msleep(30);
	}
	d->command[0] = 0x0c;
	d->command[1] = 1;
	put_unaligned_le16(length, d->command + 2);
	put_unaligned_le32(0x15, d->command + 4);
	d->command[8] = reg;
	ret = lgp_bulk(d, false, 4, d->command, 9);
	if (!ret)
		ret = lgp_bulk(d, true, 3, d->reply, length + 1);
	if (!ret && d->reply[length] != 8)
		ret = -EIO;
	if (!ret)
		memcpy(values, d->reply, length);
	return ret;
}

struct lgp_mode {
	u16 width, height, htotal, vtotal;
	u32 clock, fps, control;
};

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
	u8 stable, status, b[7], polarity;
	u32 ht, vt, clock;
	unsigned int i;
	int ret;

	for (i = 0; i < 10; i++) {
		ret = lgp_mcu(d, 0x1b, &stable, 1);
		if (ret)
			return ret;
		if (!stable)
			break;
		msleep(30);
	}
	if (stable)
		return -ENOLINK;
	usleep_range(5000, 6000);
	ret = lgp_mcu(d, 0x1c, &status, 1);
	if (ret || !(status & 4))
		return ret ? ret : -ENOLINK;
	ret = lgp_mcu(d, 0x1d, b, sizeof(b));
	if (ret)
		return ret;
	ret = lgp_mcu(d, 0x26, &polarity, 1);
	if (ret)
		return ret;
	ht = b[0] | (b[1] & 15) << 8;
	s->width = b[2] | (b[1] & 0xf0) << 4;
	vt = b[3] | (b[4] & 15) << 8;
	s->height = b[5] | (b[4] & 0xf0) << 4;
	clock = b[6] ? 3456000 / b[6] : 0;
	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		const struct lgp_mode *m = &modes[i];

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
	dev_err(&d->usb->dev, "Unsupported HDMI timing %ux%u, totals %ux%u\n",
		s->width, s->height, ht, vt);
	return -ERANGE;
}

static int lgp_check_firmware(const struct firmware *fw, size_t size, const u8 *hash)
{
	struct crypto_shash *tfm;
	struct shash_desc *desc;
	u8 digest[32];
	int ret;

	if (fw->size != size)
		return -EINVAL;
	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	desc = kmalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
	if (!desc) {
		crypto_free_shash(tfm);
		return -ENOMEM;
	}
	desc->tfm = tfm;
	ret = crypto_shash_digest(desc, fw->data, fw->size, digest);
	if (!ret && memcmp(digest, hash, sizeof(digest)))
		ret = -EINVAL;
	kfree(desc);
	crypto_free_shash(tfm);
	return ret;
}

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
		if (ret || memcmp(d->dma, fw->data + off, n))
			return ret ? ret : -EILSEQ;
	}
	return 0;
}

static int lgp_boot(struct lgp *d)
{
	static const u8 video_hash[] = {
		0x8d,0xa3,0x0f,0x6d,0xc2,0xd4,0xfe,0xe5,0xae,0xb3,0xe2,0xe0,0x6d,0xf5,0x58,0x1d,
		0xb4,0xc0,0x1d,0x14,0x6c,0x50,0xc2,0x1f,0x6a,0x5d,0x48,0x24,0x02,0x5a,0x47,0xd1
	};
	static const u8 audio_hash[] = {
		0x0e,0x8e,0xa6,0x6b,0x6a,0x3d,0xdb,0x68,0xf9,0xa2,0x83,0xf1,0x01,0x55,0xdf,0x90,
		0xce,0x8d,0x6f,0x46,0x59,0x9e,0xd0,0xec,0xf6,0xf3,0x08,0xd6,0xc9,0x02,0x10,0xf4
	};
	const struct firmware *video, *audio;
	u32 value, off, n;
	unsigned int i;
	int ret;

	ret = request_firmware(&video, "avermedia/qpvidfwusb.bin", &d->usb->dev);
	if (ret)
		return ret;
	ret = request_firmware(&audio, "avermedia/qpaudfwusb.bin", &d->usb->dev);
	if (ret)
		goto release_video;
	ret = lgp_check_firmware(video, 454064, video_hash);
	if (!ret)
		ret = lgp_check_firmware(audio, 363832, audio_hash);
	if (ret) {
		dev_err(&d->usb->dev, "Unsupported firmware (size/SHA-256 check failed)\n");
		goto release_audio;
	}
	ret = lgp_arm(d, false);
	for (i = 0; !ret && i < 3; i++) {
		ret = lgp_write(d, 0x81c + i * 12, 0x4000 + i * 0x100000);
		if (!ret)
			ret = lgp_write(d, 0x820 + i * 12, 0x103fff + i * 0x100000);
		if (!ret)
			ret = lgp_write(d, 0x824 + i * 12, i * 0x100000);
	}
	if (!ret) ret = lgp_write(d, 0x840, 0x70003124);
	if (!ret) ret = lgp_write(d, 0x840, 0x90003124);
	if (!ret) ret = lgp_read_regs(d, 0x50, &value, 1);
	if (!ret) ret = lgp_write(d, 0x50, (value & ~2) | 4);
	if (!ret) ret = lgp_write(d, 0x610, 0);
	if (!ret) ret = lgp_write(d, 0x614, 0);
	if (ret) goto release_audio;
	msleep(50);
	ret = lgp_read_regs(d, 0, &value, 1);
	if (!ret) ret = lgp_write(d, 0, value & ~0x2000);
	for (off = audio->size; !ret && off < 0x70624; off += n) {
		n = min_t(u32, LGP_DMA_SIZE, 0x70624 - off);
		memset(d->dma, 0, n);
		ret = lgp_dma(d, true, 0x100000 + off, n);
	}
	if (!ret) ret = lgp_upload(d, audio, 0x100000);
	if (!ret) ret = lgp_memory_write(d, 0x2f1094, 1);
	if (!ret) ret = lgp_memory_write(d, 0x2f1090, 0);
	if (!ret) ret = lgp_memory_write(d, 0x2f2004, 0);
	if (!ret) ret = lgp_write(d, ARM_STATUS, 0);
	if (!ret) ret = lgp_upload(d, video, 0);
	if (!ret) {
		usleep_range(1000, 1500);
		ret = lgp_arm(d, true);
	}
	if (!ret) {
		msleep(150);
		ret = lgp_send(d, 0, NULL, 0, 0);
		if (!ret) ret = lgp_mailbox_wait(d);
	}
release_audio:
	release_firmware(audio);
release_video:
	release_firmware(video);
	return ret;
}

int lgp_hw_start(struct lgp *d)
{
	struct lgp_signal s;
	u32 open[] = { 0x80000002 }, link[] = { 0x100 }, mode[] = { 0, 0, 0 };
	u32 quality[] = { 11, d->quality->val * 15 / 100 }, frames[] = { 12, 0 };
	u32 timestamps[] = { 0x17, 1 };
	u32 regs[][2] = {
		{0x6f8, 0x2101c20e}, {0x6f4, 0}, {0x6f0, 0}, {0x6ec, 0x00781f40},
		{0x6e8, 0x1f4007d0}, {0x6e4, 0x2000}, {0x6e0, 0xf199001e},
		{0x6d8, 0x10}, {0x6dc, d->height << 16 | d->width},
		{0x6d4, 0x21161080}, {0x6d0, 0x520840f4},
	};
	unsigned int i;
	int ret = lgp_drain(d, 3, 64);

	if (!ret)
		ret = lgp_drain(d, 1, LGP_DMA_SIZE);
	if (!ret)
		ret = lgp_signal(d, &s);

	if (ret)
		return ret;
	if (d->width > s.width || d->height > s.height || d->fps > s.fps)
		return -ERANGE;
	d->frame_length = 0;
	regs[1][1] = s.height << 16 | s.width;
	regs[2][1] = (s.control & ~(0x3f << 23)) | d->fps << 23;
	regs[6][1] = 0xf1990000 | d->fps;
	ret = lgp_boot(d);
	if (!ret) ret = lgp_send(d, 0xf1, open, ARRAY_SIZE(open), 0);
	if (!ret) ret = lgp_send(d, 0xf2, link, ARRAY_SIZE(link), 0);
	if (!ret) ret = lgp_send(d, 0x11, mode, ARRAY_SIZE(mode), 0);
	if (!ret) ret = lgp_send(d, 0x10, quality, ARRAY_SIZE(quality), 0);
	if (!ret) ret = lgp_send(d, 0x10, frames, ARRAY_SIZE(frames), 0);
	if (!ret) ret = lgp_send(d, 0x10, timestamps, ARRAY_SIZE(timestamps), 0);
	if (!ret) ret = lgp_mailbox_wait(d);
	for (i = 0; !ret && i < ARRAY_SIZE(regs); i++)
		ret = lgp_write(d, regs[i][0], regs[i][1]);
	if (!ret) {
		d->running = true;
		ret = lgp_send(d, 1, NULL, 0, 0);
	}
	return ret;
}

int lgp_hw_frame(struct lgp *d, unsigned int *length)
{
	u32 m[7], addr, bytes, off, i, n, end, pad;
	u8 *frame = d->frame + d->frame_length;
	int ret = lgp_receive(d, m);

	if (ret <= 0)
		return ret;
	if ((m[0] & 0xffff) == 0x51)
		return -EIO;
	if ((m[0] & 0xffff) != 0x40 && (m[0] & 0xffff) != 0x41)
		return 0;
	if ((m[1] & 0xffff) != 0x83 ||
	    m[4] > (LGP_MAX_FRAME - d->frame_length) / 4 ||
	    m[2] >= 0x1000000 || m[4] > 0x1000000 - m[2]) {
		dev_err(&d->usb->dev,
			"Invalid stream buffer: type=%#x addr=%#x words=%u pending=%u\n",
			m[1], m[2], m[4], d->frame_length);
		lgp_return_buffer(d, m, 0);
		return -EPROTO;
	}
	addr = m[2] * 4;
	bytes = m[4] * 4;
	if (!d->frame_length)
		d->frame_pts = m[5];
	for (off = 0; off < bytes; off += n) {
		n = min_t(u32, LGP_DMA_SIZE, bytes - off);
		ret = lgp_dma(d, false, addr + off, n);
		if (ret)
			return ret;
		for (i = 0; i < n; i += 4) {
			frame[off + i] = d->dma[i + 3];
			frame[off + i + 1] = d->dma[i + 2];
			frame[off + i + 2] = d->dma[i + 1];
			frame[off + i + 3] = d->dma[i];
		}
	}
	ret = lgp_return_buffer(d, m, m[4]);
	if (ret)
		return ret;
	bytes += d->frame_length;
	if (bytes < 4 || d->frame[0] != 0xff || d->frame[1] != 0xd8)
		return -EILSEQ;
	/* The firmware splits a JPEG when its circular stream buffer wraps.
	 * Return each DMA buffer immediately, but retain the partial JPEG until
	 * its continuation supplies EOI. Only the final buffer has alignment pad.
	 */
	for (pad = 0; pad < 8 && bytes >= pad + 4; pad++) {
		end = bytes - pad;
		if (d->frame[end - 2] == 0xff && d->frame[end - 1] == 0xd9) {
			dev_dbg(&d->usb->dev, "JPEG pts=%#x size=%u\n", d->frame_pts, end);
			d->frame_length = 0;
			*length = end;
			return 1;
		}
	}
	d->frame_length = bytes;
	return 0;
}

int lgp_hw_stop(struct lgp *d)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(1500);
	u32 stop[] = { 0 }, m[7];
	int ret;

	if (!d->running)
		return 0;
	d->running = false;
	if (READ_ONCE(d->disconnected))
		return -ENODEV;
	ret = lgp_send(d, 2, stop, ARRAY_SIZE(stop), 0);
	if (ret)
		return ret;
	while (time_before(jiffies, deadline)) {
		ret = lgp_receive(d, m);
		if (ret < 0)
			return ret;
		if (!ret) {
			usleep_range(1000, 1500);
			continue;
		}
		if ((m[0] & 0xffff) == 0x40 || (m[0] & 0xffff) == 0x41)
			ret = lgp_return_buffer(d, m, 0);
		else if ((m[0] & 0xffff) == 0x50 && (m[1] & 0x7fffffff) == 2)
			return lgp_mailbox_wait(d);
		if (ret < 0)
			return ret;
	}
	return -ETIMEDOUT;
}
