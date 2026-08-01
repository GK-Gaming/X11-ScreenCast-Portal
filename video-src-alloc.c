/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

/*
 [title]
 Video source using \ref pw_stream.
 [title]
 */
 
#define _GNU_SOURCE

#include "both.h"

#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <math.h>

#include <poll.h>

#include <fcntl.h>
#include <sys/mman.h>

enum event_loop_fd {
	EVENT_LOOP_PIPEWIRE,
	EVENT_LOOP_PORTAL,
};

#include <spa/param/video/format-utils.h>
#include <spa/param/tag-utils.h>
#include <spa/param/dict-utils.h>
#include <spa/debug/pod.h>
#include <spa/debug/format.h>
#include <spa/pod/vararg.h>

#include <pipewire/pipewire.h>

#include <libdrm/drm_fourcc.h>

struct pw_context *pw_context;
struct pw_core *pw_core;
struct pw_loop *pw_loop;

struct spa_source *timer;

drmtap_ctx* ctx;
drmtap_display display;
drmtap_frame_info frame;
drmtap_dmabuf_desc desc;
drmtap_cursor_info cursor;

int framerate;
unsigned spa_format;

struct portal_instance {
	bool active;
	enum pw_stream_state state;
	
	int id;
	
	uint32_t node_id;
	uint64_t pipewire_serial;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	struct spa_video_info_raw format;
	int32_t stride;
	uint32_t seq;
};

int instances;
struct portal_instance instance[MAX_SESSIONS];

int portal_fd_write;
int portal_fd_read;
struct portal_ipc ipc_buffer;

void stop_capture(struct portal_instance* data);

#define BPP		4
#define CURSOR_WIDTH	64
#define CURSOR_HEIGHT	64
#define CURSOR_BPP	4
#define CURSOR_META_SIZE(w,h)	(sizeof(struct spa_meta_cursor) + \
				 sizeof(struct spa_meta_bitmap) + w * h * CURSOR_BPP)

#define MAX_BUFFERS	64

#define M_PI_M2 ( M_PI + M_PI )


uint32_t xdpw_format_drm_fourcc_from_pw_format(enum spa_video_format format) {
	switch (format) {
	case SPA_VIDEO_FORMAT_BGRA:
		return DRM_FORMAT_ARGB8888;
	case SPA_VIDEO_FORMAT_BGRx:
		return DRM_FORMAT_XRGB8888;
	case SPA_VIDEO_FORMAT_ABGR:
		return DRM_FORMAT_RGBA8888;
	case SPA_VIDEO_FORMAT_xBGR:
		return DRM_FORMAT_RGBX8888;
	case SPA_VIDEO_FORMAT_RGBA:
		return DRM_FORMAT_ABGR8888;
	case SPA_VIDEO_FORMAT_RGBx:
		return DRM_FORMAT_XBGR8888;
	case SPA_VIDEO_FORMAT_ARGB:
		return DRM_FORMAT_BGRA8888;
	case SPA_VIDEO_FORMAT_xRGB:
		return DRM_FORMAT_BGRX8888;
	case SPA_VIDEO_FORMAT_NV12:
		return DRM_FORMAT_NV12;
	case SPA_VIDEO_FORMAT_xRGB_210LE:
		return DRM_FORMAT_XRGB2101010;
	case SPA_VIDEO_FORMAT_xBGR_210LE:
		return DRM_FORMAT_XBGR2101010;
	case SPA_VIDEO_FORMAT_RGBx_102LE:
		return DRM_FORMAT_RGBX1010102;
	case SPA_VIDEO_FORMAT_BGRx_102LE:
		return DRM_FORMAT_BGRX1010102;
	case SPA_VIDEO_FORMAT_ARGB_210LE:
		return DRM_FORMAT_ARGB2101010;
	case SPA_VIDEO_FORMAT_ABGR_210LE:
		return DRM_FORMAT_ABGR2101010;
	case SPA_VIDEO_FORMAT_RGBA_102LE:
		return DRM_FORMAT_RGBA1010102;
	case SPA_VIDEO_FORMAT_BGRA_102LE:
		return DRM_FORMAT_BGRA1010102;
	case SPA_VIDEO_FORMAT_RGB:
		return DRM_FORMAT_BGR888;
	case SPA_VIDEO_FORMAT_BGR:
		return DRM_FORMAT_RGB888;
	default:
		return DRM_FORMAT_INVALID;
	}
}
enum spa_video_format xdpw_format_pw_from_drm_fourcc(uint32_t format) {
	switch (format) {
	case DRM_FORMAT_ARGB8888:
		return SPA_VIDEO_FORMAT_BGRA;
	case DRM_FORMAT_XRGB8888:
		return SPA_VIDEO_FORMAT_BGRx;
	case DRM_FORMAT_RGBA8888:
		return SPA_VIDEO_FORMAT_ABGR;
	case DRM_FORMAT_RGBX8888:
		return SPA_VIDEO_FORMAT_xBGR;
	case DRM_FORMAT_ABGR8888:
		return SPA_VIDEO_FORMAT_RGBA;
	case DRM_FORMAT_XBGR8888:
		return SPA_VIDEO_FORMAT_RGBx;
	case DRM_FORMAT_BGRA8888:
		return SPA_VIDEO_FORMAT_ARGB;
	case DRM_FORMAT_BGRX8888:
		return SPA_VIDEO_FORMAT_xRGB;
	case DRM_FORMAT_NV12:
		return SPA_VIDEO_FORMAT_NV12;
	case DRM_FORMAT_XRGB2101010:
		return SPA_VIDEO_FORMAT_xRGB_210LE;
	case DRM_FORMAT_XBGR2101010:
		return SPA_VIDEO_FORMAT_xBGR_210LE;
	case DRM_FORMAT_RGBX1010102:
		return SPA_VIDEO_FORMAT_RGBx_102LE;
	case DRM_FORMAT_BGRX1010102:
		return SPA_VIDEO_FORMAT_BGRx_102LE;
	case DRM_FORMAT_ARGB2101010:
		return SPA_VIDEO_FORMAT_ARGB_210LE;
	case DRM_FORMAT_ABGR2101010:
		return SPA_VIDEO_FORMAT_ABGR_210LE;
	case DRM_FORMAT_RGBA1010102:
		return SPA_VIDEO_FORMAT_RGBA_102LE;
	case DRM_FORMAT_BGRA1010102:
		return SPA_VIDEO_FORMAT_BGRA_102LE;
	case DRM_FORMAT_BGR888:
		return SPA_VIDEO_FORMAT_RGB;
	case DRM_FORMAT_RGB888:
		return SPA_VIDEO_FORMAT_BGR;
	default:
		return SPA_VIDEO_FORMAT_UNKNOWN;
	}
}
enum spa_video_format xdpw_format_pw_strip_alpha(enum spa_video_format format) {
	switch (format) {
	case SPA_VIDEO_FORMAT_BGRA:
		return SPA_VIDEO_FORMAT_BGRx;
	case SPA_VIDEO_FORMAT_ABGR:
		return SPA_VIDEO_FORMAT_xBGR;
	case SPA_VIDEO_FORMAT_RGBA:
		return SPA_VIDEO_FORMAT_RGBx;
	case SPA_VIDEO_FORMAT_ARGB:
		return SPA_VIDEO_FORMAT_xRGB;
	case SPA_VIDEO_FORMAT_ARGB_210LE:
		return SPA_VIDEO_FORMAT_xRGB_210LE;
	case SPA_VIDEO_FORMAT_ABGR_210LE:
		return SPA_VIDEO_FORMAT_xBGR_210LE;
	case SPA_VIDEO_FORMAT_RGBA_102LE:
		return SPA_VIDEO_FORMAT_RGBx_102LE;
	case SPA_VIDEO_FORMAT_BGRA_102LE:
		return SPA_VIDEO_FORMAT_BGRx_102LE;
	default:
		return SPA_VIDEO_FORMAT_UNKNOWN;
	}
}

static void draw_elipse(uint32_t *dst, int width, int height, uint32_t color)
{
	int i, j, r1, r2, r12, r22, r122;

	r1 = width/2;
	r12 = r1 * r1;
	r2 = height/2;
	r22 = r2 * r2;
	r122 = r12 * r22;

	for (i = -r2; i < r2; i++) {
		for (j = -r1; j < r1; j++) {
			dst[(i + r2)*width+(j+r1)] =
				(i * i * r12 + j * j * r22 <= r122) ? color : 0x00000000;
		}
	}
}

static void on_process(void *userdata)
{
	struct portal_instance *data = userdata;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	uint32_t i, j;
	uint8_t *p;
	struct spa_meta *m;
	struct spa_meta_header *h;
	struct spa_meta_region *mc;
	struct spa_meta_cursor *mcs;

	if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
		pw_log_warn("out of buffers: %m");
		return;
	}

	buf = b->buffer;

	if ((h = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h)))) {
#if 1
		h->pts = pw_stream_get_nsec(data->stream);
#else
		h->pts = -1;
#endif
		h->flags = 0;
		h->seq = data->seq++;
		h->dts_offset = 0;
	}
	
	if ((mcs = spa_buffer_find_meta_data(buf, SPA_META_Cursor, sizeof(struct spa_meta_cursor)))) {
		if (cursor.pixels) {
			struct spa_meta_bitmap *mb;
			
			mcs->id = 1;
			mcs->position.x = cursor.x;
			mcs->position.y = cursor.y;
			mcs->hotspot.x = cursor.hot_x;
			mcs->hotspot.y = cursor.hot_y;
			mcs->bitmap_offset = 0;
			
			mb = SPA_PTROFF(mcs, mcs->bitmap_offset, struct spa_meta_bitmap);
			
			mb->format = SPA_VIDEO_FORMAT_ARGB;
			mb->offset = sizeof(struct spa_meta_bitmap);
			mb->size.width = cursor.width;
			mb->size.height = cursor.height;
			mb->stride = mb->size.width * 4;
			
			uint8_t *bitmap_data = SPA_MEMBER(mb, mb->offset, uint8_t);
			memcpy(bitmap_data, cursor.pixels, mb->stride * mb->size.height);
		} else
			mcs->id = 0;
	}


	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->size = data->format.size.height * data->stride;
	buf->datas[0].chunk->stride = data->stride;
	
	buf->datas[0].fd = frame.dma_buf_fd;
	
	pw_stream_queue_buffer(data->stream, b);
}

void update_timer(void) {
	struct timespec timeout, interval;
	
	timeout.tv_sec = 0;
	timeout.tv_nsec = 1;
	interval.tv_sec = 0;
	interval.tv_nsec = 10000000000/framerate;
	
	pw_loop_update_timer((pw_loop),
			timer, &timeout, &interval, false);
}

static void on_timeout(void *userdata, uint64_t expirations)
{
	bool any_streaming = false;
	
	pw_log_trace("timeout");
	
	for (int i = 0; i < MAX_SESSIONS; i++)
		if (instance[i].active && instance[i].state == PW_STREAM_STATE_STREAMING) {
			pw_stream_trigger_process(instance[i].stream);
			any_streaming = true;
		}
	
	//drmtap_grab(ctx, &frame);
	drmtap_cursor_release(ctx, &cursor);
	drmtap_get_cursor(ctx, &cursor);
}

static uint64_t read_object_serial(struct pw_stream *stream) {
	const struct pw_properties *props = pw_stream_get_properties(stream);
	if (!props) {
		return 0;
	}
	const char *serial_str = pw_properties_get(props, PW_KEY_OBJECT_SERIAL);
	if (!serial_str) {
		return 0;
	}
	char *end = NULL;
	errno = 0;
	uint64_t serial = strtoull(serial_str, &end, 10);
	if (errno != 0 || end == serial_str || end[0] != '\0') {
		return 0;
	}
	return serial;
}

/* when the stream is STREAMING, start the timer at 40ms intervals
 * to produce and push a frame. In other states we PAUSE the timer. */
static void on_stream_state_changed(void *_data, enum pw_stream_state old, enum pw_stream_state state,
				    const char *error)
{
	struct portal_instance *data = _data;
	data->pipewire_serial = read_object_serial(data->stream);
	data->state = state;

	printf("stream state: \"%s\" %s\n", pw_stream_state_as_string(state), error ? error : "");

	switch (state) {
	case PW_STREAM_STATE_ERROR:
	//case PW_STREAM_STATE_UNCONNECTED:
		stop_capture(data);
		break;

	case PW_STREAM_STATE_PAUSED:
		data->node_id = pw_stream_get_node_id(data->stream);
		printf("node id: %d\n", data->node_id);
		break;
	case PW_STREAM_STATE_STREAMING:
	{
		break;
	}
	default:
		break;
	}
}

/* we set the PW_STREAM_FLAG_ALLOC_BUFFERS flag when connecting so we need
 * to provide buffer memory.  */
static void on_stream_add_buffer(void *_data, struct pw_buffer *buffer)
{
	struct portal_instance *data = _data;
	struct spa_buffer *buf = buffer->buffer;
	struct spa_data *d;
	unsigned int seals;

	pw_log_info("add buffer %p", buffer);
	
	d = buf->datas;
	
	for (uint32_t plane = 0; plane < buffer->buffer->n_datas; plane++) {
		d[plane].type = SPA_DATA_DmaBuf;
		d[plane].maxsize = desc.pitches[plane] * desc.height;
		d[plane].mapoffset = 0;
		d[plane].chunk->size = d[plane].maxsize;
		d[plane].chunk->stride = desc.pitches[plane] ;
		d[plane].chunk->offset = desc.offsets[plane];
		d[plane].flags = SPA_DATA_FLAG_READABLE;
		d[plane].fd = desc.dma_buf_fd;
		d[plane].data = NULL;
		if (d[plane].chunk->size == 0)
			d[plane].chunk->size = 9; // This was choosen by a fair d20.
	}
}

/* close the memfd we set on the buffers here */
static void on_stream_remove_buffer(void *_data, struct pw_buffer *buffer)
{
	struct spa_buffer *buf = buffer->buffer;
	struct spa_data *d;

	d = buf->datas;
	pw_log_info("remove buffer %p", buffer);
}

struct spa_pod* build_format(struct spa_pod_builder *b, enum spa_video_format format,
		uint32_t width, uint32_t height, uint32_t framerate,
		uint64_t *modifiers, int modifier_count) {
	struct spa_pod_frame f[2];
	
	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);

	spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
	spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
	
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
		SPA_POD_Rectangle(&SPA_RECTANGLE(width, height)),
		0);
	
	if (modifier_count > 0) {
		int i, c;
		
		spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
		spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
		
		for (i = 0, c = 0; i < modifier_count; i++) {
			spa_pod_builder_long(b, modifiers[i]);
			if (c++ == 0)
				spa_pod_builder_long(b, modifiers[i]);
		}
		spa_pod_builder_pop(b, &f[1]);
	}
	
	// variable framerate
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate,
		SPA_POD_Fraction(&SPA_FRACTION(0, 1)), 0);
	if (framerate > 0) {
		spa_pod_builder_add(b, SPA_FORMAT_VIDEO_maxFramerate,
			SPA_POD_CHOICE_RANGE_Fraction(
				&SPA_FRACTION(framerate, 1),
				&SPA_FRACTION(1, 1),
				&SPA_FRACTION(framerate, 1)),
			0);
	}
	
	return spa_pod_builder_pop(b, &f[0]);
}

void build_formats(struct spa_pod_builder *b, struct portal_instance* data, struct spa_pod * params[5], int* n_params) {
	
	{
		// DMA
		uint64_t mods[1] = {desc.modifier};
		params[(*n_params)++] = build_format(b, xdpw_format_pw_from_drm_fourcc(desc.format), desc.width, desc.height, display.refresh_hz, mods, 1);
	}
}

static struct spa_pod *build_buffer(struct spa_pod_builder *b, uint32_t blocks, uint32_t size,
		uint32_t stride, uint32_t datatype) {
	struct spa_pod_frame f[1];

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
	spa_pod_builder_add(b, SPA_PARAM_BUFFERS_buffers,
			SPA_POD_CHOICE_RANGE_Int(2, 2, 4), 0);
	spa_pod_builder_add(b, SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(blocks), 0);
	if (size > 0) {
		spa_pod_builder_add(b, SPA_PARAM_BUFFERS_size, SPA_POD_Int(size), 0);
	}
	if (stride > 0) {
		spa_pod_builder_add(b, SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride), 0);
	}
	spa_pod_builder_add(b, SPA_PARAM_BUFFERS_align, SPA_POD_Int(16), 0);
	spa_pod_builder_add(b, SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(datatype), 0);
	return spa_pod_builder_pop(b, &f[0]);
}

static struct spa_pod *fixate_format(struct spa_pod_builder *b, enum spa_video_format format,
		uint32_t width, uint32_t height, uint32_t framerate, uint64_t *modifier)
{
	struct spa_pod_frame f[1];

	enum spa_video_format format_without_alpha = xdpw_format_pw_strip_alpha(format);

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
	spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
	/* format */
	if (modifier || format_without_alpha == SPA_VIDEO_FORMAT_UNKNOWN) {
		spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
	} else {
		spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format,
				SPA_POD_CHOICE_ENUM_Id(3, format, format, format_without_alpha), 0);
	}
	/* modifiers */
	if (modifier) {
		// implicit modifier
		spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
		spa_pod_builder_long(b, *modifier);
	}
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
		SPA_POD_Rectangle(&SPA_RECTANGLE(width, height)),
		0);
	// variable framerate
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate,
		SPA_POD_Fraction(&SPA_FRACTION(0, 1)), 0);
	if (framerate > 0) {
		spa_pod_builder_add(b, SPA_FORMAT_VIDEO_maxFramerate,
			SPA_POD_CHOICE_RANGE_Fraction(
				&SPA_FRACTION(framerate, 1),
				&SPA_FRACTION(1, 1),
				&SPA_FRACTION(framerate, 1)),
			0);
	}
	return spa_pod_builder_pop(b, &f[0]);
}

/* Be notified when the stream param changes. We're only looking at the
 * format param.
 *
 * We are now supposed to call pw_stream_update_params() with success or
 * failure, depending on if we can support the format. Because we gave
 * a list of supported formats, this should be ok.
 *
 * As part of pw_stream_update_params() we can provide parameters that
 * will control the buffer memory allocation. This includes the metadata
 * that we would like on our buffer, the size, alignment, etc.
 */
static void
on_stream_param_changed(void *_data, uint32_t id, const struct spa_pod *param)
{
	struct portal_instance *data = _data;
	struct pw_stream *stream = data->stream;
	uint8_t params_buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	struct spa_pod *params[5];
	int n_params = 0;
	int framerate = display.refresh_hz;

	if (param != NULL && (id == SPA_PARAM_Tag || id == SPA_PARAM_PeerCapability)) {
		spa_debug_pod(0, NULL, param);
		return;
	}
	if (param == NULL || id != SPA_PARAM_Format)
		return;

	fprintf(stderr, "got format:\n");
	spa_debug_format(2, NULL, param);

	spa_format_video_raw_parse(param, &data->format);
	if (data->format.max_framerate.denom > 0) {
		framerate = data->format.max_framerate.num / data->format.max_framerate.denom;
	} else {
		framerate = 0;
	}

	
	const struct spa_pod_prop *prop_modifier;
	if ((prop_modifier = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier)) != NULL) {
		uint32_t fourcc = xdpw_format_drm_fourcc_from_pw_format(data->format.format);
		if ((prop_modifier->flags & SPA_POD_PROP_FLAG_DONT_FIXATE) > 0) {
			const struct spa_pod *pod_modifier = &prop_modifier->value;
			
			uint32_t n_modifiers = SPA_POD_CHOICE_N_VALUES(pod_modifier) - 1;
			uint64_t *modifiers = SPA_POD_CHOICE_VALUES(pod_modifier);
			modifiers++;
			
			params[n_params++] = fixate_format(&b, data->format.format,
								display.width, display.height, framerate, &desc.modifier);
			
			build_formats(&b, data, params + n_params, &n_params);
			
			pw_stream_update_params(stream, (const struct spa_pod **)params, n_params);
			
			return;
		}
	}

	data->stride = SPA_ROUND_UP_N(data->format.size.width * BPP, 4);

	params[n_params++] = build_buffer(&b, desc.num_planes, 0, 0, 1<<SPA_DATA_DmaBuf);

	params[n_params++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
		SPA_PARAM_META_size, SPA_POD_Int(
			CURSOR_META_SIZE(cursor.width,cursor.height)));

	params[n_params++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
		
	params[n_params++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoTransform),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_videotransform)));

	pw_stream_update_params(stream, (const struct spa_pod **)params, n_params);
}

static void
on_trigger_done(void *_data)
{
	pw_log_trace("trigger done");
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
	.state_changed = on_stream_state_changed,
	.param_changed = on_stream_param_changed,
	//.trigger_done = on_trigger_done,
	.add_buffer = on_stream_add_buffer,
	.remove_buffer = on_stream_remove_buffer,
};

static void do_quit(void *userdata, int signal_number)
{
	//struct portal_instance *data = userdata;
	pw_loop_signal(pw_loop, false);
}

int start_capture(struct ipc_start_capture_input input, int id)
{
	struct spa_pod *params[5];
	int n_params = 0;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct portal_instance* data = instance + id;
	
	data->active = true;
	data->id = id;

	pw_loop_add_signal((pw_loop), SIGINT, do_quit, data);
	pw_loop_add_signal((pw_loop), SIGTERM, do_quit, data);
	pw_loop_add_signal((pw_loop), SIGHUP, do_quit, data);

	data->stream = pw_stream_new(pw_core, display.name,
		pw_properties_new(
			PW_KEY_MEDIA_CLASS, "Video/Source",
			PW_KEY_NODE_SUPPORTS_REQUEST, "1",
			NULL));

	build_formats(&b, data, params, &n_params);
	

	pw_stream_add_listener(data->stream,
			       &data->stream_listener,
			       &stream_events,
			       data);

	pw_stream_connect(data->stream,
			  PW_DIRECTION_OUTPUT,
			  PW_ID_ANY,
			  PW_STREAM_FLAG_DRIVER |
			  PW_STREAM_FLAG_ALLOC_BUFFERS,
			  (const struct spa_pod**)params, n_params);

	data->node_id = SPA_ID_INVALID;

	return 0;
}

void stop_capture(struct portal_instance* data) {
	if (!data->active) return;
	
	data->active = false;
	
	pw_stream_flush(data->stream, false);
	pw_stream_disconnect(data->stream);
	pw_stream_destroy(data->stream);
	
	instances--;
}




enum LOGLEVEL { QUIET, ERROR, WARN, INFO, DEBUG, TRACE };
void logprint(enum LOGLEVEL level, char *msg, ...) {
	va_list args;
	va_start(args, msg);
	vprintf(msg, args);
	va_end(args);
	printf("\n");
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char* message) {
	// If our pipewire connection drops then we won't be able to actually
	// do a screencast.  Exit the process so someone restarts us and the
	// new xdpw can reconnect to pipewire.
	logprint(ERROR, "pipewire: fatal error event from core");
	exit(1);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static struct spa_hook core_listener;

int xdpw_pwr_context_create(void) {

	logprint(DEBUG, "pipewire: establishing connection to core");

	if (!pw_context) {
		pw_context = pw_context_new(pw_loop, NULL, 0);
		if (!pw_context) {
			logprint(ERROR, "pipewire: failed to create context");
			return -1;
		}
	}

	if (!pw_core) {
		pw_core = pw_context_connect(pw_context, NULL, 0);
		if (!pw_core) {
			logprint(ERROR, "pipewire: couldn't connect to context");
			return -1;
		}

		// Setup a core listener to detect errors / disconnects
		// (i.e. in case the pipewire daemon is restarted).
		spa_zero(core_listener);
		pw_core_add_listener(pw_core, &core_listener, &core_events, NULL);
	}
	return 0;
}

void xdpw_pwr_context_destroy(void) {
	logprint(DEBUG, "pipewire: disconnecting from core");
	
	if (pw_core) {
		pw_core_disconnect(pw_core);
		pw_core = NULL;
	}
	
	if (pw_context) {
		pw_context_destroy(pw_context);
		pw_context = NULL;
	}
}

int start_capture_ipc_event(struct ipc_start_capture_input start_capture_input) {
	struct portal_instance* cast;
	bool first = !ctx;
	int instance_id;
	
	ipc_buffer.id = IPC_START_CAP_OUT;
	ipc_buffer.start_capture_output.ret = -1;
	
	for (instance_id = 0; instance_id < MAX_SESSIONS; instance_id++) {
		if (!instance[instance_id].active) {
			break;
		}
	}
	
	if (instance_id == MAX_SESSIONS)
		goto err;
	
	// First time setup
	if (first) {
		display = start_capture_input.display;
		
		drmtap_config cfg = {0};
		cfg.debug = 0;
		cfg.crtc_id = display.crtc_id;
		ctx = drmtap_open(&cfg);
		if (!ctx)
			goto err;
		
		drmtap_frame_info frame = {0};
		drmtap_grab_desc(ctx, &desc, &frame);
		drmtap_get_cursor(ctx, &cursor);
		
		framerate = display.refresh_hz;
	}
	
	cast = instance + instance_id;
	instances++;
	
	ipc_buffer.start_capture_output.ret = start_capture(start_capture_input, instance_id);
	if (ipc_buffer.start_capture_output.ret < 0) goto err;
	
	while (cast->node_id == SPA_ID_INVALID && cast->state != PW_STREAM_STATE_PAUSED) {
		int ret = pw_loop_iterate(pw_loop, 0);
		if (ret < 0) {
			logprint(ERROR, "pipewire_loop_iterate failed: %s", spa_strerror(ret));
			return ret;
		}
	}
	
	ipc_buffer.start_capture_output.node_id = cast->node_id;
	ipc_buffer.start_capture_output.pipewire_serial = cast->pipewire_serial;
	err:;
	if (write(portal_fd_write, &ipc_buffer, sizeof(ipc_buffer)) != sizeof(ipc_buffer)) {
		printf("IPC IPC_START_CAP_OUT FAILED!\n");
		exit(1);
	}
	
	if (first && ipc_buffer.start_capture_output.ret == 0)
		update_timer();
	
	return ipc_buffer.start_capture_output.ret;
}

#include <mqueue.h>
int main(int argc, char* argv[]) {
	static char fifo_name[60];
	
	if (argc < 2) return -1;
	
	snprintf(fifo_name, 60, "%s-0", argv[1]);
	portal_fd_write = open(fifo_name, O_WRONLY);
	snprintf(fifo_name, 60, "%s-1", argv[1]);
	portal_fd_read = open(fifo_name, O_RDONLY);
	if (portal_fd_write == -1 || portal_fd_read == -1) return -1;
	
	pw_init(NULL, NULL);
	pw_loop = pw_loop_new(NULL);
	pw_loop_enter(pw_loop);
	
	xdpw_pwr_context_create();
	
	timer = pw_loop_add_timer((pw_loop), on_timeout, NULL);
	
	struct pollfd pollfds[] = {
		[EVENT_LOOP_PIPEWIRE] = {
			.fd = pw_loop_get_fd(pw_loop),
			.events = POLLIN,
		},
		[EVENT_LOOP_PORTAL] = {
			.fd = portal_fd_read,
			.events = POLLIN,
		}
	};
	
	while (1) {
		int ret = poll(pollfds, sizeof(pollfds) / sizeof(pollfds[0]), -1);
		if (ret < 0) {
			logprint(ERROR, "poll failed: %s", strerror(errno));
			goto error;
		}
	
		if (pollfds[EVENT_LOOP_PIPEWIRE].revents & POLLHUP) {
			logprint(INFO, "event-loop: disconnected from pipewire");
			break;
		}
		
		if (pollfds[EVENT_LOOP_PIPEWIRE].revents & POLLIN) {
			//logprint(TRACE, "event-loop: got pipewire event");
			ret = pw_loop_iterate(pw_loop, 0);
			if (ret < 0) {
				logprint(ERROR, "pw_loop_iterate failed: %s", spa_strerror(ret));
				goto error;
			}
		}
		
		if (pollfds[EVENT_LOOP_PORTAL].revents & POLLHUP) {
			logprint(INFO, "event-loop: disconnected from portal");
			break;
		}
		
		if (pollfds[EVENT_LOOP_PORTAL].revents & POLLIN) {
			logprint(TRACE, "event-loop: got portal event");
			if (read(portal_fd_read, &ipc_buffer, sizeof(ipc_buffer)) != sizeof(ipc_buffer)) {
				printf("IPC MAIN READ FAILED!\n");
				goto error;
			}
			
			switch ((int)ipc_buffer.id) {
				case IPC_START_CAP_IN:
					start_capture_ipc_event(ipc_buffer.start_capture_input);
					break;
				case IPC_STOP_CAP_IN: {
					bool stopped = false;
					for (int i = 0; i < MAX_SESSIONS; i++) {
						if (instance[i].active && instance[i].node_id == ipc_buffer.stop_capture_input.node_id) {
							stop_capture(instance + i);
							stopped = true;
							break;
						}
					}
					
					if (!stopped)
						printf("UNKNOWN STOPPED!\n");
					
					
					ipc_buffer.id = IPC_STOP_CAP_OUT;
					if (write(portal_fd_write, &ipc_buffer, sizeof(ipc_buffer)) != sizeof(ipc_buffer)) {
						printf("IPC IPC_STOP_CAP_OUT FAILED!\n");
						goto error;
					}
					
					if (!instances)
						goto finish;
					
					break;
				}
			}
		}
	}
	
	error:;
	finish:;
	
	if (ctx) {
		drmtap_frame_release(ctx, &frame);
		drmtap_cursor_release(ctx, &cursor);
		drmtap_close(ctx);
	}
	
	xdpw_pwr_context_destroy();
	
	close(portal_fd_read);
	snprintf(fifo_name, 60, "%s-0", argv[1]);
	unlink(fifo_name);
	close(portal_fd_write);
	snprintf(fifo_name, 60, "%s-1", argv[1]);
	unlink(fifo_name);
	
	return 0;
}

