/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MEDIA_SOPHGO_VDEC_H
#define _MEDIA_SOPHGO_VDEC_H

#include <linux/types.h>

struct device;
struct module;
struct sophgo_vdec;

#define SOPHGO_VDEC_MAX_WIDTH 2880
#define SOPHGO_VDEC_MAX_HEIGHT 1620
#define SOPHGO_VDEC_MAX_PIXELS (2880 * 1620)
#define SOPHGO_VDEC_STREAM_SIZE (2 * 1024 * 1024)

/* All addresses are kernel mappings, retained until release_frame(). */
struct sophgo_vdec_frame {
	u32 width;
	u32 height;
	u32 fourcc;
	u64 timestamp;
	const u8 *data[3];
	u32 stride[3];
	u32 size[3];
};

/* Operations run in process context and must have bounded hardware waits.
 * decode() returns 1 with a frame, 0 without a frame, or a negative errno.
 * An empty end_of_stream packet drains the codec, one frame per call.
 * The backend owns the decoded picture/reference buffers. The initial
 * interface copies visible rows into vb2 buffers; it makes no zero-copy
 * or encoder/capture-node promise.
 */
struct sophgo_vdec_ops {
	struct module *owner;
	int (*open)(u32 fourcc, void **session);
	void (*close)(void *session);
	int (*reset)(void *session);
	int (*decode)(void *session, const u8 *data, size_t size,
		      u64 timestamp, bool end_of_stream,
		      struct sophgo_vdec_frame *frame);
	void (*release_frame)(void *session);
};

struct sophgo_vdec *sophgo_vdec_register(struct device *parent,
				       const struct sophgo_vdec_ops *ops);
void sophgo_vdec_unregister(struct sophgo_vdec *decoder);

#endif
