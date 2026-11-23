/*
 * Receives raw line 21 byte pairs from the capture device, decodes them, and
 * publishes the caption text currently on screen to a text file and/or an
 * OBS text source.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <obs-module.h>

#include <cstdint>
#include <mutex>
#include <string>

#include "cea608-decoder.hpp"

struct CaptionSettings {
	bool enabled = true;
	int channel = 1;
	std::string filePath;
	std::string textSource;
	int clearSeconds = 16;
	bool logRaw = false;
};

class CaptionSink {
public:
	explicit CaptionSink(obs_source_t *owner);
	~CaptionSink();

	/* Apply new settings; resets the decoder and clears the output. */
	void Configure(const CaptionSettings &settings);
	bool Enabled();

	/* Called from the DirectShow streaming thread with the CC pin's data. */
	void OnCaptionData(const unsigned char *data, size_t size);

	/* Called regularly (e.g. once per video frame) to handle the
	 * "clear after N seconds" timer and periodic diagnostics. */
	void Tick();

	/* Called when capture stops: clears the published text. */
	void Clear();

private:
	obs_source_t *owner;
	std::mutex mutex;
	CaptionSettings settings;
	Cea608Decoder decoder;
	std::string published;
	bool publishedOnce = false;

	uint64_t lastDataTime = 0;   /* last caption data for our channel */
	uint64_t lastSampleTime = 0; /* last sample of any kind from the CC pin */
	uint64_t lastStatsTime = 0;
	uint64_t lastTickTime = 0;
	bool loggedFirstSample = false;
	bool loggedNoSamples = false;

	struct Stats {
		uint64_t samples = 0;
		uint64_t bytes = 0;
		uint64_t padding = 0;
		uint64_t data = 0;
		uint64_t otherChannel = 0;
		uint64_t parityErrors = 0;
	} stats, totalStats;

	void PublishLocked(bool force = false, bool fileOnly = false);
	void WriteCaptionFile(const std::string &path, const std::string &text);
	void UpdateTextSource(const std::string &name, const std::string &text);
	void LogStatsLocked(uint64_t now);
	const char *Name() const;
};
