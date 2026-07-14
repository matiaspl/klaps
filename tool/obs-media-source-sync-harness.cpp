#include <obs.h>
#include <obs-module.h>
#include <callback/calldata.h>
#include <callback/signal.h>

#include <QApplication>
#include <QEventLoop>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../src/sync-test-output.hpp"

namespace {

constexpr const char *kOutputId = "net.nagater.obs-audio-video-sync-dock.output";
constexpr const char *kVideoAnalyzerEncoderId = "net.nagater.obs-audio-video-sync-dock.video-analyzer";
constexpr const char *kAudioAnalyzerEncoderId = "net.nagater.obs-audio-video-sync-dock.audio-analyzer";

struct Options
{
	std::string media_path = "release/av-offset-pattern-3000.mp4";
	std::string browser_url;
	std::string obs_root = "/Users/mstarzak/work/obs-studio-32.1.2";
	std::string plugin_root = "release-obs-32.1.2/obs-audio-video-sync-dock.plugin";
	int width = 1280;
	int height = 720;
	int fps_num = 30;
	int fps_den = 1;
	int sample_rate = 48000;
	int audio_buffer_ms = 20;
	int seconds = 35;
	int target_events = 0;
	bool looping = true;
	bool hw_decode = false;
	bool defer_media_start = true;
	bool fixed_audio_buffering = true;
	double source_audio_offset_ms = 0.0;
	std::string record_path;
	std::string record_video_encoder = "obs_x264";
	std::string record_audio_encoder = "ffmpeg_pcm_s16le";
	bool trace_markers = false;
};

struct Measurement
{
	uint64_t sequence = 0;
	int64_t offset_ns = 0;
	bool has_glass_to_glass = false;
	int64_t glass_to_glass_ns = 0;
	uint64_t video_ts = 0;
	uint64_t audio_ts = 0;
	float video_score = 0.0f;
	float audio_score = 0.0f;
};

struct HarnessState
{
	std::vector<Measurement> measurements;
	size_t video_markers = 0;
	size_t audio_markers = 0;
	bool trace_markers = false;
};

static void usage(const char *argv0)
{
	std::fprintf(
		stderr,
		"Usage: %s [--media path | --browser-url url] [--seconds n] [--events n] [--fps n[/d]] [--size WxH]\n"
		"          [--obs-root path] [--plugin path] [--no-loop] [--hw-decode]\n"
		"          [--source-audio-offset-ms ms] [--start-media-before-output]\n"
		"          [--audio-buffer-ms ms] [--dynamic-audio-buffering]\n"
		"          [--record path] [--record-video-encoder id|auto] [--record-audio-encoder id]\n"
		"          [--trace-markers]\n",
		argv0);
}

static bool parse_int(const char *text, int *value)
{
	char *end = nullptr;
	long parsed = std::strtol(text, &end, 10);
	if (!text[0] || (end && *end))
		return false;
	*value = (int)parsed;
	return true;
}

static bool parse_double(const char *text, double *value)
{
	char *end = nullptr;
	double parsed = std::strtod(text, &end);
	if (!text[0] || (end && *end))
		return false;
	*value = parsed;
	return true;
}

static bool parse_fps(const char *text, int *num, int *den)
{
	const char *slash = std::strchr(text, '/');
	if (!slash) {
		*den = 1;
		return parse_int(text, num);
	}

	std::string left(text, slash - text);
	std::string right(slash + 1);
	return parse_int(left.c_str(), num) && parse_int(right.c_str(), den) && *den != 0;
}

static bool parse_size(const char *text, int *width, int *height)
{
	const char *x = std::strchr(text, 'x');
	if (!x)
		x = std::strchr(text, 'X');
	if (!x)
		return false;

	std::string left(text, x - text);
	std::string right(x + 1);
	return parse_int(left.c_str(), width) && parse_int(right.c_str(), height);
}

static bool parse_options(int argc, char **argv, Options *options)
{
	for (int i = 1; i < argc; i++) {
		auto need_value = [&](const char *name) -> const char * {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "%s requires a value\n", name);
				return nullptr;
			}
			return argv[++i];
		};

		if (!std::strcmp(argv[i], "--media")) {
			const char *value = need_value(argv[i]);
			if (!value)
				return false;
			options->media_path = value;
		}
		else if (!std::strcmp(argv[i], "--browser-url")) {
			const char *value = need_value(argv[i]);
			if (!value)
				return false;
			options->browser_url = value;
			options->defer_media_start = false;
		}
		else if (!std::strcmp(argv[i], "--obs-root")) {
			const char *value = need_value(argv[i]);
			if (!value)
				return false;
			options->obs_root = value;
		}
		else if (!std::strcmp(argv[i], "--plugin")) {
			const char *value = need_value(argv[i]);
			if (!value)
				return false;
			options->plugin_root = value;
		}
		else if (!std::strcmp(argv[i], "--seconds")) {
			const char *value = need_value(argv[i]);
			if (!value || !parse_int(value, &options->seconds))
				return false;
		}
		else if (!std::strcmp(argv[i], "--events")) {
			const char *value = need_value(argv[i]);
			if (!value || !parse_int(value, &options->target_events))
				return false;
		}
		else if (!std::strcmp(argv[i], "--fps")) {
			const char *value = need_value(argv[i]);
			if (!value || !parse_fps(value, &options->fps_num, &options->fps_den))
				return false;
		}
		else if (!std::strcmp(argv[i], "--size")) {
			const char *value = need_value(argv[i]);
			if (!value || !parse_size(value, &options->width, &options->height))
				return false;
		}
		else if (!std::strcmp(argv[i], "--sample-rate")) {
			const char *value = need_value(argv[i]);
			if (!value || !parse_int(value, &options->sample_rate))
				return false;
		}
		else if (!std::strcmp(argv[i], "--audio-buffer-ms")) {
			const char *value = need_value(argv[i]);
			if (!value || !parse_int(value, &options->audio_buffer_ms))
				return false;
		}
		else if (!std::strcmp(argv[i], "--source-audio-offset-ms")) {
			const char *value = need_value(argv[i]);
			if (!value || !parse_double(value, &options->source_audio_offset_ms))
				return false;
		}
		else if (!std::strcmp(argv[i], "--record")) {
			const char *value = need_value(argv[i]);
			if (!value)
				return false;
			options->record_path = value;
		}
		else if (!std::strcmp(argv[i], "--record-audio-encoder")) {
			const char *value = need_value(argv[i]);
			if (!value)
				return false;
			options->record_audio_encoder = value;
		}
		else if (!std::strcmp(argv[i], "--record-video-encoder")) {
			const char *value = need_value(argv[i]);
			if (!value)
				return false;
			options->record_video_encoder = value;
		}
		else if (!std::strcmp(argv[i], "--no-loop")) {
			options->looping = false;
		}
		else if (!std::strcmp(argv[i], "--hw-decode")) {
			options->hw_decode = true;
		}
		else if (!std::strcmp(argv[i], "--start-media-before-output")) {
			options->defer_media_start = false;
		}
		else if (!std::strcmp(argv[i], "--dynamic-audio-buffering")) {
			options->fixed_audio_buffering = false;
		}
		else if (!std::strcmp(argv[i], "--trace-markers")) {
			options->trace_markers = true;
		}
		else if (!std::strcmp(argv[i], "--disable-gpu")) {
			/* CEF/obs-browser command-line switch; keep it in argv for CefMainArgs. */
		}
		else if (!std::strcmp(argv[i], "--in-process-gpu")) {
			/* CEF/obs-browser command-line switch; keep it in argv for CefMainArgs. */
		}
		else if (!std::strcmp(argv[i], "--single-process")) {
			/* CEF/obs-browser command-line switch; keep it in argv for CefMainArgs. */
		}
		else if (!std::strcmp(argv[i], "--help")) {
			return false;
		}
		else {
			std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
			return false;
		}
	}

	return true;
}

static void on_video_marker_found(void *param, calldata_t *cd)
{
	auto *state = static_cast<HarnessState *>(param);
	auto *marker = static_cast<video_marker_found_s *>(calldata_ptr(cd, "data"));
	if (!marker || marker->protocol < 2)
		return;

	state->video_markers++;
	if (!state->trace_markers)
		return;

	std::printf("VIDEO sequence=%llu ts_ms=%.3f score=%.3f glass_ms=", (unsigned long long)marker->sequence,
		    (double)marker->timestamp / 1000000.0, marker->score);
	if (marker->has_glass_to_glass)
		std::printf("%.3f", (double)marker->glass_to_glass_ns / 1000000.0);
	else
		std::printf("-");
	std::printf("\n");
	std::fflush(stdout);
}

static void on_audio_marker_found(void *param, calldata_t *cd)
{
	auto *state = static_cast<HarnessState *>(param);
	auto *marker = static_cast<audio_marker_found_s *>(calldata_ptr(cd, "data"));
	if (!marker || marker->protocol < 2)
		return;

	state->audio_markers++;
	if (!state->trace_markers)
		return;

	std::printf("AUDIO sequence=%llu ts_ms=%.3f score=%.3f\n", (unsigned long long)marker->sequence,
		    (double)marker->timestamp / 1000000.0, marker->score);
	std::fflush(stdout);
}

static bool use_browser_source(const Options &options)
{
	return !options.browser_url.empty();
}

static obs_data_t *create_media_settings(const Options &options, bool include_file)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_string(settings, "local_file", include_file ? options.media_path.c_str() : "");
	obs_data_set_bool(settings, "looping", options.looping);
	obs_data_set_bool(settings, "restart_on_activate", !options.defer_media_start);
	obs_data_set_bool(settings, "close_when_inactive", false);
	obs_data_set_bool(settings, "hw_decode", options.hw_decode);
	obs_data_set_int(settings, "speed_percent", 100);
	return settings;
}

static obs_data_t *create_browser_settings(const Options &options)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", false);
	obs_data_set_string(settings, "url", options.browser_url.c_str());
	obs_data_set_int(settings, "width", options.width);
	obs_data_set_int(settings, "height", options.height);
	obs_data_set_bool(settings, "fps_custom", true);
	obs_data_set_int(settings, "fps", options.fps_num / std::max(1, options.fps_den));
	obs_data_set_bool(settings, "shutdown", false);
	obs_data_set_bool(settings, "restart_when_active", false);
	obs_data_set_bool(settings, "reroute_audio", true);
	obs_data_set_int(settings, "webpage_control_level", 1);
	return settings;
}

static bool load_module(const std::string &bin_path, const std::string &data_path)
{
	obs_module_t *module = nullptr;
	const int code = obs_open_module(&module, bin_path.c_str(), data_path.c_str());
	if (code != MODULE_SUCCESS) {
		std::fprintf(stderr, "obs_open_module failed: code=%d path=%s\n", code, bin_path.c_str());
		return false;
	}
	if (!obs_init_module(module)) {
		std::fprintf(stderr, "obs_init_module failed: path=%s\n", bin_path.c_str());
		return false;
	}
	return true;
}

static void on_sync_found(void *param, calldata_t *cd)
{
	auto *state = static_cast<HarnessState *>(param);
	auto *sync = static_cast<sync_index *>(calldata_ptr(cd, "data"));
	if (!sync || sync->protocol < 2 || !sync->video_ts || !sync->audio_ts)
		return;

	Measurement measurement;
	measurement.sequence = sync->sequence;
	measurement.video_ts = sync->video_ts;
	measurement.audio_ts = sync->audio_ts;
	measurement.offset_ns = (int64_t)sync->audio_ts - (int64_t)sync->video_ts;
	measurement.has_glass_to_glass = sync->has_glass_to_glass;
	measurement.glass_to_glass_ns = sync->glass_to_glass_ns;
	measurement.video_score = sync->video_score;
	measurement.audio_score = sync->audio_score;
	state->measurements.push_back(measurement);

	std::printf("SYNC sequence=%llu offset_ms=%.3f glass_ms=", (unsigned long long)measurement.sequence,
		    (double)measurement.offset_ns / 1000000.0);
	if (measurement.has_glass_to_glass)
		std::printf("%.3f", (double)measurement.glass_to_glass_ns / 1000000.0);
	else
		std::printf("-");
	std::printf(" video_ts=%llu audio_ts=%llu video_score=%.3f audio_score=%.3f\n",
		    (unsigned long long)measurement.video_ts, (unsigned long long)measurement.audio_ts,
		    measurement.video_score, measurement.audio_score);
	std::fflush(stdout);
}

static double median_ms(std::vector<double> values)
{
	if (values.empty())
		return 0.0;
	std::sort(values.begin(), values.end());
	const size_t mid = values.size() / 2;
	if (values.size() & 1)
		return values[mid];
	return (values[mid - 1] + values[mid]) * 0.5;
}

static void print_summary(const char *label, const std::vector<Measurement> &measurements)
{
	std::vector<double> offsets;
	offsets.reserve(measurements.size());
	for (const auto &measurement : measurements)
		offsets.push_back((double)measurement.offset_ns / 1000000.0);

	if (offsets.empty()) {
		std::printf("%s events=0\n", label);
		return;
	}

	const double median = median_ms(offsets);
	std::vector<double> deviations;
	deviations.reserve(offsets.size());
	for (double offset : offsets)
		deviations.push_back(std::fabs(offset - median));

	const auto minmax = std::minmax_element(offsets.begin(), offsets.end());
	double mean = 0.0;
	for (double offset : offsets)
		mean += offset;
	mean /= (double)offsets.size();

	double variance = 0.0;
	for (double offset : offsets) {
		double delta = offset - mean;
		variance += delta * delta;
	}
	variance /= (double)offsets.size();

	std::printf("%s events=%zu median_ms=%.3f mad_ms=%.3f mean_ms=%.3f stddev_ms=%.3f min_ms=%.3f max_ms=%.3f\n",
		    label, offsets.size(), median, median_ms(deviations), mean, std::sqrt(variance), *minmax.first,
		    *minmax.second);
}

static bool looks_like_loop_wrap(uint64_t previous_sequence, uint64_t sequence)
{
	if (!previous_sequence || sequence > previous_sequence)
		return false;

	/* A drop from 255 to 0 is an 8-bit code rollover, not necessarily a media loop. */
	return previous_sequence - sequence < 128;
}

static void print_summaries(const std::vector<Measurement> &measurements)
{
	print_summary("SUMMARY", measurements);

	std::vector<Measurement> pass;
	std::vector<Measurement> settled;
	uint64_t previous_sequence = 0;
	int pass_index = 1;
	bool saw_loop_wrap = false;

	for (const auto &measurement : measurements) {
		if (!pass.empty() && looks_like_loop_wrap(previous_sequence, measurement.sequence)) {
			char label[64];
			std::snprintf(label, sizeof(label), "SUMMARY_PASS pass=%d", pass_index);
			print_summary(label, pass);
			pass.clear();
			pass_index++;
			saw_loop_wrap = true;
		}

		pass.push_back(measurement);
		if (saw_loop_wrap)
			settled.push_back(measurement);
		previous_sequence = measurement.sequence;
	}

	if (pass_index > 1) {
		char label[64];
		std::snprintf(label, sizeof(label), "SUMMARY_PASS pass=%d", pass_index);
		print_summary(label, pass);
	}

	if (!settled.empty())
		print_summary("SUMMARY_SETTLED", settled);
}

static void stop_output(obs_output_t *output)
{
	if (!output)
		return;

	obs_output_stop(output);

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (obs_output_active(output) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));

	if (obs_output_active(output)) {
		obs_output_force_stop(output);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

static std::string select_video_encoder(const std::string &requested)
{
	if (requested != "auto")
		return requested;

	std::string fallback;
	const char *id = nullptr;
	for (size_t idx = 0; obs_enum_encoder_types(idx, &id); idx++) {
		if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO)
			continue;
		const char *codec = obs_get_encoder_codec(id);
		if (!codec || std::strcmp(codec, "h264"))
			continue;
		if (fallback.empty())
			fallback = id;
		if (std::strstr(id, "apple") || std::strstr(id, "videotoolbox") || std::strstr(id, "vt"))
			return id;
	}
	return fallback;
}

static void print_record_encoders()
{
	const char *id = nullptr;
	for (size_t idx = 0; obs_enum_encoder_types(idx, &id); idx++) {
		const char *codec = obs_get_encoder_codec(id);
		std::printf("ENCODER id=%s type=%s codec=%s\n", id,
			    obs_get_encoder_type(id) == OBS_ENCODER_VIDEO ? "video" : "audio", codec ? codec : "-");
	}
}

static obs_encoder_t *create_video_encoder(const Options &options)
{
	const std::string encoder_id = select_video_encoder(options.record_video_encoder);
	if (encoder_id.empty()) {
		std::fprintf(stderr, "No H.264 video encoder is available\n");
		return nullptr;
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "rate_control", "CBR");
	obs_data_set_int(settings, "bitrate", 12000);
	obs_data_set_int(settings, "keyint_sec", 1);
	obs_data_set_string(settings, "preset", "veryfast");
	obs_data_set_string(settings, "profile", "high");
	obs_encoder_t *encoder = obs_video_encoder_create(encoder_id.c_str(), "record-video", settings, nullptr);
	obs_data_release(settings);
	if (!encoder)
		std::fprintf(stderr, "obs_video_encoder_create(%s) failed\n", encoder_id.c_str());
	else
		obs_encoder_set_video(encoder, obs_get_video());

	return encoder;
}

static obs_encoder_t *create_audio_encoder(const Options &options)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "bitrate", 192);
	obs_encoder_t *encoder =
		obs_audio_encoder_create(options.record_audio_encoder.c_str(), "record-audio", settings, 0, nullptr);
	obs_data_release(settings);
	if (!encoder)
		std::fprintf(stderr, "obs_audio_encoder_create(%s) failed\n", options.record_audio_encoder.c_str());
	else
		obs_encoder_set_audio(encoder, obs_get_audio());
	return encoder;
}

static obs_output_t *create_record_output(const Options &options, obs_encoder_t **video_encoder,
					  obs_encoder_t **audio_encoder)
{
	*video_encoder = create_video_encoder(options);
	*audio_encoder = create_audio_encoder(options);
	if (!*video_encoder || !*audio_encoder)
		return nullptr;

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "path", options.record_path.c_str());
	obs_data_set_bool(settings, "allow_overwrite", true);
	obs_output_t *output = obs_output_create("ffmpeg_muxer", "record-output", settings, nullptr);
	obs_data_release(settings);
	if (!output) {
		std::fprintf(stderr, "obs_output_create(ffmpeg_muxer) failed\n");
		return nullptr;
	}

	obs_output_set_video_encoder(output, *video_encoder);
	obs_output_set_audio_encoder(output, *audio_encoder, 0);
	return output;
}

static bool file_exists(const std::string &path)
{
	FILE *file = std::fopen(path.c_str(), "rb");
	if (!file)
		return false;
	std::fclose(file);
	return true;
}

} // namespace

int main(int argc, char **argv)
{
	Options options;
	if (!parse_options(argc, argv, &options)) {
		usage(argv[0]);
		return 2;
	}

	std::vector<const char *> obs_argv(argv, argv + argc);
	obs_set_cmdline_args(argc, obs_argv.data());

	int qt_argc = 1;
	char *qt_argv[] = {argv[0], nullptr};
	QApplication qt_app(qt_argc, qt_argv);

	const std::string obs_build = options.obs_root + "/build_macos";
	const std::string obs_ffmpeg_plugin = obs_build + "/plugins/obs-ffmpeg/RelWithDebInfo/obs-ffmpeg.plugin";
	const std::string obs_ffmpeg_bin = obs_ffmpeg_plugin + "/Contents/MacOS/obs-ffmpeg";
	const std::string obs_ffmpeg_data = obs_ffmpeg_plugin + "/Contents/Resources";
	const std::string obs_browser_plugin = "/Applications/OBS.app/Contents/PlugIns/obs-browser.plugin";
	const std::string obs_browser_bin = obs_browser_plugin + "/Contents/MacOS/obs-browser";
	const std::string obs_browser_data = obs_browser_plugin + "/Contents/Resources";
	const std::string obs_x264_plugin = "/Applications/OBS.app/Contents/PlugIns/obs-x264.plugin";
	const std::string obs_x264_bin = obs_x264_plugin + "/Contents/MacOS/obs-x264";
	const std::string obs_x264_data = obs_x264_plugin + "/Contents/Resources";
	const std::string mac_vt_plugin = "/Applications/OBS.app/Contents/PlugIns/mac-videotoolbox.plugin";
	const std::string mac_vt_bin = mac_vt_plugin + "/Contents/MacOS/mac-videotoolbox";
	const std::string mac_vt_data = mac_vt_plugin + "/Contents/Resources";
	const std::string sync_plugin_bin = options.plugin_root + "/Contents/MacOS/obs-audio-video-sync-dock";
	const std::string sync_plugin_data = options.plugin_root + "/Contents/Resources";
	const std::string graphics_module = obs_build + "/libobs-metal/RelWithDebInfo/libobs-metal.dylib";

	if (!obs_startup("en-US", "/private/tmp/obs-media-source-sync-harness", nullptr)) {
		std::fprintf(stderr, "obs_startup failed\n");
		return 1;
	}

	obs_video_info video = {};
	video.graphics_module = graphics_module.c_str();
	video.fps_num = (uint32_t)options.fps_num;
	video.fps_den = (uint32_t)options.fps_den;
	video.base_width = (uint32_t)options.width;
	video.base_height = (uint32_t)options.height;
	video.output_width = (uint32_t)options.width;
	video.output_height = (uint32_t)options.height;
	video.output_format = options.record_video_encoder == "auto" ? VIDEO_FORMAT_NV12 : VIDEO_FORMAT_BGRA;
	video.adapter = 0;
	video.gpu_conversion = true;
	video.colorspace = VIDEO_CS_DEFAULT;
	video.range = VIDEO_RANGE_DEFAULT;
	video.scale_type = OBS_SCALE_BICUBIC;

	if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS) {
		std::fprintf(stderr, "obs_reset_video failed\n");
		obs_shutdown();
		return 1;
	}

	obs_audio_info2 audio = {};
	audio.samples_per_sec = (uint32_t)options.sample_rate;
	audio.speakers = SPEAKERS_STEREO;
	audio.max_buffering_ms = options.fixed_audio_buffering ? (uint32_t)options.audio_buffer_ms : 0;
	audio.fixed_buffering = options.fixed_audio_buffering;
	if (!obs_reset_audio2(&audio)) {
		std::fprintf(stderr, "obs_reset_audio failed\n");
		obs_shutdown();
		return 1;
	}

	if (!load_module(obs_ffmpeg_bin, obs_ffmpeg_data)) {
		obs_shutdown();
		return 1;
	}
	if (use_browser_source(options) && !load_module(obs_browser_bin, obs_browser_data)) {
		obs_shutdown();
		return 1;
	}
	if (!options.record_path.empty() && file_exists(obs_x264_bin) && !load_module(obs_x264_bin, obs_x264_data)) {
		obs_shutdown();
		return 1;
	}
	if (!options.record_path.empty() && file_exists(mac_vt_bin) && !load_module(mac_vt_bin, mac_vt_data)) {
		obs_shutdown();
		return 1;
	}
	if (!options.record_path.empty()) {
		obs_post_load_modules();
		print_record_encoders();
	}
	if (!load_module(sync_plugin_bin, sync_plugin_data)) {
		obs_shutdown();
		return 1;
	}

	obs_data_t *source_settings = use_browser_source(options)
					      ? create_browser_settings(options)
					      : create_media_settings(options, !options.defer_media_start);
	if (use_browser_source(options))
		std::printf("BROWSER_AUDIO reroute_audio=true control_audio_via_obs=true\n");
	obs_source_t *test_source = obs_source_create_private(
		use_browser_source(options) ? "browser_source" : "ffmpeg_source",
		use_browser_source(options) ? "av-offset-browser" : "av-offset-media", source_settings);
	obs_data_release(source_settings);
	if (!test_source) {
		std::fprintf(stderr, "obs_source_create(%s) failed\n",
			     use_browser_source(options) ? "browser_source" : "ffmpeg_source");
		obs_shutdown();
		return 1;
	}

	obs_source_set_sync_offset(test_source, (int64_t)std::llround(options.source_audio_offset_ms * 1000000.0));

	obs_scene_t *scene = obs_scene_create_private(use_browser_source(options) ? "browser-source-sync-test"
										  : "media-source-sync-test");
	if (!scene) {
		std::fprintf(stderr, "obs_scene_create failed\n");
		obs_source_release(test_source);
		obs_shutdown();
		return 1;
	}

	obs_sceneitem_t *item = obs_scene_add(scene, test_source);
	if (!item) {
		std::fprintf(stderr, "obs_scene_add failed\n");
		obs_scene_release(scene);
		obs_source_release(test_source);
		obs_shutdown();
		return 1;
	}

	obs_transform_info transform = {};
	transform.pos = {0.0f, 0.0f};
	transform.rot = 0.0f;
	transform.scale = {1.0f, 1.0f};
	transform.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	transform.bounds_type = OBS_BOUNDS_STRETCH;
	transform.bounds = {(float)options.width, (float)options.height};
	transform.bounds_alignment = OBS_ALIGN_CENTER;
	obs_sceneitem_set_info2(item, &transform);

	obs_set_output_source(0, obs_scene_get_source(scene));

	obs_data_t *output_settings = obs_data_create();
	obs_data_set_int(output_settings, "detect_mode", SYNC_TEST_DETECT_AV_OFFSET);
	obs_output_t *output = obs_output_create(kOutputId, "sync-test-output", output_settings, nullptr);
	obs_data_release(output_settings);
	if (!output) {
		std::fprintf(stderr, "obs_output_create(%s) failed\n", kOutputId);
		obs_set_output_source(0, nullptr);
		obs_sceneitem_remove(item);
		obs_scene_release(scene);
		obs_source_release(test_source);
		obs_shutdown();
		return 1;
	}
	obs_encoder_t *analyzer_video = obs_output_get_video_encoder(output);
	obs_encoder_t *analyzer_audio = obs_output_get_audio_encoder(output, 0);
	if (!analyzer_video || !analyzer_audio ||
	    std::strcmp(obs_encoder_get_id(analyzer_video), kVideoAnalyzerEncoderId) ||
	    std::strcmp(obs_encoder_get_id(analyzer_audio), kAudioAnalyzerEncoderId)) {
		std::fprintf(stderr, "sync-test output did not attach the expected analyzer encoders\n");
		obs_output_release(output);
		obs_set_output_source(0, nullptr);
		obs_sceneitem_remove(item);
		obs_scene_release(scene);
		obs_source_release(test_source);
		obs_shutdown();
		return 1;
	}
	std::printf("ANALYZER video=%s audio=%s mixer=1\n", obs_encoder_get_id(analyzer_video),
		    obs_encoder_get_id(analyzer_audio));

	HarnessState state;
	state.trace_markers = options.trace_markers;
	signal_handler_connect(obs_output_get_signal_handler(output), "video_marker_found", on_video_marker_found,
			       &state);
	signal_handler_connect(obs_output_get_signal_handler(output), "audio_marker_found", on_audio_marker_found,
			       &state);
	signal_handler_connect(obs_output_get_signal_handler(output), "sync_found", on_sync_found, &state);

	obs_encoder_t *record_video_encoder = nullptr;
	obs_encoder_t *record_audio_encoder = nullptr;
	obs_output_t *record_output = nullptr;
	if (!options.record_path.empty()) {
		record_output = create_record_output(options, &record_video_encoder, &record_audio_encoder);
		if (!record_output) {
			obs_output_release(output);
			obs_set_output_source(0, nullptr);
			obs_sceneitem_remove(item);
			obs_scene_release(scene);
			obs_source_release(test_source);
			obs_shutdown();
			return 1;
		}

		if (!obs_output_start(record_output)) {
			std::fprintf(stderr, "obs_output_start(record) failed: %s\n",
				     obs_output_get_last_error(record_output));
			obs_output_release(record_output);
			obs_encoder_release(record_video_encoder);
			obs_encoder_release(record_audio_encoder);
			obs_output_release(output);
			obs_set_output_source(0, nullptr);
			obs_sceneitem_remove(item);
			obs_scene_release(scene);
			obs_source_release(test_source);
			obs_shutdown();
			return 1;
		}
		std::printf("RECORD path=%s\n", options.record_path.c_str());
	}

	if (!obs_output_start(output)) {
		std::fprintf(stderr, "obs_output_start failed\n");
		stop_output(record_output);
		obs_output_release(record_output);
		obs_encoder_release(record_video_encoder);
		obs_encoder_release(record_audio_encoder);
		obs_output_release(output);
		obs_set_output_source(0, nullptr);
		obs_sceneitem_remove(item);
		obs_scene_release(scene);
		obs_source_release(test_source);
		obs_shutdown();
		return 1;
	}

	if (!use_browser_source(options) && options.defer_media_start) {
		obs_data_t *start_settings = create_media_settings(options, true);
		obs_source_update(test_source, start_settings);
		obs_data_release(start_settings);
	}
	else if (!use_browser_source(options)) {
		obs_source_media_restart(test_source);
	}

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.seconds);
	while (std::chrono::steady_clock::now() < deadline) {
		if (options.target_events > 0 && (int)state.measurements.size() >= options.target_events)
			break;
		qt_app.processEvents(QEventLoop::AllEvents, 50);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	stop_output(output);
	stop_output(record_output);
	if (!use_browser_source(options))
		obs_source_media_stop(test_source);
	std::this_thread::sleep_for(std::chrono::milliseconds(250));
	std::printf("MARKERS video=%zu audio=%zu sync=%zu\n", state.video_markers, state.audio_markers,
		    state.measurements.size());
	print_summaries(state.measurements);

	signal_handler_disconnect(obs_output_get_signal_handler(output), "sync_found", on_sync_found, &state);
	signal_handler_disconnect(obs_output_get_signal_handler(output), "audio_marker_found", on_audio_marker_found,
				  &state);
	signal_handler_disconnect(obs_output_get_signal_handler(output), "video_marker_found", on_video_marker_found,
				  &state);
	obs_output_release(record_output);
	obs_encoder_release(record_video_encoder);
	obs_encoder_release(record_audio_encoder);
	obs_output_release(output);
	obs_set_output_source(0, nullptr);
	obs_sceneitem_remove(item);
	obs_scene_release(scene);
	obs_source_release(test_source);
	obs_wait_for_destroy_queue();
	obs_shutdown();
	return state.measurements.empty() ? 1 : 0;
}
