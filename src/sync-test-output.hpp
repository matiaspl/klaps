#pragma once

#include <obs-module.h>
#include <inttypes.h>
#include <stdlib.h>

#define SYNC_TEST_DETECT_LEGACY 0
#define SYNC_TEST_DETECT_AV_OFFSET 1
#define SYNC_TEST_NTP_EPOCH_S_MIN 1000000000ULL
#define SYNC_TEST_NTP_EPOCH_MS_MIN 1000000000000ULL

struct st_qr_data
{
	uint32_t protocol = 1;
	uint32_t f = 0;
	uint32_t c = 0;
	uint32_t q_ms = 0;
	uint32_t index = -1;
	uint32_t index_max = 256;
	uint32_t type_flags = 0;
	uint64_t sequence = 0;
	uint64_t ntp_ms = 0;
	uint32_t mode = 0;
	bool has_sequence = false;
	bool has_ntp_ms = false;
	bool valid = 0;

	void reset()
	{
		protocol = 1;
		f = 0;
		c = 0;
		q_ms = 0;
		index = -1;
		index_max = 256;
		type_flags = 0;
		sequence = 0;
		ntp_ms = 0;
		mode = 0;
		has_sequence = false;
		has_ntp_ms = false;
		valid = false;
	}

	void set_ntp_time(uint64_t value)
	{
		if (value >= SYNC_TEST_NTP_EPOCH_MS_MIN) {
			ntp_ms = value;
			has_ntp_ms = true;
		}
		else if (value >= SYNC_TEST_NTP_EPOCH_S_MIN) {
			ntp_ms = value * 1000ULL;
			has_ntp_ms = true;
		}
	}

	void normalize()
	{
		if (protocol >= 2 && has_ntp_ms && sequence >= SYNC_TEST_NTP_EPOCH_S_MIN && index <= 0xFF)
			sequence = index;
	}

	bool _decode_kv(char *param)
	{
		char *saveptr;
		char *key = strtok_r(param, "=", &saveptr);
		if (!key || key[1] != 0)
			return false;

		char *val = strtok_r(NULL, "=", &saveptr);
		if (!val)
			return false;

		switch (key[0]) {
		case 'p':
			protocol = (uint32_t)strtoul(val, nullptr, 10);
			return true;
		case 'f':
			f = (uint32_t)atoi(val);
			return true;
		case 'c':
			c = (uint32_t)atoi(val);
			return true;
		case 'q':
			q_ms = (uint32_t)atoi(val);
			return true;
		case 'i':
			index = (uint32_t)atoi(val);
			return true;
		case 'I':
			index_max = (uint32_t)atoi(val);
			return true;
		case 't':
			type_flags = (uint32_t)atoi(val);
			return true;
		case 's':
			sequence = strtoull(val, nullptr, 10);
			has_sequence = true;
			set_ntp_time(sequence);
			return true;
		case 'n':
		case 'u':
			set_ntp_time(strtoull(val, nullptr, 10));
			return true;
		case 'm':
			mode = (uint32_t)strtoul(val, nullptr, 10);
			return true;
		default:
			/* Ignored */
			return true;
		}

		return false;
	}

	bool check()
	{
		if (protocol < 1 || protocol > 2) {
			blog(LOG_WARNING, "p: out of range: %u", protocol);
			return false;
		}
		if (protocol >= 2 && !has_sequence && !has_ntp_ms) {
			blog(LOG_WARNING, "s: missing sequence");
			return false;
		}
		if (protocol == 1 && (f < 10 || 32000 < f)) {
			blog(LOG_WARNING, "f: out of range: %u", f);
			return false;
		}
		if (protocol == 1 && (c < 1 || f < c)) {
			blog(LOG_WARNING, "c: out of range: %u", c);
			return false;
		}
		if (protocol >= 2 && f > 0 && 32000 < f) {
			blog(LOG_WARNING, "f: out of range: %u", f);
			return false;
		}
		if (protocol >= 2 && c > 0 && f > 0 && f < c) {
			blog(LOG_WARNING, "c: out of range: %u", c);
			return false;
		}
		if (q_ms < 1 || 1000 < q_ms) {
			blog(LOG_WARNING, "q: out of range: %u", q_ms);
			return false;
		}
		if (index & ~0xFF) {
			blog(LOG_WARNING, "i: out of range: %u", index);
			return false;
		}
		return true;
	}

	bool decode(char *payload)
	{
		reset();
		char *saveptr;
		char *param = strtok_r(payload, ",", &saveptr);
		while (param) {
			if (!_decode_kv(param))
				return false;
			param = strtok_r(NULL, ",", &saveptr);
		}
		normalize();
		if (!check())
			return false;
		valid = true;
		return true;
	}
};

struct video_marker_found_s
{
	uint64_t timestamp;
	float score;
	uint32_t protocol;
	uint64_t sequence;
	bool has_glass_to_glass;
	int64_t glass_to_glass_ns;
	uint64_t source_epoch_ns;
	uint64_t video_epoch_ns;
	struct st_qr_data qr_data;
};

struct audio_marker_found_s
{
	uint64_t timestamp;
	int index;
	float score;
	uint32_t index_max;
	uint32_t protocol;
	uint64_t sequence;
};

struct sync_index
{
	int index = -1;
	uint64_t video_ts = 0;
	uint64_t audio_ts = 0;
	uint32_t index_max = 256;
	uint32_t protocol = 1;
	uint64_t sequence = 0;
	float video_score = 0.0f;
	float audio_score = 0.0f;
	bool has_glass_to_glass = false;
	int64_t glass_to_glass_ns = 0;
	uint64_t source_epoch_ns = 0;
	uint64_t video_epoch_ns = 0;
};
