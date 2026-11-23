/*
 * Software "bit slicer" for EIA-608 line 21 closed captions.
 *
 * Takes raw sampled VBI lines (what the capture chip digitized from the
 * vertical blanking interval) and recovers the two caption bytes, the same way
 * a television's caption decoder does: find the clock run-in, lock onto the
 * start bits, then read 16 data bits.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/* Mirrors KS_VBIINFOHEADER from ksmedia.h */
struct VbiFormat {
	uint32_t startLine = 0;
	uint32_t endLine = 0;
	uint32_t samplingFrequency = 0; /* Hz */
	uint32_t minLineStartTime = 0;  /* 1/100 us after hsync */
	uint32_t maxLineStartTime = 0;
	uint32_t actualLineStartTime = 0;
	uint32_t actualLineEndTime = 0;
	uint32_t videoStandard = 0;
	uint32_t samplesPerLine = 0;
	uint32_t strideInBytes = 0;
	uint32_t bufferSize = 0;

	bool valid = false;

	static VbiFormat Parse(const unsigned char *format, size_t size);
	std::string Describe() const;
};

class Line21Slicer {
public:
	struct Result {
		bool runIn = false; /* a clock run-in + start bits were found */
		bool valid = false; /* ...and both bytes passed parity */
		uint8_t b1 = 0, b2 = 0;
		float score = 0.0f;
	};

	void Configure(const VbiFormat &format);

	/* Slice one line of 8-bit samples. */
	Result SliceLine(const uint8_t *samples, size_t count);

	double SamplesPerBit() const { return samplesPerBit; }

private:
	double samplesPerBit = 53.6;
	std::vector<float> smooth;

	float At(double x) const;
	float Bit(double center) const;
};
