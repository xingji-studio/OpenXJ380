#pragma once
#include <proto.hpp>
// font/font.cpp
void WriteAscii(const FrameBufferConfig &fbc, int x, int y, char ch, const PixelColor &c);
void WriteDec(const FrameBufferConfig &fbc, int x, int y, unsigned long long dec, const PixelColor &c);
void WriteString(const FrameBufferConfig &fbc, int x, int y, const char *s, const PixelColor &c);
void WriteHex(const FrameBufferConfig &fbc, int x, int y, unsigned long long hex, const PixelColor &c);
void PrintFont(SHEET_INFO *sht, SHEET *csheet, int x, int y, char ch, const SHEET_BUFFER &color);
void PrintFmt(SHEET_INFO *sht, SHEET *csheet, int x, int y, const SHEET_BUFFER &color, const char *fmt, ...);
void PrintString(SHEET_INFO *sht, SHEET *csheet, int x, int y, const char *s, const SHEET_BUFFER &color);
void PrintHex(SHEET_INFO *sht, SHEET *csheet, int x, int y, unsigned long long hex, const SHEET_BUFFER &color);
void PrintDec(SHEET_INFO *sht, SHEET *csheet, int x, int y, unsigned long long dec, const SHEET_BUFFER &color);
