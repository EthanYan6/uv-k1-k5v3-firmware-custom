/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include <string.h>

#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "font.h"
#include "ui/helper.h"
#include "ui/inputbox.h"
#include "misc.h"


void UI_GenerateChannelString(char *pString, const uint16_t Channel)
{
    unsigned int i;

    if (gInputBoxIndex == 0)
    {
        sprintf(pString, "CH-%02u", Channel + 1);
        return;
    }

    pString[0] = 'C';
    pString[1] = 'H';
    pString[2] = '-';
    for (i = 0; i < 2; i++)
        pString[i + 3] = (gInputBox[i] == 10) ? '-' : gInputBox[i] + '0';
}

void UI_GenerateChannelStringEx(char *pString, const bool bShowPrefix, const uint16_t ChannelNumber)
{
    if (gInputBoxIndex > 0) {
        for (unsigned int i = 0; i < 3; i++) {
            pString[i] = (gInputBox[i] == 10) ? '-' : gInputBox[i] + '0';
        }

        pString[3] = 0;
        return;
    }

    if (bShowPrefix) {
        // BUG here? Prefixed NULLs are allowed
        sprintf(pString, "CH-%03u", ChannelNumber + 1);
    } else if (ChannelNumber == MR_CHANNEL_LAST + 1) {
        strcpy(pString, "None");
    } else if (ChannelNumber == 0xFFFF) {
        strcpy(pString, "NULL");
    } else {
        sprintf(pString, "%03u", ChannelNumber + 1);
    }
}

void UI_PrintStringBuffer(const char *pString, uint8_t * buffer, uint32_t char_width, const uint8_t *font)
{
    const size_t Length = strlen(pString);
    const unsigned int char_spacing = char_width + 1;
    for (size_t i = 0; i < Length; i++) {
        const unsigned int index = pString[i] - ' ' - 1;
        if (pString[i] > ' ' && pString[i] < 127) {
            const uint32_t offset = i * char_spacing + 1;
            memcpy(buffer + offset, font + index * char_width, char_width);
        }
    }
}

void UI_PrintString(const char *pString, uint8_t Start, uint8_t End, uint8_t Line, uint8_t Width)
{
    size_t i;
    size_t Length = strlen(pString);

    if (End > Start)
        Start += (((End - Start) - (Length * Width)) + 1) / 2;

    for (i = 0; i < Length; i++)
    {
        const unsigned int ofs   = (unsigned int)Start + (i * Width);
        if (pString[i] > ' ' && pString[i] < 127)
        {
            const unsigned int index = pString[i] - ' ' - 1;
            memcpy(gFrameBuffer[Line + 0] + ofs, &gFontBig[index][0], 7);
            memcpy(gFrameBuffer[Line + 1] + ofs, &gFontBig[index][7], 7);
        }
    }
}

void UI_PrintStringSmall(const char *pString, uint8_t Start, uint8_t End, uint8_t Line, uint8_t char_width, const uint8_t *font)
{
    const size_t Length = strlen(pString);
    const unsigned int char_spacing = char_width + 1;

    if (End > Start) {
        Start += (((End - Start) - Length * char_spacing) + 1) / 2;
    }

    UI_PrintStringBuffer(pString, gFrameBuffer[Line] + Start, char_width, font);
}

/* 黑底白字：仅在字体笔画处清空像素，不整块反色；适用于任意长度与字体 */
static void UI_PrintStringBufferNegative(const char *pString, uint8_t *buffer, uint32_t char_width, const uint8_t *font)
{
    const size_t Length = strlen(pString);
    const unsigned int char_spacing = char_width + 1;
    for (size_t i = 0; i < Length; i++) {
        if (pString[i] <= ' ' || pString[i] >= 127) continue;
        const unsigned int index = (unsigned int)(pString[i] - ' ' - 1);
        const uint32_t offset = i * char_spacing + 1;
        for (uint32_t c = 0; c < char_width; c++) {
            buffer[offset + c] &= ~(font[index * char_width + c]);
        }
    }
}


void UI_PrintStringSmallNormal(const char *pString, uint8_t Start, uint8_t End, uint8_t Line)
{
    UI_PrintStringSmall(pString, Start, End, Line, ARRAY_SIZE(gFontSmall[0]), (const uint8_t *)gFontSmall);
}

#ifdef ENABLE_FEAT_F4HWN
/* 小号正文字体画在单行内且底对齐：字体落在该行下 7 像素，与 8 像素高大字底边对齐，不跨行 */
void UI_PrintStringSmallNormalBottomInRow(const char *pString, uint8_t Start, uint8_t End, uint8_t Line)
{
    if (Line >= 8) return;
    const size_t Length = strlen(pString);
    const uint8_t char_width = ARRAY_SIZE(gFontSmall[0]);
    const unsigned int char_spacing = char_width + 1;
    uint8_t x_start = Start;
    if (End > Start && Length > 0)
        x_start += (((End - Start) - (unsigned int)Length * char_spacing) + 1) / 2;
    uint8_t *buffer = gFrameBuffer[Line];
    for (size_t i = 0; i < Length; i++) {
        if (pString[i] <= ' ' || pString[i] >= 127) continue;
        const unsigned int index = (unsigned int)(pString[i] - ' ' - 1);
        const uint32_t col_offset = i * char_spacing + 1;
        for (uint8_t c = 0; c < char_width && (x_start + col_offset + c) < 128; c++) {
            uint8_t V = gFontSmall[index][c];
            buffer[x_start + col_offset + c] |= (uint8_t)(V >> 1);  /* 下 7 像素，底对齐 */
        }
    }
}

/* 小号正文在像素 (x,y) 处整体绘制，y 需为 8 的倍数；整段文字一起移动，不拆行 */
void UI_PrintStringSmallNormalAt(const char *pString, uint8_t x, uint8_t y)
{
    if (y >= 64) return;
    const uint8_t row = (uint8_t)(y / 8u);
    const uint8_t char_width = ARRAY_SIZE(gFontSmall[0]);
    UI_PrintStringBuffer(pString, gFrameBuffer[row] + x, char_width, (const uint8_t *)gFontSmall);
}
#endif

/* 黑底白字：仅在笔画处清空，不整块反色；与 UI_PrintStringSmallNormal 同参数、同居中 */
void UI_PrintStringSmallNormalNegative(const char *pString, uint8_t Start, uint8_t End, uint8_t Line)
{
    const size_t Length = strlen(pString);
    const uint8_t char_width = ARRAY_SIZE(gFontSmall[0]);
    const unsigned int char_spacing = char_width + 1;
    uint8_t x_start = Start;
    if (End > Start && Length > 0)
        x_start += (((End - Start) - (unsigned int)Length * char_spacing) + 1) / 2;
    UI_PrintStringBufferNegative(pString, gFrameBuffer[Line] + x_start, char_width, (const uint8_t *)gFontSmall);
}

#ifdef ENABLE_FEAT_F4HWN
/* 黑底白字且行顶留 1 像素黑（字体下移 1 像素）：用于按钮内文字视觉居中 */
static void UI_PrintStringBufferNegativeTopMargin1(const char *pString, uint8_t *buffer, uint32_t char_width, const uint8_t *font)
{
    const size_t Length = strlen(pString);
    const unsigned int char_spacing = char_width + 1;
    for (size_t i = 0; i < Length; i++) {
        if (pString[i] <= ' ' || pString[i] >= 127) continue;
        const unsigned int index = (unsigned int)(pString[i] - ' ' - 1);
        const uint32_t offset = i * char_spacing + 1;
        for (uint32_t c = 0; c < char_width; c++) {
            const uint8_t V = font[index * char_width + c];
            buffer[offset + c] &= (uint8_t)~((V >> 1) << 1);  /* 顶 1 bit 不清，字体下移 1 像素 */
        }
    }
}
void UI_PrintStringSmallNormalNegativeTopMargin1(const char *pString, uint8_t Start, uint8_t End, uint8_t Line)
{
    const size_t Length = strlen(pString);
    const uint8_t char_width = ARRAY_SIZE(gFontSmall[0]);
    const unsigned int char_spacing = char_width + 1;
    uint8_t x_start = Start;
    if (End > Start && Length > 0)
        x_start += (((End - Start) - (unsigned int)Length * char_spacing) + 1) / 2;
    UI_PrintStringBufferNegativeTopMargin1(pString, gFrameBuffer[Line] + x_start, char_width, (const uint8_t *)gFontSmall);
}
#endif

#ifdef ENABLE_FEAT_F4HWN
/* 小号正文字体，带垂直像素偏移 vOffset(0~7)，用于小幅度上下移动 */
void UI_PrintStringSmallNormalVOffset(const char *pString, uint8_t Start, uint8_t End, uint8_t Line, uint8_t vOffset)
{
    if (Line >= 7) return;
    const size_t Length = strlen(pString);
    const uint8_t char_width = ARRAY_SIZE(gFontSmall[0]);
    const unsigned int char_spacing = char_width + 1;
    uint8_t x_start = Start;
    if (End > Start && Length > 0)
        x_start += (((End - Start) - (unsigned int)Length * char_spacing) + 1) / 2;

    if (vOffset == 0) {
        UI_PrintStringSmallNormal(pString, Start, End, Line);
        return;
    }
    /* 下移 vOffset 像素：字顶 (8-vOffset) bit -> 当前行底 (8-vOffset) bit，字底 vOffset bit -> 下一行顶 vOffset bit */
    for (size_t i = 0; i < Length; i++) {
        if (pString[i] <= ' ' || pString[i] >= 127) continue;
        const unsigned int index = (unsigned int)(pString[i] - ' ' - 1);
        const uint32_t col_offset = i * char_spacing + 1;
        for (uint8_t c = 0; c < char_width && (x_start + col_offset + c) < 128; c++) {
            uint8_t V = gFontSmall[index][c];
            uint8_t *p0 = &gFrameBuffer[Line][x_start + col_offset + c];
            uint8_t *p1 = &gFrameBuffer[Line + 1][x_start + col_offset + c];
            *p0 |= (V >> vOffset) << vOffset;
            *p1 |= (uint8_t)((V & ((1u << vOffset) - 1u)) << (8u - vOffset));
        }
    }
}

/* 小号正文字体，向上偏移 upOffset 像素(1~7)，仅上移不跑到顶行；MSB=屏顶 */
void UI_PrintStringSmallNormalVOffsetUp(const char *pString, uint8_t Start, uint8_t End, uint8_t Line, uint8_t upOffset)
{
    if (Line < 1 || Line >= 7 || upOffset == 0 || upOffset > 7) return;
    const size_t Length = strlen(pString);
    const uint8_t char_width = ARRAY_SIZE(gFontSmall[0]);
    const unsigned int char_spacing = char_width + 1;
    uint8_t x_start = Start;
    if (End > Start && Length > 0)
        x_start += (((End - Start) - (unsigned int)Length * char_spacing) + 1) / 2;
    /* 上移：字顶 upOffset bit(MSB) -> 上一行底；字底 (8-upOffset) bit -> 当前行顶 */
    for (size_t i = 0; i < Length; i++) {
        if (pString[i] <= ' ' || pString[i] >= 127) continue;
        const unsigned int index = (unsigned int)(pString[i] - ' ' - 1);
        const uint32_t col_offset = i * char_spacing + 1;
        for (uint8_t c = 0; c < char_width && (x_start + col_offset + c) < 128; c++) {
            uint8_t V = gFontSmall[index][c];
            uint8_t *p0 = &gFrameBuffer[Line - 1][x_start + col_offset + c];
            uint8_t *p1 = &gFrameBuffer[Line][x_start + col_offset + c];
            *p0 |= (V >> (8u - upOffset));  /* 字顶 upOffset bit -> 上一行底 */
            *p1 |= (uint8_t)((V << upOffset) & 0xFF);  /* 字底 (8-upOffset) bit -> 当前行顶 */
        }
    }
}
#endif

void UI_PrintStringSmallNormalInverse(const char *pString, uint8_t Start, uint8_t End, uint8_t Line)
{
    // First draw the string normally (this may center and thus change effective Start)
    UI_PrintStringSmallNormal(pString, Start, End, Line);

    // Invert the framebuffer over the *actual* text region (same centering as UI_PrintStringSmall)
    const size_t len = strlen(pString);
    const unsigned int char_width = 6;  // gFontSmall width
    const unsigned int char_spacing = char_width + 1;  // 7
    uint8_t x_start = Start;
    if (End > Start && len > 0) {
        x_start += (((End - Start) - (unsigned int)len * char_spacing) + 1) / 2;
    }
    uint8_t x_end = x_start + (uint8_t)(len * char_spacing) + 1;
    if (End != 0 && x_end > End)
        x_end = End;

    // Clamp to avoid out-of-bounds (e.g. x_start-3 underflow)
    if (x_start >= 3) {
        gFrameBuffer[Line][x_start - 3] ^= 0x3E;
        gFrameBuffer[Line][x_start - 2] ^= 0x7F;
        gFrameBuffer[Line][x_start - 1] ^= 0xFF;
    }
    /* 只反色文字所在行；小字体只画在这一行，反色上一行会破坏分隔线/按钮边框 */
    for (uint8_t x = x_start; x < x_end && x < 128; x++) {
        gFrameBuffer[Line][x] ^= 0xFF;
    }
    if (x_end < 128) {
        gFrameBuffer[Line][x_end + 0] ^= 0xFF;
        if (x_end + 1 < 128) gFrameBuffer[Line][x_end + 1] ^= 0x7F;
        if (x_end + 2 < 128) gFrameBuffer[Line][x_end + 2] ^= 0x3E;
    }
}


void UI_PrintStringSmallBold(const char *pString, uint8_t Start, uint8_t End, uint8_t Line)
{
#ifdef ENABLE_SMALL_BOLD
    const uint8_t *font = (uint8_t *)gFontSmallBold;
    const uint8_t char_width = ARRAY_SIZE(gFontSmallBold[0]);
#else
    const uint8_t *font = (uint8_t *)gFontSmall;
    const uint8_t char_width = ARRAY_SIZE(gFontSmall[0]);
#endif

    UI_PrintStringSmall(pString, Start, End, Line, char_width, font);
}

/* 黑底白字，Bold 小字：仅在笔画处清空 */
void UI_PrintStringSmallBoldNegative(const char *pString, uint8_t Start, uint8_t End, uint8_t Line)
{
#ifdef ENABLE_SMALL_BOLD
    const uint8_t *font = (uint8_t *)gFontSmallBold;
    const uint8_t char_width = ARRAY_SIZE(gFontSmallBold[0]);
#else
    const uint8_t *font = (uint8_t *)gFontSmall;
    const uint8_t char_width = ARRAY_SIZE(gFontSmall[0]);
#endif
    const size_t Length = strlen(pString);
    const unsigned int char_spacing = char_width + 1;
    uint8_t x_start = Start;
    if (End > Start && Length > 0)
        x_start += (((End - Start) - (unsigned int)Length * char_spacing) + 1) / 2;
    UI_PrintStringBufferNegative(pString, gFrameBuffer[Line] + x_start, char_width, font);
}

void UI_PrintStringSmallBoldInverse(const char *pString, uint8_t Start, uint8_t End, uint8_t Line)
{
    // First draw the string normally
    UI_PrintStringSmallBold(pString, Start, End, Line);

    // Now invert the framebuffer bits for the rendered area
    uint8_t len = strlen(pString);
    uint8_t char_width = 7; // small font is typically 6px wide

    uint8_t x_start = Start;
    uint8_t x_end   = Start + (len * char_width);

    if (End != 0 && x_end > End)
        x_end = End;

    gFrameBuffer[Line][x_start] ^= 0x7F;
    for (uint8_t x = x_start + 1; x < x_end; x++)
    {
        gFrameBuffer[Line][x] ^= 0x41;
    }
    gFrameBuffer[Line][x_end + 1] ^= 0x7F;
}


void UI_PrintStringSmallBufferNormal(const char *pString, uint8_t * buffer)
{
    UI_PrintStringBuffer(pString, buffer, ARRAY_SIZE(gFontSmall[0]), (uint8_t *)gFontSmall);
}

void UI_PrintStringSmallBufferBold(const char *pString, uint8_t * buffer)
{
#ifdef ENABLE_SMALL_BOLD
    const uint8_t *font = (uint8_t *)gFontSmallBold;
    const uint8_t char_width = ARRAY_SIZE(gFontSmallBold[0]);
#else
    const uint8_t *font = (uint8_t *)gFontSmall;
    const uint8_t char_width = ARRAY_SIZE(gFontSmall[0]);
#endif
    UI_PrintStringBuffer(pString, buffer, char_width, font);
}

void UI_DisplayFrequency(const char *string, uint8_t X, uint8_t Y, bool center)
{
    const unsigned int char_width  = 13;
    uint8_t           *pFb0        = gFrameBuffer[Y] + X;
    uint8_t           *pFb1        = pFb0 + 128;
    bool               bCanDisplay = false;

    uint8_t len = strlen(string);
    for(int i = 0; i < len; i++) {
        char c = string[i];
        if(c=='-') c = '9' + 1;
        if (bCanDisplay || c != ' ')
        {
            bCanDisplay = true;
            if(c>='0' && c<='9' + 1) {
                memcpy(pFb0 + 2, gFontBigDigits[c-'0'],                  char_width - 3);
                memcpy(pFb1 + 2, gFontBigDigits[c-'0'] + char_width - 3, char_width - 3);
            }
            else if(c=='.') {
                *pFb1 = 0x60; pFb0++; pFb1++;
                *pFb1 = 0x60; pFb0++; pFb1++;
                *pFb1 = 0x60; pFb0++; pFb1++;
                continue;
            }

        }
        else if (center) {
            pFb0 -= 6;
            pFb1 -= 6;
        }
        pFb0 += char_width;
        pFb1 += char_width;
    }
}

#ifdef ENABLE_FEAT_F4HWN
/* 大号频率整体上移 2 像素：多画一行(Y-1)接住字形顶部 2 像素，整字一起上移。maxXTop 表示 row Y-1 只写到列 < maxXTop，避免盖住信道名 */
void UI_DisplayFrequencyUp2(const char *string, uint8_t X, uint8_t Y, bool center, uint8_t maxXTop)
{
    if (Y < 1 || Y + 1 >= 7) {
        UI_DisplayFrequency(string, X, Y, center);
        return;
    }
    const unsigned int char_width = 13;
    uint8_t *pFbM1 = gFrameBuffer[Y - 1] + X;
    uint8_t *pFb0  = gFrameBuffer[Y] + X;
    uint8_t *pFb1  = gFrameBuffer[Y + 1] + X;
    bool bCanDisplay = false;
    uint8_t len = strlen(string);
    for (int i = 0; i < len; i++) {
        char c = string[i];
        if (c == '-') c = '9' + 1;
        if (bCanDisplay || c != ' ') {
            bCanDisplay = true;
            if (c >= '0' && c <= '9' + 1) {
                const uint8_t *glyph = gFontBigDigits[c - '0'];
                for (int col = 0; col < 10; col++) {
                    uint8_t b0 = glyph[col];
                    uint8_t b1 = glyph[10 + col];
                    uint16_t xcol = (uint16_t)(X + 2 + col);
                    if (maxXTop == 0 || xcol < maxXTop)
                        pFbM1[2 + col] |= (uint8_t)(b0 >> 6);
                    pFb0[2 + col]  = (uint8_t)((b0 << 2) | (b1 >> 6));
                    pFb1[2 + col]  = (uint8_t)(b1 << 2);
                }
            } else if (c == '.') {
                uint8_t v = 0x60;
                for (int k = 0; k < 3; k++) {
                    if (maxXTop == 0 || (uint16_t)(pFb0 - gFrameBuffer[Y]) < maxXTop)
                        *pFbM1 |= (uint8_t)(v >> 6);
                    *pFb0  = (uint8_t)(v << 2);
                    *pFb1  = 0u;
                    pFbM1++; pFb0++; pFb1++;
                }
                continue;
            }
        } else if (center) {
            pFbM1 -= 6;
            pFb0 -= 6;
            pFb1 -= 6;
        }
        pFbM1 += char_width;
        pFb0 += char_width;
        pFb1 += char_width;
    }
}
#endif

/*
void UI_DisplayFrequency(const char *string, uint8_t X, uint8_t Y, bool center)
{
    const unsigned int char_width  = 13;
    uint8_t           *pFb0        = gFrameBuffer[Y] + X;
    uint8_t           *pFb1        = pFb0 + 128;
    bool               bCanDisplay = false;

    if (center) {
        uint8_t len = 0;
        for (const char *ptr = string; *ptr; ptr++)
            if (*ptr != ' ') len++; // Ignores spaces for centering

        X -= (len * char_width) / 2; // Centering adjustment
        pFb0 = gFrameBuffer[Y] + X;
        pFb1 = pFb0 + 128;
    }

    for (; *string; string++) {
        char c = *string;
        if (c == '-') c = '9' + 1; // Remap of '-' symbol

        if (bCanDisplay || c != ' ') {
            bCanDisplay = true;
            if (c >= '0' && c <= '9' + 1) {
                memcpy(pFb0 + 2, gFontBigDigits[c - '0'], char_width - 3);
                memcpy(pFb1 + 2, gFontBigDigits[c - '0'] + char_width - 3, char_width - 3);
            } else if (c == '.') {
                memset(pFb1, 0x60, 3); // Replaces the three assignments
                pFb0 += 3;
                pFb1 += 3;
                continue;
            }
        }
        pFb0 += char_width;
        pFb1 += char_width;
    }
}
*/

void UI_DrawPixelBuffer(uint8_t (*buffer)[128], uint8_t x, uint8_t y, bool black)
{
    const uint8_t pattern = 1 << (y % 8);
    if(black)
        buffer[y/8][x] |= pattern;
    else
        buffer[y/8][x] &= ~pattern;
}

static void sort(int16_t *a, int16_t *b)
{
    if(*a > *b) {
        int16_t t = *a;
        *a = *b;
        *b = t;
    }
}

#ifdef ENABLE_FEAT_F4HWN
    /*
    void UI_DrawLineDottedBuffer(uint8_t (*buffer)[128], int16_t x1, int16_t y1, int16_t x2, int16_t y2, bool black)
    {
        if(x2==x1) {
            sort(&y1, &y2);
            for(int16_t i = y1; i <= y2; i+=2) {
                UI_DrawPixelBuffer(buffer, x1, i, black);
            }
        } else {
            const int multipl = 1000;
            int a = (y2-y1)*multipl / (x2-x1);
            int b = y1 - a * x1 / multipl;

            sort(&x1, &x2);
            for(int i = x1; i<= x2; i+=2)
            {
                UI_DrawPixelBuffer(buffer, i, i*a/multipl +b, black);
            }
        }
    }
    */

    void PutPixel(uint8_t x, uint8_t y, bool fill) {
      UI_DrawPixelBuffer(gFrameBuffer, x, y, fill);
    }

    void PutPixelStatus(uint8_t x, uint8_t y, bool fill) {
      UI_DrawPixelBuffer(&gStatusLine, x, y, fill);
    }

    void GUI_DisplaySmallest(const char *pString, uint8_t x, uint8_t y,
                                    bool statusbar, bool fill) {
      uint8_t c;
      uint8_t pixels;
      const uint8_t *p = (const uint8_t *)pString;

      while ((c = *p++) && c != '\0') {
        c -= 0x20;
        for (int i = 0; i < 3; ++i) {
          pixels = gFont3x5[c][i];
          for (int j = 0; j < 6; ++j) {
            if (pixels & 1) {
              if (statusbar)
                PutPixelStatus(x + i, y + j, fill);
              else
                PutPixel(x + i, y + j, fill);
            }
            pixels >>= 1;
          }
        }
        x += 4;
      }
    }

    /* 最小字反色：黑底白字，先涂黑再在笔画处清空，画到 gFrameBuffer */
    void GUI_DisplaySmallestNegative(const char *pString, uint8_t x, uint8_t y) {
      const size_t len = strlen(pString);
      if (len == 0) return;
      const uint8_t w = (uint8_t)(len * 4);
      const uint8_t h = 6;
      for (uint8_t py = 0; py < h && (y + py) < 64; py++)
        for (uint8_t px = 0; px < w && (x + px) < 128; px++)
          UI_DrawPixelBuffer(gFrameBuffer, x + px, y + py, true);
      const uint8_t *p = (const uint8_t *)pString;
      uint8_t cx = x;
      uint8_t c;
      while ((c = *p++) && c != '\0') {
        c -= 0x20;
        for (int i = 0; i < 3; ++i) {
          uint8_t pixels = gFont3x5[c][i];
          for (int j = 0; j < 6; ++j) {
            if (pixels & 1)
              UI_DrawPixelBuffer(gFrameBuffer, cx + i, y + j, false);
            pixels >>= 1;
          }
        }
        cx += 4;
      }
    }
#endif
    
void UI_DrawLineBuffer(uint8_t (*buffer)[128], int16_t x1, int16_t y1, int16_t x2, int16_t y2, bool black)
{
    if(x2==x1) {
        sort(&y1, &y2);
        for(int16_t i = y1; i <= y2; i++) {
            UI_DrawPixelBuffer(buffer, x1, i, black);
        }
    } else {
        const int multipl = 1000;
        int a = (y2-y1)*multipl / (x2-x1);
        int b = y1 - a * x1 / multipl;

        sort(&x1, &x2);
        for(int i = x1; i<= x2; i++)
        {
            UI_DrawPixelBuffer(buffer, i, i*a/multipl +b, black);
        }
    }
}

void UI_DrawRectangleBuffer(uint8_t (*buffer)[128], int16_t x1, int16_t y1, int16_t x2, int16_t y2, bool black)
{
    UI_DrawLineBuffer(buffer, x1,y1, x1,y2, black);
    UI_DrawLineBuffer(buffer, x1,y1, x2,y1, black);
    UI_DrawLineBuffer(buffer, x2,y1, x2,y2, black);
    UI_DrawLineBuffer(buffer, x1,y2, x2,y2, black);
}


void UI_DisplayPopup(const char *string)
{
    UI_DisplayClear();

    // for(uint8_t i = 1; i < 5; i++) {
    //  memset(gFrameBuffer[i]+8, 0x00, 111);
    // }

    // for(uint8_t x = 10; x < 118; x++) {
    //  UI_DrawPixelBuffer(x, 10, true);
    //  UI_DrawPixelBuffer(x, 46-9, true);
    // }

    // for(uint8_t y = 11; y < 37; y++) {
    //  UI_DrawPixelBuffer(10, y, true);
    //  UI_DrawPixelBuffer(117, y, true);
    // }
    // DrawRectangle(9,9, 118,38, true);
    UI_PrintString(string, 9, 118, 2, 8);
    UI_PrintStringSmallNormal("Press EXIT", 9, 118, 6);
}

void UI_DisplayClear()
{
    memset(gFrameBuffer, 0, sizeof(gFrameBuffer));
}
