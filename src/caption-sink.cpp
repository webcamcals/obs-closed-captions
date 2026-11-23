/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "caption-sink.hpp"

#include <util/platform.h>
#include <util/bmem.h>

#include <windows.h>
#include <cstdio>

#define NS_PER_SEC 1000000000ULL
#define STATS_INTERVAL_NS (30ULL * NS_PER_SEC)
#define NO_DATA_WARNING_NS (10ULL * NS_PER_SEC)

CaptionSink::CaptionSink(obs_source_t *owner_) : owner(owner_) {}

CaptionSink::~CaptionSink()
{
	std::lock_guard<std::mutex> lock(mutex);
	decoder.Reset();
	/* don't touch other sources here: we may be in OBS shutdown */
	PublishLocked(false, true);
}

const char *CaptionSink::Name() const
{
	return obs_source_get_name(owner);
}

bool CaptionSink::Enabled()
{
	std::lock_guard<std::mutex> lock(mutex);
	return settings.enabled;
}

void CaptionSink::Configure(const CaptionSettings &newSettings)
{
	std::lock_guard<std::mutex> lock(mutex);

	/* clear whatever the old settings were publishing to */
	if (publishedOnce && (newSettings.filePath != settings.filePath ||
			      newSettings.textSource != settings.textSource || !newSettings.enabled)) {
		decoder.Reset();
		PublishLocked(true);
	}

	settings = newSettings;
	decoder.Reset();
	decoder.SetChannel(settings.channel);

	uint64_t now = os_gettime_ns();
	lastDataTime = now;
	lastSampleTime = 0;
	lastStatsTime = now;
	lastTickTime = now;
	loggedFirstSample = false;
	loggedNoSamples = false;
	stats = Stats();
	totalStats = Stats();

	if (settings.enabled) {
		blog(LOG_INFO,
		     "[Closed Captions: '%s'] enabled\n"
		     "\tchannel: CC%d\n"
		     "\ttext file: %s\n"
		     "\ttext source: %s\n"
		     "\tclear after: %d seconds\n"
		     "\tlog raw data: %s",
		     Name(), settings.channel, settings.filePath.empty() ? "(none)" : settings.filePath.c_str(),
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

void CaptionSink::OnCaptionData(const unsigned char *data, size_t size)
{
	std::lock_guard<std::mutex> lock(mutex);

	if (!settings.enabled)
		return;

	uint64_t now = os_gettime_ns();
	lastSampleTime = now;

	if (!loggedFirstSample) {
		loggedFirstSample = true;
		blog(LOG_INFO, "[Closed Captions: '%s'] receiving data from the CC pin (%u bytes per sample)",
		     Name(), (unsigned)size);
	}

	stats.samples++;
	stats.bytes += size;

	for (size_t i = 0; i + 1 < size; i += 2) {
		uint8_t b1 = data[i];
		uint8_t b2 = data[i + 1];

		Cea608Decoder::PairResult result = decoder.Decode(b1, b2);

		switch (result) {
		case Cea608Decoder::PairResult::Padding:
			stats.padding++;
			break;
		case Cea608Decoder::PairResult::ParityError:
			stats.parityErrors++;
			break;
		case Cea608Decoder::PairResult::OtherChannel:
			stats.otherChannel++;
			break;
		case Cea608Decoder::PairResult::Repeat:
		case Cea608Decoder::PairResult::Data:
			stats.data++;
			lastDataTime = now;
			break;
		}

		if (settings.logRaw && result != Cea608Decoder::PairResult::Padding) {
			std::string desc = Cea608Decoder::Describe(b1, b2);
			blog(LOG_INFO, "[Closed Captions: '%s'] raw %02X %02X  %s%s", Name(), b1, b2, desc.c_str(),
			     result == Cea608Decoder::PairResult::ParityError ? "  (parity error)" : "");
		}
	}

	PublishLocked();
}

void CaptionSink::Tick()
{
	std::lock_guard<std::mutex> lock(mutex);

	if (!settings.enabled)
		return;

	uint64_t now = os_gettime_ns();

	/* Tick may be called for every video frame; only do work ~4x/sec */
	if (now - lastTickTime < NS_PER_SEC / 4)
		return;
	lastTickTime = now;

	if (!loggedFirstSample && !loggedNoSamples && now - lastStatsTime > NO_DATA_WARNING_NS) {
		loggedNoSamples = true;
		blog(LOG_WARNING,
		     "[Closed Captions: '%s'] video is arriving, but no data has come from the "
		     "device's CC pin in 10 seconds. Look above for lines starting with "
		     "'DShow: Closed captions' for details.",
		     Name());
	}

	if (settings.clearSeconds > 0 && !decoder.DisplayedIsEmpty() &&
	    now - lastDataTime > (uint64_t)settings.clearSeconds * NS_PER_SEC) {
		decoder.EraseDisplayed();
		PublishLocked();
	}

	if (now - lastStatsTime >= STATS_INTERVAL_NS)
		LogStatsLocked(now);
}

void CaptionSink::Clear()
{
	std::lock_guard<std::mutex> lock(mutex);
	decoder.Reset();
	PublishLocked();
}

void CaptionSink::LogStatsLocked(uint64_t now)
{
	lastStatsTime = now;

	if (!stats.samples)
		return;

	std::string current = decoder.DisplayedText();
	for (char &c : current)
		if (c == '\n')
			c = '|';

	blog(LOG_INFO,
	     "[Closed Captions: '%s'] last 30s: %llu samples, %llu caption pairs, %llu other-channel pairs, "
	     "%llu empty pairs, %llu parity errors. On screen now: \"%s\"",
	     Name(), (unsigned long long)stats.samples, (unsigned long long)stats.data,
	     (unsigned long long)stats.otherChannel, (unsigned long long)stats.padding,
	     (unsigned long long)stats.parityErrors, current.c_str());

	stats = Stats();
}

void CaptionSink::PublishLocked(bool force, bool fileOnly)
{
	std::string text = settings.enabled ? decoder.DisplayedText() : std::string();

	if (!force && publishedOnce && text == published)
		return;

	published = text;
	publishedOnce = true;

	if (!settings.filePath.empty())
		WriteCaptionFile(settings.filePath, text);
	if (!fileOnly && !settings.textSource.empty())
		UpdateTextSource(settings.textSource, text);
}

static bool WriteAll(const wchar_t *path, const std::string &text)
{
	HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
				  FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;

	DWORD written = 0;
	BOOL ok = TRUE;
	if (!text.empty())
		ok = WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr);
	CloseHandle(file);
	return ok && written == (DWORD)text.size();
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
	if (WriteAll(temp.c_str(), text)) {
		for (int attempt = 0; attempt < 3 && !ok; attempt++) {
			ok = MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
			if (!ok)
				Sleep(2);
		}
		if (!ok)
			DeleteFileW(temp.c_str());
	}

	if (!ok)
		ok = WriteAll(target.c_str(), text);

	static bool warned = false;
	if (!ok && !warned) {
		warned = true;
		blog(LOG_WARNING, "[Closed Captions: '%s'] could not write caption file '%s' (error %lu)", Name(),
		     path.c_str(), GetLastError());
	}
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
