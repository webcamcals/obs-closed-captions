/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "caption-sink.hpp"

#include <util/platform.h>
#include <util/bmem.h>
#include <util/threading.h>

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#define NS_PER_SEC 1000000000ULL
#define STATS_INTERVAL_NS (30ULL * NS_PER_SEC)
#define NO_DATA_WARNING_NS (10ULL * NS_PER_SEC)
#define DUMP_START_TIMEOUT_NS (20ULL * NS_PER_SEC)
#define DUMP_MAX_SAMPLES 600

/* DirectShow time units (100 ns) per NTSC frame / field */
#define NTSC_FRAME_100NS 333667LL
#define NTSC_FIELD_100NS 166833.5

CaptionSink::CaptionSink(obs_source_t *owner_) : owner(owner_)
{
	worker = std::thread(&CaptionSink::WorkerLoop, this);
}

CaptionSink::~CaptionSink()
{
	std::string filePath;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!settings.appendToFile)
			filePath = settings.filePath;
	}

	{
		std::lock_guard<std::mutex> lock(jobMutex);
		stopping = true;
	}
	jobCond.notify_all();
	if (worker.joinable())
		worker.join();

	/* Leave an empty caption file behind. Don't touch other sources here:
	 * this may run during OBS shutdown. */
	if (!filePath.empty())
		WriteCaptionFile(filePath, std::string());
}

const char *CaptionSink::Name() const
{
	return obs_source_get_name(owner);
}

/* ------------------------------------------------------------------------ */

void CaptionSink::ResetDecodingLocked()
{
	slots.clear();
	activeKey = CC_PIN_KEY;
	haveActive = false;
	vbiSampleCounter = 0;
	fieldIndex = 0;
	haveLastVbiStart = false;
	steadyFieldDeltas = 0;
	haveLastCcStart = false;
}

void CaptionSink::Configure(const CaptionSettings &newSettings)
{
	std::lock_guard<std::mutex> lock(mutex);

	/* clear whatever the old settings were publishing to */
	if (haveLastText && (newSettings.filePath != settings.filePath ||
			     newSettings.appendToFile != settings.appendToFile ||
			     newSettings.textSource != settings.textSource || !newSettings.enabled)) {
		QueueJob({std::string(), settings.appendToFile ? std::string() : settings.filePath,
			  settings.textSource, nullptr});
	}

	settings = newSettings;
	ResetDecodingLocked();

	uint64_t now = os_gettime_ns();
	lastDataTime = now;
	lastStatsTime = now;
	lastTickTime = now;
	configureTime = now;
	loggedFirstCc = false;
	loggedFirstVbi = false;
	loggedNoSamples = false;
	loggedVbiFormat = false;
	warnedNoVbiFormat = false;
	vbiFormatBlock.clear();
	vbiFormat = VbiFormat();
	dump.clear();
	dumpSamples = 0;
	dumpDone = false;
	dumpStarted = false;
	stats = Stats();

	if (settings.enabled) {
		blog(LOG_INFO,
		     "[Closed Captions: '%s'] enabled\n"
		     "\tinput: %s\n"
		     "\tchannel: CC%d\n"
		     "\ttext file: %s (%s)\n"
		     "\ttext source: %s\n"
		     "\tclear after: %d seconds\n"
		     "\tlog raw data: %s",
		     Name(),
		     settings.input == CaptionInput::Vbi ? "VBI pin (decoded by plugin)"
							 : "CC pin (decoded by device)",
		     settings.channel, settings.filePath.empty() ? "(none)" : settings.filePath.c_str(),
		     settings.appendToFile ? "transcript: finished lines are appended" : "current caption",
		     settings.textSource.empty() ? "(none)" : settings.textSource.c_str(), settings.clearSeconds,
		     settings.logRaw ? "yes" : "no");

		if (settings.filePath.empty() && settings.textSource.empty())
			blog(LOG_WARNING,
			     "[Closed Captions: '%s'] no text file or text source is set, "
			     "so captions will only appear in this log",
			     Name());
	} else {
		blog(LOG_INFO, "[Closed Captions: '%s'] disabled", Name());
	}

	PublishLocked(true);
}

/* ------------------------------------------------------------------------ */

CaptionSink::Slot &CaptionSink::GetSlot(int key)
{
	std::unique_ptr<Slot> &slot = slots[key];
	if (!slot) {
		slot.reset(new Slot());
		slot->decoder.SetChannel(settings.channel);
	}
	return *slot;
}

Cea608Decoder *CaptionSink::ActiveDecoder()
{
	if (!haveActive)
		return nullptr;
	auto it = slots.find(activeKey);
	return it == slots.end() ? nullptr : &it->second->decoder;
}

void CaptionSink::FeedPair(int key, uint8_t b1, uint8_t b2, uint64_t now)
{
	Slot &slot = GetSlot(key);

	/* Evidence about which field this is: CC1/CC2 live in field 1 and use
	 * 0x14/0x1C commands; field 2 uses 0x15/0x1D commands and XDS. */
	uint8_t c1 = b1 & 0x7F, c2 = b2 & 0x7F;
	if (c2 >= 0x20 && c2 <= 0x2F && (c1 & 0xF7) == 0x14)
		slot.score += 3;
	else if (c2 >= 0x20 && c2 <= 0x2F && (c1 & 0xF7) == 0x15)
		slot.score -= 3;
	else if (c1 >= 0x01 && c1 <= 0x0F)
		slot.score -= 1;

	Cea608Decoder::PairResult result = slot.decoder.Decode(b1, b2);
	slot.found++;

	finishedScratch.clear();
	slot.decoder.TakeFinishedLines(finishedScratch);
	if (!finishedScratch.empty() && haveActive && key == activeKey && settings.appendToFile &&
	    !settings.filePath.empty()) {
		std::string text;
		for (const std::string &line : finishedScratch) {
			text += line;
			text += '\n';
		}
		Job job;
		job.text = text;
		job.filePath = settings.filePath;
		job.append = true;
		QueueJob(std::move(job));
	}
	if (result != Cea608Decoder::PairResult::Padding)
		slot.dataPairs++;

	bool active = haveActive && key == activeKey;
	if (active) {
		stats.activePairs++;
		if (result == Cea608Decoder::PairResult::Data || result == Cea608Decoder::PairResult::Repeat) {
			stats.activeData++;
			lastDataTime = now;
		}
		if (result == Cea608Decoder::PairResult::ParityError)
			stats.activeParityFails++;
	}

	if (settings.logRaw && result != Cea608Decoder::PairResult::Padding && (active || key == CC_PIN_KEY)) {
		std::string desc = Cea608Decoder::Describe(b1, b2);
		blog(LOG_INFO, "[Closed Captions: '%s'] raw %02X %02X  %s", Name(), b1, b2, desc.c_str());
	}
}

void CaptionSink::ChooseActiveLocked()
{
	int bestKey = 0;
	Slot *best = nullptr;

	for (auto &it : slots) {
		Slot *s = it.second.get();
		if (!best || s->score > best->score || (s->score == best->score && s->dataPairs > best->dataPairs)) {
			best = s;
			bestKey = it.first;
		}
	}

	if (!best)
		return;

	if (haveActive && bestKey == activeKey)
		return;

	if (haveActive) {
		auto cur = slots.find(activeKey);
		if (cur != slots.end()) {
			/* hysteresis: don't flip-flop between candidates */
			Slot *c = cur->second.get();
			bool clearlyBetter = best->score > c->score + 6 ||
					     (best->score == c->score && best->dataPairs > c->dataPairs + 30);
			if (!clearlyBetter)
				return;
		}
	}

	activeKey = bestKey;
	haveActive = true;

	if (bestKey == CC_PIN_KEY)
		blog(LOG_INFO, "[Closed Captions: '%s'] reading captions from the CC pin", Name());
	else
		blog(LOG_INFO, "[Closed Captions: '%s'] reading captions from VBI line %d (field %c)", Name(),
		     best->line, best->field ? 'B' : 'A');

	PublishLocked(true);
}

/* ------------------------------------------------------------------------ */
/* CC pin: byte pairs already decoded by the capture device                  */

void CaptionSink::OnCaptionData(const unsigned char *data, size_t size, long long startTime, bool hasTime)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (!settings.enabled)
		return;

	uint64_t now = os_gettime_ns();

	if (!loggedFirstCc) {
		loggedFirstCc = true;
		blog(LOG_INFO, "[Closed Captions: '%s'] receiving data from the CC pin (%u bytes per sample, %s)",
		     Name(), (unsigned)size, hasTime ? "timestamped" : "no timestamps");
	}

	/* Detect samples the device never delivered, using timestamps. */
	if (hasTime) {
		if (haveLastCcStart) {
			long long delta = startTime - lastCcStart;
			if (delta > NTSC_FRAME_100NS * 3 / 2 && delta < NTSC_FRAME_100NS * 300) {
				stats.ccGaps++;
				stats.ccMissed += (uint64_t)((delta + NTSC_FRAME_100NS / 2) / NTSC_FRAME_100NS) - 1;
			}
		}
		lastCcStart = startTime;
		haveLastCcStart = true;
	}

	stats.ccSamples++;

	if (!haveActive) {
		activeKey = CC_PIN_KEY;
		haveActive = true;
	}

	for (size_t i = 0; i + 1 < size; i += 2)
		FeedPair(CC_PIN_KEY, data[i], data[i + 1], now);

	PublishLocked();
}

/* ------------------------------------------------------------------------ */
/* VBI pin: raw waveforms, decoded here                                      */

void CaptionSink::DumpSampleLocked(const unsigned char *data, size_t size, long long startTime, bool hasTime)
{
	if (dumpDone || !settings.logRaw || settings.dumpPath.empty())
		return;

	if (dump.empty()) {
		const char magic[8] = {'V', 'B', 'I', 'D', 'U', 'M', 'P', '1'};
		dump.insert(dump.end(), magic, magic + 8);
		uint32_t fsize = (uint32_t)vbiFormatBlock.size();
		dump.insert(dump.end(), (unsigned char *)&fsize, (unsigned char *)&fsize + 4);
		dump.insert(dump.end(), vbiFormatBlock.begin(), vbiFormatBlock.end());
	}

	int64_t ts = startTime;
	uint8_t ht = hasTime ? 1 : 0;
	uint32_t sz = (uint32_t)size;
	dump.insert(dump.end(), (unsigned char *)&ts, (unsigned char *)&ts + 8);
	dump.push_back(ht);
	dump.insert(dump.end(), (unsigned char *)&sz, (unsigned char *)&sz + 4);
	dump.insert(dump.end(), data, data + size);

	if (++dumpSamples >= DUMP_MAX_SAMPLES) {
		dumpDone = true;
		auto shared = std::make_shared<std::vector<unsigned char>>(std::move(dump));
		dump.clear();
		Job job;
		job.filePath = settings.dumpPath;
		job.dumpData = shared;
		QueueJob(std::move(job));
	}
}

void CaptionSink::OnVbiData(const unsigned char *data, size_t size, long long startTime, bool hasTime,
			    const unsigned char *format, size_t formatSize)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (!settings.enabled)
		return;

	uint64_t now = os_gettime_ns();

	if (formatSize && (vbiFormatBlock.size() != formatSize ||
			   memcmp(vbiFormatBlock.data(), format, formatSize) != 0)) {
		vbiFormatBlock.assign(format, format + formatSize);
		vbiFormat = VbiFormat::Parse(format, formatSize);
		slicer.Configure(vbiFormat);
		loggedVbiFormat = false;
	}

	if (!loggedFirstVbi) {
		loggedFirstVbi = true;
		blog(LOG_INFO, "[Closed Captions: '%s'] receiving raw VBI from the VBI pin (%u bytes per sample, %s)",
		     Name(), (unsigned)size, hasTime ? "timestamped" : "no timestamps");
	}

	VbiFormat fmt = vbiFormat;
	if (!fmt.valid) {
		/* No usable format block: guess the common layouts. */
		uint32_t guess = (size % 1440 == 0) ? 1440 : (size % 720 == 0) ? 720 : 0;
		if (!warnedNoVbiFormat) {
			warnedNoVbiFormat = true;
			blog(LOG_WARNING,
			     "[Closed Captions: '%s'] the VBI pin did not describe its format (%u-byte format "
			     "block); guessing %u samples per line",
			     Name(), (unsigned)formatSize, guess);
		}
		if (!guess)
			return;
		fmt.samplesPerLine = fmt.strideInBytes = guess;
		fmt.samplingFrequency = guess == 1440 ? 27000000 : 13500000;
		fmt.startLine = 10;
		fmt.endLine = 21;
		fmt.valid = true;
		slicer.Configure(fmt);
	}

	if (!loggedVbiFormat) {
		loggedVbiFormat = true;
		blog(LOG_INFO, "[Closed Captions: '%s'] VBI format: %s (%.1f samples per caption bit)", Name(),
		     fmt.Describe().c_str(), slicer.SamplesPerBit());
	}

	stats.vbiSamples++;

	bool anyRunIn = false;

	size_t stride = fmt.strideInBytes;
	size_t rows = stride ? size / stride : 0;
	size_t perField = fmt.endLine - fmt.startLine + 1;
	bool twoFields = rows >= 2 * perField;

	/* Which field is this?  Fields strictly alternate, so toggle on every
	 * sample.  Only use timestamps to notice dropped fields, and only once
	 * they've proven to tick steadily at one field period. (Absolute
	 * timestamps can sit halfway between field boundaries, so rounding
	 * them is not reliable.) */
	if (vbiSampleCounter > 0) {
		fieldIndex++;
		if (hasTime && haveLastVbiStart) {
			long long delta = startTime - lastVbiStart;
			stats.vbiDeltaMin = std::min(stats.vbiDeltaMin, delta);
			stats.vbiDeltaMax = std::max(stats.vbiDeltaMax, delta);

			bool oneField = delta > NTSC_FIELD_100NS * 0.8 && delta < NTSC_FIELD_100NS * 1.2;
			if (oneField) {
				if (steadyFieldDeltas < 1000)
					steadyFieldDeltas++;
			} else if (steadyFieldDeltas >= 30 && delta > NTSC_FIELD_100NS * 1.5 &&
				   delta < NTSC_FIELD_100NS * 600) {
				long long missed = std::llround((double)delta / NTSC_FIELD_100NS) - 1;
				fieldIndex += (uint64_t)missed;
				stats.vbiMissedFields += (uint64_t)missed;
			} else {
				steadyFieldDeltas = 0;
			}
		}
	}
	if (hasTime) {
		lastVbiStart = startTime;
		haveLastVbiStart = true;
	}
	vbiSampleCounter++;
	int parity = (int)(fieldIndex & 1);

	size_t lineSamples = std::min<size_t>(fmt.samplesPerLine, stride);

	for (size_t row = 0; row < rows; row++) {
		Line21Slicer::Result res = slicer.SliceLine(data + row * stride, lineSamples);
		if (!res.runIn)
			continue;

		anyRunIn = true;
		stats.vbiLinesWithCc++;

		int field = twoFields ? (row >= rows / 2 ? 1 : 0) : parity;
		int key = (int)row * 2 + (twoFields ? 0 : parity);

		Slot &slot = GetSlot(key);
		slot.line = (int)fmt.startLine + (int)(twoFields ? row % perField : row);
		slot.field = field;

		if (!res.valid) {
			slot.runInNoParity++;
			if (haveActive && key == activeKey)
				stats.activeParityFails++;
			continue;
		}

		FeedPair(key, res.b1, res.b2, now);
	}

	if (!dumpStarted && settings.logRaw && (anyRunIn || now - configureTime > DUMP_START_TIMEOUT_NS)) {
		dumpStarted = true;
		blog(LOG_INFO, "[Closed Captions: '%s'] saving a raw VBI sample for troubleshooting...", Name());
	}
	if (dumpStarted)
		DumpSampleLocked(data, size, startTime, hasTime);

	ChooseActiveLocked();
	PublishLocked();
}

/* ------------------------------------------------------------------------ */

void CaptionSink::Tick()
{
	std::lock_guard<std::mutex> lock(mutex);

	if (!settings.enabled)
		return;

	stats.videoFrames++;

	uint64_t now = os_gettime_ns();

	/* only do the rest ~4x per second */
	if (now - lastTickTime < NS_PER_SEC / 4)
		return;
	lastTickTime = now;

	if (!loggedFirstCc && !loggedFirstVbi && !loggedNoSamples && now - configureTime > NO_DATA_WARNING_NS) {
		loggedNoSamples = true;
		blog(LOG_WARNING,
		     "[Closed Captions: '%s'] video is arriving, but no caption data has come from the "
		     "device in 10 seconds. Look above for lines starting with "
		     "'DShow: Closed captions' for details.",
		     Name());
	}

	Cea608Decoder *dec = ActiveDecoder();
	if (dec && settings.clearSeconds > 0 && !dec->DisplayedIsEmpty() &&
	    now - lastDataTime > (uint64_t)settings.clearSeconds * NS_PER_SEC) {
		dec->EraseDisplayed();
		PublishLocked();
	}

	if (now - lastStatsTime >= STATS_INTERVAL_NS)
		LogStatsLocked(now);
}

void CaptionSink::Clear()
{
	std::lock_guard<std::mutex> lock(mutex);
	ResetDecodingLocked();
	PublishLocked();
}

void CaptionSink::LogStatsLocked(uint64_t now)
{
	lastStatsTime = now;

	if (!stats.ccSamples && !stats.vbiSamples) {
		stats = Stats();
		return;
	}

	Cea608Decoder *dec = ActiveDecoder();
	std::string current = dec ? dec->DisplayedText() : std::string();
	for (char &c : current)
		if (c == '\n')
			c = '|';

	if (stats.vbiSamples) {
		std::string where = "none yet";
		if (haveActive && activeKey != CC_PIN_KEY) {
			auto it = slots.find(activeKey);
			if (it != slots.end()) {
				char buf[64];
				snprintf(buf, sizeof(buf), "line %d field %c", it->second->line,
					 it->second->field ? 'B' : 'A');
				where = buf;
			}
		}

		blog(LOG_INFO,
		     "[Closed Captions: '%s'] last 30s: %llu video frames, %llu VBI samples, %llu lines with a "
		     "caption signal. Reading %s: %llu pairs decoded, %llu unreadable (parity), %llu caption "
		     "pairs. Timestamp steps %lld-%lld, %llu fields dropped. On screen now: \"%s\"",
		     Name(), (unsigned long long)stats.videoFrames, (unsigned long long)stats.vbiSamples,
		     (unsigned long long)stats.vbiLinesWithCc, where.c_str(), (unsigned long long)stats.activePairs,
		     (unsigned long long)stats.activeParityFails, (unsigned long long)stats.activeData,
		     stats.vbiDeltaMin == LLONG_MAX ? 0LL : stats.vbiDeltaMin,
		     stats.vbiDeltaMax == LLONG_MIN ? 0LL : stats.vbiDeltaMax,
		     (unsigned long long)stats.vbiMissedFields, current.c_str());

		if (settings.logRaw) {
			for (auto &it : slots) {
				if (it.first == CC_PIN_KEY)
					continue;
				Slot *s = it.second.get();
				blog(LOG_INFO,
				     "[Closed Captions: '%s']   VBI line %d field %c: score %lld, %llu decoded, "
				     "%llu with data, %llu unreadable",
				     Name(), s->line, s->field ? 'B' : 'A', (long long)s->score,
				     (unsigned long long)s->found, (unsigned long long)s->dataPairs,
				     (unsigned long long)s->runInNoParity);
			}
		}
	} else {
		double pct = stats.videoFrames ? 100.0 * (double)stats.ccSamples / (double)stats.videoFrames : 0.0;
		blog(LOG_INFO,
		     "[Closed Captions: '%s'] last 30s: %llu video frames, %llu CC samples (%.1f%%), "
		     "%llu gaps (~%llu samples missed), %llu caption pairs. On screen now: \"%s\"",
		     Name(), (unsigned long long)stats.videoFrames, (unsigned long long)stats.ccSamples, pct,
		     (unsigned long long)stats.ccGaps, (unsigned long long)stats.ccMissed,
		     (unsigned long long)stats.activeData, current.c_str());
	}

	stats = Stats();
}

void CaptionSink::PublishLocked(bool force)
{
	Cea608Decoder *dec = ActiveDecoder();
	std::string text = (settings.enabled && dec) ? dec->DisplayedText() : std::string();

	if (!force && haveLastText && text == lastText)
		return;

	lastText = text;
	haveLastText = true;

	std::string file = settings.appendToFile ? std::string() : settings.filePath;
	if (file.empty() && settings.textSource.empty())
		return;

	QueueJob({text, file, settings.textSource, nullptr});
}

/* ------------------------------------------------------------------------ */
/* Output worker: file writes and text source updates happen here, never on  */
/* the capture threads.                                                      */

void CaptionSink::QueueJob(Job job)
{
	{
		std::lock_guard<std::mutex> lock(jobMutex);
		jobs.push_back(std::move(job));
	}
	jobCond.notify_one();
}

void CaptionSink::WorkerLoop()
{
	os_set_thread_name("closed-captions: output");

	for (;;) {
		std::deque<Job> batch;
		{
			std::unique_lock<std::mutex> lock(jobMutex);
			jobCond.wait(lock, [this] { return stopping || !jobs.empty(); });
			if (stopping && jobs.empty())
				return;
			batch.swap(jobs);
		}

		for (size_t i = 0; i < batch.size(); i++) {
			const Job &job = batch[i];

			if (job.dumpData) {
				WriteDump(job.filePath, *job.dumpData);
				continue;
			}
			if (job.append) {
				AppendToFile(job.filePath, job.text);
				continue;
			}

			/* If several updates piled up, only the newest one for
			 * each destination matters. */
			bool superseded = false;
			for (size_t j = i + 1; j < batch.size(); j++) {
				if (!batch[j].dumpData && !batch[j].append && batch[j].filePath == job.filePath &&
				    batch[j].textSource == job.textSource) {
					superseded = true;
					break;
				}
			}
			if (superseded)
				continue;

			if (!job.filePath.empty())
				WriteCaptionFile(job.filePath, job.text);
			if (!job.textSource.empty())
				UpdateTextSource(job.textSource, job.text);
		}
	}
}

static bool WriteAll(const wchar_t *path, const void *data, size_t size)
{
	HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
				  FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;

	bool ok = true;
	const unsigned char *p = (const unsigned char *)data;
	while (size > 0 && ok) {
		DWORD chunk = (DWORD)std::min<size_t>(size, (size_t)1 << 20);
		DWORD written = 0;
		ok = WriteFile(file, p, chunk, &written, nullptr) && written == chunk;
		p += chunk;
		size -= chunk;
	}
	CloseHandle(file);
	return ok;
}

void CaptionSink::WriteCaptionFile(const std::string &path, const std::string &text)
{
	wchar_t *wpath = nullptr;
	if (!os_utf8_to_wcs_ptr(path.c_str(), path.size(), &wpath) || !wpath)
		return;

	std::wstring target = wpath;
	std::wstring temp = target + L".tmp";
	bfree(wpath);

	/* Write to a temporary file and swap it in, so OBS never reads a
	 * half-written file.  If the swap fails (e.g. the file is open in
	 * another program at that instant), overwrite it directly. */
	bool ok = false;
	if (WriteAll(temp.c_str(), text.data(), text.size())) {
		for (int attempt = 0; attempt < 5 && !ok; attempt++) {
			ok = MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
			if (!ok)
				Sleep(5);
		}
		if (!ok)
			DeleteFileW(temp.c_str());
	}

	if (!ok)
		ok = WriteAll(target.c_str(), text.data(), text.size());

	static bool warned = false;
	if (!ok && !warned) {
		warned = true;
		blog(LOG_WARNING, "[Closed Captions] could not write caption file '%s' (error %lu)", path.c_str(),
		     GetLastError());
	}
}

void CaptionSink::AppendToFile(const std::string &path, const std::string &text)
{
	wchar_t *wpath = nullptr;
	if (!os_utf8_to_wcs_ptr(path.c_str(), path.size(), &wpath) || !wpath)
		return;

	HANDLE file = INVALID_HANDLE_VALUE;
	for (int attempt = 0; attempt < 5 && file == INVALID_HANDLE_VALUE; attempt++) {
		file = CreateFileW(wpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
				   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			Sleep(5);
	}
	bfree(wpath);

	if (file == INVALID_HANDLE_VALUE) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			blog(LOG_WARNING, "[Closed Captions] could not append to '%s' (error %lu)", path.c_str(),
			     GetLastError());
		}
		return;
	}

	DWORD written = 0;
	WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr);
	CloseHandle(file);
}

void CaptionSink::WriteDump(const std::string &path, const std::vector<unsigned char> &data)
{
	std::string dir = path;
	size_t slash = dir.find_last_of("/\\");
	if (slash != std::string::npos) {
		dir.resize(slash);
		os_mkdirs(dir.c_str());
	}

	wchar_t *wpath = nullptr;
	if (!os_utf8_to_wcs_ptr(path.c_str(), path.size(), &wpath) || !wpath)
		return;
	bool ok = WriteAll(wpath, data.data(), data.size());
	bfree(wpath);

	blog(ok ? LOG_INFO : LOG_WARNING, "[Closed Captions] %s raw VBI sample (%u KB) to '%s'",
	     ok ? "saved" : "FAILED to save", (unsigned)(data.size() / 1024), path.c_str());
}

void CaptionSink::UpdateTextSource(const std::string &name, const std::string &text)
{
	obs_source_t *target = obs_get_source_by_name(name.c_str());
	if (!target)
		return;

	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "text", text.c_str());
	obs_source_update(target, data);
	obs_data_release(data);
	obs_source_release(target);
}
