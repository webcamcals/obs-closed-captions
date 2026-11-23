/*
 * Receives closed caption data from the capture device (either raw VBI
 * waveforms or the device's own decoded CC byte pairs), decodes it, and
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

#include <climits>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cea608-decoder.hpp"
#include "vbi-slicer.hpp"

enum class CaptionInput : int {
	Vbi = 0,    /* raw VBI pin, decoded by this plugin */
	CcPin = 1,  /* the device's CC pin (device decodes) */
};

struct CaptionSettings {
	bool enabled = true;
	CaptionInput input = CaptionInput::Vbi;
	int channel = 1;
	std::string filePath;
	bool appendToFile = false; /* transcript mode: add finished lines to the end */
	std::string textSource;
	int clearSeconds = 16;
	bool logRaw = false;
	std::string dumpPath; /* where to save a raw VBI sample when logRaw is on */
};

class CaptionSink {
public:
	explicit CaptionSink(obs_source_t *owner);
	~CaptionSink();

	/* Apply new settings; resets the decoders and clears the output. */
	void Configure(const CaptionSettings &settings);

	/* Called from DirectShow streaming threads.  Must return quickly:
	 * while they run, the device can't deliver the next sample.  All slow
	 * work (files, other sources) happens on a worker thread. */
	void OnCaptionData(const unsigned char *data, size_t size, long long startTime, bool hasTime);
	void OnVbiData(const unsigned char *data, size_t size, long long startTime, bool hasTime,
		       const unsigned char *format, size_t formatSize);

	/* Called for every video frame: drives the "clear after N seconds"
	 * timer and periodic diagnostics. */
	void Tick();

	/* Called when capture stops: clears the published text. */
	void Clear();

private:
	obs_source_t *owner;

	/* ---- decoding state (protected by mutex) ---- */
	std::mutex mutex;
	CaptionSettings settings;

	/* One decoder per place caption data can come from: each VBI line of
	 * each field gets its own (key = row * 2 + field), and the CC pin
	 * uses key CC_PIN_KEY.  The one carrying CC1/CC2 is "active". */
	struct Slot {
		Cea608Decoder decoder;
		int64_t score = 0;      /* evidence that this is field 1 (CC1/CC2) */
		uint64_t dataPairs = 0; /* non-empty pairs seen */
		uint64_t found = 0;     /* valid pairs decoded */
		uint64_t runInNoParity = 0;
		int line = 0;
		int field = 0;
	};
	static constexpr int CC_PIN_KEY = -1;
	std::map<int, std::unique_ptr<Slot>> slots;
	int activeKey = CC_PIN_KEY;
	bool haveActive = false;

	Slot &GetSlot(int key);
	void FeedPair(int key, uint8_t b1, uint8_t b2, uint64_t now);
	void ChooseActiveLocked();
	Cea608Decoder *ActiveDecoder();

	/* VBI */
	VbiFormat vbiFormat;
	std::vector<unsigned char> vbiFormatBlock;
	Line21Slicer slicer;
	uint64_t vbiSampleCounter = 0;
	uint64_t fieldIndex = 0;
	long long lastVbiStart = 0;
	bool haveLastVbiStart = false;
	int steadyFieldDeltas = 0;
	bool loggedVbiFormat = false;
	bool warnedNoVbiFormat = false;

	/* raw VBI dump for troubleshooting */
	std::vector<unsigned char> dump;
	uint32_t dumpSamples = 0;
	bool dumpDone = false;
	bool dumpStarted = false;
	void DumpSampleLocked(const unsigned char *data, size_t size, long long startTime, bool hasTime);

	std::string lastText;
	bool haveLastText = false;
	std::vector<std::string> finishedScratch;

	uint64_t lastDataTime = 0;
	uint64_t lastStatsTime = 0;
	uint64_t lastTickTime = 0;
	uint64_t configureTime = 0;
	bool loggedFirstCc = false;
	bool loggedFirstVbi = false;
	bool loggedNoSamples = false;
	long long lastCcStart = 0;
	bool haveLastCcStart = false;

	struct Stats {
		uint64_t videoFrames = 0;
		uint64_t ccSamples = 0;
		uint64_t ccGaps = 0;
		uint64_t ccMissed = 0;
		uint64_t vbiSamples = 0;
		uint64_t vbiLinesWithCc = 0;
		uint64_t activePairs = 0;
		uint64_t activeParityFails = 0;
		uint64_t activeData = 0;
		uint64_t vbiMissedFields = 0;
		long long vbiDeltaMin = LLONG_MAX;
		long long vbiDeltaMax = LLONG_MIN;
	} stats;

	void PublishLocked(bool force = false);
	void LogStatsLocked(uint64_t now);
	void ResetDecodingLocked();
	const char *Name() const;

	/* ---- output worker ---- */
	struct Job {
		std::string text;
		std::string filePath;
		std::string textSource;
		std::shared_ptr<std::vector<unsigned char>> dumpData;
		bool append = false;
	};

	std::mutex jobMutex;
	std::condition_variable jobCond;
	std::deque<Job> jobs;
	bool stopping = false;
	std::thread worker;

	void QueueJob(Job job);
	void WorkerLoop();
	void WriteCaptionFile(const std::string &path, const std::string &text);
	void AppendToFile(const std::string &path, const std::string &text);
	void WriteDump(const std::string &path, const std::vector<unsigned char> &data);
	static void UpdateTextSource(const std::string &name, const std::string &text);
};
