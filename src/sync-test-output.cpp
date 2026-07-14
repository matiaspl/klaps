/*
OBS Audio Video Sync Dock
Copyright (C) 2023 Norihiro Kamae <norihiro@nagater.net>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <obs-module.h>
#include <inttypes.h>
#include <deque>
#include <list>
#include <stdlib.h>
#include <algorithm>
#include <mutex>
#include <complex>
#include <vector>
#include <chrono>
#include <util/platform.h>
#include "quirc.h"
#include "sync-test-output.hpp"
#include "peak-finder.hpp"

#include "plugin-macros.generated.h"

#define N_CORNERS 4

#define N_AUDIO_SYMBOLS 16
#define N_SYMBOL_BUFFER 20

#define V2_MARKER_MS 80
#define V2_MARKER_F0 500.0f
#define V2_MARKER_F1 2500.0f
#define V2_MARKER_TICK_MS 6
#define V2_MARKER_CHIRP_GAIN 0.72f
#define V2_MARKER_TICK_GAIN 0.28f
#define V2_SYMBOL_MS 70
#define V2_GUARD_MS 60
#define V2_PAYLOAD_SYMBOLS 5
#define V2_DTMF_ROWS 4
#define V2_DTMF_COLS 3
#define V2_PAYLOAD_BASE (V2_DTMF_ROWS * V2_DTMF_COLS)
#define V2_CORRELATION_STEP 4
#define V2_CORRELATION_THRESHOLD 0.34f
#define V2_PAYLOAD_SEARCH_MS 5
#define V2_DUPLICATE_WINDOW_MS 500
#define V2_PAIR_WINDOW_NS 10000000000ULL
#define V2_PACKET_SAMPLES_MAX_SECONDS 4
#define ANALYZER_AUDIO_FRAME_SIZE 1024

/* There are several reason to limit the width and the height.
 * - Since a square of 3/8 QR-code-length is calculated using uint32_t,
 *   the 3/8 of width or height cannot exceed the square root of uint32_t max.
 * - Since a sum of the pixels in a line is accumurated on uint32_t,
 *   the width must be less than 1/255 of uint32_t max.
 *   */
#define MAX_WIDTH_HEIGHT 87378u

struct st_audio_buffer
{
	std::deque<std::pair<int32_t, int32_t>> buffer;

	void push_back(int16_t xr, int16_t xi, size_t length)
	{
		int32_t vr = xr, vi = xi;
		if (buffer.size()) {
			vr += buffer.back().first;
			vi += buffer.back().second;
		}
		buffer.push_back(std::make_pair(vr, vi));

		if (buffer.size() <= length)
			return;

		buffer.pop_front();
	};

	std::pair<int32_t, int32_t> sum(size_t n_from_last)
	{
		if (buffer.size() <= 0)
			return std::make_pair(0, 0);
		if (n_from_last >= buffer.size())
			return buffer[0];
		return buffer[buffer.size() - n_from_last - 1];
	}
};

std::pair<int32_t, int32_t> operator-(std::pair<int32_t, int32_t> a, std::pair<int32_t, int32_t> b)
{
	return std::make_pair(a.first - b.first, a.second - b.second);
}

std::complex<float> int16_to_complex(std::pair<int32_t, int32_t> x)
{
	return std::complex<float>((float)x.first / 32768.0f, (float)x.second / 32768.0f);
}

struct corner_type
{
	uint32_t x, y;
	uint32_t r = 0;
};

struct st_audio_v2_candidate
{
	uint64_t marker_start_sample = 0;
	uint64_t packet_end_sample = 0;
	float score = 0.0f;
};

struct sync_test_output
{
	obs_output_t *context;
	uint32_t detect_mode = SYNC_TEST_DETECT_LEGACY;
	bool packet_callback_registered = false;

	/* Configuration from OBS output context */
	uint32_t video_width = 0, video_height = 0;
	uint32_t video_fps_num = 0, video_fps_den = 0;
	uint32_t video_pixelsize = 0;
	uint32_t video_pixeloffset = 0;
	uint8_t (*video_get_intensity)(const uint8_t *data) = nullptr;

	uint32_t audio_sample_rate = 0;
	size_t audio_channels = 0;

	/* Sync pattern detection from video */
	struct quirc *qr = nullptr;
	uint32_t qr_step;
	struct corner_type qr_corners[N_CORNERS];
	st_qr_data qr_data;

	int64_t video_level_prev = 0;
	uint64_t video_level_prev_ts = 0;
	uint64_t video_marker_min_ts = 0;
	uint64_t video_marker_max_ts = 0;

	/* Sync pattern detection from audio */
	struct st_audio_buffer audio_buffer;
	struct peak_finder audio_marker_finder;
	uint32_t last_audio_index_max = 256;

	std::deque<float> audio_v2_raw_buffer;
	std::deque<uint64_t> audio_v2_raw_ts_buffer;
	uint64_t audio_v2_raw_first_sample = 0;
	uint64_t audio_v2_next_sample = 0;
	uint64_t audio_v2_first_ts = 0;
	std::vector<float> audio_v2_marker;
	std::deque<float> audio_v2_corr_buffer;
	struct peak_finder audio_v2_marker_finder;
	std::list<struct st_audio_v2_candidate> audio_v2_candidates;
	uint64_t audio_v2_last_marker_ts = 0;

	/* Multiplex sync pattern detection result */
	std::list<struct sync_index> sync_indices;

	std::mutex mutex;

	/* Audio pattern information from video to audio */
	uint32_t f = 0;
	uint32_t c = 0;
	uint32_t q_ms = 0;

	uint32_t f_last = 0;
	uint32_t c_last = 0;

	/* Encoder PTS to OBS compositor clock mapping */
	std::mutex clock_mutex;
	bool cts_origin_valid = false;
	uint64_t cts_origin_ns = 0;

	~sync_test_output()
	{
		if (qr)
			quirc_destroy(qr);
	}
};

static void video_marker_found(struct sync_test_output *st, uint64_t timestamp, float score);
static bool st_create_analyzer_encoders(struct sync_test_output *st);
static void st_packet_timing(obs_output_t *output, struct encoder_packet *packet,
			     struct encoder_packet_time *packet_time, void *param);

static const char *st_get_name(void *)
{
	return "sync-test-output";
}

static void *st_create(obs_data_t *settings, obs_output_t *output)
{
	static const char *signals[] = {
		"void video_marker_found(ptr data)",
		"void audio_marker_found(ptr data)",
		"void qrcode_found(int timestamp, int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3)",
		"void sync_found(ptr data)",
		NULL,
	};
	signal_handler_add_array(obs_output_get_signal_handler(output), signals);

	auto *st = new sync_test_output;
	st->context = output;
	if (settings)
		st->detect_mode = (uint32_t)obs_data_get_int(settings, "detect_mode");
	if (!st_create_analyzer_encoders(st)) {
		delete st;
		return nullptr;
	}

	return st;
}

static void st_destroy(void *data)
{
	auto *st = (struct sync_test_output *)data;
	if (st->packet_callback_registered)
		obs_output_remove_packet_callback(st->context, st_packet_timing, st);
	obs_output_set_video_encoder(st->context, nullptr);
	obs_output_set_audio_encoder(st->context, nullptr, 0);
	delete st;
}

static uint8_t get_intensity_10le(const uint8_t *data)
{
	uint16_t v = (data[0] >> 2) | (data[1] << 6);
	return (uint8_t)std::min<uint16_t>(v, 0xFF);
}

static uint32_t ms_to_samples(uint32_t sample_rate, uint32_t ms)
{
	return (uint32_t)util_mul_div64(sample_rate, ms, 1000);
}

static uint64_t samples_to_ns(uint64_t samples, uint32_t sample_rate)
{
	return util_mul_div64(samples, 1000000000ULL, sample_rate);
}

static uint64_t system_epoch_ns()
{
	using namespace std::chrono;
	return (uint64_t)duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

static uint64_t obs_clock_to_epoch_ns(uint64_t timestamp)
{
	const uint64_t now_epoch_ns = system_epoch_ns();
	const uint64_t now_obs_ns = os_gettime_ns();

	if (timestamp <= now_obs_ns) {
		const uint64_t delta = now_obs_ns - timestamp;
		return delta < now_epoch_ns ? now_epoch_ns - delta : 0;
	}

	return now_epoch_ns + (timestamp - now_obs_ns);
}

static bool glass_to_glass_from_qr(struct sync_test_output *st, const struct st_qr_data &qr_data, uint64_t timestamp,
				   uint64_t *source_epoch_ns, uint64_t *video_epoch_ns, int64_t *glass_to_glass_ns)
{
	if (!qr_data.has_ntp_ms)
		return false;

	uint64_t cts_origin_ns;
	{
		std::lock_guard<std::mutex> lock(st->clock_mutex);
		if (!st->cts_origin_valid)
			return false;
		cts_origin_ns = st->cts_origin_ns;
	}

	*source_epoch_ns = qr_data.ntp_ms * 1000000ULL;
	*video_epoch_ns = obs_clock_to_epoch_ns(cts_origin_ns + timestamp);
	*glass_to_glass_ns = (int64_t)*video_epoch_ns - (int64_t)*source_epoch_ns;
	return true;
}

static std::vector<float> make_audio_v2_marker(uint32_t sample_rate)
{
	const uint32_t n = ms_to_samples(sample_rate, V2_MARKER_MS);
	const uint32_t center = n / 2;
	const uint32_t first_count = std::max<uint32_t>(1, center);
	const uint32_t second_count = std::max<uint32_t>(1, n - center);
	const double first_duration = (double)first_count / (double)sample_rate;
	const double second_duration = (double)second_count / (double)sample_rate;
	const double up_slope = ((double)V2_MARKER_F1 - (double)V2_MARKER_F0) / first_duration;
	const double down_slope = ((double)V2_MARKER_F1 - (double)V2_MARKER_F0) / second_duration;
	const double phase_center =
		2.0 * M_PI * ((double)V2_MARKER_F0 * first_duration + 0.5 * up_slope * first_duration * first_duration);
	const double tick_sigma = ((double)V2_MARKER_TICK_MS / 1000.0) / 6.0;
	const double tick_radius = (double)V2_MARKER_TICK_MS / 2000.0;
	std::vector<float> marker;
	marker.reserve(n);
	float peak = 0.0f;

	for (uint32_t i = 0; i < n; i++) {
		double phase;
		if (i < center) {
			const double t = (double)i / (double)sample_rate;
			phase = 2.0 * M_PI * ((double)V2_MARKER_F0 * t + 0.5 * up_slope * t * t);
		}
		else {
			const double t = (double)(i - center) / (double)sample_rate;
			phase = phase_center + 2.0 * M_PI * ((double)V2_MARKER_F1 * t - 0.5 * down_slope * t * t);
		}

		const double window = 0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)std::max<uint32_t>(1, n - 1));
		const double chirp = sin(phase) * window;
		const double center_time = (double)((int64_t)i - (int64_t)center) / (double)sample_rate;
		double tick = 0.0;
		if (fabs(center_time) <= tick_radius) {
			const double x = center_time / tick_sigma;
			tick = (1.0 - x * x) * exp(-0.5 * x * x);
		}

		const float sample = V2_MARKER_CHIRP_GAIN * (float)chirp + V2_MARKER_TICK_GAIN * (float)tick;
		peak = std::max(peak, fabsf(sample));
		marker.push_back(sample);
	}

	if (peak > 0.0f) {
		for (auto &sample : marker)
			sample /= peak;
	}

	return marker;
}

static bool st_start(void *data)
{
	auto *st = (struct sync_test_output *)data;

	const video_t *video = obs_get_video();
	if (!video) {
		blog(LOG_ERROR, "Program video is unavailable");
		return false;
	}
	const audio_t *audio = obs_get_audio();
	if (!audio) {
		blog(LOG_ERROR, "OBS audio is unavailable");
		return false;
	}

	st->video_width = video_output_get_width(video);
	st->video_height = video_output_get_height(video);
	const struct video_output_info *video_info = video_output_get_info(video);
	st->video_fps_num = video_info->fps_num;
	st->video_fps_den = video_info->fps_den;
	if (st->video_width > MAX_WIDTH_HEIGHT || st->video_height > MAX_WIDTH_HEIGHT) {
		blog(LOG_ERROR, "Requested size %ux%u exceeds maximum size %ux%u", st->video_width, st->video_height,
		     MAX_WIDTH_HEIGHT, MAX_WIDTH_HEIGHT);
		return false;
	}

	/* The software analyzer encoder requests I420 regardless of the Program
	 * canvas format, so the detector always receives an 8-bit luma plane. */
	enum video_format video_format = VIDEO_FORMAT_I420;
	switch (video_format) {
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_NV12:
	case VIDEO_FORMAT_I444:
	case VIDEO_FORMAT_I422:
	case VIDEO_FORMAT_I40A:
	case VIDEO_FORMAT_I42A:
	case VIDEO_FORMAT_YUVA:
		st->video_pixelsize = 1;
		st->video_pixeloffset = 0;
		st->video_get_intensity = nullptr;
		break;
	case VIDEO_FORMAT_I010:
		st->video_pixelsize = 2;
		st->video_pixeloffset = 0;
		st->video_get_intensity = get_intensity_10le;
		break;
	case VIDEO_FORMAT_P010:
		st->video_pixelsize = 2;
		st->video_pixeloffset = 1;
		st->video_get_intensity = nullptr;
		break;
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(29, 1, 0)
	case VIDEO_FORMAT_P216:
	case VIDEO_FORMAT_P416:
		st->video_pixelsize = 2;
		st->video_pixeloffset = 1; // little endian
		st->video_get_intensity = nullptr;
		break;
#endif
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
		st->video_pixelsize = 4;
		st->video_pixeloffset = 1; // green channel
		st->video_get_intensity = nullptr;
		break;
	default:
		blog(LOG_ERROR, "unsupported pixel format %d", video_format);
		return false;
	}

	uint32_t qr_width = st->video_width;
	uint32_t qr_height = st->video_height;
	st->qr_step = 1;
	while (qr_width * qr_height > 640 * 480) {
		qr_width /= 2;
		qr_height /= 2;
		st->qr_step *= 2;
	}
	if (!st->qr)
		st->qr = quirc_new();
	if (!st->qr) {
		blog(LOG_ERROR, "failed to create QR code encoding context");
		return false;
	}
	if (quirc_resize(st->qr, qr_width, qr_height) < 0) {
		blog(LOG_ERROR, "failed to set-up QR code encoding context");
		return false;
	}

	st->audio_sample_rate = audio_output_get_sample_rate(audio);
	st->audio_channels = audio_output_get_channels(audio);
	st->audio_v2_marker = make_audio_v2_marker(st->audio_sample_rate);
	st->audio_v2_raw_buffer.clear();
	st->audio_v2_raw_ts_buffer.clear();
	st->audio_v2_corr_buffer.clear();
	st->audio_v2_candidates.clear();
	st->audio_v2_raw_first_sample = 0;
	st->audio_v2_next_sample = 0;
	st->audio_v2_first_ts = 0;
	st->audio_v2_last_marker_ts = 0;
	st->audio_v2_marker_finder = peak_finder();
	st->audio_v2_marker_finder.dumping_range = 1000000000ULL;
	{
		std::lock_guard<std::mutex> lock(st->clock_mutex);
		st->cts_origin_valid = false;
		st->cts_origin_ns = 0;
	}

	if (!obs_output_initialize_encoders(st->context, 0)) {
		blog(LOG_ERROR, "Failed to initialize analyzer encoders");
		return false;
	}

	obs_output_add_packet_callback(st->context, st_packet_timing, st);
	st->packet_callback_registered = true;
	if (!obs_output_begin_data_capture(st->context, 0)) {
		obs_output_remove_packet_callback(st->context, st_packet_timing, st);
		st->packet_callback_registered = false;
		blog(LOG_ERROR, "Failed to start analyzer data capture");
		return false;
	}

	blog(LOG_INFO, "Analyzer started on Program video %ux%u at %u/%u FPS and audio Track 1 at %u Hz",
	     st->video_width, st->video_height, st->video_fps_num, st->video_fps_den, st->audio_sample_rate);

	return true;
}

static void st_stop(void *data, uint64_t)
{
	auto *st = (struct sync_test_output *)data;

	if (st->packet_callback_registered) {
		obs_output_remove_packet_callback(st->context, st_packet_timing, st);
		st->packet_callback_registered = false;
	}
	obs_output_end_data_capture(st->context);
}

template<typename T> T sq(T x)
{
	return x * x;
}

static inline uint32_t diff_u32(uint32_t x, uint32_t y)
{
	if (x < y)
		return y - x;
	else
		return x - y;
}

static inline uint32_t sqrt_u32(uint32_t x)
{
	uint32_t r = 0;
	for (uint32_t b = 1 << 15; b; b >>= 1) {
		if (sq(r | b) <= x)
			r |= b;
	}
	return r;
}

static inline int qrcode_length(const struct corner_type *cc)
{
	auto l02 = hypotf((float)((int)cc[0].x - (int)cc[2].x), (float)((int)cc[0].y - (int)cc[2].y));
	auto l13 = hypotf((float)((int)cc[1].x - (int)cc[3].x), (float)((int)cc[1].y - (int)cc[3].y));
	return (int)((l02 + l13) * (float)(M_SQRT1_2 / 2.0f));
}

static inline void adjust_corners(struct corner_type *cc)
{
	int cx = 0, cy = 0;
	for (int i = 0; i < 4; i++) {
		cx += cc[i].x;
		cy += cc[i].y;
	}

	cx /= 4;
	cy /= 4;
	int r = qrcode_length(cc) / 4;

	// Move (x, y) to center side so that the circles will cover the pattern.
	for (int i = 0; i < 4; i++) {
		cc[i].x = (cc[i].x * 15 + cx * 9) / 24;
		cc[i].y = (cc[i].y * 15 + cy * 9) / 24;
		cc[i].r = r;
	}
}

static void signal_qrcode_found(obs_output_t *ctx, uint64_t timestamp, const struct corner_type *corners)
{
	uint8_t stack[384];
	struct calldata cd;
	calldata_init_fixed(&cd, stack, sizeof(stack));
	auto *sh = obs_output_get_signal_handler(ctx);

	calldata_set_int(&cd, "timestamp", timestamp);
	calldata_set_int(&cd, "x0", corners[0].x);
	calldata_set_int(&cd, "y0", corners[0].y);
	calldata_set_int(&cd, "x1", corners[1].x);
	calldata_set_int(&cd, "y1", corners[1].y);
	calldata_set_int(&cd, "x2", corners[2].x);
	calldata_set_int(&cd, "y2", corners[2].y);
	calldata_set_int(&cd, "x3", corners[3].x);
	calldata_set_int(&cd, "y3", corners[3].y);
	signal_handler_signal(sh, "qrcode_found", &cd);
}

static void st_raw_video_qrcode_decode(struct sync_test_output *st, struct video_data *frame)
{
	int w, h;
	auto qr = st->qr;
	uint8_t *qrbuf = quirc_begin(qr, &w, &h);

	const auto qr_step = st->qr_step;
	const auto pixelsize = st->video_pixelsize * qr_step;
	const uint8_t *linedata = frame->data[0] + frame->linesize[0] * (qr_step / 2);
	auto *ptr = qrbuf;
	for (int y = 0; y < h; y++) {
		const uint8_t *data = linedata + st->video_pixeloffset + st->video_pixelsize * (qr_step / 2);
		if (!st->video_get_intensity) {
			for (int x = 0; x < w; x++) {
				*ptr++ = *data;
				data += pixelsize;
			}
		}
		else {
			for (int x = 0; x < w; x++) {
				*ptr++ = st->video_get_intensity(data);
				data += pixelsize;
			}
		}

		linedata += frame->linesize[0] * qr_step;
	}

	quirc_end(qr);

	int num_codes = quirc_count(qr);

	for (int i = 0; i < num_codes; i++) {
		// (x0, y0): top left
		// (x1, y1): top right
		// (x2, y2): bottom right
		// (x3, y3): bottom left

		struct quirc_code code;
		struct quirc_data data;
		quirc_extract(qr, i, &code);
		auto err = quirc_decode(&code, &data);
		if (err == QUIRC_ERROR_DATA_ECC) {
			quirc_flip(&code);
			err = quirc_decode(&code, &data);
		}

		if (err)
			continue;

		data.payload[QUIRC_MAX_PAYLOAD - 1] = 0;
		if (!st->qr_data.decode((char *)data.payload))
			continue;

		for (int j = 0; j < 4; j++) {
			st->qr_corners[j].x = code.corners[j].x * st->qr_step;
			st->qr_corners[j].y = code.corners[j].y * st->qr_step;
		}

		signal_qrcode_found(st->context, frame->timestamp, st->qr_corners);

		adjust_corners(st->qr_corners);

		if (st->qr_data.f > 0 && st->qr_data.c > 0) {
			std::unique_lock<std::mutex> lock(st->mutex);
			st->f = st->qr_data.f;
			st->c = st->qr_data.c;
			st->q_ms = st->qr_data.q_ms;
		}

		if (st->qr_data.protocol >= 2 && st->qr_data.q_ms > 0)
			st->video_marker_min_ts = frame->timestamp + ((uint64_t)st->qr_data.q_ms * 1000000ULL) / 2;
		else
			st->video_marker_min_ts = 0;
		st->video_marker_max_ts = frame->timestamp + st->qr_data.q_ms * 3 * 1000000;
		st->video_level_prev = 0;
	}
}

static void st_raw_video_find_marker(struct sync_test_output *st, struct video_data *frame)
{
	int64_t sum = 0;

	if (frame->timestamp > st->video_marker_max_ts) {
		st->video_level_prev = 0;
		return;
	}
	if (st->qr_data.protocol >= 2 && frame->timestamp < st->video_marker_min_ts) {
		st->video_level_prev = 0;
		return;
	}

	const uint8_t *linedata = frame->data[0];
	const uint32_t pixelsize = st->video_pixelsize;

	for (size_t i = 0; i < N_CORNERS; i++) {
		const struct corner_type c = st->qr_corners[i];
		if (c.r == 0)
			return;
		uint32_t y0 = c.y > c.r ? c.y - c.r : 0;
		uint32_t y1 = std::min(c.y + c.r, st->video_height);
		uint32_t sq_r = sq(c.r);

		for (uint32_t y = y0; y < y1; y++) {
			uint32_t dx = sqrt_u32(sq_r - sq(diff_u32(y, c.y)));
			uint32_t x0 = c.x > dx ? c.x - dx : 0;
			uint32_t x1 = std::min(c.x + dx, st->video_width);

			const uint8_t *data =
				linedata + frame->linesize[0] * y + st->video_pixeloffset + st->video_pixelsize * x0;

			uint32_t line_sum = 0;

			if (!st->video_get_intensity) {
				for (uint32_t x = x0; x < x1; x++) {
					line_sum += *data;
					data += pixelsize;
				}
			}
			else {
				for (uint32_t x = x0; x < x1; x++) {
					line_sum += st->video_get_intensity(data);
					data += pixelsize;
				}
			}

			if (i & 1)
				sum += line_sum;
			else
				sum -= line_sum;
		}
	}

	// blog(LOG_INFO, "st_raw_video-plot: %.03f %f", frame->timestamp * 1e-9, (double)sum / (255.0 * M_PI * sq(st->qr_corners[0].r)));

	if (st->qr_data.valid && st->video_level_prev < 0 && sum >= 0) {
		uint64_t t = frame->timestamp - st->video_level_prev_ts;
		uint64_t add;
		if (st->qr_data.protocol >= 2) {
			/* v2 polarity-step: report the linear zero-cross. The audio side
			 * reports the sample-accurate marker-center, so video must use the
			 * same "requested event time" semantic; the +T/2 below biases v2
			 * by half a frame. */
			const int64_t denom = sum - st->video_level_prev;
			add = denom > 0 ? util_mul_div64(t, (uint64_t)(-st->video_level_prev), (uint64_t)denom) : t / 2;
		}
		else {
			/* v1 single-frame pulse: the time half frame later than the zero-cross. */
			add = util_mul_div64(t, sum - st->video_level_prev * 3, (sum - st->video_level_prev) * 2);
		}
		video_marker_found(st, st->video_level_prev_ts + add, (float)(sum - st->video_level_prev));
	}
	st->video_level_prev = sum;
	st->video_level_prev_ts = frame->timestamp;
}

static bool is_overlapped(uint32_t index, uint32_t index_max, uint32_t next_index)
{
	return index_max && ((index_max + next_index - index) % index_max) > index_max / 2;
}

static bool sync_index_matches(const struct sync_index &si, uint32_t protocol, uint64_t sequence)
{
	return si.protocol == protocol && si.sequence == sequence;
}

static uint64_t abs_diff_u64(uint64_t a, uint64_t b)
{
	return a > b ? a - b : b - a;
}

static uint64_t latest_sync_ts(const struct sync_index &si)
{
	return std::max(si.video_ts, si.audio_ts);
}

static void signal_sync_found(obs_output_t *ctx, const struct sync_index *si)
{
	uint8_t stack[64];
	struct calldata cd;
	calldata_init_fixed(&cd, stack, sizeof(stack));
	auto *sh = obs_output_get_signal_handler(ctx);

	calldata_set_ptr(&cd, "data", const_cast<sync_index *>(si));
	signal_handler_signal(sh, "sync_found", &cd);
}

static void set_sync_glass_to_glass(struct sync_index &si, bool has_glass_to_glass, int64_t glass_to_glass_ns,
				    uint64_t source_epoch_ns, uint64_t video_epoch_ns)
{
	if (!has_glass_to_glass)
		return;

	si.has_glass_to_glass = true;
	si.glass_to_glass_ns = glass_to_glass_ns;
	si.source_epoch_ns = source_epoch_ns;
	si.video_epoch_ns = video_epoch_ns;
}

static void sync_sequence_found(struct sync_test_output *st, uint64_t sequence, uint64_t ts, bool is_video, int index,
				float score, bool has_glass_to_glass = false, int64_t glass_to_glass_ns = 0,
				uint64_t source_epoch_ns = 0, uint64_t video_epoch_ns = 0)
{
	std::unique_lock<std::mutex> lock(st->mutex);

	for (auto it = st->sync_indices.begin(); it != st->sync_indices.end();) {
		if (it->protocol == 2) {
			const uint64_t latest_ts = latest_sync_ts(*it);
			if (latest_ts && latest_ts + V2_PAIR_WINDOW_NS < ts) {
				it = st->sync_indices.erase(it);
				continue;
			}
		}

		if (!sync_index_matches(*it, 2, sequence)) {
			it++;
			continue;
		}

		if ((is_video && it->video_ts) || (!is_video && it->audio_ts)) {
			const uint64_t side_ts = is_video ? it->video_ts : it->audio_ts;
			if (abs_diff_u64(side_ts, ts) <= V2_PAIR_WINDOW_NS)
				return;

			st->sync_indices.erase(it);
			break;
		}

		const uint64_t other_ts = is_video ? it->audio_ts : it->video_ts;
		if (other_ts && abs_diff_u64(other_ts, ts) > V2_PAIR_WINDOW_NS) {
			st->sync_indices.erase(it);
			break;
		}

		(is_video ? it->video_ts : it->audio_ts) = ts;
		(is_video ? it->video_score : it->audio_score) = score;
		if (is_video)
			set_sync_glass_to_glass(*it, has_glass_to_glass, glass_to_glass_ns, source_epoch_ns,
						video_epoch_ns);
		signal_sync_found(st->context, &*it);
		return;
	}

	while (st->sync_indices.size() >= 256)
		st->sync_indices.erase(st->sync_indices.begin());

	auto &ref = st->sync_indices.emplace_back();
	ref.protocol = 2;
	ref.sequence = sequence;
	ref.index = index;
	ref.index_max = 0;
	(is_video ? ref.video_ts : ref.audio_ts) = ts;
	(is_video ? ref.video_score : ref.audio_score) = score;
	if (is_video)
		set_sync_glass_to_glass(ref, has_glass_to_glass, glass_to_glass_ns, source_epoch_ns, video_epoch_ns);
}

static void sync_index_found(struct sync_test_output *st, int index, uint64_t ts, bool is_video, uint32_t index_max)
{
	std::unique_lock<std::mutex> lock(st->mutex);

	for (auto it = st->sync_indices.begin(); it != st->sync_indices.end();) {
		if (it->protocol != 1) {
			it++;
			continue;
		}

		if ((it->video_ts && is_video) || (it->audio_ts && !is_video)) {
			if (is_overlapped(it->index, it->index_max, index)) {
				st->sync_indices.erase(it++);
				continue;
			}
		}

		if (it->index != index) {
			it++;
			continue;
		}

		if ((it->video_ts && !is_video) || (it->audio_ts && is_video)) {
			(is_video ? it->video_ts : it->audio_ts) = ts;
			if (is_video)
				it->index_max = index_max;

			signal_sync_found(st->context, &*it);

			/* Do not erase `it` so that `identify_audio_index_max` can refer the last found pattern.
			 * Current `it` will be erased at the next call of this function. */
			return;
		}

		/* Remove the old one. Later, insert the new one to the end */
		st->sync_indices.erase(it);
		break;
	}

	while (st->sync_indices.size() >= 128)
		st->sync_indices.erase(st->sync_indices.begin());

	auto &ref = st->sync_indices.emplace_back();
	ref.protocol = 1;
	ref.index = index;
	(is_video ? ref.video_ts : ref.audio_ts) = ts;
	ref.index_max = index_max;
}

static void video_marker_found(struct sync_test_output *st, uint64_t timestamp, float score)
{
	uint8_t stack[64];
	struct calldata cd;
	calldata_init_fixed(&cd, stack, sizeof(stack));
	auto *sh = obs_output_get_signal_handler(st->context);

	struct video_marker_found_s data;
	data.timestamp = timestamp;
	data.score = score;
	data.protocol = st->qr_data.protocol;
	data.sequence = st->qr_data.sequence;
	data.glass_to_glass_ns = 0;
	data.source_epoch_ns = 0;
	data.video_epoch_ns = 0;
	data.has_glass_to_glass = glass_to_glass_from_qr(st, st->qr_data, timestamp, &data.source_epoch_ns,
							 &data.video_epoch_ns, &data.glass_to_glass_ns);
	data.qr_data = st->qr_data;

	calldata_set_ptr(&cd, "data", &data);
	signal_handler_signal(sh, "video_marker_found", &cd);

	if (data.protocol >= 2)
		sync_sequence_found(st, data.sequence, data.timestamp, true, data.qr_data.index, data.score,
				    data.has_glass_to_glass, data.glass_to_glass_ns, data.source_epoch_ns,
				    data.video_epoch_ns);
	else
		sync_index_found(st, data.qr_data.index, data.timestamp, true, data.qr_data.index_max);
}

static void st_raw_video(void *data, struct video_data *frame)
{
	auto *st = (struct sync_test_output *)data;

	if (!st->video_pixelsize)
		return;

	st_raw_video_qrcode_decode(st, frame);
	st_raw_video_find_marker(st, frame);
}

static uint32_t identify_audio_index_max(struct sync_test_output *st, int index)
{
	/* Find `index_max` for video marker that have the biggest index but
	 * the index is less than or equal to the given index.
	 * In other words, find the closest but not future video marker.
	 */

	std::unique_lock<std::mutex> lock(st->mutex);
	uint32_t last_index_max = 256;
	uint32_t cand = st->last_audio_index_max;
	uint32_t cand_diff = 256;

	for (auto it = st->sync_indices.begin(); it != st->sync_indices.end(); it++) {
		if (it->protocol != 1 || !it->video_ts || !it->index_max)
			continue;
		uint32_t diff = (last_index_max + index - it->index) % last_index_max;
		if (diff < cand_diff) {
			cand = it->index_max;
			cand_diff = diff;
		}
		last_index_max = it->index_max;
	}

	return st->last_audio_index_max = cand;
}

static uint32_t crc4_check(uint32_t data, uint32_t size)
{
	uint32_t p = 0x13 << (size - 5);
	while (size > 4) {
		if (data & (1 << (size - 1)))
			data ^= p;
		size--;
		p >>= 1;
	}
	return data;
}

static uint8_t crc8_u8(uint8_t data)
{
	uint8_t crc = data;
	for (int bit = 0; bit < 8; bit++) {
		if (crc & 0x80)
			crc = (uint8_t)((crc << 1) ^ 0x07);
		else
			crc <<= 1;
	}
	return crc;
}

static uint64_t audio_v2_sample_from_ts(struct sync_test_output *st, uint64_t ts)
{
	if (st->audio_v2_raw_ts_buffer.empty())
		return 0;

	for (size_t i = st->audio_v2_raw_ts_buffer.size(); i > 0; i--) {
		if (st->audio_v2_raw_ts_buffer[i - 1] <= ts)
			return st->audio_v2_raw_first_sample + i - 1;
	}

	return st->audio_v2_raw_first_sample;
}

static bool audio_v2_raw_available(struct sync_test_output *st, uint64_t start, uint64_t count)
{
	if (start < st->audio_v2_raw_first_sample)
		return false;
	return start + count <= st->audio_v2_raw_first_sample + st->audio_v2_raw_buffer.size();
}

static bool audio_v2_ts_from_sample(struct sync_test_output *st, uint64_t sample, uint64_t *ts)
{
	if (!audio_v2_raw_available(st, sample, 1))
		return false;

	const size_t offset = (size_t)(sample - st->audio_v2_raw_first_sample);
	if (offset >= st->audio_v2_raw_ts_buffer.size())
		return false;

	*ts = st->audio_v2_raw_ts_buffer[offset];
	return true;
}

static bool audio_v2_ts_from_sample_offset(struct sync_test_output *st, uint64_t sample, double sample_offset,
					   uint64_t *ts)
{
	uint64_t base_ts = 0;
	if (!audio_v2_ts_from_sample(st, sample, &base_ts))
		return false;

	const int64_t offset_ns = (int64_t)llround(sample_offset * 1000000000.0 / (double)st->audio_sample_rate);
	if (offset_ns < 0) {
		const uint64_t delta = (uint64_t)-offset_ns;
		*ts = base_ts > delta ? base_ts - delta : 0;
	}
	else {
		*ts = base_ts + (uint64_t)offset_ns;
	}
	return true;
}

static double audio_v2_goertzel_power(struct sync_test_output *st, uint64_t start, uint32_t count, float freq)
{
	if (!audio_v2_raw_available(st, start, count))
		return -1.0;

	const double omega = 2.0 * M_PI * (double)freq / (double)st->audio_sample_rate;
	const double coeff = 2.0 * cos(omega);
	double q0 = 0.0;
	double q1 = 0.0;
	double q2 = 0.0;
	size_t offset = (size_t)(start - st->audio_v2_raw_first_sample);

	for (uint32_t i = 0; i < count; i++) {
		const double sample = st->audio_v2_raw_buffer[offset + i];
		q0 = coeff * q1 - q2 + sample;
		q2 = q1;
		q1 = q0;
	}

	return q1 * q1 + q2 * q2 - coeff * q1 * q2;
}

static float audio_v2_marker_score_at(struct sync_test_output *st, uint64_t start)
{
	const uint32_t marker_samples = (uint32_t)st->audio_v2_marker.size();
	if (!audio_v2_raw_available(st, start, marker_samples))
		return 0.0f;

	double corr = 0.0;
	double energy = 0.0;
	double ref_energy = 0.0;
	size_t offset = (size_t)(start - st->audio_v2_raw_first_sample);
	for (uint32_t i = 0; i < marker_samples; i++) {
		const double x = st->audio_v2_raw_buffer[offset + i];
		const double ref = st->audio_v2_marker[i];
		corr += x * ref;
		energy += x * x;
		ref_energy += ref * ref;
	}

	if (energy <= 1e-9 || ref_energy <= 1e-9)
		return 0.0f;

	return (float)(fabs(corr) / sqrt(energy * ref_energy));
}

static bool audio_v2_refine_marker_start(struct sync_test_output *st, uint64_t coarse_start, uint64_t *refined_start,
					 double *sample_offset, float *refined_score)
{
	const uint32_t radius = std::max<uint32_t>(2, V2_CORRELATION_STEP);
	const uint64_t scan_start = coarse_start > radius ? coarse_start - radius : 0;
	const uint64_t scan_end = coarse_start + radius;

	uint64_t best_start = coarse_start;
	float best_score = -1.0f;
	for (uint64_t start = scan_start; start <= scan_end; start++) {
		const float score = audio_v2_marker_score_at(st, start);
		if (score > best_score) {
			best_score = score;
			best_start = start;
		}
	}

	double offset = 0.0;
	if (best_start > scan_start && best_start < scan_end) {
		const double y0 = best_score;
		const double ym = audio_v2_marker_score_at(st, best_start - 1);
		const double yp = audio_v2_marker_score_at(st, best_start + 1);
		const double denom = ym - 2.0 * y0 + yp;
		if (fabs(denom) > 1e-12) {
			const double delta = 0.5 * (ym - yp) / denom;
			if (delta >= -1.0 && delta <= 1.0)
				offset = delta;
		}
	}

	*refined_start = best_start;
	*sample_offset = offset;
	*refined_score = best_score;
	return best_score >= 0.0f;
}

static bool audio_v2_decode_payload_at(struct sync_test_output *st, uint64_t payload_start_sample, uint32_t *sequence,
				       float *confidence)
{
	static const float rows[V2_DTMF_ROWS] = {697.0f, 770.0f, 852.0f, 941.0f};
	static const float cols[V2_DTMF_COLS] = {1209.0f, 1336.0f, 1477.0f};
	const uint32_t symbol_samples = ms_to_samples(st->audio_sample_rate, V2_SYMBOL_MS);
	const uint32_t guard_samples = ms_to_samples(st->audio_sample_rate, V2_GUARD_MS);
	const uint32_t symbol_stride = symbol_samples + guard_samples;

	uint64_t payload = 0;
	float min_margin = 1000.0f;

	for (uint32_t symbol = 0; symbol < V2_PAYLOAD_SYMBOLS; symbol++) {
		const uint64_t symbol_start = payload_start_sample + (uint64_t)symbol * symbol_stride;
		double row_power[V2_DTMF_ROWS];
		double col_power[V2_DTMF_COLS];

		for (int i = 0; i < V2_DTMF_ROWS; i++) {
			row_power[i] = audio_v2_goertzel_power(st, symbol_start, symbol_samples, rows[i]);
			if (row_power[i] < 0.0)
				return false;
		}
		for (int i = 0; i < V2_DTMF_COLS; i++) {
			col_power[i] = audio_v2_goertzel_power(st, symbol_start, symbol_samples, cols[i]);
			if (col_power[i] < 0.0)
				return false;
		}

		int row0 = 0, row1 = 1, col0 = 0, col1 = 1;
		for (int i = 1; i < V2_DTMF_ROWS; i++) {
			if (row_power[i] > row_power[row0]) {
				row1 = row0;
				row0 = i;
			}
			else if (row_power[i] > row_power[row1]) {
				row1 = i;
			}
		}

		for (int i = 1; i < V2_DTMF_COLS; i++) {
			if (col_power[i] > col_power[col0]) {
				col1 = col0;
				col0 = i;
			}
			else if (col_power[i] > col_power[col1]) {
				col1 = i;
			}
		}

		const float row_margin = (float)(row_power[row0] / std::max(1e-9, row_power[row1]));
		const float col_margin = (float)(col_power[col0] / std::max(1e-9, col_power[col1]));
		min_margin = std::min(min_margin, std::min(row_margin, col_margin));
		if (row_margin < 1.45f || col_margin < 1.45f)
			return false;

		payload = payload * V2_PAYLOAD_BASE + (uint64_t)(row0 * V2_DTMF_COLS + col0);
	}

	if (payload > 0xFFFF)
		return false;

	const uint8_t code = (uint8_t)(payload >> 8);
	const uint8_t crc = (uint8_t)(payload & 0xFF);
	if (crc != crc8_u8(code))
		return false;

	*sequence = code;
	*confidence = min_margin;
	return true;
}

static bool audio_v2_decode_candidate(struct sync_test_output *st, const struct st_audio_v2_candidate &cand,
				      uint32_t *sequence, uint64_t *event_ts, float *confidence)
{
	const uint32_t marker_samples = (uint32_t)st->audio_v2_marker.size();
	const uint32_t marker_center_samples = marker_samples / 2;
	const uint32_t guard_samples = ms_to_samples(st->audio_sample_rate, V2_GUARD_MS);
	const uint32_t step_samples = std::max<uint32_t>(1, st->audio_sample_rate / 2000);

	uint64_t refined_start = cand.marker_start_sample;
	double sample_offset = 0.0;
	float refined_score = 0.0f;
	if (!audio_v2_refine_marker_start(st, refined_start, &refined_start, &sample_offset, &refined_score) ||
	    refined_score < V2_CORRELATION_THRESHOLD)
		return false;

	uint32_t decoded_sequence = 0;
	float payload_confidence = 0.0f;
	const uint64_t payload_start = refined_start + marker_samples + guard_samples;
	if (!audio_v2_decode_payload_at(st, payload_start, &decoded_sequence, &payload_confidence)) {
		const uint32_t decode_radius = ms_to_samples(st->audio_sample_rate, V2_PAYLOAD_SEARCH_MS);
		bool decoded = false;
		for (uint32_t delta = step_samples; delta <= decode_radius && !decoded; delta += step_samples) {
			for (int direction = -1; direction <= 1; direction += 2) {
				uint64_t candidate_payload_start = 0;
				if (direction < 0) {
					if (payload_start < delta)
						continue;
					candidate_payload_start = payload_start - delta;
				}
				else {
					candidate_payload_start = payload_start + delta;
				}
				if (audio_v2_decode_payload_at(st, candidate_payload_start, &decoded_sequence,
							       &payload_confidence)) {
					decoded = true;
					break;
				}
			}
		}
		if (!decoded)
			return false;
	}

	*sequence = decoded_sequence;
	if (!audio_v2_ts_from_sample_offset(st, refined_start + marker_center_samples, sample_offset, event_ts))
		return false;
	*confidence = std::min(refined_score, payload_confidence);
	return true;
}

static void audio_v2_signal_marker(struct sync_test_output *st, uint32_t sequence, uint64_t timestamp, float score)
{
	uint8_t stack[64];
	struct calldata cd;
	calldata_init_fixed(&cd, stack, sizeof(stack));
	auto *sh = obs_output_get_signal_handler(st->context);

	struct audio_marker_found_s data;
	data.timestamp = timestamp;
	data.index = (int)(sequence & 0xFF);
	data.score = score;
	data.index_max = 0;
	data.protocol = 2;
	data.sequence = sequence;

	calldata_set_ptr(&cd, "data", &data);
	signal_handler_signal(sh, "audio_marker_found", &cd);

	sync_sequence_found(st, sequence, data.timestamp, false, data.index, data.score);
}

static void audio_v2_process_candidates(struct sync_test_output *st)
{
	for (auto it = st->audio_v2_candidates.begin(); it != st->audio_v2_candidates.end();) {
		if (it->marker_start_sample < st->audio_v2_raw_first_sample) {
			st->audio_v2_candidates.erase(it++);
			continue;
		}

		if (st->audio_v2_next_sample < it->packet_end_sample) {
			it++;
			continue;
		}

		uint32_t sequence = 0;
		float confidence = 0.0f;
		uint64_t event_ts = 0;
		if (audio_v2_decode_candidate(st, *it, &sequence, &event_ts, &confidence))
			audio_v2_signal_marker(st, sequence, event_ts, confidence);

		st->audio_v2_candidates.erase(it++);
	}
}

static void audio_v2_push_sample(struct sync_test_output *st, float sample, uint64_t ts)
{
	if (!st->audio_v2_first_ts)
		st->audio_v2_first_ts = ts;

	const uint64_t sample_index = st->audio_v2_next_sample++;
	const size_t raw_max = (size_t)st->audio_sample_rate * V2_PACKET_SAMPLES_MAX_SECONDS;
	st->audio_v2_raw_buffer.push_back(sample);
	st->audio_v2_raw_ts_buffer.push_back(ts);
	while (st->audio_v2_raw_buffer.size() > raw_max) {
		st->audio_v2_raw_buffer.pop_front();
		st->audio_v2_raw_ts_buffer.pop_front();
		st->audio_v2_raw_first_sample++;
	}

	if (st->audio_v2_marker.empty())
		return;

	st->audio_v2_corr_buffer.push_back(sample);
	while (st->audio_v2_corr_buffer.size() > st->audio_v2_marker.size())
		st->audio_v2_corr_buffer.pop_front();

	if (st->audio_v2_corr_buffer.size() < st->audio_v2_marker.size() || sample_index % V2_CORRELATION_STEP != 0) {
		audio_v2_process_candidates(st);
		return;
	}

	double corr = 0.0;
	double energy = 0.0;
	double ref_energy = 0.0;
	for (size_t i = 0; i < st->audio_v2_marker.size(); i++) {
		const double x = st->audio_v2_corr_buffer[i];
		const double ref = st->audio_v2_marker[i];
		corr += x * ref;
		energy += x * x;
		ref_energy += ref * ref;
	}

	float score = 0.0f;
	if (energy > 1e-9 && ref_energy > 1e-9)
		score = (float)(fabs(corr) / sqrt(energy * ref_energy));

	const uint64_t wait_ts = samples_to_ns(st->audio_v2_marker.size() / 2, st->audio_sample_rate);
	if (st->audio_v2_marker_finder.append(score, ts, wait_ts) &&
	    st->audio_v2_marker_finder.last_score >= V2_CORRELATION_THRESHOLD) {
		const uint64_t peak_ts = st->audio_v2_marker_finder.last_ts;
		if (!st->audio_v2_last_marker_ts ||
		    peak_ts > st->audio_v2_last_marker_ts + (uint64_t)V2_DUPLICATE_WINDOW_MS * 1000000ULL) {
			const uint64_t peak_sample = audio_v2_sample_from_ts(st, peak_ts);
			const uint64_t marker_samples = st->audio_v2_marker.size();
			const uint64_t marker_start =
				peak_sample + 1 > marker_samples ? peak_sample + 1 - marker_samples : 0;
			const uint32_t guard_samples = ms_to_samples(st->audio_sample_rate, V2_GUARD_MS);
			const uint32_t symbol_samples = ms_to_samples(st->audio_sample_rate, V2_SYMBOL_MS);
			const uint32_t symbol_stride = symbol_samples + guard_samples;
			const uint32_t payload_search_samples =
				ms_to_samples(st->audio_sample_rate, V2_PAYLOAD_SEARCH_MS);

			struct st_audio_v2_candidate cand;
			cand.marker_start_sample = marker_start;
			cand.packet_end_sample = marker_start + marker_samples + guard_samples +
						 (uint64_t)V2_PAYLOAD_SYMBOLS * symbol_stride + payload_search_samples;
			cand.score = st->audio_v2_marker_finder.last_score;
			st->audio_v2_candidates.push_back(cand);
			st->audio_v2_last_marker_ts = peak_ts;
		}
	}

	audio_v2_process_candidates(st);
}

static inline void st_raw_audio_decode_data(struct sync_test_output *st, std::complex<float> phase, uint64_t ts)
{
	uint32_t symbol_num = st->audio_sample_rate * st->c_last;
	uint32_t symbol_den = st->f_last;

	uint16_t index = 0;
	for (int i = 0; i < 12; i += 2) {
		auto s0 = st->audio_buffer.sum(symbol_num * i / 2 / symbol_den);
		auto s1 = st->audio_buffer.sum(symbol_num * (i / 2 + 1) / symbol_den);
		auto x = int16_to_complex(s0 - s1);
		auto real = (x / phase).real();
		auto imag = (x / phase).imag();
		if (real > 0.0f)
			index |= 1 << i;
		if (imag > 0.0f)
			index |= 2 << i;
	}

	auto crc4 = crc4_check(0xF0000 | index, 20);

	if (crc4 != 0) {
		blog(LOG_DEBUG, "st_raw_audio_decode_data: CRC mismatch: received data=0x%03X crc=0x%X", index, crc4);
		return;
	}

	uint8_t stack[64];
	struct calldata cd;
	calldata_init_fixed(&cd, stack, sizeof(stack));
	auto *sh = obs_output_get_signal_handler(st->context);

	struct audio_marker_found_s data;
	data.timestamp = ts;
	data.index = index >> 4;
	data.score = 0.0f;
	data.index_max = identify_audio_index_max(st, index >> 4);
	data.protocol = 1;
	data.sequence = 0;

	calldata_set_ptr(&cd, "data", &data);
	signal_handler_signal(sh, "audio_marker_found", &cd);

	sync_index_found(st, index >> 4, ts, false, data.index_max);
}

static inline void st_raw_audio_test_preamble(struct sync_test_output *st, uint64_t ts, float v0)
{
	uint32_t f = st->f_last;
	uint32_t c1 = st->c_last / 2;
	uint64_t symbol_ns = util_mul_div64(c1, 1000000000ULL, f);
	size_t buffer_length = (size_t)(st->audio_sample_rate * c1 * N_SYMBOL_BUFFER / f);

	/* Test the preamble pattern 0xF0  */
	auto s0 = st->audio_buffer.sum(0);
	auto s4 = st->audio_buffer.sum(buffer_length * 4 / N_SYMBOL_BUFFER);
	auto s8 = st->audio_buffer.sum(buffer_length * 8 / N_SYMBOL_BUFFER);
	auto s12 = st->audio_buffer.sum(buffer_length * 12 / N_SYMBOL_BUFFER);

	float det8_0 = std::abs(int16_to_complex(s4 - s0) - int16_to_complex(s8 - s4));
	float det12_8 = det8_0 * 0.5f - std::abs(int16_to_complex(s12 - s8));
	float det = det8_0 + det12_8;

	UNUSED_PARAMETER(v0);
	// auto dbg = int16_to_complex(st->audio_buffer.sum(1) - s0);
	// blog(LOG_INFO, "st_raw_audio-plot: %.05f %f %f %f %f", ts * 1e-9, v0, det, dbg.real(), dbg.imag());

	if (st->audio_marker_finder.append(det, ts, symbol_ns * 12)) {
		auto s12 = st->audio_buffer.sum(buffer_length * 12 / N_SYMBOL_BUFFER);
		auto s16 = st->audio_buffer.sum(buffer_length * 16 / N_SYMBOL_BUFFER);
		auto s20 = st->audio_buffer.sum(buffer_length * 20 / N_SYMBOL_BUFFER);

		auto x = int16_to_complex(s16 - s20) - int16_to_complex(s12 - s16);
		x *= std::complex(1.0f, -1.0f);

		ts = st->audio_marker_finder.last_ts - symbol_ns * N_AUDIO_SYMBOLS / 2;

		st_raw_audio_decode_data(st, x / std::abs(x), ts);
	}
}

static void st_raw_audio(void *data, struct audio_data *frames)
{
	auto *st = (struct sync_test_output *)data;

	if (st->detect_mode == SYNC_TEST_DETECT_AV_OFFSET) {
		for (uint32_t i = 0; i < frames->frames; i++) {
			uint64_t ts = frames->timestamp + util_mul_div64(i, 1000000000ULL, st->audio_sample_rate);
			float v0 = ((float *)frames->data[0])[i];
			float v1 = st->audio_channels >= 2 ? ((float *)frames->data[1])[i] : v0;
			audio_v2_push_sample(st, (v0 + v1) * 0.5f, ts);
		}
	}

	std::unique_lock<std::mutex> lock(st->mutex);
	uint32_t f = st->f;
	uint32_t c = st->c;
	uint32_t q_ms = st->q_ms;
	lock.unlock();

	if (f <= 0 || c <= 0)
		return;

	if (f != st->f_last || c != st->c_last) {
		st->f_last = f;
		st->c_last = c;
		st->audio_buffer.buffer.clear();
	}

	if (q_ms > 0)
		st->audio_marker_finder.dumping_range = q_ms * 1000000 * 6 * 2;

	float phase = (frames->timestamp % 1000000000) * (float)(1e-9 * 2 * M_PI * f);
	float phase_step = (float)(2 * M_PI * f) / st->audio_sample_rate;
	size_t buffer_length = (size_t)(st->audio_sample_rate * c * N_SYMBOL_BUFFER / f);

	for (uint32_t i = 0; i < frames->frames; i++) {
		float osc0 = sinf(phase + phase_step * i);
		float osc1 = cosf(phase + phase_step * i);
		uint64_t ts = frames->timestamp + util_mul_div64(i, 1000000000ULL, st->audio_sample_rate);

		float v0 = ((float *)frames->data[0])[i];
		float v1 = st->audio_channels >= 2 ? ((float *)frames->data[1])[i] : 0.0f;
		int16_t vr = (int16_t)((v0 * osc0 - v1 * osc1) * 16383.0f);
		int16_t vi = (int16_t)((v0 * osc1 + v1 * osc0) * 16383.0f);
		st->audio_buffer.push_back(vr, vi, buffer_length);

		if (st->audio_buffer.buffer.size() < buffer_length)
			continue;

		st_raw_audio_test_preamble(st, ts, v0);
	}
}

static uint64_t video_pts_to_ns(const struct sync_test_output *st, int64_t pts)
{
	if (pts <= 0 || !st->video_fps_num)
		return 0;
	return util_mul_div64((uint64_t)pts, 1000000000ULL, st->video_fps_num);
}

static uint64_t audio_pts_to_ns(const struct sync_test_output *st, int64_t pts)
{
	if (pts <= 0 || !st->audio_sample_rate)
		return 0;
	return samples_to_ns((uint64_t)pts, st->audio_sample_rate);
}

static void fill_analyzer_packet(struct encoder_packet *packet, struct encoder_frame *frame, enum obs_encoder_type type)
{
	static uint8_t placeholder;

	packet->data = &placeholder;
	packet->size = sizeof(placeholder);
	packet->pts = frame->pts;
	packet->dts = frame->pts;
	packet->type = type;
	packet->keyframe = type == OBS_ENCODER_VIDEO;
}

static void *analyzer_encoder_create(obs_data_t *settings, obs_encoder_t *)
{
	return (void *)(uintptr_t)obs_data_get_int(settings, "session_ptr");
}

static void analyzer_encoder_destroy(void *) {}

static const char *video_analyzer_encoder_name(void *)
{
	return "AV Sync Video Analyzer";
}

static const char *audio_analyzer_encoder_name(void *)
{
	return "AV Sync Audio Analyzer";
}

static void video_analyzer_get_info(void *, struct video_scale_info *info)
{
	info->format = VIDEO_FORMAT_I420;
}

static void audio_analyzer_get_info(void *, struct audio_convert_info *info)
{
	info->format = AUDIO_FORMAT_FLOAT_PLANAR;
}

static size_t audio_analyzer_frame_size(void *)
{
	return ANALYZER_AUDIO_FRAME_SIZE;
}

static bool video_analyzer_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet,
				  bool *received_packet)
{
	auto *st = (struct sync_test_output *)data;
	if (!st || !frame || !packet || !received_packet)
		return false;

	struct video_data video = {};
	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		video.data[i] = frame->data[i];
		video.linesize[i] = frame->linesize[i];
	}
	video.timestamp = video_pts_to_ns(st, frame->pts);
	st_raw_video(st, &video);

	fill_analyzer_packet(packet, frame, OBS_ENCODER_VIDEO);
	*received_packet = true;
	return true;
}

static bool audio_analyzer_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet,
				  bool *received_packet)
{
	auto *st = (struct sync_test_output *)data;
	if (!st || !frame || !packet || !received_packet)
		return false;

	struct audio_data audio = {};
	for (size_t i = 0; i < MAX_AV_PLANES; i++)
		audio.data[i] = frame->data[i];
	audio.frames = frame->frames;
	audio.timestamp = audio_pts_to_ns(st, frame->pts);
	st_raw_audio(st, &audio);

	fill_analyzer_packet(packet, frame, OBS_ENCODER_AUDIO);
	*received_packet = true;
	return true;
}

static void st_packet_timing(obs_output_t *, struct encoder_packet *packet, struct encoder_packet_time *packet_time,
			     void *param)
{
	auto *st = (struct sync_test_output *)param;
	if (!st || !packet || packet->type != OBS_ENCODER_VIDEO || !packet_time || packet_time->pts < 0)
		return;

	const uint64_t pts_ns = video_pts_to_ns(st, packet_time->pts);
	if (packet_time->cts < pts_ns)
		return;

	std::lock_guard<std::mutex> lock(st->clock_mutex);
	if (st->cts_origin_valid)
		return;

	st->cts_origin_ns = packet_time->cts - pts_ns;
	st->cts_origin_valid = true;
	blog(LOG_INFO, "Analyzer compositor CTS anchor established at %" PRIu64 " ns", st->cts_origin_ns);
}

static void st_encoded_packet(void *, struct encoder_packet *) {}

static bool st_create_analyzer_encoders(struct sync_test_output *st)
{
	video_t *program_video = obs_get_video();
	audio_t *program_audio = obs_get_audio();
	if (!program_video || !program_audio) {
		blog(LOG_ERROR, "Program video or OBS audio is unavailable while creating analyzer encoders");
		return false;
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "session_ptr", (long long)(uintptr_t)st);

	obs_encoder_t *video =
		obs_video_encoder_create(VIDEO_ANALYZER_ENCODER_ID, "av-sync-video-analyzer", settings, nullptr);
	obs_encoder_t *audio =
		obs_audio_encoder_create(AUDIO_ANALYZER_ENCODER_ID, "av-sync-audio-analyzer", settings, 0, nullptr);
	obs_data_release(settings);
	if (!video || !audio) {
		blog(LOG_ERROR, "Failed to create analyzer encoders");
		obs_encoder_release(video);
		obs_encoder_release(audio);
		return false;
	}

	obs_encoder_set_video(video, program_video);
	obs_encoder_set_audio(audio, program_audio);
	obs_output_set_video_encoder(st->context, video);
	obs_output_set_audio_encoder(st->context, audio, 0);
	obs_encoder_release(video);
	obs_encoder_release(audio);
	return true;
}

extern "C" void register_sync_test_analyzer_encoders()
{
	struct obs_encoder_info video = {};
	video.id = VIDEO_ANALYZER_ENCODER_ID;
	video.type = OBS_ENCODER_VIDEO;
	video.codec = "av-sync-analyzer-video";
	video.get_name = video_analyzer_encoder_name;
	video.create = analyzer_encoder_create;
	video.destroy = analyzer_encoder_destroy;
	video.encode = video_analyzer_encode;
	video.get_video_info = video_analyzer_get_info;
	obs_register_encoder(&video);

	struct obs_encoder_info audio = {};
	audio.id = AUDIO_ANALYZER_ENCODER_ID;
	audio.type = OBS_ENCODER_AUDIO;
	audio.codec = "av-sync-analyzer-audio";
	audio.get_name = audio_analyzer_encoder_name;
	audio.create = analyzer_encoder_create;
	audio.destroy = analyzer_encoder_destroy;
	audio.encode = audio_analyzer_encode;
	audio.get_frame_size = audio_analyzer_frame_size;
	audio.get_audio_info = audio_analyzer_get_info;
	obs_register_encoder(&audio);
}

extern "C" void register_sync_test_output()
{
	struct obs_output_info info = {};
	info.id = OUTPUT_ID;
	info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED;
	info.get_name = st_get_name;
	info.create = st_create;
	info.destroy = st_destroy;
	info.start = st_start;
	info.stop = st_stop;
	info.encoded_packet = st_encoded_packet;

	obs_register_output(&info);
}
