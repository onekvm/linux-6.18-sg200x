// SPDX-License-Identifier: GPL-2.0-only
/* SG2002 stateful decoder frontend. Hardware operations belong to the BSP. */
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <media/sophgo-vdec.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-vmalloc.h>

struct sophgo_vdec {
	struct v4l2_device v4l2;
	struct video_device video;
	struct v4l2_m2m_dev *m2m;
	struct mutex queue_lock;
	struct workqueue_struct *workqueue;
	const struct sophgo_vdec_ops *ops;
};

struct sophgo_ctx {
	struct v4l2_fh fh;
	struct sophgo_vdec *dev;
	struct v4l2_ctrl_handler controls;
	struct mutex state_lock;
	struct work_struct work;
	struct v4l2_pix_format_mplane output;
	struct v4l2_pix_format_mplane capture;
	struct sophgo_vdec_frame frame;
	void *session;
	u32 output_sequence;
	u32 capture_sequence;
	u32 visible_width;
	u32 visible_height;
	bool have_frame;
	bool have_sequence;
	bool reconfigure;
	bool draining;
	bool stopped;
	bool aborting;
};

static const u32 coded_formats[] = {
	V4L2_PIX_FMT_H264, V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_JPEG,
};
static const u32 raw_formats[] = {
	V4L2_PIX_FMT_YUV420M, V4L2_PIX_FMT_YUV422M,
	V4L2_PIX_FMT_YUV444M, V4L2_PIX_FMT_GREY,
};

static struct sophgo_ctx *file_to_ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct sophgo_ctx, fh);
}

static bool is_coded(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(coded_formats); i++)
		if (coded_formats[i] == fourcc)
			return true;
	return false;
}

static unsigned int raw_planes(u32 fourcc)
{
	return fourcc == V4L2_PIX_FMT_GREY ? 1 : 3;
}

static void plane_geometry(u32 fourcc, u32 width, u32 height, unsigned int p,
			   u32 *columns, u32 *rows)
{
	*columns = width;
	*rows = height;
	if (p && fourcc != V4L2_PIX_FMT_YUV444M)
		*columns = DIV_ROUND_UP(width, 2);
	if (p && fourcc == V4L2_PIX_FMT_YUV420M)
		*rows = DIV_ROUND_UP(height, 2);
}

static void make_format(struct v4l2_pix_format_mplane *pix, bool coded)
{
	u32 width, height;
	unsigned int p;

	width = clamp_t(u32, pix->width, 32, SOPHGO_VDEC_MAX_WIDTH);
	height = clamp_t(u32, pix->height, 32, SOPHGO_VDEC_MAX_HEIGHT);
	if (width * height > SOPHGO_VDEC_MAX_PIXELS)
		height = SOPHGO_VDEC_MAX_PIXELS / width;
	pix->width = coded ? width : ALIGN(width, 16);
	pix->height = coded ? height : ALIGN(height, 16);
	pix->field = V4L2_FIELD_NONE;
	pix->num_planes = coded ? 1 : raw_planes(pix->pixelformat);
	pix->colorspace = V4L2_COLORSPACE_REC709;
	pix->xfer_func = V4L2_XFER_FUNC_DEFAULT;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	pix->quantization = V4L2_QUANTIZATION_DEFAULT;
	pix->flags = 0;
	memset(pix->reserved, 0, sizeof(pix->reserved));
	memset(pix->plane_fmt, 0, sizeof(pix->plane_fmt));
	for (p = 0; p < pix->num_planes; p++) {
		if (coded) {
			pix->plane_fmt[p].sizeimage = SOPHGO_VDEC_STREAM_SIZE;
			continue;
		}
		/*
		 * Keep every capture plane large enough for the largest supported
		 * frame. A queued buffer can outlive a SOURCE_CHANGE handshake.
		 */
		pix->plane_fmt[p].bytesperline = p ?
			DIV_ROUND_UP(SOPHGO_VDEC_MAX_WIDTH, 2) : SOPHGO_VDEC_MAX_WIDTH;
		pix->plane_fmt[p].sizeimage = SOPHGO_VDEC_MAX_PIXELS;
	}
}

static void set_colorspace(struct v4l2_pix_format_mplane *pix, u32 coded_fourcc)
{
	if (coded_fourcc == V4L2_PIX_FMT_JPEG || coded_fourcc == V4L2_PIX_FMT_MJPEG) {
		pix->colorspace = V4L2_COLORSPACE_JPEG;
		pix->xfer_func = V4L2_XFER_FUNC_SRGB;
		pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
		pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	}
}

static int querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	strscpy(cap->driver, "sophgo-vdec", sizeof(cap->driver));
	strscpy(cap->card, "SG2002 video decoder", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:sophgo-vdec", sizeof(cap->bus_info));
	return 0;
}

static int enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *fmt)
{
	struct sophgo_ctx *ctx = file_to_ctx(file);

	if (V4L2_TYPE_IS_OUTPUT(fmt->type)) {
		if (fmt->index >= ARRAY_SIZE(coded_formats))
			return -EINVAL;
		fmt->pixelformat = coded_formats[fmt->index];
		fmt->flags = V4L2_FMT_FLAG_COMPRESSED | V4L2_FMT_FLAG_DYN_RESOLUTION;
	} else {
		fmt->flags = 0;
		if (ctx->output.pixelformat == V4L2_PIX_FMT_H264 && fmt->index)
			return -EINVAL;
		if (fmt->index >= ARRAY_SIZE(raw_formats))
			return -EINVAL;
		fmt->pixelformat = raw_formats[fmt->index];
	}
	return 0;
}

static int enum_framesizes(struct file *file, void *fh,
			  struct v4l2_frmsizeenum *size)
{
	struct sophgo_ctx *ctx = file_to_ctx(file);
	bool coded = is_coded(size->pixel_format);
	unsigned int i;

	if (size->index)
		return -EINVAL;
	if (!coded) {
		for (i = 0; i < ARRAY_SIZE(raw_formats); i++)
			if (raw_formats[i] == size->pixel_format)
				break;
		if (i == ARRAY_SIZE(raw_formats) ||
		    (ctx->output.pixelformat == V4L2_PIX_FMT_H264 && i))
			return -EINVAL;
	}

	size->type = coded ? V4L2_FRMSIZE_TYPE_CONTINUOUS : V4L2_FRMSIZE_TYPE_STEPWISE;
	size->stepwise.min_width = size->stepwise.min_height = 32;
	size->stepwise.max_width = SOPHGO_VDEC_MAX_WIDTH;
	size->stepwise.max_height = coded ? SOPHGO_VDEC_MAX_HEIGHT :
		ALIGN(SOPHGO_VDEC_MAX_HEIGHT, 16);
	size->stepwise.step_width = size->stepwise.step_height = coded ? 1 : 16;
	return 0;
}

static int try_fmt(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct sophgo_ctx *ctx = file_to_ctx(file);
	struct v4l2_pix_format_mplane *pix = &fmt->fmt.pix_mp;
	bool coded = V4L2_TYPE_IS_OUTPUT(fmt->type);
	unsigned int i;

	if (coded) {
		if (!is_coded(pix->pixelformat))
			pix->pixelformat = V4L2_PIX_FMT_H264;
	} else {
		for (i = 0; i < ARRAY_SIZE(raw_formats); i++)
			if (raw_formats[i] == pix->pixelformat)
				break;
		if (i == ARRAY_SIZE(raw_formats))
			pix->pixelformat = V4L2_PIX_FMT_YUV420M;
		if (ctx->output.pixelformat == V4L2_PIX_FMT_H264)
			pix->pixelformat = V4L2_PIX_FMT_YUV420M;
		mutex_lock(&ctx->state_lock);
		if (ctx->have_sequence)
			*pix = ctx->capture;
		mutex_unlock(&ctx->state_lock);
	}
	make_format(pix, coded);
	set_colorspace(pix, coded ? pix->pixelformat : ctx->output.pixelformat);
	return 0;
}

static int get_fmt(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct sophgo_ctx *ctx = file_to_ctx(file);

	mutex_lock(&ctx->state_lock);
	fmt->fmt.pix_mp = V4L2_TYPE_IS_OUTPUT(fmt->type) ?
		ctx->output : ctx->capture;
	mutex_unlock(&ctx->state_lock);
	return 0;
}

static int set_fmt(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct sophgo_ctx *ctx = file_to_ctx(file);
	struct vb2_queue *q = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, fmt->type);
	int ret;

	if (vb2_is_busy(q))
		return -EBUSY;
	if (V4L2_TYPE_IS_OUTPUT(fmt->type) &&
	    vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return -EBUSY;
	ret = try_fmt(file, fh, fmt);
	if (ret)
		return ret;
	mutex_lock(&ctx->state_lock);
	if (V4L2_TYPE_IS_OUTPUT(fmt->type)) {
		ctx->output = fmt->fmt.pix_mp;
		ctx->capture.width = ctx->output.width;
		ctx->capture.height = ctx->output.height;
		ctx->capture.pixelformat = V4L2_PIX_FMT_YUV420M;
		make_format(&ctx->capture, false);
		set_colorspace(&ctx->capture, ctx->output.pixelformat);
		ctx->have_sequence = false;
	} else {
		ctx->capture = fmt->fmt.pix_mp;
	}
	mutex_unlock(&ctx->state_lock);
	return 0;
}

static int get_selection(struct file *file, void *fh, struct v4l2_selection *s)
{
	struct sophgo_ctx *ctx = file_to_ctx(file);

	if (s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	if (s->target != V4L2_SEL_TGT_COMPOSE &&
	    s->target != V4L2_SEL_TGT_COMPOSE_DEFAULT &&
	    s->target != V4L2_SEL_TGT_COMPOSE_BOUNDS)
		return -EINVAL;
	mutex_lock(&ctx->state_lock);
	s->r.left = s->r.top = 0;
	s->r.width = ctx->have_sequence ? ctx->visible_width : ctx->capture.width;
	s->r.height = ctx->have_sequence ? ctx->visible_height : ctx->capture.height;
	if (s->target == V4L2_SEL_TGT_COMPOSE_BOUNDS) {
		s->r.width = ctx->capture.width;
		s->r.height = ctx->capture.height;
	}
	mutex_unlock(&ctx->state_lock);
	return 0;
}

static int subscribe_event(struct v4l2_fh *fh,
			   const struct v4l2_event_subscription *sub)
{
	if (sub->type == V4L2_EVENT_SOURCE_CHANGE)
		return v4l2_src_change_event_subscribe(fh, sub);
	if (sub->type == V4L2_EVENT_EOS)
		return v4l2_event_subscribe(fh, sub, 2, NULL);
	return v4l2_ctrl_subscribe_event(fh, sub);
}

static int try_decoder_cmd(struct file *file, void *fh,
			   struct v4l2_decoder_cmd *cmd)
{
	if (cmd->cmd != V4L2_DEC_CMD_STOP && cmd->cmd != V4L2_DEC_CMD_START)
		return -EINVAL;
	cmd->flags = 0;
	memset(&cmd->start, 0, sizeof(cmd->start));
	return 0;
}

static int decoder_cmd(struct file *file, void *fh, struct v4l2_decoder_cmd *cmd)
{
	struct sophgo_ctx *ctx = file_to_ctx(file);
	int ret = try_decoder_cmd(file, fh, cmd);

	if (ret)
		return ret;
	mutex_lock(&ctx->state_lock);
	if (cmd->cmd == V4L2_DEC_CMD_STOP) {
		if (!ctx->session) {
			ret = -EINVAL;
			goto out;
		}
		if (ctx->stopped || ctx->draining)
			goto out;
		ctx->draining = true;
	} else {
		if (ctx->draining) {
			ret = -EBUSY;
			goto out;
		}
		if (ctx->stopped && ctx->session && ctx->dev->ops->reset) {
			ret = ctx->dev->ops->reset(ctx->session);
			if (ret)
				goto out;
		}
		ctx->stopped = false;
		vb2_clear_last_buffer_dequeued(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
	}
out:
	mutex_unlock(&ctx->state_lock);
	if (!ret)
		v4l2_m2m_try_schedule(ctx->fh.m2m_ctx);
	return ret;
}

static int queue_buffer(struct file *file, void *fh, struct v4l2_buffer *buffer)
{
	struct sophgo_ctx *ctx = file_to_ctx(file);
	int ret;

	if (V4L2_TYPE_IS_OUTPUT(buffer->type) &&
	    (READ_ONCE(ctx->draining) || READ_ONCE(ctx->stopped)))
		return -EBUSY;
	ret = v4l2_m2m_ioctl_qbuf(file, fh, buffer);
	return ret;
}

static int streamon(struct file *file, void *fh, enum v4l2_buf_type type)
{
	struct sophgo_ctx *ctx = file_to_ctx(file);
	int ret;

	ret = v4l2_m2m_ioctl_streamon(file, fh, type);
	if (!ret && V4L2_TYPE_IS_CAPTURE(type)) {
		/*
		 * VB2 can start a queue automatically while QBUF fills it. Keep
		 * the decoder stopped until userspace completes STREAMON after a
		 * format-change handshake.
		 */
		mutex_lock(&ctx->state_lock);
		ctx->reconfigure = false;
		mutex_unlock(&ctx->state_lock);
		v4l2_m2m_try_schedule(ctx->fh.m2m_ctx);
	}
	return ret;
}

static const struct v4l2_ioctl_ops ioctl_ops = {
	.vidioc_querycap = querycap,
	.vidioc_enum_fmt_vid_out = enum_fmt,
	.vidioc_enum_fmt_vid_cap = enum_fmt,
	.vidioc_enum_framesizes = enum_framesizes,
	.vidioc_g_fmt_vid_out_mplane = get_fmt,
	.vidioc_g_fmt_vid_cap_mplane = get_fmt,
	.vidioc_try_fmt_vid_out_mplane = try_fmt,
	.vidioc_try_fmt_vid_cap_mplane = try_fmt,
	.vidioc_s_fmt_vid_out_mplane = set_fmt,
	.vidioc_s_fmt_vid_cap_mplane = set_fmt,
	.vidioc_g_selection = get_selection,
	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_qbuf = queue_buffer,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon = streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
	.vidioc_try_decoder_cmd = try_decoder_cmd,
	.vidioc_decoder_cmd = decoder_cmd,
	.vidioc_subscribe_event = subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static void return_buffers(struct sophgo_ctx *ctx, bool output,
			   enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *buf;

	for (;;) {
		buf = output ? v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
			v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!buf)
			break;
		v4l2_m2m_buf_done(buf, state);
	}
}

static int queue_setup(struct vb2_queue *q, unsigned int *buffers,
		       unsigned int *planes, unsigned int sizes[],
		       struct device *alloc_devs[])
{
	struct sophgo_ctx *ctx = vb2_get_drv_priv(q);
	struct v4l2_pix_format_mplane *pix = V4L2_TYPE_IS_OUTPUT(q->type) ?
		&ctx->output : &ctx->capture;
	unsigned int p;
	u32 bytes = 0, limit, allocated;
	int ret = 0;

	mutex_lock(&ctx->state_lock);
	if (*planes && *planes != pix->num_planes) {
		ret = -EINVAL;
		goto out;
	}
	for (p = 0; p < pix->num_planes; p++) {
		if (*planes && sizes[p] < pix->plane_fmt[p].sizeimage) {
			ret = -EINVAL;
			goto out;
		}
		sizes[p] = pix->plane_fmt[p].sizeimage;
		bytes += sizes[p];
	}
	*planes = pix->num_planes;
	/* Bound userspace's allocation on the 256 MiB SG2002. */
	limit = min_t(u32, 8, (32 * 1024 * 1024) / bytes);
	allocated = vb2_get_num_buffers(q);
	if (allocated >= limit) {
		ret = -ENOMEM;
		goto out;
	}
	*buffers = clamp_t(unsigned int, *buffers, 1, limit - allocated);
out:
	mutex_unlock(&ctx->state_lock);
	return ret;
}

static int buffer_prepare(struct vb2_buffer *vb)
{
	struct sophgo_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_pix_format_mplane *pix = V4L2_TYPE_IS_OUTPUT(vb->type) ?
		&ctx->output : &ctx->capture;
	unsigned int p;
	int ret = 0;

	mutex_lock(&ctx->state_lock);
	for (p = 0; p < pix->num_planes; p++)
		if (vb2_plane_size(vb, p) < pix->plane_fmt[p].sizeimage) {
			ret = -EINVAL;
			break;
		}
	mutex_unlock(&ctx->state_lock);
	return ret;
}

static void buffer_queue(struct vb2_buffer *vb)
{
	struct sophgo_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static int start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct sophgo_ctx *ctx = vb2_get_drv_priv(q);
	int ret = 0;

	mutex_lock(&ctx->state_lock);
	ctx->aborting = false;
	if (V4L2_TYPE_IS_OUTPUT(q->type)) {
		ret = ctx->dev->ops->open(ctx->output.pixelformat, &ctx->session);
		ctx->output_sequence = 0;
		ctx->stopped = ctx->draining = false;
	} else {
		ctx->capture_sequence = 0;
	}
	mutex_unlock(&ctx->state_lock);
	if (ret)
		return_buffers(ctx, V4L2_TYPE_IS_OUTPUT(q->type), VB2_BUF_STATE_QUEUED);
	return ret;
}

static void stop_streaming(struct vb2_queue *q)
{
	struct sophgo_ctx *ctx = vb2_get_drv_priv(q);

	/* job_abort/finish has quiesced work before vb2 calls this operation. */
	flush_work(&ctx->work);
	mutex_lock(&ctx->state_lock);
	if (V4L2_TYPE_IS_OUTPUT(q->type)) {
		if (ctx->have_frame)
			ctx->dev->ops->release_frame(ctx->session);
		ctx->have_frame = ctx->have_sequence = ctx->reconfigure = false;
		if (ctx->session)
			ctx->dev->ops->close(ctx->session);
		ctx->session = NULL;
		ctx->draining = ctx->stopped = false;
	}
	mutex_unlock(&ctx->state_lock);
	return_buffers(ctx, V4L2_TYPE_IS_OUTPUT(q->type), VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops queue_ops = {
	.queue_setup = queue_setup,
	.buf_prepare = buffer_prepare,
	.buf_queue = buffer_queue,
	.start_streaming = start_streaming,
	.stop_streaming = stop_streaming,
};

static int queue_init(void *priv, struct vb2_queue *src, struct vb2_queue *dst)
{
	struct sophgo_ctx *ctx = priv;
	struct vb2_queue *queues[] = { src, dst };
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(queues); i++) {
		struct vb2_queue *q = queues[i];

		q->type = i ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
			V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		q->io_modes = VB2_MMAP | VB2_DMABUF;
		q->drv_priv = ctx;
		q->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
		q->ops = &queue_ops;
		q->mem_ops = &vb2_vmalloc_memops;
		q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
		q->lock = &ctx->dev->queue_lock;
		q->dev = ctx->dev->v4l2.dev;
		q->allow_zero_bytesused = 1;
		/*
		 * Keep the capture queue stopped until the userspace format-change
		 * handshake has queued the complete small buffer set.  Starting on
		 * the first QBUF lets the decoder race a subsequent SOURCE_CHANGE
		 * while setup_capture() is still queuing the old-format buffers.
		 */
		if (V4L2_TYPE_IS_CAPTURE(q->type))
			q->min_queued_buffers = 1;
		/* This kernel's VB2 API requires at least VB2_MAX_FRAME slots. */
		q->max_num_buffers = VB2_MAX_FRAME;
		ret = vb2_queue_init(q);
		if (ret)
			return ret;
	}
	return 0;
}

static int copy_frame(struct sophgo_ctx *ctx, struct vb2_v4l2_buffer *buf)
{
	struct sophgo_vdec_frame *frame = &ctx->frame;
	unsigned int p;

	for (p = 0; p < ctx->capture.num_planes; p++) {
		u32 columns, rows, row;
		u32 stride = ctx->capture.plane_fmt[p].bytesperline;
		u32 size = ctx->capture.plane_fmt[p].sizeimage;
		u8 *dst = vb2_plane_vaddr(&buf->vb2_buf, p);

		plane_geometry(frame->fourcc, frame->width, frame->height, p,
			       &columns, &rows);
		if (!dst || !frame->data[p] || frame->stride[p] < columns ||
		    frame->size[p] < (rows - 1) * frame->stride[p] + columns ||
		    stride < columns || vb2_plane_size(&buf->vb2_buf, p) < size)
			return -EIO;
		memset(dst, p ? 128 : 0, size);
		for (row = 0; row < rows; row++)
			memcpy(dst + row * stride,
			       frame->data[p] + row * frame->stride[p], columns);
		buf->vb2_buf.planes[p].data_offset = 0;
		vb2_set_plane_payload(&buf->vb2_buf, p, size);
	}
	buf->vb2_buf.timestamp = frame->timestamp;
	buf->field = V4L2_FIELD_NONE;
	buf->sequence = ctx->capture_sequence++;
	return 0;
}

static void send_last(struct sophgo_ctx *ctx, bool end_of_stream)
{
	struct vb2_v4l2_buffer *buf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	struct v4l2_event event = { .type = V4L2_EVENT_EOS };
	unsigned int p;

	if (!buf)
		return;
	for (p = 0; p < buf->vb2_buf.num_planes; p++)
		vb2_set_plane_payload(&buf->vb2_buf, p, 0);
	buf->flags |= V4L2_BUF_FLAG_LAST;
	buf->sequence = ctx->capture_sequence++;
	v4l2_m2m_buf_done(buf, VB2_BUF_STATE_DONE);
	if (end_of_stream) {
		v4l2_event_queue_fh(&ctx->fh, &event);
		ctx->stopped = true;
		ctx->draining = false;
	}
}

static void decode_work(struct work_struct *work)
{
	struct sophgo_ctx *ctx = container_of(work, struct sophgo_ctx, work);
	struct vb2_v4l2_buffer *src = NULL, *dst;
	struct v4l2_event event = { .type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION };
	int ret;

	mutex_lock(&ctx->state_lock);
	if (ctx->aborting || !ctx->session || ctx->stopped || ctx->reconfigure)
		goto finish;
	if (!ctx->have_frame) {
		const u8 *data = NULL;
		size_t size = 0;
		u64 timestamp = 0;

		src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		if (src) {
			u32 offset = src->vb2_buf.planes[0].data_offset;
			u32 payload = vb2_get_plane_payload(&src->vb2_buf, 0);

			src->sequence = ctx->output_sequence++;
			data = vb2_plane_vaddr(&src->vb2_buf, 0);
			if (!data || offset > payload || payload > SOPHGO_VDEC_STREAM_SIZE) {
				v4l2_m2m_buf_done(src, VB2_BUF_STATE_ERROR);
				goto finish;
			}
			data += offset;
			size = payload - offset;
			timestamp = src->vb2_buf.timestamp;
			if (!size)
				ctx->draining = true;
		}
		ret = ctx->dev->ops->decode(ctx->session, data, size, timestamp,
					 !size && ctx->draining, &ctx->frame);
		if (src)
			v4l2_m2m_buf_done(src, ret < 0 ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
		if (ret < 0) {
			v4l2_err(&ctx->dev->v4l2, "decode failed: %d\n", ret);
			if (!src) {
				ctx->draining = false;
				ctx->stopped = true;
				vb2_queue_error(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
			}
			goto finish;
		}
		if (!ret) {
			if (!src && ctx->draining)
				send_last(ctx, true);
			goto finish;
		}
		ctx->have_frame = true;
		if (!ctx->frame.width || !ctx->frame.height ||
		    ctx->frame.width > SOPHGO_VDEC_MAX_WIDTH ||
		    ctx->frame.height > SOPHGO_VDEC_MAX_HEIGHT ||
		    ctx->frame.width * ctx->frame.height > SOPHGO_VDEC_MAX_PIXELS) {
			ctx->dev->ops->release_frame(ctx->session);
			ctx->have_frame = false;
			goto finish;
		}
		if (!ctx->have_sequence ||
		    ctx->visible_width != ctx->frame.width ||
		    ctx->visible_height != ctx->frame.height ||
		    ctx->capture.width != ALIGN(ctx->frame.width, 16) ||
		    ctx->capture.height != ALIGN(ctx->frame.height, 16) ||
		    ctx->capture.pixelformat != ctx->frame.fourcc) {
			ctx->capture.width = ctx->frame.width;
			ctx->capture.height = ctx->frame.height;
			ctx->visible_width = ctx->frame.width;
			ctx->visible_height = ctx->frame.height;
			ctx->capture.pixelformat = ctx->frame.fourcc;
			make_format(&ctx->capture, false);
			set_colorspace(&ctx->capture, ctx->output.pixelformat);
			ctx->have_sequence = ctx->reconfigure = true;
			v4l2_event_queue_fh(&ctx->fh, &event);
			if (vb2_is_streaming(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
				send_last(ctx, false);
			goto finish;
		}
	}
	if (!ctx->reconfigure &&
	    vb2_is_streaming(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx))) {
		dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (dst) {
			ret = copy_frame(ctx, dst);
			ctx->dev->ops->release_frame(ctx->session);
			ctx->have_frame = false;
			v4l2_m2m_buf_done(dst, ret ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
		}
	}
finish:
	mutex_unlock(&ctx->state_lock);
	v4l2_m2m_job_finish(ctx->dev->m2m, ctx->fh.m2m_ctx);
}

static int job_ready(void *priv)
{
	struct sophgo_ctx *ctx = priv;
	bool capture_ready = vb2_is_streaming(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)) &&
		v4l2_m2m_num_dst_bufs_ready(ctx->fh.m2m_ctx);

	if (READ_ONCE(ctx->aborting) || !READ_ONCE(ctx->session) ||
	    READ_ONCE(ctx->reconfigure) || READ_ONCE(ctx->stopped))
		return 0;
	if (READ_ONCE(ctx->have_frame))
		return capture_ready;
	if (!v4l2_m2m_num_src_bufs_ready(ctx->fh.m2m_ctx))
		return READ_ONCE(ctx->draining) && capture_ready;
	return !READ_ONCE(ctx->have_sequence) || capture_ready;
}

static void device_run(void *priv)
{
	struct sophgo_ctx *ctx = priv;

	queue_work(ctx->dev->workqueue, &ctx->work);
}

static void job_abort(void *priv)
{
	struct sophgo_ctx *ctx = priv;

	WRITE_ONCE(ctx->aborting, true);
	/* Work owns job_finish(), including when it has not started yet. */
	flush_work(&ctx->work);
}

static const struct v4l2_m2m_ops m2m_ops = {
	.device_run = device_run,
	.job_ready = job_ready,
	.job_abort = job_abort,
};

static int decoder_open(struct file *file)
{
	struct sophgo_vdec *dev = video_drvdata(file);
	struct sophgo_ctx *ctx;
	struct v4l2_ctrl *ctrl;
	int ret;

	if (!try_module_get(dev->ops->owner))
		return -ENODEV;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto put_module;
	}
	ctx->dev = dev;
	mutex_init(&ctx->state_lock);
	INIT_WORK(&ctx->work, decode_work);
	v4l2_fh_init(&ctx->fh, &dev->video);
	v4l2_ctrl_handler_init(&ctx->controls, 1);
	ctrl = v4l2_ctrl_new_std(&ctx->controls, NULL,
		V4L2_CID_MIN_BUFFERS_FOR_CAPTURE, 1, 8, 1, 1);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	ctx->fh.ctrl_handler = &ctx->controls;
	ret = ctx->controls.error;
	if (ret)
		goto free_ctx;
	ctx->output.width = ctx->capture.width = 1920;
	ctx->output.height = ctx->capture.height = 1080;
	ctx->output.pixelformat = V4L2_PIX_FMT_H264;
	ctx->capture.pixelformat = V4L2_PIX_FMT_YUV420M;
	make_format(&ctx->output, true);
	make_format(&ctx->capture, false);
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->m2m, ctx, queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto free_ctx;
	}
	ctx->fh.m2m_ctx->ignore_cap_streaming = true;
	v4l2_m2m_set_src_buffered(ctx->fh.m2m_ctx, true);
	v4l2_m2m_set_dst_buffered(ctx->fh.m2m_ctx, true);
	v4l2_device_get(&dev->v4l2);
	v4l2_fh_add(&ctx->fh, file);
	return 0;
free_ctx:
	v4l2_ctrl_handler_free(&ctx->controls);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
put_module:
	module_put(dev->ops->owner);
	return ret;
}

static int decoder_release(struct file *file)
{
	struct sophgo_ctx *ctx = file_to_ctx(file);
	struct sophgo_vdec *dev = ctx->dev;

	mutex_lock(&dev->queue_lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	cancel_work_sync(&ctx->work);
	if (ctx->session) {
		if (ctx->have_frame)
			dev->ops->release_frame(ctx->session);
		dev->ops->close(ctx->session);
	}
	v4l2_ctrl_handler_free(&ctx->controls);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	mutex_unlock(&dev->queue_lock);
	kfree(ctx);
	module_put(dev->ops->owner);
	v4l2_device_put(&dev->v4l2);
	return 0;
}

static const struct v4l2_file_operations file_ops = {
	.owner = THIS_MODULE,
	.open = decoder_open,
	.release = decoder_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static void decoder_video_release(struct video_device *video)
{
	v4l2_device_put(video->v4l2_dev);
}

static void decoder_device_release(struct v4l2_device *v4l2)
{
	struct sophgo_vdec *dev = container_of(v4l2, struct sophgo_vdec, v4l2);

	v4l2_m2m_release(dev->m2m);
	destroy_workqueue(dev->workqueue);
	kfree(dev);
}

struct sophgo_vdec *sophgo_vdec_register(struct device *parent,
				       const struct sophgo_vdec_ops *ops)
{
	struct sophgo_vdec *dev;
	int ret;

	if (!ops || !ops->open || !ops->close || !ops->decode || !ops->release_frame)
		return ERR_PTR(-EINVAL);
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return ERR_PTR(-ENOMEM);
	dev->ops = ops;
	mutex_init(&dev->queue_lock);
	dev->workqueue = alloc_ordered_workqueue("sophgo-vdec", WQ_MEM_RECLAIM);
	if (!dev->workqueue) {
		ret = -ENOMEM;
		goto free_dev;
	}
	ret = v4l2_device_register(parent, &dev->v4l2);
	if (ret)
		goto free_workqueue;
	dev->v4l2.release = decoder_device_release;
	dev->m2m = v4l2_m2m_init(&m2m_ops);
	if (IS_ERR(dev->m2m)) {
		ret = PTR_ERR(dev->m2m);
		goto unregister_v4l2;
	}
	strscpy(dev->video.name, "SG2002 decoder", sizeof(dev->video.name));
	dev->video.v4l2_dev = &dev->v4l2;
	dev->video.fops = &file_ops;
	dev->video.ioctl_ops = &ioctl_ops;
	dev->video.release = decoder_video_release;
	dev->video.lock = &dev->queue_lock;
	dev->video.vfl_dir = VFL_DIR_M2M;
	dev->video.device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	video_set_drvdata(&dev->video, dev);
	/*
	 * The video core can retain its embedded node past the last file's
	 * release callback. Keep our container alive until video->release.
	 */
	v4l2_device_get(&dev->v4l2);
	ret = video_register_device(&dev->video, VFL_TYPE_VIDEO, -1);
	if (ret) {
		v4l2_device_put(&dev->v4l2);
		goto free_m2m;
	}
	v4l2_info(&dev->v4l2, "decoder registered as /dev/video%d\n", dev->video.num);
	return dev;
free_m2m:
	v4l2_m2m_release(dev->m2m);
unregister_v4l2:
	v4l2_device_unregister(&dev->v4l2);
free_workqueue:
	destroy_workqueue(dev->workqueue);
free_dev:
	kfree(dev);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(sophgo_vdec_register);

void sophgo_vdec_unregister(struct sophgo_vdec *dev)
{
	if (!dev || IS_ERR(dev))
		return;
	v4l2_device_disconnect(&dev->v4l2);
	video_unregister_device(&dev->video);
	v4l2_device_unregister(&dev->v4l2);
	v4l2_device_put(&dev->v4l2);
}
EXPORT_SYMBOL_GPL(sophgo_vdec_unregister);

MODULE_DESCRIPTION("Sophgo SG2002 V4L2 stateful decoder frontend");
MODULE_LICENSE("GPL");
