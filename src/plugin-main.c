/*
 * OBS Fast Cam  -  video filter that streams frames to another process
 * through shared memory (Windows only).
 *
 * Add it to any source/scene: right click > Filters > Effect Filters >
 * "Fast Cam (shared memory)". Every rendered frame is copied into a named
 * shared-memory block and a named event is signalled, so a reader wakes up
 * the instant a new frame is ready. No DirectShow, no virtual camera, no
 * queue: the reader always gets the newest frame.
 *
 * Memory layout (all little endian), mapping name = <stream name>,
 * event name = <stream name>_evt  (auto-reset):
 *
 *   offset  size  field
 *   0       4     magic 'OBSF' (0x4653424F)
 *   4       4     version (1)
 *   8       8     seq   seqlock counter, odd while the writer is copying
 *   16      4     width
 *   20      4     height
 *   24      4     stride in bytes (width * 4)
 *   28      4     format (0 = BGRA, 8 bit)
 *   32      8     timestamp_ns (OBS os_gettime_ns, QPC based)
 *   40      8     frame_id
 *   48      16    reserved
 *   64      ...   pixels, rows top to bottom
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <windows.h>
#include <stdint.h>
#include <string.h>

OBS_DECLARE_MODULE()

#define SHM_MAX_W 3840
#define SHM_MAX_H 2160
#define SHM_HEADER 64
#define SHM_SIZE ((size_t)SHM_HEADER + (size_t)SHM_MAX_W * SHM_MAX_H * 4)
#define SHM_MAGIC 0x4653424Fu

#define S_NAME "stream_name"
#define S_WIDTH "out_width"
#define S_FPS "fps_limit"
#define DEFAULT_NAME "obs_fast_cam"

struct shm_header {
	uint32_t magic;
	uint32_t version;
	volatile LONG64 seq;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
	uint64_t timestamp_ns;
	uint64_t frame_id;
	uint8_t reserved[16];
};

struct fast_cam {
	obs_source_t *context;

	gs_texrender_t *texrender;
	gs_stagesurf_t *stage;
	uint32_t out_w, out_h;

	int req_width; /* 0 = native size */
	int fps_limit; /* 0 = every OBS frame */
	uint64_t next_ns;
	char name[128];

	HANDLE map;
	HANDLE evt;
	uint8_t *view;
	uint64_t frame_id;
};

static void shm_close(struct fast_cam *f)
{
	if (f->view) {
		UnmapViewOfFile(f->view);
		f->view = NULL;
	}
	if (f->map) {
		CloseHandle(f->map);
		f->map = NULL;
	}
	if (f->evt) {
		CloseHandle(f->evt);
		f->evt = NULL;
	}
}

static void shm_open(struct fast_cam *f)
{
	char evt_name[160];

	shm_close(f);

	f->map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)((uint64_t)SHM_SIZE >> 32),
				    (DWORD)(SHM_SIZE & 0xFFFFFFFFu), f->name);
	if (!f->map) {
		blog(LOG_ERROR, "[fast-cam] CreateFileMapping failed (%lu)", GetLastError());
		return;
	}
	f->view = (uint8_t *)MapViewOfFile(f->map, FILE_MAP_ALL_ACCESS, 0, 0, SHM_SIZE);
	if (!f->view) {
		blog(LOG_ERROR, "[fast-cam] MapViewOfFile failed (%lu)", GetLastError());
		shm_close(f);
		return;
	}
	snprintf(evt_name, sizeof(evt_name), "%s_evt", f->name);
	f->evt = CreateEventA(NULL, FALSE, FALSE, evt_name); /* auto-reset */

	struct shm_header *h = (struct shm_header *)f->view;
	h->magic = SHM_MAGIC;
	h->version = 1;
	h->format = 0;
	blog(LOG_INFO, "[fast-cam] streaming to shared memory '%s'", f->name);
}

static const char *fc_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Fast Cam (shared memory)";
}

static void fc_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_NAME, DEFAULT_NAME);
	obs_data_set_default_int(settings, S_WIDTH, 0);
	obs_data_set_default_int(settings, S_FPS, 0);
}

static obs_properties_t *fc_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_text(p, S_NAME, "Stream name", OBS_TEXT_DEFAULT);
	obs_properties_add_int(p, S_WIDTH, "Output width (0 = source size)", 0, SHM_MAX_W, 1);
	obs_properties_add_int(p, S_FPS, "Max FPS (0 = every OBS frame, can't exceed OBS's own FPS)", 0, 240, 1);
	return p;
}

static void fc_update(void *data, obs_data_t *settings)
{
	struct fast_cam *f = data;
	const char *name = obs_data_get_string(settings, S_NAME);
	if (!name || !*name)
		name = DEFAULT_NAME;

	f->req_width = (int)obs_data_get_int(settings, S_WIDTH);
	f->fps_limit = (int)obs_data_get_int(settings, S_FPS);
	f->next_ns = 0;

	if (!f->view || strcmp(name, f->name) != 0) {
		snprintf(f->name, sizeof(f->name), "%s", name);
		shm_open(f);
	}
}

static void *fc_create(obs_data_t *settings, obs_source_t *source)
{
	struct fast_cam *f = bzalloc(sizeof(struct fast_cam));
	f->context = source;
	fc_update(f, settings);
	return f;
}

static void fc_destroy(void *data)
{
	struct fast_cam *f = data;

	obs_enter_graphics();
	if (f->texrender)
		gs_texrender_destroy(f->texrender);
	if (f->stage)
		gs_stagesurface_destroy(f->stage);
	obs_leave_graphics();

	shm_close(f);
	bfree(f);
}

static void ensure_surfaces(struct fast_cam *f, uint32_t ow, uint32_t oh)
{
	if (!f->texrender)
		f->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	if (!f->stage || f->out_w != ow || f->out_h != oh) {
		if (f->stage)
			gs_stagesurface_destroy(f->stage);
		f->stage = gs_stagesurface_create(ow, oh, GS_BGRA);
		f->out_w = ow;
		f->out_h = oh;
	}
}

static void publish(struct fast_cam *f, const uint8_t *src, uint32_t linesize)
{
	struct shm_header *h = (struct shm_header *)f->view;
	uint32_t stride = f->out_w * 4;
	uint8_t *dst = f->view + SHM_HEADER;

	InterlockedIncrement64(&h->seq); /* odd: writing */

	h->width = f->out_w;
	h->height = f->out_h;
	h->stride = stride;
	h->timestamp_ns = os_gettime_ns();
	h->frame_id = ++f->frame_id;

	if (linesize == stride) {
		memcpy(dst, src, (size_t)stride * f->out_h);
	} else {
		for (uint32_t y = 0; y < f->out_h; y++)
			memcpy(dst + (size_t)y * stride, src + (size_t)y * linesize, stride);
	}

	InterlockedIncrement64(&h->seq); /* even: done */
	if (f->evt)
		SetEvent(f->evt);
}

static void fc_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct fast_cam *f = data;

	obs_source_t *target = obs_filter_get_target(f->context);
	uint32_t w = target ? obs_source_get_base_width(target) : 0;
	uint32_t h = target ? obs_source_get_base_height(target) : 0;

	if (!f->view || !w || !h) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	/* frame cap: skip capture entirely (cheaper) when it's not time yet */
	if (f->fps_limit > 0) {
		const uint64_t interval = 1000000000ULL / (uint64_t)f->fps_limit;
		const uint64_t now = os_gettime_ns();
		if (now + 2000000ULL < f->next_ns) { /* 2 ms slack for OBS tick jitter */
			obs_source_skip_video_filter(f->context);
			return;
		}
		f->next_ns += interval;
		if (f->next_ns + interval < now || f->next_ns == interval)
			f->next_ns = now + interval;
	}

	/* output size: native, or scaled to the requested width */
	uint32_t ow = w, oh = h;
	if (f->req_width > 0 && (uint32_t)f->req_width != w) {
		ow = (uint32_t)f->req_width;
		oh = (uint32_t)(((uint64_t)h * ow + w / 2) / w);
	}
	if (ow > SHM_MAX_W) {
		oh = (uint32_t)((uint64_t)oh * SHM_MAX_W / ow);
		ow = SHM_MAX_W;
	}
	if (oh > SHM_MAX_H) {
		ow = (uint32_t)((uint64_t)ow * SHM_MAX_H / oh);
		oh = SHM_MAX_H;
	}
	if (ow < 2)
		ow = 2;
	if (oh < 2)
		oh = 2;

	ensure_surfaces(f, ow, oh);

	gs_texrender_reset(f->texrender);

	/* draw the source as plain 8-bit sRGB values, no linear conversion */
	const bool prev_linear = gs_set_linear_srgb(false);
	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(f->texrender, ow, oh)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		obs_source_video_render(target);
		gs_texrender_end(f->texrender);
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prev_srgb);
	gs_set_linear_srgb(prev_linear);

	gs_texture_t *tex = gs_texrender_get_texture(f->texrender);
	if (tex) {
		gs_stage_texture(f->stage, tex);

		uint8_t *src = NULL;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(f->stage, &src, &linesize)) {
			publish(f, src, linesize);
			gs_stagesurface_unmap(f->stage);
		}
	}

	/* the filter does not change the picture: draw the source normally */
	obs_source_skip_video_filter(f->context);
}

static struct obs_source_info fast_cam_filter = {
	.id = "fast_cam_shm_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = fc_get_name,
	.create = fc_create,
	.destroy = fc_destroy,
	.update = fc_update,
	.get_defaults = fc_get_defaults,
	.get_properties = fc_get_properties,
	.video_render = fc_render,
};

bool obs_module_load(void)
{
	obs_register_source(&fast_cam_filter);
	return true;
}
