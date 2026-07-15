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
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTimer>
#include <QMainWindow>
#include <algorithm>
#include <cstdlib>
#include <vector>
#include <obs-frontend-api.h>
#include "plugin-macros.generated.h"
#include "sync-test-dock.hpp"

#define ASSERT_THREAD(type)                                                                     \
	do {                                                                                    \
		if (!obs_in_task_thread(type))                                                  \
			blog(LOG_ERROR, "%s: ASSERT_THREAD failed: Expected " #type, __func__); \
	} while (false)

SyncTestDock::SyncTestDock(QWidget *parent) : QFrame(parent)
{
	QVBoxLayout *mainLayout = new QVBoxLayout();
	QGridLayout *topLayout = new QGridLayout();

	int y = 0;

	QHBoxLayout *buttonLayout = new QHBoxLayout();

	startButton = new QPushButton(obs_module_text("Button.Start"), this);
	buttonLayout->addWidget(startButton);
	connect(startButton, &QPushButton::clicked, this, &SyncTestDock::on_start_stop);

	resetButton = new QPushButton(obs_module_text("Button.Reset"), this);
	buttonLayout->addWidget(resetButton);
	connect(resetButton, &QPushButton::clicked, this, &SyncTestDock::on_reset);

	mainLayout->addLayout(buttonLayout);

	QLabel *label;
	label = new QLabel(obs_module_text("Label.Mode"), this);
	topLayout->addWidget(label, y, 0);

	modeSelector = new QComboBox(this);
	modeSelector->addItem(obs_module_text("Mode.AVOffsetClip"), SYNC_TEST_DETECT_AV_OFFSET);
	modeSelector->addItem(obs_module_text("Mode.Legacy"), SYNC_TEST_DETECT_LEGACY);
	topLayout->addWidget(modeSelector, y++, 1);

	latencyLabel = new QLabel(obs_module_text("Label.AVOffset"), this);
	latencyLabel->setProperty("class", "text-large");
	topLayout->addWidget(latencyLabel, y, 0);

	latencyDisplay = new QLabel("-", this);
	latencyDisplay->setObjectName("latencyDisplay");
	latencyDisplay->setProperty("class", "text-large");
	topLayout->addWidget(latencyDisplay, y++, 1);

	latencyPolarity = new QLabel("-", this);
	latencyPolarity->setObjectName("latencyPolarity");
	topLayout->addWidget(latencyPolarity, y++, 1);

	label = new QLabel(obs_module_text("Label.GlassToGlass"), this);
	topLayout->addWidget(label, y, 0);

	glassToGlassDisplay = new QLabel("-", this);
	glassToGlassDisplay->setObjectName("glassToGlassDisplay");
	glassToGlassDisplay->setProperty("class", "text-large");
	topLayout->addWidget(glassToGlassDisplay, y++, 1);

	label = new QLabel(obs_module_text("Label.Index"), this);
	topLayout->addWidget(label, y, 0);

	indexDisplay = new QLabel("-", this);
	indexDisplay->setObjectName("indexDisplay");
	topLayout->addWidget(indexDisplay, y++, 1);

	label = new QLabel(obs_module_text("Label.Frequency"), this);
	topLayout->addWidget(label, y, 0);

	frequencyDisplay = new QLabel("-", this);
	frequencyDisplay->setObjectName("frequencyDisplay");
	topLayout->addWidget(frequencyDisplay, y++, 1);

	label = new QLabel(obs_module_text("Label.VideoIndex"), this);
	topLayout->addWidget(label, y, 0);

	videoIndexDisplay = new QLabel("-", this);
	videoIndexDisplay->setObjectName("videoIndexDisplay");
	topLayout->addWidget(videoIndexDisplay, y++, 1);

	label = new QLabel(obs_module_text("Label.AudioIndex"), this);
	topLayout->addWidget(label, y, 0);

	audioIndexDisplay = new QLabel("-", this);
	audioIndexDisplay->setObjectName("audioIndexDisplay");
	topLayout->addWidget(audioIndexDisplay, y++, 1);

	mainLayout->addLayout(topLayout);
	setLayout(mainLayout);
	reset_analysis_stats(active_detect_mode);
}

SyncTestDock::~SyncTestDock()
{
	if (sync_test) {
		obs_output_stop(sync_test);
		sync_test = nullptr;
	}
}

extern "C" QWidget *create_sync_test_dock()
{
	const auto main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	return static_cast<QWidget *>(new SyncTestDock(main_window));
}

#define CD_TO_LOCAL(type, name, get_func) \
	type name;                        \
	if (!get_func(cd, #name, &name))  \
		return;

void SyncTestDock::cb_video_marker_found(void *param, calldata_t *cd)
{
	auto *dock = (SyncTestDock *)param;

	CD_TO_LOCAL(video_marker_found_s *, data, calldata_get_ptr);
	video_marker_found_s found = *data;

	QMetaObject::invokeMethod(dock, [dock, found]() { dock->on_video_marker_found(found); });
};

void SyncTestDock::cb_audio_marker_found(void *param, calldata_t *cd)
{
	auto *dock = (SyncTestDock *)param;

	CD_TO_LOCAL(audio_marker_found_s *, data, calldata_get_ptr);
	audio_marker_found_s found = *data;

	QMetaObject::invokeMethod(dock, [dock, found]() { dock->on_audio_marker_found(found); });
};

void SyncTestDock::cb_sync_found(void *param, calldata_t *cd)
{
	auto *dock = (SyncTestDock *)param;

	CD_TO_LOCAL(sync_index *, data, calldata_get_ptr);
	sync_index found = *data;

	QMetaObject::invokeMethod(dock, [dock, found]() { dock->on_sync_found(found); });
}

void SyncTestDock::on_start_stop()
{
	if (!sync_test) /* request to start */ {
		OBSDataAutoRelease settings = obs_data_create();
		const int detect_mode = modeSelector ? modeSelector->currentData().toInt() : SYNC_TEST_DETECT_AV_OFFSET;
		active_detect_mode = detect_mode;
		obs_data_set_int(settings, "detect_mode", detect_mode);
		OBSOutputAutoRelease o = obs_output_create(OUTPUT_ID, "sync-test-output", settings, nullptr);
		if (!o) {
			blog(LOG_ERROR, "Failed to create sync-test-output.");
			return;
		}

		reset_analysis_stats(detect_mode);

		auto *sh = obs_output_get_signal_handler(o);
		signal_handler_connect(sh, "video_marker_found", cb_video_marker_found, this);
		signal_handler_connect(sh, "audio_marker_found", cb_audio_marker_found, this);
		signal_handler_connect(sh, "sync_found", cb_sync_found, this);

		bool success = obs_output_start(o);

		if (!success)
			latencyPolarity->setText(obs_module_text("Display.Polarity.Failure"));

		if (startButton)
			startButton->setText(obs_module_text("Button.Stop"));

		sync_test = o;
	}
	else /* request to stop */ {
		obs_output_stop(sync_test);
		sync_test = nullptr;

		if (startButton)
			startButton->setText(obs_module_text("Button.Start"));
	}
}

void SyncTestDock::on_reset()
{
	const int detect_mode =
		sync_test ? active_detect_mode
			  : (modeSelector ? modeSelector->currentData().toInt() : SYNC_TEST_DETECT_AV_OFFSET);
	reset_analysis_stats(detect_mode);
}

void SyncTestDock::reset_analysis_stats(int detect_mode)
{
	last_video_ix = last_audio_ix = -1;
	missed_video_ix = missed_audio_ix = 0;
	received_video_ix = received_audio_ix = 0;
	received_video_index_max = 256;
	received_audio_index_max = 256;
	audio_index_max = 256;
	av_offset_samples.clear();

	if (latencyLabel)
		latencyLabel->setText(obs_module_text(detect_mode == SYNC_TEST_DETECT_AV_OFFSET ? "Label.AVOffset"
												: "Label.Latency"));
	if (latencyDisplay)
		latencyDisplay->setText(QStringLiteral("-"));
	if (latencyPolarity)
		latencyPolarity->setText(QStringLiteral("-"));
	if (glassToGlassDisplay)
		glassToGlassDisplay->setText(QStringLiteral("-"));
	if (indexDisplay)
		indexDisplay->setText(QStringLiteral("-"));
	if (frequencyDisplay)
		frequencyDisplay->setText(QStringLiteral("-"));
	if (videoIndexDisplay)
		videoIndexDisplay->setText(QStringLiteral("-"));
	if (audioIndexDisplay)
		audioIndexDisplay->setText(QStringLiteral("-"));
}

static int missed_markers(int index, int last_index, int max_index)
{
	if (index == last_index + 1 || last_index < 0 || max_index <= 0)
		return 0;
	return (max_index + index - last_index - 1) % max_index;
}

static constexpr int64_t AV_OFFSET_PHASE_RESET_NS = 15000000;

static int64_t median_ns(const std::deque<int64_t> &samples)
{
	std::vector<int64_t> sorted(samples.begin(), samples.end());
	std::sort(sorted.begin(), sorted.end());
	const size_t mid = sorted.size() / 2;
	if (sorted.size() & 1)
		return sorted[mid];
	return (sorted[mid - 1] + sorted[mid]) / 2;
}

static int64_t median_abs_deviation_ns(const std::deque<int64_t> &samples, int64_t median)
{
	std::deque<int64_t> deviations;
	for (int64_t sample : samples)
		deviations.push_back(std::llabs(sample - median));
	return median_ns(deviations);
}

static double ns_to_ms(int64_t ns)
{
	return (double)ns * 1e-6;
}

void SyncTestDock::on_video_marker_found(struct video_marker_found_s data)
{
	if (data.protocol >= 2) {
		frequencyDisplay->setText(obs_module_text("Display.AcousticV2"));
		const uint64_t display_sequence = data.qr_data.has_ntp_ms ? data.qr_data.index : data.sequence;
		videoIndexDisplay->setText(QStringLiteral("%1").arg(display_sequence));
		if (glassToGlassDisplay) {
			if (data.has_glass_to_glass)
				glassToGlassDisplay->setText(
					QStringLiteral("%1 ms").arg(ns_to_ms(data.glass_to_glass_ns), 0, 'f', 1));
			else
				glassToGlassDisplay->setText(QStringLiteral("-"));
		}
		return;
	}

	const int index = data.qr_data.index;
	missed_video_ix += missed_markers(index, last_video_ix, received_video_index_max);
	last_video_ix = index;
	received_video_index_max = data.qr_data.index_max;
	received_video_ix++;
	frequencyDisplay->setText(QStringLiteral("%1 Hz").arg(data.qr_data.f));
	int missed = missed_video_ix * 100 / (received_video_ix + missed_video_ix);
	videoIndexDisplay->setText(QStringLiteral("%1 (%2% missed)").arg(index).arg(missed));
}

void SyncTestDock::on_audio_marker_found(struct audio_marker_found_s data)
{
	if (data.protocol >= 2) {
		audioIndexDisplay->setText(QStringLiteral("%1").arg(data.sequence));
		return;
	}

	const int index = data.index;
	missed_audio_ix += missed_markers(index, last_audio_ix, received_audio_index_max);
	last_audio_ix = index;
	received_audio_index_max = data.index_max;
	received_audio_ix++;
	int missed = missed_audio_ix * 100 / (received_audio_ix + missed_audio_ix);
	audioIndexDisplay->setText(QStringLiteral("%1 (%2% missed)").arg(index).arg(missed));
}

void SyncTestDock::on_sync_found(sync_index data)
{
	int64_t ts = (int64_t)data.audio_ts - (int64_t)data.video_ts;
	int64_t display_ts = ts;
	if (data.protocol >= 2) {
		if (latencyLabel)
			latencyLabel->setText(obs_module_text("Label.AVOffset"));
		if (data.has_glass_to_glass && glassToGlassDisplay)
			glassToGlassDisplay->setText(
				QStringLiteral("%1 ms").arg(ns_to_ms(data.glass_to_glass_ns), 0, 'f', 1));
		if (!av_offset_samples.empty()) {
			const int64_t median = median_ns(av_offset_samples);
			if (std::llabs(ts - median) > AV_OFFSET_PHASE_RESET_NS)
				av_offset_samples.clear();
		}
		av_offset_samples.push_back(ts);
		while (av_offset_samples.size() > 9)
			av_offset_samples.pop_front();
		if (av_offset_samples.size() >= 3) {
			display_ts = median_ns(av_offset_samples);
			const int64_t jitter = median_abs_deviation_ns(av_offset_samples, display_ts);
			latencyDisplay->setText(QStringLiteral("%1 ms (raw %2, +/- %3)")
							.arg(ns_to_ms(display_ts), 0, 'f', 1)
							.arg(ns_to_ms(ts), 0, 'f', 1)
							.arg(ns_to_ms(jitter), 0, 'f', 1));
		}
		else {
			latencyDisplay->setText(QStringLiteral("%1 ms").arg(ns_to_ms(ts), 0, 'f', 1));
		}
		indexDisplay->setText(QStringLiteral("%1").arg(data.sequence));
	}
	else {
		if (latencyLabel)
			latencyLabel->setText(obs_module_text("Label.Latency"));
		latencyDisplay->setText(QStringLiteral("%1 ms").arg(ns_to_ms(ts), 0, 'f', 1));
		indexDisplay->setText(QStringLiteral("%1").arg(data.index));
	}
	if (display_ts > 0)
		latencyPolarity->setText(obs_module_text("Display.Polarity.Positive"));
	else if (display_ts < 0)
		latencyPolarity->setText(obs_module_text("Display.Polarity.Negative"));
}
