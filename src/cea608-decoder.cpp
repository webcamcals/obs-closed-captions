/*
 * EIA/CEA-608 ("line 21") closed caption decoder.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "cea608-decoder.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

/* ------------------------------------------------------------------------ */
/* Character tables                                                          */

/* A cell that holds a "transparent space" (an empty but occupied cell). */
static constexpr char32_t TRANSPARENT_SPACE = 0x20;

static char32_t BasicChar(uint8_t c)
{
	switch (c) {
	case 0x2A: return 0x00E1; /* á */
	case 0x5C: return 0x00E9; /* é */
	case 0x5E: return 0x00ED; /* í */
	case 0x5F: return 0x00F3; /* ó */
	case 0x60: return 0x00FA; /* ú */
	case 0x7B: return 0x00E7; /* ç */
	case 0x7C: return 0x00F7; /* ÷ */
	case 0x7D: return 0x00D1; /* Ñ */
	case 0x7E: return 0x00F1; /* ñ */
	case 0x7F: return 0x2588; /* solid block */
	default: return (char32_t)c;
	}
}

/* 0x11 0x30-0x3F */
static const char32_t specialChars[16] = {
	0x00AE, /* ® */ 0x00B0, /* ° */ 0x00BD, /* ½ */ 0x00BF, /* ¿ */
	0x2122, /* ™ */ 0x00A2, /* ¢ */ 0x00A3, /* £ */ 0x266A, /* ♪ */
	0x00E0, /* à */ TRANSPARENT_SPACE,      0x00E8, /* è */ 0x00E2, /* â */
	0x00EA, /* ê */ 0x00EE, /* î */ 0x00F4, /* ô */ 0x00FB, /* û */
};

/* 0x12 0x20-0x3F: Spanish / miscellaneous / French */
static const char32_t extendedChars12[32] = {
	0x00C1, 0x00C9, 0x00D3, 0x00DA, 0x00DC, 0x00FC, 0x2018, 0x00A1, /* Á É Ó Ú Ü ü ‘ ¡ */
	0x002A, 0x2019, 0x2014, 0x00A9, 0x2120, 0x2022, 0x201C, 0x201D, /* * ’ — © ℠ • “ ” */
	0x00C0, 0x00C2, 0x00C7, 0x00C8, 0x00CA, 0x00CB, 0x00EB, 0x00CE, /* À Â Ç È Ê Ë ë Î */
	0x00CF, 0x00EF, 0x00D4, 0x00D9, 0x00F9, 0x00DB, 0x00AB, 0x00BB, /* Ï ï Ô Ù ù Û « » */
};

/* 0x13 0x20-0x3F: Portuguese / German / Danish */
static const char32_t extendedChars13[32] = {
	0x00C3, 0x00E3, 0x00CD, 0x00CC, 0x00EC, 0x00D2, 0x00F2, 0x00D5, /* Ã ã Í Ì ì Ò ò Õ */
	0x00F5, 0x007B, 0x007D, 0x005C, 0x005E, 0x005F, 0x007C, 0x007E, /* õ { } \ ^ _ | ~ */
	0x00C4, 0x00E4, 0x00D6, 0x00F6, 0x00DF, 0x00A5, 0x00A4, 0x2502, /* Ä ä Ö ö ß ¥ ¤ │ */
	0x00C5, 0x00E5, 0x00D8, 0x00F8, 0x250C, 0x2510, 0x2514, 0x2518, /* Å å Ø ø ┌ ┐ └ ┘ */
};

/* Preamble address code row (0-based) by first byte (0x10-0x17) and by
 * whether bit 5 of the second byte is set. -1 = invalid. */
static const int pacRows[8][2] = {
	{10, -1}, /* 0x10: row 11 */
	{0, 1},   /* 0x11: rows 1, 2 */
	{2, 3},   /* 0x12: rows 3, 4 */
	{11, 12}, /* 0x13: rows 12, 13 */
	{13, 14}, /* 0x14: rows 14, 15 */
	{4, 5},   /* 0x15: rows 5, 6 */
	{6, 7},   /* 0x16: rows 7, 8 */
	{8, 9},   /* 0x17: rows 9, 10 */
};

static bool OddParity(uint8_t b)
{
	int bits = 0;
	for (int i = 0; i < 8; i++)
		bits += (b >> i) & 1;
	return (bits & 1) == 1;
}

static void AppendUtf8(std::string &out, char32_t cp)
{
	if (cp < 0x80) {
		out += (char)cp;
	} else if (cp < 0x800) {
		out += (char)(0xC0 | (cp >> 6));
		out += (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out += (char)(0xE0 | (cp >> 12));
		out += (char)(0x80 | ((cp >> 6) & 0x3F));
		out += (char)(0x80 | (cp & 0x3F));
	} else {
		out += (char)(0xF0 | (cp >> 18));
		out += (char)(0x80 | ((cp >> 12) & 0x3F));
		out += (char)(0x80 | ((cp >> 6) & 0x3F));
		out += (char)(0x80 | (cp & 0x3F));
	}
}

/* ------------------------------------------------------------------------ */

Cea608Decoder::Cea608Decoder(int channel_)
{
	SetChannel(channel_);
	Reset();
}

void Cea608Decoder::SetChannel(int channel_)
{
	channel = (channel_ == 2) ? 2 : 1;
}

void Cea608Decoder::ClearMemory(Memory &mem)
{
	for (int r = 0; r < ROWS; r++)
		ClearRow(mem, r);
}

void Cea608Decoder::ClearRow(Memory &mem, int r)
{
	for (int c = 0; c < COLS; c++)
		mem[r][c] = 0;
}

void Cea608Decoder::Reset()
{
	ClearMemory(memA);
	ClearMemory(memB);
	displayed = &memA;
	nonDisplayed = &memB;
	dataChannel = 0;
	inXds = false;
	mode = Mode::None;
	rollRows = 2;
	baseRow = ROWS - 1;
	row = ROWS - 1;
	col = 0;
	havePrevControl = false;
	finished.clear();
}

void Cea608Decoder::EraseDisplayed()
{
	ClearMemory(*displayed);
}

bool Cea608Decoder::DisplayedIsEmpty() const
{
	for (int r = 0; r < ROWS; r++)
		for (int c = 0; c < COLS; c++)
			if ((*displayed)[r][c] && (*displayed)[r][c] != 0x20)
				return false;
	return true;
}

std::string Cea608Decoder::RowText(const Memory &mem, int r)
{
	std::string out;
	int first = -1, last = -1;
	for (int c = 0; c < COLS; c++) {
		char32_t ch = mem[r][c];
		if (ch && ch != 0x20) {
			if (first < 0)
				first = c;
			last = c;
		}
	}

	for (int c = first; first >= 0 && c <= last; c++) {
		char32_t ch = mem[r][c];
		AppendUtf8(out, ch ? ch : 0x20);
	}
	return out;
}

std::string Cea608Decoder::DisplayedText() const
{
	std::string out;

	for (int r = 0; r < ROWS; r++) {
		std::string row = RowText(*displayed, r);
		if (row.empty())
			continue;
		if (!out.empty())
			out += '\n';
		out += row;
	}

	return out;
}

void Cea608Decoder::FinishRow(const Memory &mem, int r)
{
	std::string row = RowText(mem, r);
	if (!row.empty())
		finished.push_back(row);
}

void Cea608Decoder::FinishAll(const Memory &mem)
{
	for (int r = 0; r < ROWS; r++)
		FinishRow(mem, r);
}

void Cea608Decoder::TakeFinishedLines(std::vector<std::string> &out)
{
	out.insert(out.end(), finished.begin(), finished.end());
	finished.clear();
}

/* ------------------------------------------------------------------------ */

void Cea608Decoder::PutChar(char32_t ch)
{
	if (mode == Mode::None || mode == Mode::Text)
		return;

	Memory &mem = Target();
	mem[row][col] = ch;
	if (col < COLS - 1)
		col++;
}

void Cea608Decoder::Backspace()
{
	if (mode == Mode::None || mode == Mode::Text)
		return;

	if (col > 0)
		col--;
	Target()[row][col] = 0;
}

void Cea608Decoder::StartRollUp(int rows)
{
	if (mode != Mode::RollUp) {
		/* switching into roll-up from another style erases everything */
		ClearMemory(*displayed);
		ClearMemory(*nonDisplayed);
		baseRow = ROWS - 1;
		row = baseRow;
		col = 0;
	} else {
		/* shrinking the window: clear rows that fall outside of it */
		for (int r = 0; r <= baseRow - rows; r++)
			ClearRow(*displayed, r);
	}

	mode = Mode::RollUp;
	rollRows = rows;
	if (baseRow < rollRows - 1)
		baseRow = rollRows - 1;
	row = baseRow;
}

void Cea608Decoder::CarriageReturn()
{
	if (mode != Mode::RollUp)
		return;

	int top = baseRow - rollRows + 1;
	if (top < 0)
		top = 0;

	FinishRow(*displayed, baseRow);

	for (int r = top; r < baseRow; r++) {
		for (int c = 0; c < COLS; c++)
			(*displayed)[r][c] = (*displayed)[r + 1][c];
	}
	ClearRow(*displayed, baseRow);

	/* anything above the window is no longer shown */
	for (int r = 0; r < top; r++)
		ClearRow(*displayed, r);

	row = baseRow;
	col = 0;
}

void Cea608Decoder::MoveRollUpWindow(int newBase)
{
	if (newBase < rollRows - 1)
		newBase = rollRows - 1;
	if (newBase == baseRow)
		return;

	char32_t window[ROWS][COLS] = {};
	for (int i = 0; i < rollRows; i++) {
		int src = baseRow - i;
		if (src < 0)
			break;
		for (int c = 0; c < COLS; c++)
			window[i][c] = (*displayed)[src][c];
	}

	ClearMemory(*displayed);

	for (int i = 0; i < rollRows; i++) {
		int dst = newBase - i;
		if (dst < 0)
			break;
		for (int c = 0; c < COLS; c++)
			(*displayed)[dst][c] = window[i][c];
	}

	baseRow = newBase;
}

void Cea608Decoder::HandlePAC(uint8_t c1, uint8_t c2)
{
	int r = pacRows[c1 & 0x07][(c2 & 0x20) ? 1 : 0];
	if (r < 0)
		return;

	uint8_t attr = c2 & 0x1F;
	int indent = (attr >= 0x10) ? ((attr >> 1) & 0x07) * 4 : 0;

	if (mode == Mode::RollUp) {
		MoveRollUpWindow(r);
		row = baseRow;
	} else {
		row = r;
	}

	col = indent;
}

void Cea608Decoder::HandleMiscControl(uint8_t c2)
{
	switch (c2) {
	case 0x20: /* RCL: resume caption loading (pop-on) */
		if (mode == Mode::PaintOn)
			FinishAll(*displayed);
		else if (mode == Mode::RollUp)
			FinishRow(*displayed, baseRow);
		mode = Mode::PopOn;
		break;
	case 0x21: /* BS: backspace */
		Backspace();
		break;
	case 0x22: /* AOF: reserved */
	case 0x23: /* AON: reserved */
		break;
	case 0x24: /* DER: delete to end of row */
		if (mode != Mode::None && mode != Mode::Text) {
			Memory &mem = Target();
			for (int c = col; c < COLS; c++)
				mem[row][c] = 0;
		}
		break;
	case 0x25: /* RU2 */
		StartRollUp(2);
		break;
	case 0x26: /* RU3 */
		StartRollUp(3);
		break;
	case 0x27: /* RU4 */
		StartRollUp(4);
		break;
	case 0x28: /* FON: flash on */
		break;
	case 0x29: /* RDC: resume direct captioning (paint-on) */
		mode = Mode::PaintOn;
		break;
	case 0x2A: /* TR: text restart */
	case 0x2B: /* RTD: resume text display */
		mode = Mode::Text;
		break;
	case 0x2C: /* EDM: erase displayed memory */
		if (mode == Mode::PaintOn)
			FinishAll(*displayed);
		else if (mode == Mode::RollUp)
			FinishRow(*displayed, baseRow);
		ClearMemory(*displayed);
		break;
	case 0x2D: /* CR: carriage return */
		CarriageReturn();
		break;
	case 0x2E: /* ENM: erase non-displayed memory */
		ClearMemory(*nonDisplayed);
		break;
	case 0x2F: /* EOC: end of caption (flip memories) */
		if (mode == Mode::PaintOn)
			FinishAll(*displayed);
		std::swap(displayed, nonDisplayed);
		mode = Mode::PopOn;
		FinishAll(*displayed);
		break;
	}
}

void Cea608Decoder::HandleControl(uint8_t c1, uint8_t c2)
{
	/* c1 has already had its channel bit (0x08) removed: 0x10-0x17 */

	if (c2 >= 0x40) {
		HandlePAC(c1, c2);
		return;
	}

	switch (c1) {
	case 0x10:
		/* 0x10 0x20-0x2F: background attributes (optional) - ignore */
		break;

	case 0x11:
		if (c2 >= 0x20 && c2 <= 0x2F) {
			/* mid-row style code: shown as a space */
			PutChar(0x20);
		} else if (c2 >= 0x30 && c2 <= 0x3F) {
			PutChar(specialChars[c2 - 0x30]);
		}
		break;

	case 0x12:
	case 0x13:
		if (c2 >= 0x20 && c2 <= 0x3F) {
			/* extended characters replace the standard character
			 * that was sent just before them as a fallback */
			const char32_t *table = (c1 == 0x12) ? extendedChars12 : extendedChars13;
			Backspace();
			PutChar(table[c2 - 0x20]);
		}
		break;

	case 0x14:
	case 0x15:
		if (c2 >= 0x20 && c2 <= 0x2F)
			HandleMiscControl(c2);
		break;

	case 0x17:
		if (c2 >= 0x21 && c2 <= 0x23) {
			/* tab offsets 1-3 */
			col += c2 - 0x20;
			if (col > COLS - 1)
				col = COLS - 1;
		}
		/* other 0x17 codes are attributes - ignore */
		break;

	default:
		break;
	}
}

Cea608Decoder::PairResult Cea608Decoder::Decode(uint8_t b1, uint8_t b2)
{
	bool p1 = OddParity(b1);
	bool p2 = OddParity(b2);
	uint8_t c1 = b1 & 0x7F;
	uint8_t c2 = b2 & 0x7F;

	if (c1 == 0 && c2 == 0 && p1 && p2)
		return PairResult::Padding;

	if (!p1)
		return PairResult::ParityError;

	/* ---- control codes (0x10-0x1F) ---- */
	if (c1 >= 0x10 && c1 <= 0x1F) {
		if (!p2)
			return PairResult::ParityError;

		inXds = false;

		/* control codes are sent twice; ignore the redundant copy */
		if (havePrevControl && c1 == prevC1 && c2 == prevC2) {
			havePrevControl = false;
			return PairResult::Repeat;
		}
		havePrevControl = true;
		prevC1 = c1;
		prevC2 = c2;

		/* 0x14/0x1C misc codes are CC1/CC2 (field 1); 0x15/0x1D misc
		 * codes are CC3/CC4 (field 2). */
		if ((c1 & 0xF7) == 0x15 && c2 >= 0x20 && c2 <= 0x2F)
			dataChannel = (c1 & 0x08) ? 4 : 3;
		else
			dataChannel = (c1 & 0x08) ? 2 : 1;
		if (dataChannel != channel)
			return PairResult::OtherChannel;

		HandleControl(c1 & 0xF7, c2);
		return PairResult::Data;
	}

	havePrevControl = false;

	/* ---- XDS (field 2 only, but be safe) ---- */
	if (c1 >= 0x01 && c1 <= 0x0F) {
		inXds = (c1 != 0x0F);
		return PairResult::OtherChannel;
	}
	if (inXds)
		return PairResult::OtherChannel;

	/* ---- printable characters ---- */
	if (dataChannel != channel)
		return PairResult::OtherChannel;

	if (c1 >= 0x20)
		PutChar(BasicChar(c1));
	if (c2 >= 0x20 && p2)
		PutChar(BasicChar(c2));

	return PairResult::Data;
}

/* ------------------------------------------------------------------------ */

static const char *miscNames[16] = {"RCL", "BS",  "AOF", "AON", "DER", "RU2", "RU3", "RU4",
				    "FON", "RDC", "TR",  "RTD", "EDM", "CR",  "ENM", "EOC"};

std::string Cea608Decoder::Describe(uint8_t b1, uint8_t b2)
{
	uint8_t c1 = b1 & 0x7F;
	uint8_t c2 = b2 & 0x7F;
	char buf[64];

	if (c1 == 0 && c2 == 0)
		return "(null)";

	if (c1 >= 0x10 && c1 <= 0x1F) {
		uint8_t base = c1 & 0xF7;
		int ch = (c1 & 0x08) ? 2 : 1;
		if (base == 0x15 && c2 >= 0x20 && c2 <= 0x2F)
			ch += 2;

		if (c2 >= 0x40) {
			int r = pacRows[base & 0x07][(c2 & 0x20) ? 1 : 0];
			uint8_t attr = c2 & 0x1F;
			int indent = (attr >= 0x10) ? ((attr >> 1) & 0x07) * 4 : 0;
			snprintf(buf, sizeof(buf), "CC%d PAC row %d col %d", ch, r + 1, indent);
		} else if ((base == 0x14 || base == 0x15) && c2 >= 0x20 && c2 <= 0x2F) {
			snprintf(buf, sizeof(buf), "CC%d %s", ch, miscNames[c2 - 0x20]);
		} else if (base == 0x11 && c2 >= 0x30 && c2 <= 0x3F) {
			snprintf(buf, sizeof(buf), "CC%d special char", ch);
		} else if (base == 0x11 && c2 >= 0x20 && c2 <= 0x2F) {
			snprintf(buf, sizeof(buf), "CC%d mid-row style", ch);
		} else if ((base == 0x12 || base == 0x13) && c2 >= 0x20 && c2 <= 0x3F) {
			snprintf(buf, sizeof(buf), "CC%d extended char", ch);
		} else if (base == 0x17 && c2 >= 0x21 && c2 <= 0x23) {
			snprintf(buf, sizeof(buf), "CC%d tab %d", ch, c2 - 0x20);
		} else {
			snprintf(buf, sizeof(buf), "CC%d control", ch);
		}
		return buf;
	}

	if (c1 < 0x10)
		return "XDS";

	std::string s = "'";
	AppendUtf8(s, BasicChar(c1));
	if (c2 >= 0x20)
		AppendUtf8(s, BasicChar(c2));
	s += "'";
	return s;
}
