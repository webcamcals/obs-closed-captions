/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "vbi-slicer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

/* EIA-608 data rate: 32 x the NTSC line rate (15734.264 Hz) */
static constexpr double LINE21_BIT_RATE = 503496.5;

/* Fallback when the device doesn't report its sampling rate */
static constexpr double DEFAULT_SAMPLING_RATE = 27000000.0;

VbiFormat VbiFormat::Parse(const unsigned char *format, size_t size)
{
	VbiFormat f;
	if (!format || size < 11 * sizeof(uint32_t))
		return f;

	uint32_t v[11];
	memcpy(v, format, sizeof(v));
	f.startLine = v[0];
	f.endLine = v[1];
	f.samplingFrequency = v[2];
	f.minLineStartTime = v[3];
	f.maxLineStartTime = v[4];
	f.actualLineStartTime = v[5];
	f.actualLineEndTime = v[6];
	f.videoStandard = v[7];
	f.samplesPerLine = v[8];
	f.strideInBytes = v[9];
	f.bufferSize = v[10];

	f.valid = f.samplesPerLine > 0 && f.samplesPerLine <= 8192 && f.endLine >= f.startLine &&
		  f.endLine - f.startLine < 64;
	if (f.valid && f.strideInBytes < f.samplesPerLine)
		f.strideInBytes = f.samplesPerLine;
	return f;
}

std::string VbiFormat::Describe() const
{
	char buf[512];
	snprintf(buf, sizeof(buf),
		 "lines %u-%u, %u Hz, %u samples/line, stride %u, buffer %u bytes, "
		 "line start %u (min %u, max %u), line end %u, standard 0x%X",
		 startLine, endLine, samplingFrequency, samplesPerLine, strideInBytes, bufferSize,
		 actualLineStartTime, minLineStartTime, maxLineStartTime, actualLineEndTime, videoStandard);
	return buf;
}

/* ------------------------------------------------------------------------ */

void Line21Slicer::Configure(const VbiFormat &format)
{
	double fs = format.samplingFrequency ? (double)format.samplingFrequency : DEFAULT_SAMPLING_RATE;
	samplesPerBit = fs / LINE21_BIT_RATE;
}

float Line21Slicer::Bit(double center) const
{
	/* average over the middle of the bit cell for noise immunity */
	const double d = samplesPerBit * 0.2;
	return (At(center - d) + At(center) + At(center + d)) / 3.0f;
}

float Line21Slicer::At(double x) const
{
	if (x <= 0.0)
		return smooth.front();
	size_t i = (size_t)x;
	if (i + 1 >= smooth.size())
		return smooth.back();
	float frac = (float)(x - (double)i);
	return smooth[i] + (smooth[i + 1] - smooth[i]) * frac;
}

static bool OddParity(uint8_t b)
{
	int bits = 0;
	for (int i = 0; i < 8; i++)
		bits += (b >> i) & 1;
	return (bits & 1) == 1;
}

Line21Slicer::Result Line21Slicer::SliceLine(const uint8_t *samples, size_t count)
{
	Result best;
	const double T = samplesPerBit;

	/* need room for 6 run-in cycles + 3 start bits + 16 data bits */
	if (count < (size_t)(25.5 * T))
		return best;

	/* Low-pass the line a little (box filter of ~1/4 bit) to tame noise. */
	int half = std::max(1, (int)(T / 8.0));
	smooth.resize(count);
	{
		int sum = 0, n = 0;
		int lo = 0, hi = -1;
		for (size_t i = 0; i < count; i++) {
			int wantHi = std::min((int)count - 1, (int)i + half);
			int wantLo = std::max(0, (int)i - half);
			while (hi < wantHi) {
				sum += samples[++hi];
				n++;
			}
			while (lo < wantLo) {
				sum -= samples[lo++];
				n--;
			}
			smooth[i] = (float)sum / (float)n;
		}
	}

	/* Slicing level: halfway between the blanking level and the peaks. */
	std::vector<float> sorted(smooth);
	size_t p10 = sorted.size() / 10;
	size_t p95 = sorted.size() * 95 / 100;
	std::nth_element(sorted.begin(), sorted.begin() + p10, sorted.end());
	float low = sorted[p10];
	std::nth_element(sorted.begin(), sorted.begin() + p95, sorted.end());
	float high = sorted[p95];

	float range = high - low;
	if (range < 20.0f)
		return best; /* flat line: nothing here */

	const float thr = (low + high) * 0.5f;
	/* a real clock run-in swings across most of the signal range */
	const float minRunInSwing = range * 0.45f;

	/* x0 = position of the first start bit. Data needs 19 bits after it;
	 * the run-in needs ~7 bits before it. */
	double xMin = 6.0 * T;
	double xMax = (double)count - 19.2 * T;

	for (double x0 = xMin; x0 < xMax; x0 += 1.0) {
		/* start bits: 0, 0, 1 */
		float s0 = Bit(x0 + 0.5 * T);
		float s1 = Bit(x0 + 1.5 * T);
		float s2 = Bit(x0 + 2.5 * T);
		if (!(s0 < thr && s1 < thr && s2 > thr))
			continue;

		/* clock run-in: one full cycle per bit period before the start
		 * bits. Accept either phase, but it must be consistent and
		 * strong in every one of the last 6 cycles. */
		int phaseA = 0, phaseB = 0;
		float runInSwing = 0.0f;
		for (int j = 1; j <= 6; j++) {
			float a = At(x0 - j * T + 0.25 * T);
			float b = At(x0 - j * T + 0.75 * T);
			float d = a - b;
			if (d > minRunInSwing)
				phaseA++;
			else if (d < -minRunInSwing)
				phaseB++;
			runInSwing += std::fabs(d);
		}
		if (phaseA < 6 && phaseB < 6)
			continue;

		/* data bits, LSB first */
		uint16_t bits = 0;
		float sumHi = s2, sumLo = s0 + s1;
		int nHi = 1, nLo = 2;
		float worstMargin = std::min(std::min(thr - s0, thr - s1), s2 - thr);
		for (int i = 0; i < 16; i++) {
			float v = Bit(x0 + (3.5 + i) * T);
			if (v > thr) {
				bits |= (uint16_t)(1u << i);
				sumHi += v;
				nHi++;
			} else {
				sumLo += v;
				nLo++;
			}
			worstMargin = std::min(worstMargin, std::fabs(v - thr));
		}

		/* ones and zeros must be clearly separated */
		float eye = sumHi / (float)nHi - sumLo / (float)nLo;
		if (eye < range * 0.5f)
			continue;

		uint8_t b1 = (uint8_t)(bits & 0xFF);
		uint8_t b2 = (uint8_t)(bits >> 8);
		bool valid = OddParity(b1) && OddParity(b2);
		float score = eye + worstMargin * 2.0f + runInSwing / 6.0f;

		/* prefer valid decodes; among equals, the most confident */
		bool better = (valid && !best.valid) || (valid == best.valid && score > best.score);
		if (better) {
			best.runIn = true;
			best.valid = valid;
			best.b1 = b1;
			best.b2 = b2;
			best.score = score;
		}
	}

	return best;
}
