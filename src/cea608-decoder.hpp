/*
 * EIA/CEA-608 ("line 21") closed caption decoder.
 *
 * Feeds on the raw byte pairs carried on line 21 of an NTSC signal and keeps
 * track of what a television's caption decoder would currently be showing on
 * screen (pop-on, roll-up and paint-on styles).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstdint>
#include <string>

class Cea608Decoder {
public:
	enum class PairResult {
		Padding,     /* null filler (0x80 0x80) */
		ParityError, /* corrupted pair, ignored */
		OtherChannel,/* valid data for a channel we're not showing */
		Repeat,      /* redundant copy of the previous control code */
		Data,        /* caption data for our channel */
	};

	explicit Cea608Decoder(int channel = 1);

	/* 1 = CC1, 2 = CC2 (both carried in field 1) */
	void SetChannel(int channel);
	int Channel() const { return channel; }

	/* Process one byte pair exactly as received (parity bits included). */
	PairResult Decode(uint8_t b1, uint8_t b2);

	/* Forget everything (caption memories, mode, cursor). */
	void Reset();

	/* Erase only what is currently displayed. */
	void EraseDisplayed();

	/* The caption text currently on screen as UTF-8, one line per caption
	 * row (top to bottom), with surrounding spaces trimmed.  Empty when
	 * nothing is displayed. */
	std::string DisplayedText() const;

	bool DisplayedIsEmpty() const;

	/* Describes a byte pair for troubleshooting logs, e.g. "EOC" or
	 * "'He'". Parity is stripped before describing. */
	static std::string Describe(uint8_t b1, uint8_t b2);

private:
	static constexpr int ROWS = 15;
	static constexpr int COLS = 32;

	enum class Mode { None, PopOn, RollUp, PaintOn, Text };

	typedef char32_t Memory[ROWS][COLS];

	Memory memA;
	Memory memB;
	Memory *displayed = &memA;
	Memory *nonDisplayed = &memB;

	int channel = 1;
	int dataChannel = 0; /* channel addressed by the last control code */
	bool inXds = false;
	Mode mode = Mode::None;
	int rollRows = 2;
	int baseRow = ROWS - 1;
	int row = ROWS - 1;
	int col = 0;

	bool havePrevControl = false;
	uint8_t prevC1 = 0, prevC2 = 0;

	Memory &Target() { return (mode == Mode::PopOn) ? *nonDisplayed : *displayed; }

	static void ClearMemory(Memory &mem);
	static void ClearRow(Memory &mem, int r);

	void PutChar(char32_t ch);
	void Backspace();
	void HandleControl(uint8_t c1, uint8_t c2);
	void HandleMiscControl(uint8_t c2);
	void HandlePAC(uint8_t c1, uint8_t c2);
	void StartRollUp(int rows);
	void CarriageReturn();
	void MoveRollUpWindow(int newBase);
};
