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
#include <stdlib.h>  // abs()

#include "app/chFrScanner.h"
#include "app/dtmf.h"
#ifdef ENABLE_AM_FIX
    #include "am_fix.h"
#endif
#include "bitmaps.h"
#include "board.h"
#include "font.h"
#include "driver/bk4819.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/helper.h"
#include "ui/battery.h"
#include "ui/inputbox.h"
#include "ui/main.h"
#include "ui/ui.h"
#include "audio.h"
#include "menu.h"

#ifdef ENABLE_FEAT_F4HWN
    #include "driver/system.h"
#endif

center_line_t center_line = CENTER_LINE_NONE;

#ifdef ENABLE_FEAT_F4HWN
    // static int8_t RxBlink;
    static int8_t RxBlinkLed = 0;
    static int8_t RxBlinkLedCounter;
    static int8_t RxLine;
    static uint32_t RxOnVfofrequency;

    bool isMainOnlyInputDTMF = false;

    static bool isMainOnly()
    {
        return (gEeprom.DUAL_WATCH == DUAL_WATCH_OFF) && (gEeprom.CROSS_BAND_RX_TX == CROSS_BAND_OFF);
    }
#endif

const char *VfoStateStr[] = {
       [VFO_STATE_NORMAL]="",
       [VFO_STATE_BUSY]="BUSY",
       [VFO_STATE_BAT_LOW]="BAT LOW",
       [VFO_STATE_TX_DISABLE]="TX DISABLE",
       [VFO_STATE_TIMEOUT]="TIMEOUT",
       [VFO_STATE_ALARM]="ALARM",
       [VFO_STATE_VOLTAGE_HIGH]="VOLT HIGH"
};

#ifdef ENABLE_FEAT_F4HWN
/* Dual-VFO main layout (RxMode != MAIN ONLY): display only; no radio logic changes. */
static bool DualVfoShouldUseLegacyMain(void)
{
#ifdef ENABLE_SCAN_RANGES
    if (gScanRangeStart)
        return true;
#endif
    if (gDTMF_InputMode
#ifdef ENABLE_DTMF_CALLING
        || gDTMF_CallState != DTMF_CALL_STATE_NONE || gDTMF_IsTx
#endif
        )
        return true;
    return false;
}

/* 主界面、频率(VFO)模式、正在键入频率数字：专用全屏输入页，不走双面板/旧布局 */
static bool DualVfoMainFreqEntryScreen(void)
{
    return gScreenToDisplay == DISPLAY_MAIN && !gAirCopyBootMode && gInputBoxIndex > 0 &&
           IS_FREQ_CHANNEL(gEeprom.ScreenChannel[gEeprom.TX_VFO]);
}

static void UI_DisplayMain_FreqInputBare(void)
{
    char            fs[16];
    const char *    ascii     = INPUTBOX_GetAscii();
    const uint32_t  frequency = gEeprom.VfoInfo[gEeprom.TX_VFO].pRX->Frequency;
    const bool      isGigaF   = frequency >= _1GHz_in_KHz;

    sprintf(fs, "%.*s.%.3s", 3 + (unsigned)isGigaF, ascii, ascii + 3 + (unsigned)isGigaF);

    /* 仅大字频率，水平居中；与旧逻辑相同格式，占 framebuffer 两行 */
    UI_PrintString(fs, 0, LCD_WIDTH, 3, 8);
}

static const char *DualVfoPowerWord(uint8_t vfoIdx)
{
    const VFO_Info_t *v = &gEeprom.VfoInfo[vfoIdx];
    uint8_t           p = v->OUTPUT_POWER % 8u;
    if (p == OUTPUT_POWER_USER)
        p = (uint8_t)(gSetting_set_pwr + 1u);
    static const char *const lowNames[5] = {"LOW1", "LOW2", "LOW3", "LOW4", "LOW5"};
    if (p >= 1u && p <= 5u)
        return lowNames[p - 1u];
    if (p == 6u)
        return "MID";
    if (p == 7u)
        return "HIGH";
    return "";
}

static void DualVfoFmtChId(unsigned int vfoIdx, char *out, size_t outLen)
{
    const uint16_t ch = gEeprom.ScreenChannel[vfoIdx];
    if (IS_MR_CHANNEL(ch))
        snprintf(out, outLen, "M-%04u", (unsigned)(ch + 1u));
    else if (IS_FREQ_CHANNEL(ch))
    {
        const uint8_t f   = (uint8_t)(1u + ch - FREQ_CHANNEL_FIRST);
        const bool    gig = gEeprom.VfoInfo[vfoIdx].pRX->Frequency >= _1GHz_in_KHz;
        snprintf(out, outLen, gig ? "F%u+" : "F%u", (unsigned)f);
    }
#ifdef ENABLE_NOAA
    else if (IS_NOAA_CHANNEL(ch))
        snprintf(out, outLen, "N%u", (unsigned)(1u + ch - NOAA_CHANNEL_FIRST));
#endif
    else
        snprintf(out, outLen, "?");
}

static void DualVfoHeaderLeft(unsigned int vfoIdx, char *out, size_t outLen)
{
    const uint16_t ch = gEeprom.ScreenChannel[vfoIdx];
    if (IS_MR_CHANNEL(ch))
    {
        SETTINGS_FetchChannelName(out, ch);
        if (out[0] == 0)
            snprintf(out, outLen, "CH%04u", (unsigned)(ch + 1u));
    }
    else
        snprintf(out, outLen, "VFO");
    out[outLen - 1u] = 0;
    if (strlen(out) > 10u)
        out[10] = 0;
}

static void DualVfoHeaderRight(unsigned int vfoIdx, char *out, size_t outLen)
{
    const VFO_Info_t *v = &gEeprom.VfoInfo[vfoIdx];
#ifdef ENABLE_FEAT_F4HWN_NARROWER
    bool narrower = (v->CHANNEL_BANDWIDTH == BANDWIDTH_NARROW && gSetting_set_nfm == 1);
    const char *bw =
        (v->CHANNEL_BANDWIDTH == BANDWIDTH_WIDE) ? "WIDE" : (narrower ? "NAR+" : "NAR");
#else
    const char *bw = (v->CHANNEL_BANDWIDTH == BANDWIDTH_WIDE) ? "WIDE" : "NAR";
#endif
    snprintf(out, outLen, "%s %s %s", gModulationStr[v->Modulation], bw, DualVfoPowerWord(vfoIdx));
}

/* 最小字宽约 4px；S 表左侧清空到该列，与频率区错开 */
#define DUAL_VFO_FREQ_COL 62u

/*
 * 双 VFO 像素布局；主信道 A/B 宽 7px、高 6px；下面板 A/B 再各 +2px；A/B 下 1px 再画信道号；右侧 2x 频率。
 */
#define DUAL_VFO_AB_TALL_H 6u
#define DUAL_VFO_AB_TALL_W 7u
#define DUAL_VFO_AB_BOT_H  (DUAL_VFO_AB_TALL_H + 2u)
#define DUAL_VFO_AB_BOT_W  (DUAL_VFO_AB_TALL_W + 2u)
#define DV_Y_TOP_HDR   0u
#define DV_Y_TOP_AB    10u /* 主信道 A/B 及其下方信道号下移 2px */
#define DV_Y_TOP_CH    9u  /* 主信道右侧 2x 频率基线 */
#define DV_Y_TOP_DET   22u
#define DV_Y_BOT_HDR   29u
#define DV_Y_BOT_MAIN       39u /* A/B 上移 1px；旁信道号/RX 用 DV_Y_BOT_BESIDE_AB */
#define DV_Y_BOT_BESIDE_AB  (DV_Y_BOT_MAIN + 2u) /* 框旁信道号、RX、TX 字下移 2px */
#define DV_Y_BOT_FREQ_LINE  ((DV_Y_BOT_MAIN + DUAL_VFO_AB_BOT_H + 1u) - 10u) /* 较原单独行位置上移 10px */
/* 底栏：S 表在左；右为电池 8px + 其下居中百分比；DV_Y_METER 对齐电池页顶 */
#define DV_Y_METER          50u /* 电池图标（及底栏）下移 2px */
#define DV_BAT_ICON_H       8u
#define DV_Y_PCT            (DV_Y_METER + DV_BAT_ICON_H - 1u) /* 百分比下移 1px */
#define DV_Y_RXMODE         (DV_Y_METER + 7u)            /* 右下角 A/B 模式上移 4px */
#define DV_BAT_FLUSH_RIGHT  0u /* batX = LCD_WIDTH - batW - 此值，0 即贴右 */
#define DV_BAT_MODE_SHIFT_R (-1) /* 右下角 A/B 模式相对原布局左移 3px（原 +2 -> 现 -1） */
#define DV_BAT_PCT_SHIFT_R  2u /* 电量百分比再右移 */
/* S 表：刻度较原下移 5px；其下横条 + 9 条竖线；S/+dB 与「1-3-5-7-9」右端隔 2px，两行同 x */
#define DV_SMETER_BAR_X0    1u
#define DV_SMETER_BAR_W     33u
#define DV_SMETER_LABEL_Y   (DV_Y_METER + 3u) /* S 表整体上移 2px */
#define DV_SMETER_SCALE_W   (9u * 4u) /* "1-3-5-7-9" 最小字宽 */
#define DV_SMETER_S_GAP     2u        /* 刻度末字符与 S 读数间隔 */
#define DV_SMETER_SREAD_X   (DV_SMETER_SCALE_W + DV_SMETER_S_GAP)
#define DV_SMETER_SVALUE_Y  DV_SMETER_LABEL_Y           /* S 值与刻度同一行 */
#define DV_SMETER_DBB_Y     (DV_SMETER_LABEL_Y + 6u)    /* +dB 与 S 同列 */
#define DV_SMETER_BAR_Y0    (DV_SMETER_LABEL_Y + 6u)
#define DV_SMETER_VLINE_Y0  DV_SMETER_BAR_Y0
#define DV_SMETER_VLINE_Y1  63u
#define DV_SMETER_BAR_Y1    DV_SMETER_VLINE_Y1 /* 伸缩条与竖线同高 */

/* 副信道频率：3 列宽 2+2+1px（较原 1px/列总宽约 +2）；CHAR_W 含字后空，间隔较原再减 1px */
#define DUAL_VFO_SUB_FREQ_H       8u
#define DUAL_VFO_SUB_FREQ_CHAR_W  7u
/* A/B 行下方预留 1 像素再显示信道号 */
#define DV_Y_TOP_CHID    (DV_Y_TOP_AB + DUAL_VFO_AB_TALL_H + 1u)
/* Tx 偏提示：仅主信道；副信道不显示 */
#define DV_TXOFS_GAP_L_MAIN 16u

static void DualVfoXorHStripColumns(uint8_t x0, uint8_t yTop, uint8_t yBottom)
{
    for (uint8_t y = yTop; y <= yBottom; y++)
    {
        const uint8_t bit = (uint8_t)(1u << (y % 8u));
        const uint8_t row = (uint8_t)(y / 8u);
        for (uint8_t x = x0; x < LCD_WIDTH; x++)
            gFrameBuffer[row][x] ^= bit;
    }
}

static void DualVfoFillRectBlack(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
    for (uint8_t y = y0; y <= y1; y++)
        for (uint8_t x = x0; x <= x1; x++)
            PutPixel(x, y, true);
}

static void DualVfoClearRectPx(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
    if (y0 > y1 || x0 > x1)
        return;
    for (uint8_t yy = y0; yy <= y1; yy++)
        for (uint8_t xx = x0; xx <= x1 && xx < LCD_WIDTH; xx++)
            PutPixel(xx, yy, false);
}

/* 频率数字样式：用 gFontBigDigits(10x16) 做缩放，接近截图里的粗体数字风格 */
static bool DualVfoBigDigitPixel(char ch, uint8_t sx, uint8_t sy)
{
    if (ch < '0' || ch > '9')
        return false;
    if (sx >= 10u || sy >= 16u)
        return false;

    const uint8_t *glyph = gFontBigDigits[(uint8_t)(ch - '0')];
    if (sy < 8u)
        return ((glyph[sx] >> sy) & 1u) != 0u;
    return ((glyph[10u + sx] >> (sy - 8u)) & 1u) != 0u;
}

static void DualVfoDrawScaledBigDigit(char ch, uint8_t x0, uint8_t y0, uint8_t dstW, uint8_t dstH)
{
    for (uint8_t dx = 0; dx < dstW; dx++)
    {
        const uint8_t sx = (uint8_t)((uint16_t)dx * 10u / dstW);
        for (uint8_t dy = 0; dy < dstH; dy++)
        {
            const uint8_t sy = (uint8_t)((uint16_t)dy * 16u / dstH);
            if (DualVfoBigDigitPixel(ch, sx, sy))
                PutPixel((uint8_t)(x0 + dx), (uint8_t)(y0 + dy), true);
        }
    }
}

/* 主频：每字符 8px 步进，数字本体 7x12，风格接近截图 */
static uint8_t DualVfoDrawString2x(uint8_t x0, uint8_t y0, const char *s)
{
    uint8_t x = x0;
    for (; *s; s++)
    {
        const char ch = *s;
        if (ch >= '0' && ch <= '9')
        {
            DualVfoDrawScaledBigDigit(ch, x, y0, 7u, 12u);
        }
        else if (ch == '.')
        {
            /* 小数点放在下沿，3px 宽 */
            PutPixel((uint8_t)(x + 1u), (uint8_t)(y0 + 10u), true);
            PutPixel((uint8_t)(x + 2u), (uint8_t)(y0 + 10u), true);
            PutPixel((uint8_t)(x + 1u), (uint8_t)(y0 + 11u), true);
            PutPixel((uint8_t)(x + 2u), (uint8_t)(y0 + 11u), true);
        }
        x = (uint8_t)(x + 8u);
    }
    return x;
}

/* 副频：保持原行高 8px，数字本体 5x8，但沿用与主频一致的数字骨架 */
static uint8_t DualVfoDrawStringSubFreqTall(uint8_t x0, uint8_t y0, const char *s)
{
    uint8_t x = x0;
    for (; *s; s++)
    {
        const char ch = *s;
        if (ch >= '0' && ch <= '9')
        {
            DualVfoDrawScaledBigDigit(ch, x, y0, 5u, DUAL_VFO_SUB_FREQ_H);
        }
        else if (ch == '.')
        {
            PutPixel((uint8_t)(x + 1u), (uint8_t)(y0 + DUAL_VFO_SUB_FREQ_H - 2u), true);
            PutPixel((uint8_t)(x + 2u), (uint8_t)(y0 + DUAL_VFO_SUB_FREQ_H - 2u), true);
            PutPixel((uint8_t)(x + 1u), (uint8_t)(y0 + DUAL_VFO_SUB_FREQ_H - 1u), true);
            PutPixel((uint8_t)(x + 2u), (uint8_t)(y0 + DUAL_VFO_SUB_FREQ_H - 1u), true);
        }
        x = (uint8_t)(x + DUAL_VFO_SUB_FREQ_CHAR_W);
    }
    return x;
}

/* shortenTopBlack：为真时去掉反色条上方 1px 黑带（下面信道名与 A/B 之间少 1px 连续黑） */
static void DualVfoDrawInvertedHeaderPx(uint8_t y, unsigned int vfoIdx, bool shortenTopBlack)
{
    char L[20];
    char R[22];
    /* 信道名行：字体仍为 GUI_DisplaySmallest；默认上沿 y-1 黑带，下扩至 y+6 */
    const uint8_t yTop = shortenTopBlack ? y : ((y > 0u) ? (uint8_t)(y - 1u) : 0u);
    const uint8_t yBot = (uint8_t)(y + 6u); /* 原 y..y+5 共 6 行字区，下扩 1px */
    DualVfoFillRectBlack(0, yTop, (uint8_t)(LCD_WIDTH - 1u), yBot);
    DualVfoHeaderLeft(vfoIdx, L, sizeof(L));
    DualVfoHeaderRight(vfoIdx, R, sizeof(R));
    /* 黑底范围不变；字下移 1 像素 */
    GUI_DisplaySmallest(L, 2, (uint8_t)(y + 1u), false, false);
    {
        const unsigned int rw = (unsigned int)strlen(R) * 4u;
        const uint8_t      x = (rw < LCD_WIDTH - 4u) ? (uint8_t)(LCD_WIDTH - 2u - rw) : 2u;
        GUI_DisplaySmallest(R, x, (uint8_t)(y + 1u), false, false);
    }
}

/* 单字符 3x5 纵向映射到高度 tallH，列宽 2px（总宽约 6px） */
static void DualVfoDrawChar3x5Tall(uint8_t x0, uint8_t y0, char ch, bool setBlack, uint8_t tallH)
{
    if ((uint8_t)ch < 0x20u || (uint8_t)ch > 0x7fu)
        return;
    const uint8_t ci = (uint8_t)(ch - 0x20u);
    for (unsigned col = 0; col < 3u; col++)
    {
        uint8_t pixels = gFont3x5[ci][col];
        for (unsigned sr = 0; sr < 6u; sr++)
        {
            const unsigned dr0 = (sr * tallH) / 6u;
            const unsigned dr1 = ((sr + 1u) * tallH + 5u) / 6u;
            if (pixels & 1u)
            {
                for (unsigned dr = dr0; dr < dr1 && dr < tallH; dr++)
                    for (unsigned dx = 0; dx < 2u; dx++)
                    {
                        const uint8_t px = (uint8_t)(x0 + col * 2u + dx);
                        if (px < LCD_WIDTH)
                            PutPixel(px, (uint8_t)(y0 + (uint8_t)dr), setBlack);
                    }
            }
            pixels >>= 1;
        }
    }
}

static void DualVfoDrawTallAbInverse(uint8_t x0, uint8_t y0, char ch)
{
    for (uint8_t py = 0; py < DUAL_VFO_AB_TALL_H; py++)
        for (uint8_t px = 0; px < DUAL_VFO_AB_TALL_W; px++)
            PutPixel((uint8_t)(x0 + px), (uint8_t)(y0 + py), true);
    DualVfoDrawChar3x5Tall((uint8_t)(x0 + 1u), y0, ch, false, DUAL_VFO_AB_TALL_H);
}

static void DualVfoDrawAb1pxBlackMargins(uint8_t y, uint8_t xL, uint8_t xR)
{
    const uint8_t yB = (uint8_t)(y + DUAL_VFO_AB_TALL_H - 1u);
    if (y >= 1u)
        DualVfoFillRectBlack((uint8_t)(xL - 1u), (uint8_t)(y - 1u), (uint8_t)(xR + 1u), (uint8_t)(y - 1u));
    DualVfoFillRectBlack((uint8_t)(xL - 1u), y, (uint8_t)(xL - 1u), yB);
    DualVfoFillRectBlack((uint8_t)(xR + 1u), y, (uint8_t)(xR + 1u), yB);
}

/* A/B 闪烁：phase 每刷新递增；>>DV_DUAL_VFO_AB_BLINK_SH 为半周期（越小越快） */
#define DV_DUAL_VFO_AB_BLINK_SH 1u
static uint16_t s_DualVfoAbBlinkPhase;
static bool     s_DualVfoAbBlinkShowAb = true;
static bool     s_DualVfoAbBlinkPrevRx;
static unsigned s_DualVfoAbBlinkPrevRxVfo;

/* 内区 innerL..innerR 为 abW×abH。主：黑底+镂空 A/B（固定 DUAL_VFO_AB_TALL_*）；副：空心框+最小字 A/B 框内居中（BOT_*）。
 * 仅当本 VFO 正在接收时 A/B 随 s_DualVfoAbBlinkShowAb 间歇擦除（闪烁）；RX/TX 字每帧照画。
 * labelY：框旁 RX/TX 最小字基线 y（上面板与 y 相同；下面板为 DV_Y_BOT_BESIDE_AB）。
 * rxBesideAb：主信道在框旁画 RX；副信道 RX 由 DualVfoDrawBottomChannel 绘制（与框隔 2px）。 */
static void DualVfoDrawAbRxTxOnlyPx(unsigned int vfoIdx, uint8_t y, unsigned int activeTxVFO,
                                    bool topInverseStyle, bool rxBesideAb, uint8_t abW, uint8_t abH,
                                    uint8_t labelY)
{
    const char letter = (vfoIdx == 0) ? 'A' : 'B';

    const bool rxHere =
        (bool)(FUNCTION_IsRx() && gEeprom.RX_VFO == vfoIdx && VfoState[vfoIdx] == VFO_STATE_NORMAL);
    const bool txHere =
        (bool)(gCurrentFunction == FUNCTION_TRANSMIT && activeTxVFO == vfoIdx);

    const uint8_t innerL = 1u;
    const uint8_t innerR = (uint8_t)(innerL + abW - 1u);
    const uint8_t rxX = (uint8_t)(4u + abW + 1u + 2u);
    const uint8_t txX = (uint8_t)(innerR + 2u + 2u);

    const bool    showAb = (!rxHere) || s_DualVfoAbBlinkShowAb;
    const uint8_t yTopC  = (y >= 1u) ? (uint8_t)(y - 1u) : 0u;
    uint8_t       yEndC  = (uint8_t)(y + abH);
    if (yEndC > 63u)
        yEndC = 63u;
    const uint8_t xClr1 = (uint8_t)(innerR + 2u); /* 主/副 同一水平占位（含 1px 边距带） */

    if (rxHere && !showAb)
        DualVfoClearRectPx(0, yTopC, xClr1, yEndC);

    if (!rxHere || showAb)
    {
        if (topInverseStyle)
        {
            DualVfoDrawAb1pxBlackMargins(y, innerL, innerR);
            DualVfoDrawTallAbInverse(innerL, y, letter);
        }
        else
        {
            /* 下面板：最小字 GUI_DisplaySmallest 单字约 4×6（与 helper.c 一致），在框内居中 */
            UI_DrawRectangleBuffer(gFrameBuffer, (int16_t)innerL, (int16_t)y, (int16_t)innerR,
                                   (int16_t)(y + (int16_t)abH - 1), true);
            {
                char          abStr[2] = { letter, '\0' };
                const uint8_t smCellW  = 4u;
                const uint8_t smH      = 6u;
                const uint8_t tx =
                    (uint8_t)(innerL + (abW > smCellW ? (uint8_t)((abW - smCellW) / 2u) : 0u) + 1u);
                const uint8_t ty =
                    (uint8_t)(y + (abH > smH ? (uint8_t)((abH - smH) / 2u) : 0u) + 1u);
                GUI_DisplaySmallest(abStr, tx, ty, false, true);
            }
        }
    }

    if (rxHere)
    {
        if (rxBesideAb)
            GUI_DisplaySmallest("RX", rxX, labelY, false, true);
    }
    else if (txHere)
        GUI_DisplaySmallest("TX", txX, labelY, false, true);
}

static void DualVfoDrawChIdSmallest(unsigned int vfoIdx, uint8_t x, uint8_t y)
{
    char chId[14];
    DualVfoFmtChId(vfoIdx, chId, sizeof(chId));
    GUI_DisplaySmallest(chId, x, y, false, true);
}

/* TxOffs：与菜单相同存贮，格式化为无末尾 0 的小数字符串（如 6.8、0.5） */
static void DualVfoFmtTxOffsMHzTrim(char *out, size_t cap, uint32_t o)
{
    const unsigned hi = (unsigned)(o / 100000u);
    const unsigned lo = (unsigned)(o % 100000u);
    if (lo == 0u)
    {
        snprintf(out, cap, "%u", hi);
        return;
    }
    char frac[8];
    snprintf(frac, sizeof(frac), "%05u", lo);
    size_t n = strlen(frac);
    while (n > 1u && frac[n - 1u] == '0')
        n--;
    frac[n] = '\0';
    snprintf(out, cap, "%u.%s", hi, frac);
}

/* 频率数字左侧空隙内水平居中：TxODir（+/-）与 TxOffs 之间留 1px；OFF 时不显示 */
static void DualVfoDrawTxOffsetSmallCentered(unsigned int vfoIdx, uint8_t gapL, uint8_t xFreqStart, uint8_t y)
{
    const VFO_Info_t *v = &gEeprom.VfoInfo[vfoIdx];
    unsigned          d = (unsigned)v->TX_OFFSET_FREQUENCY_DIRECTION % 3u;

    if (d == TX_OFFSET_FREQUENCY_DIRECTION_OFF)
        return;

    if (xFreqStart <= gapL + 4u)
        return;

    char             num[16];
    const uint32_t   o = v->TX_OFFSET_FREQUENCY;
    DualVfoFmtTxOffsMHzTrim(num, sizeof(num), o);

    const char *const dir   = gSubMenu_SFT_D[d];
    const unsigned int numw = (unsigned int)strlen(num) * 4u;
    /* 方向 1 字宽 4px + 1px 间隔 + 数值 */
    const unsigned int totalw = 4u + 1u + numw;

    const uint8_t      gapR = (uint8_t)(xFreqStart - 2u);
    const unsigned int maxw = (unsigned int)(gapR - gapL + 1u);

    if (totalw > maxw)
        return;

    const uint8_t x = (uint8_t)((unsigned int)gapL + (maxw - totalw) / 2u);
    if ((unsigned int)x + totalw > LCD_WIDTH)
        return;

    GUI_DisplaySmallest(dir, x, y, false, true);
    GUI_DisplaySmallest(num, (uint8_t)(x + 4u + 1u), y, false, true);
}

static void DualVfoDrawSubFreqSmallest(uint8_t y, uint32_t frequency, bool invertTail)
{
    char fs[16];
    sprintf(fs, "%3u.%05u", (unsigned)(frequency / 100000u), (unsigned)(frequency % 100000u));
    const unsigned int w  = (unsigned int)strlen(fs) * DUAL_VFO_SUB_FREQ_CHAR_W;
    const uint8_t      x0 = (w < LCD_WIDTH - 2u) ? (uint8_t)(LCD_WIDTH - 2u - w) : 2u;
    DualVfoDrawStringSubFreqTall(x0, y, fs);
    if (invertTail)
        DualVfoXorHStripColumns(x0, y, (uint8_t)(y + DUAL_VFO_SUB_FREQ_H - 1u));
}

static void DualVfoDrawMainFreq2x(unsigned int vfoIdx, uint32_t frequency, bool invertTail)
{
    char fs[16];
    sprintf(fs, "%3u.%05u", (unsigned)(frequency / 100000u), (unsigned)(frequency % 100000u));
    const unsigned int w  = (unsigned int)strlen(fs) * 8u;
    uint8_t            x0 = (w < LCD_WIDTH - 4u) ? (uint8_t)(LCD_WIDTH - 2u - w) : 2u;
    if (x0 < 44u)
        x0 = 44u;
    DualVfoDrawTxOffsetSmallCentered(vfoIdx, DV_TXOFS_GAP_L_MAIN, x0, (uint8_t)(DV_Y_TOP_CH + 3u));
    DualVfoDrawString2x(x0, DV_Y_TOP_CH, fs);
    if (invertTail)
        DualVfoXorHStripColumns(x0, DV_Y_TOP_CH, (uint8_t)(DV_Y_TOP_CH + 11u));
}

static void DualVfoAppendTone(char *buf, size_t cap, char tag, const FREQ_Config_t *pc)
{
    const size_t n0 = strlen(buf);
    char         tmp[22];
    if (pc->CodeType == CODE_TYPE_OFF || n0 + 8u >= cap)
        return;
    if (pc->CodeType == CODE_TYPE_CONTINUOUS_TONE)
        snprintf(tmp, sizeof(tmp), "%c%u.%u", tag, CTCSS_Options[pc->Code] / 10u,
                 CTCSS_Options[pc->Code] % 10u);
    else if (pc->CodeType == CODE_TYPE_DIGITAL)
        snprintf(tmp, sizeof(tmp), "%c%03o", tag, DCS_Options[pc->Code]);
    else if (pc->CodeType == CODE_TYPE_REVERSE_DIGITAL)
        snprintf(tmp, sizeof(tmp), "%c%03oI", tag, DCS_Options[pc->Code]);
    else
        return;
    snprintf(buf + n0, cap - n0, " %s", tmp);
}

static void DualVfoDrawTopDetailRowPx(unsigned int topVfoIdx, uint8_t y)
{
    const VFO_Info_t *v = &gEeprom.VfoInfo[topVfoIdx];
    char              buf[48];
    uint8_t           sq = (uint8_t)((v->SquelchOpenRSSIThresh * 9u + 255u) / 256u);
    if (sq > 9u)
        sq = 9u;
    snprintf(buf, sizeof(buf), "SQ%u", sq);
    if ((v->StepFrequency / 100u) < 100u)
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " %d.%02uK", v->StepFrequency / 100,
                 v->StepFrequency % 100);
    else
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " %dK", v->StepFrequency / 100);
    DualVfoAppendTone(buf, sizeof(buf), 'R', &v->freq_config_RX);
    DualVfoAppendTone(buf, sizeof(buf), 'T', &v->freq_config_TX);
    {
        const unsigned int lw = (unsigned int)strlen(buf) * 4u;
        const uint8_t      x  = (lw < LCD_WIDTH - 4u) ? (uint8_t)(LCD_WIDTH - 2u - lw) : 2u;
        GUI_DisplaySmallest(buf, x, y, false, true);
    }
}

static void DualVfoDrawTopChannel(unsigned int vfoIdx)
{
    const unsigned int activeTxVFO = gRxVfoIsActive ? gEeprom.RX_VFO : gEeprom.TX_VFO;
    enum VfoState_t    state       = VfoState[vfoIdx];

#ifdef ENABLE_ALARM
    if (gCurrentFunction == FUNCTION_TRANSMIT && gAlarmState == ALARM_STATE_SITE_ALARM && activeTxVFO == vfoIdx)
        state = VFO_STATE_ALARM;
#endif

    DualVfoDrawInvertedHeaderPx(DV_Y_TOP_HDR, vfoIdx, false);

    if (state != VFO_STATE_NORMAL)
    {
        if (state < ARRAY_SIZE(VfoStateStr))
            GUI_DisplaySmallest(VfoStateStr[state], 2, DV_Y_TOP_AB, false, true);
        return;
    }

    DualVfoDrawAbRxTxOnlyPx(vfoIdx, DV_Y_TOP_AB, activeTxVFO, true, true, DUAL_VFO_AB_TALL_W,
                            DUAL_VFO_AB_TALL_H, DV_Y_TOP_AB);
    /* A/B 下 1px 间隔后左侧显示信道号 */
    DualVfoDrawChIdSmallest(vfoIdx, 2, DV_Y_TOP_CHID);

    {
        const bool rxHere =
            (bool)(FUNCTION_IsRx() && gEeprom.RX_VFO == vfoIdx && VfoState[vfoIdx] == VFO_STATE_NORMAL);
        const bool txHere =
            (bool)(gCurrentFunction == FUNCTION_TRANSMIT && activeTxVFO == vfoIdx);
        uint32_t   frequency = gEeprom.VfoInfo[vfoIdx].pRX->Frequency;
        if (txHere)
            frequency = gEeprom.VfoInfo[vfoIdx].pTX->Frequency;
        DualVfoDrawMainFreq2x(vfoIdx, frequency, rxHere || txHere);
    }

    DualVfoDrawTopDetailRowPx(vfoIdx, DV_Y_TOP_DET);
}

static void DualVfoDrawBottomChannel(unsigned int vfoIdx)
{
    const unsigned int activeTxVFO = gRxVfoIsActive ? gEeprom.RX_VFO : gEeprom.TX_VFO;
    enum VfoState_t    state       = VfoState[vfoIdx];

#ifdef ENABLE_ALARM
    if (gCurrentFunction == FUNCTION_TRANSMIT && gAlarmState == ALARM_STATE_SITE_ALARM && activeTxVFO == vfoIdx)
        state = VFO_STATE_ALARM;
#endif

    DualVfoDrawInvertedHeaderPx(DV_Y_BOT_HDR, vfoIdx, true);

    if (state != VFO_STATE_NORMAL)
    {
        if (state < ARRAY_SIZE(VfoStateStr))
            GUI_DisplaySmallest(VfoStateStr[state], 2, DV_Y_BOT_MAIN, false, true);
        return;
    }

    DualVfoDrawAbRxTxOnlyPx(vfoIdx, DV_Y_BOT_MAIN, activeTxVFO, false, false, DUAL_VFO_AB_BOT_W,
                            DUAL_VFO_AB_BOT_H, DV_Y_BOT_BESIDE_AB);

    const bool rxHere =
        (bool)(FUNCTION_IsRx() && gEeprom.RX_VFO == vfoIdx && VfoState[vfoIdx] == VFO_STATE_NORMAL);
    const bool txHere =
        (bool)(gCurrentFunction == FUNCTION_TRANSMIT && activeTxVFO == vfoIdx);

    /* 框右缘 innerR=1+abW-1 后留 2px 再画信道号；接收只画 RX、不画信道号 */
    {
        char            chId[14];
        const uint8_t besideX0 = (uint8_t)(1u + DUAL_VFO_AB_BOT_W + 2u); /* innerR + 1 + 2px 间隔 */
        if (rxHere)
            GUI_DisplaySmallest("RX", besideX0, DV_Y_BOT_BESIDE_AB, false, true);
        else
        {
            DualVfoFmtChId(vfoIdx, chId, sizeof(chId));
            uint8_t xch = besideX0;
            if (txHere)
                xch = (uint8_t)(xch + 9u);
            GUI_DisplaySmallest(chId, xch, DV_Y_BOT_BESIDE_AB, false, true);
        }
    }

    {
        uint32_t frequency = gEeprom.VfoInfo[vfoIdx].pRX->Frequency;
        if (txHere)
            frequency = gEeprom.VfoInfo[vfoIdx].pTX->Frequency;
        DualVfoDrawSubFreqSmallest(DV_Y_BOT_FREQ_LINE, frequency, rxHere || txHere);
    }
}

/* 与菜单 RxMode（gSubMenu_RXMode）四项顺序一致，底栏单行缩写 */
static const char *DualVfoRxModeShortLabel(void)
{
    static const char *const abbrev[4] = {"MAIN", "A/B", "CROSS", "A"};
    unsigned                 idx =
        (gEeprom.DUAL_WATCH != DUAL_WATCH_OFF ? 1u : 0u) + (gEeprom.CROSS_BAND_RX_TX != CROSS_BAND_OFF ? 2u : 0u);
    if (idx >= 4u)
        idx = 0u;
    return abbrev[idx];
}

/* S9 以上：实际超出 S9 的分贝映射为显示 +1dB～+5dB（0～40 线性，40 以上视为 5 档） */
static uint8_t DualVfoMapOverS9ToDisplayStep(unsigned overRaw)
{
    if (overRaw >= 40u)
        return 5u;
    if (overRaw == 0u)
        return 1u;
    {
        int16_t m = map((int16_t)overRaw, 0, 40, 1, 5);
        if (m < 1)
            m = 1;
        if (m > 5)
            m = 5;
        return (uint8_t)m;
    }
}

/* S 表横条像素数：右缘与竖线刻度（mL..mR 分 10 格、第 i 条在 mL+i*(mR-mL)/10）对齐；≤S9 在 tick1～tick9 间按 RSSI 插值，>S9 再到条带满宽 */
static unsigned DualVfoSmeterBarPxFromRssi(int16_t rssi_dBm, int16_t s0_dBm, int16_t s9_dBm, uint8_t mL,
                                           uint8_t mR, uint8_t barX0, uint8_t barW)
{
    uint8_t       tick[10];
    const uint8_t rightFull = (uint8_t)(barX0 + barW - 1u);
    unsigned      i;

    for (i = 1u; i <= 9u; i++)
        tick[i] = (uint8_t)(mL + i * (mR - mL) / 10u);

    int16_t rightEdge = 0;

    if (rssi_dBm <= s9_dBm)
    {
        int32_t sx = (int32_t)(rssi_dBm - s0_dBm) * 8000L / (int32_t)(s9_dBm - s0_dBm) + 1000L;
        if (sx <= 1000)
            rightEdge = 0;
        else if (sx >= 9000)
            rightEdge = (int16_t)tick[9];
        else
        {
            const unsigned idx  = (unsigned)(sx - 1000) / 1000u;
            const unsigned frac = (unsigned)(sx - 1000) % 1000u;
            rightEdge =
                (int16_t)tick[idx + 1u] +
                (int16_t)(((int32_t)tick[idx + 2u] - (int32_t)tick[idx + 1u]) * (int32_t)frac / 1000L);
        }
    }
    else
    {
        int16_t re =
            map(rssi_dBm, s9_dBm, (int16_t)(s9_dBm + 40), (int16_t)tick[9], (int16_t)rightFull);
        if (re < (int16_t)tick[9])
            re = (int16_t)tick[9];
        if (re > (int16_t)rightFull)
            re = (int16_t)rightFull;
        rightEdge = re;
    }

    if (rightEdge < (int16_t)barX0)
        return 0u;
    {
        unsigned px = (unsigned)((int16_t)rightEdge - (int16_t)barX0 + 1);
        if (px > barW)
            px = barW;
        return px;
    }
}

/* 主画布最底行：S 表 + S 值 + 电池（最后绘制，独占一行） */
static void DualVfoDrawBottomSMeterAndBattery(void)
{
    const uint8_t row   = (uint8_t)(DV_Y_METER / 8u);
    uint8_t      *rowFb = gFrameBuffer[row];
    uint8_t      *rowFbNext =
        (row + 1u < FRAME_LINES) ? gFrameBuffer[row + 1u] : rowFb;

    for (unsigned c = 0; c < DUAL_VFO_FREQ_COL; c++)
    {
        rowFb[c] = 0;
        rowFbNext[c] = 0;
    }

    const uint8_t mL = 0;
    const uint8_t mR = 34;

    GUI_DisplaySmallest("1-3-5-7-9", 0, DV_SMETER_LABEL_Y, false, true);

    {
        const bool   rxActive = FUNCTION_IsRx();
        int16_t      rssi_dBm = 0;
        unsigned int barPx    = 0;
        char         s9b[8]   = "";
        char         dbb[10]  = "";

        if (rxActive)
        {
            rssi_dBm = BK4819_GetRSSI_dBm() + dBmCorrTable[gRxVfo->Band];
#ifdef ENABLE_AM_FIX
            if (gSetting_AM_fix && gRxVfo->Modulation == MODULATION_AM)
                rssi_dBm = (int16_t)(rssi_dBm + AM_fix_get_gain_diff());
#endif
            const int16_t s9_dBm = -93;
            const int16_t s0_dBm = -141;
            /* 横条右缘对齐 9 条竖线刻度；最强（>S9）拉满条宽 */
            barPx = DualVfoSmeterBarPxFromRssi(rssi_dBm, s0_dBm, s9_dBm, mL, mR, DV_SMETER_BAR_X0,
                                               DV_SMETER_BAR_W);
            if (rssi_dBm <= s9_dBm)
            {
                int16_t s_num = map(rssi_dBm, s0_dBm, s9_dBm, 1, 9);
                if (s_num < 1)
                    s_num = 1;
                if (s_num > 9)
                    s_num = 9;
                sprintf(s9b, "S%u", (unsigned)s_num);
            }
            else
            {
                int32_t overDb = (int32_t)rssi_dBm - (int32_t)s9_dBm;
                if (overDb < 0)
                    overDb = 0;
                strcpy(s9b, "S9");
                sprintf(dbb, "+%udB", (unsigned)DualVfoMapOverS9ToDisplayStep((unsigned)overDb));
            }
        }

        for (unsigned int i = 0; i < barPx; i++)
        {
            const uint8_t x = (uint8_t)(DV_SMETER_BAR_X0 + i);
            for (uint8_t y = DV_SMETER_BAR_Y0; y <= DV_SMETER_BAR_Y1; y++)
                PutPixel(x, y, true);
        }

        /* S1..S9 位 9 条竖线（刻度字下方） */
        for (unsigned i = 1u; i <= 9u; i++)
        {
            const uint8_t tx = (uint8_t)(mL + (uint32_t)i * (uint32_t)(mR - mL) / 10u);
            for (uint8_t y = DV_SMETER_VLINE_Y0; y <= DV_SMETER_VLINE_Y1; y++)
                PutPixel(tx, y, true);
        }

        if (s9b[0] != 0)
            GUI_DisplaySmallest(s9b, DV_SMETER_SREAD_X, DV_SMETER_SVALUE_Y, false, true);
        if (dbb[0] != 0)
            GUI_DisplaySmallest(dbb, DV_SMETER_SREAD_X, DV_SMETER_DBB_Y, false, true);
    }

    {
        const unsigned batW = (unsigned int)sizeof(BITMAP_BatteryLevel1);
        const unsigned batX = LCD_WIDTH - batW - DV_BAT_FLUSH_RIGHT;
        uint8_t        bat[sizeof(BITMAP_BatteryLevel1)];

        for (unsigned c = batX; c < LCD_WIDTH; c++)
        {
            rowFb[c] = 0;
            rowFbNext[c] = 0;
        }
        UI_DrawBattery(bat, gBatteryDisplayLevel, gLowBatteryBlink);
        memcpy(rowFb + batX, bat, batW);

        char         pb[8];
        const unsigned int pctV = BATTERY_VoltsToPercent(gBatteryVoltageAverage);
        sprintf(pb, "%u%%", pctV);
        {
            const uint8_t textW = (uint8_t)(strlen(pb) * 4u);
            const uint8_t gapRx = 2u;
            uint8_t       pctPx;
            if (batW >= textW)
                pctPx = (uint8_t)(batX + (batW - textW) / 2u);
            else
                pctPx = (uint8_t)batX;
            if ((uint32_t)pctPx + textW > LCD_WIDTH)
                pctPx = (uint8_t)(LCD_WIDTH - textW);
            if ((uint32_t)pctPx + textW + DV_BAT_PCT_SHIFT_R <= LCD_WIDTH)
                pctPx = (uint8_t)(pctPx + DV_BAT_PCT_SHIFT_R);

            const char   *rxLab = DualVfoRxModeShortLabel();
            const uint8_t rxW   = (uint8_t)(strlen(rxLab) * 4u);
            const int32_t rxX = (int32_t)batX - (int32_t)gapRx - (int32_t)rxW + (int32_t)DV_BAT_MODE_SHIFT_R;
            const bool    drawRx =
                (rxX >= (int32_t)(DUAL_VFO_FREQ_COL + 1u) && (uint32_t)rxX + (uint32_t)rxW <= (uint32_t)batX);

            unsigned clearFrom = (unsigned)pctPx;
            if (drawRx && (unsigned)rxX < clearFrom)
                clearFrom = (unsigned)rxX;
            for (unsigned c = clearFrom; c < LCD_WIDTH; c++)
            {
                rowFb[c] = 0;
                rowFbNext[c] = 0;
            }
            memcpy(rowFb + batX, bat, batW);
            if (drawRx)
                GUI_DisplaySmallest(rxLab, (uint8_t)rxX, DV_Y_RXMODE, false, true);
            GUI_DisplaySmallest(pb, pctPx, DV_Y_PCT, false, true);
        }
    }
}

static bool UI_DisplayMain_DualVfoTwoPanel(void)
{
    const unsigned int tx  = gEeprom.TX_VFO;
    const unsigned int oth = (unsigned int)(1u - tx);

    if (FUNCTION_IsRx())
    {
        /* 刚进入接收或切换 RX 信道时复位相位，避免沿用旧计数长时间不闪 */
        if (!s_DualVfoAbBlinkPrevRx ||
            s_DualVfoAbBlinkPrevRxVfo != (unsigned)gEeprom.RX_VFO)
        {
            s_DualVfoAbBlinkPhase     = 0;
            s_DualVfoAbBlinkShowAb    = true;
            s_DualVfoAbBlinkPrevRxVfo = (unsigned)gEeprom.RX_VFO;
        }
        s_DualVfoAbBlinkPrevRx = true;
        s_DualVfoAbBlinkPhase++;
        s_DualVfoAbBlinkShowAb =
            ((s_DualVfoAbBlinkPhase >> DV_DUAL_VFO_AB_BLINK_SH) & 1u) == 0u;
    }
    else
    {
        s_DualVfoAbBlinkPrevRx = false;
        s_DualVfoAbBlinkPhase  = 0;
        s_DualVfoAbBlinkShowAb = true;
    }

    DualVfoDrawTopChannel(tx);
    DualVfoDrawBottomChannel(oth);
    DualVfoDrawBottomSMeterAndBattery();

    RxLine = -1;
    return true;
}
#endif /* ENABLE_FEAT_F4HWN */

#ifdef ENABLE_FEAT_F4HWN
static void ST7565_BlitMainPerMode(void)
{
    if (UI_IsDualVfoMainScreen())
        ST7565_BlitFullScreenDualVfoTightTop();
    else
        ST7565_BlitFullScreen();
}
#endif

// ----------------------------------------

static void DrawSmallPowerBars(uint8_t *p, unsigned int level)
{
    if(level>6)
        level = 6;

    char bar = 0b00111110;

    for(uint8_t i = 0; i <= level; i++) {
        if(gSetting_set_gui) {
            bar = (0xff << (6-i)) & 0x7F;
        }
        memset(p + 2 + i*3, bar, 2);
    }
}
#if defined ENABLE_AUDIO_BAR || defined ENABLE_RSSI_BAR

static void DrawLevelBar(uint8_t xpos, uint8_t line, uint8_t level, uint8_t bars)
{
#ifndef ENABLE_FEAT_F4HWN
    const char hollowBar[] = {
        0b01111111,
        0b01000001,
        0b01000001,
        0b01111111
    };
#endif
    
    uint8_t *p_line = gFrameBuffer[line];
    level = MIN(level, bars);

    for(uint8_t i = 0; i < level; i++) {
#ifdef ENABLE_FEAT_F4HWN
        if(gSetting_set_met)
        {
            const char hollowBar[] = {
                0b01111111,
                0b01000001,
                0b01000001,
                0b01111111
            };

            if(i < bars - 4) {
                for(uint8_t j = 0; j < 4; j++)
                    p_line[xpos + i * 5 + j] = (~(0x7F >> (i + 1))) & 0x7F;
            }
            else {
                memcpy(p_line + (xpos + i * 5), &hollowBar, ARRAY_SIZE(hollowBar));
            }
        }
        else
        {
            const char hollowBar[] = {
                0b00111110,
                0b00100010,
                0b00100010,
                0b00111110
            };

            const char simpleBar[] = {
                0b00111110,
                0b00111110,
                0b00111110,
                0b00111110
            };

            if(i < bars - 4) {
                memcpy(p_line + (xpos + i * 5), &simpleBar, ARRAY_SIZE(simpleBar));
            }
            else {
                memcpy(p_line + (xpos + i * 5), &hollowBar, ARRAY_SIZE(hollowBar));
            }
        }
#else
        if(i < bars - 4) {
            for(uint8_t j = 0; j < 4; j++)
                p_line[xpos + i * 5 + j] = (~(0x7F >> (i+1))) & 0x7F;
        }
        else {
            memcpy(p_line + (xpos + i * 5), &hollowBar, ARRAY_SIZE(hollowBar));
        }
#endif
    }
}
#endif

#ifdef ENABLE_AUDIO_BAR
// Approximation of a logarithmic scale using integer arithmetic
static uint8_t log2_approx(unsigned int value) {
    uint8_t log = 0;
    while (value >>= 1) {
        log++;
    }
    return log;
}
#endif

#ifdef ENABLE_AUDIO_BAR

void UI_DisplayAudioBar(void)
{
    if (gSetting_mic_bar)
    {
        if(gLowBattery && !gLowBatteryConfirmed)
            return;

#ifdef ENABLE_FEAT_F4HWN
        RxBlinkLed = 0;
        RxBlinkLedCounter = 0;
        BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);
        unsigned int line;
        if (isMainOnly())
        {
            line = 5;
        }
        else
        {
            line = 3;
        }
#else
        const unsigned int line = 3;
#endif

        if (gCurrentFunction != FUNCTION_TRANSMIT ||
            gScreenToDisplay != DISPLAY_MAIN
#ifdef ENABLE_DTMF_CALLING
            || gDTMF_CallState != DTMF_CALL_STATE_NONE
#endif
            )
        {
            return;  // screen is in use
        }

#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)
        if (gAlarmState != ALARM_STATE_OFF)
            return;
#endif
        static uint8_t barsOld = 0;
        const uint8_t thresold = 18; // arbitrary thresold
        //const uint8_t barsList[] = {0, 0, 0, 1, 2, 3, 4, 5, 6, 8, 10, 13, 16, 20, 25, 25};
        const uint8_t barsList[] = {0, 0, 0, 1, 2, 3, 5, 7, 9, 12, 15, 18, 21, 25, 25, 25};
        uint8_t logLevel;
        uint8_t bars;

        unsigned int voiceLevel  = BK4819_GetVoiceAmplitudeOut();  // 15:0

        voiceLevel = (voiceLevel >= thresold) ? (voiceLevel - thresold) : 0;
        logLevel = log2_approx(MIN(voiceLevel * 16, 32768u) + 1);
        bars = barsList[logLevel];
        barsOld = (barsOld - bars > 1) ? (barsOld - 1) : bars;

        uint8_t *p_line = gFrameBuffer[line];
        memset(p_line, 0, LCD_WIDTH);

        DrawLevelBar(2, line, barsOld, 25);

        if (gCurrentFunction == FUNCTION_TRANSMIT)
            ST7565_BlitMainPerMode();
    }
}
#endif

#ifdef ENABLE_FEAT_F4HWN_AUDIO_SCOPE

#define SCOPE_SAMPLES        43   // number of columns (43 × 3px = 128px wide)
#define SCOPE_NOISE_GATE     50u  // minimum range below which the display shows baseline
#define SCOPE_FLOOR_RISE     2u   // floor rise per frame (+100 units/s at 20ms/frame)
#define SCOPE_FLOOR_DROP_SHR 3u   // floor drop IIR shift: drop by (floor-min) >> N per frame (~160ms to halve)
#define SCOPE_VOLUME_MIN     200u // let's assume that the sound level in silence is 200

void UI_DisplayAudioScope(void)
{
    static uint16_t g_scope_buf[SCOPE_SAMPLES];
    static uint8_t  g_scope_write      = 0;
    static uint16_t g_scope_floor      = SCOPE_VOLUME_MIN;     // persistent floor: snaps down fast, rises slowly
    static uint8_t  g_scope_ready      = 0;                    // number of valid samples since TX entry

    // REG_64 (VoiceAmplitudeOut) is only meaningful in TX (mic input).
    // FM RX audio is frequency-encoded — no register gives the instantaneous waveform.

// ------------------------------ Sample audio amplitude ------------------------------

    static bool s_was_tx = false;

    if (gCurrentFunction != FUNCTION_TRANSMIT) {
        s_was_tx = false;
        return;
    }

    // This prevents a sudden spike on the bar caused by release the PTT button
    if (!GPIO_IsPttPressed()
#ifdef ENABLE_VOX
    && !gEeprom.VOX_SWITCH
#endif
#ifdef ENABLE_FEAT_F4HWN
    && !gSetting_set_ptt_session
#endif
    )
    return;

    if (!s_was_tx) {
        // TX entry: full reset so every new transmission starts from a clean state
        for (uint8_t i = 0; i < SCOPE_SAMPLES; i++) g_scope_buf[i] = SCOPE_VOLUME_MIN;
        g_scope_write      = 0u;
        g_scope_floor      = SCOPE_VOLUME_MIN;
        s_was_tx           = true;
    }

    // The first 7 bars after turning on the radio
    // will not display any values: they cause high bars.
    if (g_scope_ready >= 7)
        g_scope_buf[g_scope_write] = BK4819_GetVoiceAmplitudeOut();
    else
        g_scope_ready++;
        
    // If the reading is 0, it is definitely an incorrect value
    // caused by the microphone being muted - set it to 200.
    if (g_scope_buf[g_scope_write] == 0) 
        g_scope_buf[g_scope_write] =  SCOPE_VOLUME_MIN;

    g_scope_write = (g_scope_write + 1u) % SCOPE_SAMPLES;

// --------------------------------- Refresh display ---------------------------------

    if (gLowBattery && !gLowBatteryConfirmed)
        return;

    if (gScreenToDisplay != DISPLAY_MAIN
#ifdef ENABLE_DTMF_CALLING
        || gDTMF_CallState != DTMF_CALL_STATE_NONE
#endif
        )
        return;

#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)
    if (gAlarmState != ALARM_STATE_OFF)
        return;
#endif

#ifdef ENABLE_FEAT_F4HWN
    RxBlinkLed = 0;
    RxBlinkLedCounter = 0;
    BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);
    const unsigned int line = isMainOnly() ? 5 : 3;
#else
    const unsigned int line = 3;
#endif

    uint8_t *p_line = gFrameBuffer[line];
    memset(p_line, 0, LCD_WIDTH);

    // Find min and max across current buffer
    uint16_t min_val = g_scope_buf[0];
    uint16_t max_val = g_scope_buf[0];
    for (uint8_t i = 1u; i < SCOPE_SAMPLES; i++) {
        if (g_scope_buf[i] < min_val) min_val = g_scope_buf[i];
        if (g_scope_buf[i] > max_val) max_val = g_scope_buf[i];
    }

    // Floor tracks buffer minimum with asymmetric IIR:
    // - drops toward min smoothly (SCOPE_FLOOR_DROP_SHR), avoiding instant-snap ghost
    // - rises slowly (SCOPE_FLOOR_RISE/frame) to handle loud constant voice
    if (g_scope_floor > min_val)
        g_scope_floor -= ((g_scope_floor - min_val) >> SCOPE_FLOOR_DROP_SHR) + 1u;
    else
        g_scope_floor += SCOPE_FLOOR_RISE;

    const uint16_t range = (max_val > g_scope_floor) ? (max_val - g_scope_floor) : 0u;

    for (uint8_t i = 0u; i < SCOPE_SAMPLES; i++) {
        const uint8_t  idx    = (g_scope_write + i) % SCOPE_SAMPLES;
        uint8_t        height = 0u;
        if (range >= SCOPE_NOISE_GATE) {
            const uint16_t v = (g_scope_buf[idx] > g_scope_floor) ? (g_scope_buf[idx] - g_scope_floor) : 0u;
            height = (uint8_t)((uint32_t)v * 7u / range);
        }
        // Filled column using bits 6..0 only (bit 7 always off to avoid overlap with text below)
        // At silence (height 0): single pixel at bit 6 (baseline)
        const uint8_t mask = (height > 0u) ? (uint8_t)((0x7Fu << (7u - height)) & 0x7Fu) : 0x40u;
        // 2px column + 1px gap per sample

        uint8_t *p_col = &p_line[i * 3u];
        p_col[0] = mask;
        p_col[1] = mask;

    }

    ST7565_BlitLine(line);
}
#endif  // ENABLE_FEAT_F4HWN_AUDIO_SCOPE

void DisplayRSSIBar(const bool now)
{
#if defined(ENABLE_RSSI_BAR)

    const unsigned int txt_width    = 7 * 8;                 // 8 text chars
    const unsigned int bar_x        = 2 + txt_width + 4;     // X coord of bar graph

#ifdef ENABLE_FEAT_F4HWN
    /*
    const char empty[] = {
        0b00000000,
        0b00000000,
        0b00000000,
        0b00000000,
        0b00000000,
        0b00000000,
        0b00000000,
    };
    */

    unsigned int line;
    if (isMainOnly())
    {
        line = 5;
    }
    else
    {
        line = 3;
    }

    //char rx[4];
    //sprintf(String, "%d", RxBlink);
    //UI_PrintStringSmallBold(String, 80, 0, RxLine);

    if(RxLine >= 0 && center_line != CENTER_LINE_IN_USE && !isMainOnly())
    {
        static bool clean = false;
        uint8_t *p_line0 = gFrameBuffer[RxLine + 0];

        clean = !clean;

        if(clean) {
            for(uint8_t i = 0; i < sizeof(BITMAP_VFO_Default); i++)
                p_line0[i] = (p_line0[i] & 0x80) | BITMAP_VFO_Default[i];
        } else {
            for(uint8_t i = 0; i < sizeof(BITMAP_VFO_Empty); i++)
                p_line0[i] = (p_line0[i] & 0x80) | BITMAP_VFO_Empty[i];
        }

        ST7565_BlitLine(RxLine);
    }
#else
    const unsigned int line = 3;
#endif
    uint8_t           *p_line        = gFrameBuffer[line];
    char               str[16];

#ifndef ENABLE_FEAT_F4HWN
    const char plus[] = {
        0b00011000,
        0b00011000,
        0b01111110,
        0b01111110,
        0b01111110,
        0b00011000,
        0b00011000,
    };
#endif

    if (gEeprom.KEY_LOCK && gKeypadLocked > 0)
        return;

#ifdef ENABLE_FEAT_F4HWN
    const bool dualStatusRssi = !isMainOnly() && !DualVfoShouldUseLegacyMain();
#else
    const bool dualStatusRssi = false;
#endif

    if (!dualStatusRssi && center_line != CENTER_LINE_RSSI)
        return;     // display is in use

    if (gCurrentFunction == FUNCTION_TRANSMIT ||
        gScreenToDisplay != DISPLAY_MAIN
#ifdef ENABLE_DTMF_CALLING
        || gDTMF_CallState != DTMF_CALL_STATE_NONE
#endif
        )
        return;     // display is in use

    if (!dualStatusRssi && now)
        memset(p_line, 0, LCD_WIDTH);

#ifdef ENABLE_FEAT_F4HWN
    int16_t rssi_dBm =
        BK4819_GetRSSI_dBm()
#ifdef ENABLE_AM_FIX
        + ((gSetting_AM_fix && gRxVfo->Modulation == MODULATION_AM) ? AM_fix_get_gain_diff() : 0)
#endif
        + dBmCorrTable[gRxVfo->Band];

    // S9 = -93 dBm, S0 = -141 dBm (IARU standard)
    const int16_t s9_dBm = -93;
    const int16_t s0_dBm = -141;

    uint8_t s_level    = 1;
    uint8_t overS9dBm  = 0;
    uint8_t overS9Bars = 0;

    if (rssi_dBm <= s9_dBm) {
        int16_t sn = map(rssi_dBm, s0_dBm, s9_dBm, 1, 9);
        if (sn < 1)
            sn = 1;
        if (sn > 9)
            sn = 9;
        s_level = (uint8_t)sn;
    } else {
        /* 相对 S9 的实际 dB；格条约每 10dB 一格，最多 4 格 */
        s_level = 9;
        {
            int32_t od = (int32_t)rssi_dBm - (int32_t)s9_dBm;
            if (od < 0)
                od = 0;
            if (od > 255)
                od = 255;
            overS9dBm = (uint8_t)od;
        }
        overS9Bars = MIN(overS9dBm / 10u, 4u);
    }
#else
    const int16_t s0_dBm   = -gEeprom.S0_LEVEL;                  // S0 .. base level
    const int16_t rssi_dBm =
        BK4819_GetRSSI_dBm()
#ifdef ENABLE_AM_FIX
        + ((gSetting_AM_fix && gRxVfo->Modulation == MODULATION_AM) ? AM_fix_get_gain_diff() : 0)
#endif
        + dBmCorrTable[gRxVfo->Band];

    int s0_9 = gEeprom.S0_LEVEL - gEeprom.S9_LEVEL;
    const uint8_t s_level = MIN(MAX((int32_t)(rssi_dBm - s0_dBm)*100 / (s0_9*100/9), 0), 9); // S0 - S9
    uint8_t overS9dBm = MIN(MAX(rssi_dBm + gEeprom.S9_LEVEL, 0), 99);
    uint8_t overS9Bars = MIN(overS9dBm/10, 4);
#endif

#ifdef ENABLE_FEAT_F4HWN
    if (!dualStatusRssi)
    {
        if (gSetting_set_gui)
        {
            sprintf(str, "%3d", rssi_dBm);
            UI_PrintStringSmallNormal(str, LCD_WIDTH + 8, 0, line - 1);
        }
        else
        {
            sprintf(str, "% 4d %s", rssi_dBm, "dBm");
            if (isMainOnly())
                GUI_DisplaySmallest(str, 2, 41, false, true);
            else
                GUI_DisplaySmallest(str, 2, 25, false, true);
        }

        if (overS9Bars == 0)
            sprintf(str, "S%d", s_level);
        else
            sprintf(str, "+%udB", (unsigned)DualVfoMapOverS9ToDisplayStep((unsigned)overS9dBm));

        UI_PrintStringSmallNormal(str, LCD_WIDTH + 38, 0, line - 1);
    }
#else
    if(overS9Bars == 0) {
        sprintf(str, "% 4d S%d", -rssi_dBm, s_level);
    }
    else {
        sprintf(str, "% 4d  %2d", -rssi_dBm, overS9dBm);
        memcpy(p_line + 2 + 7*5, &plus, ARRAY_SIZE(plus));
    }

    UI_PrintStringSmallNormal(str, 2, 0, line);
#endif
    if (!dualStatusRssi)
    {
        DrawLevelBar(bar_x, line, s_level + overS9Bars, 13);
        if (now)
            ST7565_BlitLine(line);
    }
    // 供顶部状态栏 5 格信号条使用：将 0~13 映射到 0~6
    {
        const uint8_t raw = s_level + overS9Bars;
        gVFO_RSSI_bar_level[gEeprom.RX_VFO] = (raw * 6u + 6u) / 13u;
        if (gVFO_RSSI_bar_level[gEeprom.RX_VFO] > 6u)
            gVFO_RSSI_bar_level[gEeprom.RX_VFO] = 6u;
    }
#else
    int16_t rssi = BK4819_GetRSSI();
    uint8_t Level;

    if (rssi >= gEEPROM_RSSI_CALIB[gRxVfo->Band][3]) {
        Level = 6;
    } else if (rssi >= gEEPROM_RSSI_CALIB[gRxVfo->Band][2]) {
        Level = 4;
    } else if (rssi >= gEEPROM_RSSI_CALIB[gRxVfo->Band][1]) {
        Level = 2;
    } else if (rssi >= gEEPROM_RSSI_CALIB[gRxVfo->Band][0]) {
        Level = 1;
    } else {
        Level = 0;
    }

    uint8_t *pLine = (gEeprom.RX_VFO == 0)? gFrameBuffer[2] : gFrameBuffer[6];
    if (now)
        memset(pLine, 0, 23);
    DrawSmallPowerBars(pLine, Level);
    if (now)
#ifdef ENABLE_FEAT_F4HWN
        ST7565_BlitMainPerMode();
#else
        ST7565_BlitFullScreen();
#endif
    gVFO_RSSI_bar_level[gEeprom.RX_VFO] = Level;
#endif

}

#ifdef ENABLE_AGC_SHOW_DATA
void UI_MAIN_PrintAGC(bool now)
{
    char buf[20];
    memset(gFrameBuffer[3], 0, 128);
    union {
        struct {
            uint16_t _ : 5;
            uint16_t agcSigStrength : 7;
            int16_t gainIdx : 3;
            uint16_t agcEnab : 1;
        };
        uint16_t __raw;
    } reg7e;
    reg7e.__raw = BK4819_ReadRegister(0x7E);
    uint8_t gainAddr = reg7e.gainIdx < 0 ? 0x14 : 0x10 + reg7e.gainIdx;
    union {
        struct {
            uint16_t pga:3;
            uint16_t mixer:2;
            uint16_t lna:3;
            uint16_t lnaS:2;
        };
        uint16_t __raw;
    } agcGainReg;
    agcGainReg.__raw = BK4819_ReadRegister(gainAddr);
    int8_t lnaShortTab[] = {-28, -24, -19, 0};
    int8_t lnaTab[] = {-24, -19, -14, -9, -6, -4, -2, 0};
    int8_t mixerTab[] = {-8, -6, -3, 0};
    int8_t pgaTab[] = {-33, -27, -21, -15, -9, -6, -3, 0};
    int16_t agcGain = lnaShortTab[agcGainReg.lnaS] + lnaTab[agcGainReg.lna] + mixerTab[agcGainReg.mixer] + pgaTab[agcGainReg.pga];

    sprintf(buf, "%d%2d %2d %2d %3d", reg7e.agcEnab, reg7e.gainIdx, -agcGain, reg7e.agcSigStrength, BK4819_GetRSSI());
    UI_PrintStringSmallNormal(buf, 2, 0, 3);
    if(now)
        ST7565_BlitLine(3);
}
#endif

void UI_MAIN_TimeSlice500ms(void)
{
    if(gScreenToDisplay==DISPLAY_MAIN) {
#ifdef ENABLE_AGC_SHOW_DATA
        UI_MAIN_PrintAGC(true);
        return;
#endif

        if(FUNCTION_IsRx()) {
            DisplayRSSIBar(true);
#ifdef ENABLE_FEAT_F4HWN_RX_TX_TIMER
            // 接收时每 500ms 触发主屏刷新，使 RX 正计时实时更新
            gUpdateDisplay = true;
#endif
        }
        else {
#ifdef ENABLE_FEAT_F4HWN_RX_TX_TIMER
            // 发射时每 500ms 触发主屏刷新，使 TX 倒计时实时更新
            gUpdateDisplay = true;
#endif
#ifdef ENABLE_FEAT_F4HWN // Blink Green Led for white...
            if(gSetting_set_eot > 0 && RxBlinkLed == 2)
        {
            if(RxBlinkLedCounter <= 8)
            {
                if(RxBlinkLedCounter % 2 == 0)
                {
                    if(gSetting_set_eot > 1 )
                    {
                        BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);
                    }
                }
                else
                {
                    if(gSetting_set_eot > 1 )
                    {
                        BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, true);
                    }

                    if(gSetting_set_eot == 1 || gSetting_set_eot == 3)
                    {
                        switch(RxBlinkLedCounter)
                        {
                            case 1:
                            AUDIO_PlayBeep(BEEP_400HZ_30MS);
                            break;

                            case 3:
                            AUDIO_PlayBeep(BEEP_400HZ_30MS);
                            break;

                            case 5:
                            AUDIO_PlayBeep(BEEP_500HZ_30MS);
                            break;

                            case 7:
                            AUDIO_PlayBeep(BEEP_600HZ_30MS);
                            break;
                        }
                    }
                }
                RxBlinkLedCounter += 1;
            }
            else
            {
                RxBlinkLed = 0;
            }
        }
#endif
        }
    }
}

// ----------------------------------------

void UI_DisplayMain(void)
{
    char               String[22];

    center_line = CENTER_LINE_NONE;

    // clear the screen
    UI_DisplayClear();

    if(gLowBattery && !gLowBatteryConfirmed) {
        UI_DisplayPopup("LOW BATTERY");
        ST7565_BlitFullScreen();
        return;
    }

#ifndef ENABLE_FEAT_F4HWN
    if (gEeprom.KEY_LOCK && gKeypadLocked > 0)
    {   // tell user how to unlock the keyboard
        UI_PrintString("Long press #", 0, LCD_WIDTH, 1, 8);
        UI_PrintString("to unlock",    0, LCD_WIDTH, 3, 8);
        ST7565_BlitFullScreen();
        return;
    }
#else
    UI_DisplayUnlockKeyboard(isMainOnly() ? 5 : 3);
#endif

#ifdef ENABLE_FEAT_F4HWN
    if (DualVfoMainFreqEntryScreen() && !(gEeprom.KEY_LOCK && gKeypadLocked > 0))
    {
        UI_DisplayMain_FreqInputBare();
        goto display_main_after_vfo_loop;
    }
#endif

    // 主页面 (MAIN ONLY) 定制布局：横线、计时、大矩形+左侧条、信道/频率、底部两按钮
#ifdef ENABLE_FEAT_F4HWN
    if (isMainOnly() && !gAirCopyBootMode && gScreenToDisplay == DISPLAY_MAIN &&
        !(gEeprom.KEY_LOCK && gKeypadLocked > 0)) {
        const uint8_t vfo = gEeprom.TX_VFO;
        const VFO_Info_t *pVfo = &gEeprom.VfoInfo[vfo];

        // 顶线 + 左侧小长条（RX 接收时空心，平时实心）+ 方框
        for (unsigned int i = 0; i < LCD_WIDTH; i++)
            gFrameBuffer[0][i] |= 0x01;

        const bool hollowBar = FUNCTION_IsRx();
        const int barX0 = 0, barX1 = 7, rectX0 = 7, rectY0 = 2, rectY1 = 33;
        const int contentX = rectX0 + 4;

        for (int y = rectY0; y <= rectY1; y++) {
            for (int x = barX0; x < barX1; x++) {
                if (!hollowBar) {
                    UI_DrawPixelBuffer(gFrameBuffer, x, y, true);   // 非 RX：实心
                } else {
                    const bool border =
                        (x == barX0) || (x == barX1 - 1) || (y == rectY0) || (y == rectY1);
                    if (border)
                        UI_DrawPixelBuffer(gFrameBuffer, x, y, true);   // RX：空心边框
                }
            }
        }

        UI_DrawLineBuffer(gFrameBuffer, rectX0, rectY0, LCD_WIDTH - 1, rectY0, true);
        UI_DrawLineBuffer(gFrameBuffer, LCD_WIDTH - 1, rectY0, LCD_WIDTH - 1, rectY1, true);

        // 右上角：信道模式默认显示信道号，接收时显示灵敏度；频率模式默认不显示，接收时显示灵敏度
        {
            const int slotY = 4;
            const int rightEdge = (int)(LCD_WIDTH - 1);
            if (FUNCTION_IsRx()) {
                char dBmStr[12];
                int16_t rssi_dBm =
                    BK4819_GetRSSI_dBm()
#ifdef ENABLE_AM_FIX
                    + ((gSetting_AM_fix && gRxVfo->Modulation == MODULATION_AM) ? AM_fix_get_gain_diff() : 0)
#endif
                    + dBmCorrTable[gRxVfo->Band];
                rssi_dBm = -rssi_dBm;
                if (rssi_dBm > 141) rssi_dBm = 141;
                if (rssi_dBm < 53) rssi_dBm = 53;
                sprintf(dBmStr, "%d dBm", rssi_dBm);
                const unsigned int w = strlen(dBmStr) * 4;
                const int x0 = (rightEdge - (int)w) > (int)contentX ? (rightEdge - (int)w) : (int)contentX;
                GUI_DisplaySmallest(dBmStr, (uint8_t)x0, slotY, false, true);
                {
                    uint8_t s_level, overS9Bars = 0;
                    if (rssi_dBm >= 93) {
                        s_level = (uint8_t)((141 - rssi_dBm) * 8u / 48u + 1u);
                        if (s_level > 9) s_level = 9;
                    } else {
                        s_level = 9;
                        uint8_t overS9dBm = (rssi_dBm >= 53) ? (uint8_t)(93 - rssi_dBm) : 40;
                        if (overS9dBm > 40) overS9dBm = 40;
                        overS9Bars = overS9dBm * 4u / 40u;
                    }
                    uint8_t raw = s_level + overS9Bars;
                    gVFO_RSSI_bar_level[vfo] = (raw * 6u + 6u) / 13u;
                    if (gVFO_RSSI_bar_level[vfo] > 6u) gVFO_RSSI_bar_level[vfo] = 6u;
                }
                gUpdateStatus = true;
            } else if (IS_MR_CHANNEL(gEeprom.ScreenChannel[vfo])) {
                sprintf(String, "%04u", gEeprom.ScreenChannel[vfo] + 1);
                const uint8_t chNumW = 4 * 4;
                const int x0 = (rightEdge - (int)chNumW) > (int)contentX ? (rightEdge - (int)chNumW) : (int)contentX;
                GUI_DisplaySmallest(String, (uint8_t)x0, slotY, false, true);
            }
            /* 频率模式且未接收：右上角不显示 */
        }

        // 信道名与频率整体上移 3 像素：按行上移一行(约 8px)，整字不拆
        if (IS_MR_CHANNEL(gEeprom.ScreenChannel[vfo])) {
            char chName[16];
            SETTINGS_FetchChannelName(chName, gEeprom.ScreenChannel[vfo]);
            if (chName[0]) UI_PrintStringSmallNormal(chName, contentX, contentX, 1);
        } else {
            UI_PrintStringSmallNormal("VFO", contentX, contentX, 1);
        }

        {
            uint32_t f = (gCurrentFunction == FUNCTION_TRANSMIT) ? pVfo->pTX->Frequency : pVfo->pRX->Frequency;
            sprintf(String, "%3u.%05u", f / 100000, f % 100000);
            char lastTwo[3];
            lastTwo[0] = String[7];
            lastTwo[1] = String[8];
            lastTwo[2] = '\0';
            String[7] = '\0';
            const int freqMainPixels = 6 * 8;
            UI_PrintString(String, contentX, contentX, 2, 8);
            /* 后两位与主频率同一行：用最小字体 3x5 画在主频率右侧，后画以免被挡 */
            const int lastTwoX = contentX + freqMainPixels + 8;
            const int lastTwoY = 23;  /* 向下 2 像素 */
            GUI_DisplaySmallest(lastTwo, (uint8_t)lastTwoX, lastTwoY, false, true);
        }

        // 方框底边：与上边/右边同样用 1 像素线画
        UI_DrawLineBuffer(gFrameBuffer, rectX0, rectY1, LCD_WIDTH - 1, rectY1, true);

        // 方框下两行：第一行 time: 计时，第二行 亚音，均右对齐；整体上移 1px
        const int line1Y = 33, line2Y = 39;
        const int smallCharW = 4;  /* 最小字约 4 像素/字 */
#ifdef ENABLE_FEAT_F4HWN_RX_TX_TIMER
        {
            uint16_t t = (FUNCTION_IsRx()) ? (3600 - gRxTimerCountdown_500ms / 2) : (gTxTimerCountdown_500ms / 2);
            uint16_t m = t / 60;
            uint8_t s = (uint8_t)(t % 60);
            sprintf(String, "time: %02u:%02u", (unsigned)m, s);
            const int w1 = (int)strlen(String) * smallCharW;
            const int x1 = 127 - w1;
            GUI_DisplaySmallest(String, (uint8_t)(x1 > 0 ? x1 : 0), line1Y + 2, false, true);
        }
#endif
        {
            char toneBuf[48];
            uint8_t pos = 0;
            const FREQ_Config_t *pRx = &pVfo->freq_config_RX;
            const FREQ_Config_t *pTx = &pVfo->freq_config_TX;
            if (pRx->CodeType != CODE_TYPE_OFF) {
                pos += sprintf(toneBuf + pos, "R");
                if (pRx->CodeType == CODE_TYPE_CONTINUOUS_TONE) {
                    pos += sprintf(toneBuf + pos, "CT%u.%u", CTCSS_Options[pRx->Code] / 10, CTCSS_Options[pRx->Code] % 10);
                } else if (pRx->CodeType == CODE_TYPE_DIGITAL) {
                    pos += sprintf(toneBuf + pos, "DCS%03oN", DCS_Options[pRx->Code]);
                } else {
                    pos += sprintf(toneBuf + pos, "DCS%03oI", DCS_Options[pRx->Code]);
                }
                if (pTx->CodeType != CODE_TYPE_OFF)
                    toneBuf[pos++] = ' ';
            }
            if (pTx->CodeType != CODE_TYPE_OFF) {
                pos += sprintf(toneBuf + pos, "T");
                if (pTx->CodeType == CODE_TYPE_CONTINUOUS_TONE) {
                    pos += sprintf(toneBuf + pos, "CT%u.%u", CTCSS_Options[pTx->Code] / 10, CTCSS_Options[pTx->Code] % 10);
                } else if (pTx->CodeType == CODE_TYPE_DIGITAL) {
                    pos += sprintf(toneBuf + pos, "DCS%03oN", DCS_Options[pTx->Code]);
                } else {
                    pos += sprintf(toneBuf + pos, "DCS%03oI", DCS_Options[pTx->Code]);
                }
            }
            toneBuf[pos] = '\0';
            if (pos > 0) {
                const int w2 = (int)strlen(toneBuf) * smallCharW;
                const int x2 = 127 - w2;
                GUI_DisplaySmallest(toneBuf, (uint8_t)(x2 > 0 ? x2 : 0), line2Y + 2, false, true);
            }
        }

        // 底部两按钮：黑底先增高 1 像素（row5 底一行黑），再 row6 黑底，中间白线，笔画用 Negative 清空（字不动）
        const int btnLY = 6, btnL5 = 5, btnLX0 = 0, btnLX1 = 62, btnRX0 = 64, btnRX1 = 127;
        const int sepX = 63;
        for (int x = btnLX0; x <= btnLX1; x++)
            gFrameBuffer[btnL5][x] |= 0x80;
        for (int x = btnRX0; x <= btnRX1; x++)
            gFrameBuffer[btnL5][x] |= 0x80;
        gFrameBuffer[btnL5][sepX] &= (uint8_t)~0x80;
        for (int x = btnLX0; x <= btnLX1; x++)
            gFrameBuffer[btnLY][x] = 0xFF;
        for (int x = btnRX0; x <= btnRX1; x++)
            gFrameBuffer[btnLY][x] = 0xFF;
        gFrameBuffer[btnLY][sepX] = 0x00;

        UI_PrintStringSmallNormalNegative("Menu", btnLX0, btnLX1, btnLY);
        UI_PrintStringSmallNormalNegative(gModulationStr[pVfo->Modulation], btnRX0, btnRX1, btnLY);

        ST7565_BlitFullScreen();
        return;
    }
#endif

#ifdef ENABLE_FEAT_F4HWN
    if (!isMainOnly() && !DualVfoShouldUseLegacyMain() && gScreenToDisplay == DISPLAY_MAIN && !gAirCopyBootMode)
    {
        UI_DisplayMain_DualVfoTwoPanel();
        goto display_main_after_vfo_loop;
    }
#endif

    unsigned int activeTxVFO = gRxVfoIsActive ? gEeprom.RX_VFO : gEeprom.TX_VFO;

    for (unsigned int vfo_num = 0; vfo_num < 2; vfo_num++)
    {
#ifdef ENABLE_FEAT_F4HWN
        const unsigned int line0 = 0;  // text screen line
        const unsigned int line1 = 4;
        unsigned int line;
        if (isMainOnly())
        {
            line       = 0;
        }
        else
        {
            line       = (vfo_num == 0) ? line0 : line1;
        }
        const bool         isMainVFO  = (vfo_num == gEeprom.TX_VFO);
        uint8_t           *p_line0    = gFrameBuffer[line + 0];
        uint8_t           *p_line1    = gFrameBuffer[line + 1];
        enum Vfo_txtr_mode mode       = VFO_MODE_NONE;      
#else
        const unsigned int line0 = 0;  // text screen line
        const unsigned int line1 = 4;
        const unsigned int line       = (vfo_num == 0) ? line0 : line1;
        const bool         isMainVFO  = (vfo_num == gEeprom.TX_VFO);
        uint8_t           *p_line0    = gFrameBuffer[line + 0];
        uint8_t           *p_line1    = gFrameBuffer[line + 1];
        enum Vfo_txtr_mode mode       = VFO_MODE_NONE;
#endif

#ifdef ENABLE_FEAT_F4HWN
    if (isMainOnly())
    {
        if (activeTxVFO != vfo_num)
        {
            continue;
        }
    }
#endif

#ifdef ENABLE_FEAT_F4HWN
        if (activeTxVFO != vfo_num || isMainOnly())
#else
        if (activeTxVFO != vfo_num) // this is not active TX VFO
#endif
        {
#ifdef ENABLE_SCAN_RANGES
            if(gScanRangeStart) {

#ifdef ENABLE_FEAT_F4HWN
                //if(IS_FREQ_CHANNEL(gEeprom.ScreenChannel[0]) && IS_FREQ_CHANNEL(gEeprom.ScreenChannel[1])) {
                if(IS_FREQ_CHANNEL(gEeprom.ScreenChannel[activeTxVFO])) {

                    uint8_t shift = 0;

                    if (isMainOnly())
                    {
                        shift = 3;
                    }

                    UI_PrintString("ScnRng", 5, 0, line + shift, 8);
                    sprintf(String, "%3u.%05u", gScanRangeStart / 100000, gScanRangeStart % 100000);
                    UI_PrintStringSmallNormal(String, 56, 0, line + shift);
                    sprintf(String, "%3u.%05u", gScanRangeStop / 100000, gScanRangeStop % 100000);
                    UI_PrintStringSmallNormal(String, 56, 0, line + shift + 1);

                    if (!isMainOnly())
                        continue;
                }
                else
                {
                    gScanRangeStart = 0;
                }
#else
                UI_PrintString("ScnRng", 5, 0, line, 8);
                sprintf(String, "%3u.%05u", gScanRangeStart / 100000, gScanRangeStart % 100000);
                UI_PrintStringSmallNormal(String, 56, 0, line);
                sprintf(String, "%3u.%05u", gScanRangeStop / 100000, gScanRangeStop % 100000);
                UI_PrintStringSmallNormal(String, 56, 0, line + 1);
                continue;
#endif
            }
#endif


            if (gDTMF_InputMode
#ifdef ENABLE_DTMF_CALLING
                || gDTMF_CallState != DTMF_CALL_STATE_NONE || gDTMF_IsTx
#endif
            ) {
                char *pPrintStr = "";
                // show DTMF stuff
#ifdef ENABLE_DTMF_CALLING
                char Contact[16];
                if (!gDTMF_InputMode) {
                    if (gDTMF_CallState == DTMF_CALL_STATE_CALL_OUT) {
                        pPrintStr = DTMF_FindContact(gDTMF_String, Contact) ? Contact : gDTMF_String;
                    } else if (gDTMF_CallState == DTMF_CALL_STATE_RECEIVED || gDTMF_CallState == DTMF_CALL_STATE_RECEIVED_STAY){
                        pPrintStr = DTMF_FindContact(gDTMF_Callee, Contact) ? Contact : gDTMF_Callee;
                    }else if (gDTMF_IsTx) {
                        pPrintStr = gDTMF_String;
                    }
                }

                UI_PrintString(pPrintStr, 2, 0, 2 + (vfo_num * 3), 8);

                pPrintStr = "";
                if (!gDTMF_InputMode) {
                    if (gDTMF_CallState == DTMF_CALL_STATE_CALL_OUT) {
                        pPrintStr = (gDTMF_State == DTMF_STATE_CALL_OUT_RSP) ? "CALL OUT(RSP)" : "CALL OUT";
                    } else if (gDTMF_CallState == DTMF_CALL_STATE_RECEIVED || gDTMF_CallState == DTMF_CALL_STATE_RECEIVED_STAY) {
                        sprintf(String, "CALL FRM:%s", (DTMF_FindContact(gDTMF_Caller, Contact)) ? Contact : gDTMF_Caller);
                        pPrintStr = String;
                    } else if (gDTMF_IsTx) {
                        pPrintStr = (gDTMF_State == DTMF_STATE_TX_SUCC) ? "DTMF TX(SUCC)" : "DTMF TX";
                    }
                }
                else
#endif
                {
                    sprintf(String, ">%s", gDTMF_InputBox);
                    pPrintStr = String;
                }

#ifdef ENABLE_FEAT_F4HWN
                if (isMainOnly())
                {
                    UI_PrintString(pPrintStr, 2, 0, 5, 8);
                    isMainOnlyInputDTMF = true;
                    center_line = CENTER_LINE_IN_USE;
                }
                else
                {
                    UI_PrintString(pPrintStr, 2, 0, 0 + (vfo_num * 3), 8);
                    isMainOnlyInputDTMF = false;
                    center_line = CENTER_LINE_IN_USE;
                    continue;
                }
#else
                UI_PrintString(pPrintStr, 2, 0, 0 + (vfo_num * 3), 8);
                center_line = CENTER_LINE_IN_USE;
                continue;
#endif
            }

            // highlight the selected/used VFO with a marker
            if (isMainVFO)
                memcpy(p_line0 + 0, BITMAP_VFO_Default, sizeof(BITMAP_VFO_Default));
        }
        else // active TX VFO
        {   // highlight the selected/used VFO with a marker
            if (isMainVFO)
                memcpy(p_line0 + 0, BITMAP_VFO_Default, sizeof(BITMAP_VFO_Default));
            else
                memcpy(p_line0 + 0, BITMAP_VFO_NotDefault, sizeof(BITMAP_VFO_NotDefault));
        }

        uint32_t frequency = gEeprom.VfoInfo[vfo_num].pRX->Frequency;

        if(TX_freq_check(frequency) != 0 && gEeprom.VfoInfo[vfo_num].TX_LOCK == true && !FUNCTION_IsRx())
        {
            if(isMainOnly())
                memcpy(p_line0 + 25, BITMAP_VFO_Lock, sizeof(BITMAP_VFO_Lock));
            else
                memcpy(p_line0 + 25, BITMAP_VFO_Lock, sizeof(BITMAP_VFO_Lock));
        }

        if (gCurrentFunction == FUNCTION_TRANSMIT)
        {   // transmitting

#ifdef ENABLE_ALARM
            if (gAlarmState == ALARM_STATE_SITE_ALARM)
                mode = VFO_MODE_RX;
            else
#endif
            {
                if (activeTxVFO == vfo_num)
                {   // show the TX symbol
                    mode = VFO_MODE_TX;
                    //UI_PrintStringSmallBold("TX", 8, 0, line);
                    GUI_DisplaySmallest("TX", 10, line == 0 ? 1 : 33, false, true);

                }
            }
        }
        else
        {   // receiving .. show the RX symbol
            mode = VFO_MODE_RX;
            //if (FUNCTION_IsRx() && gEeprom.RX_VFO == vfo_num) {
            if (FUNCTION_IsRx()) {
                if (gEeprom.RX_VFO == vfo_num && VfoState[vfo_num] == VFO_STATE_NORMAL) {
#ifdef ENABLE_FEAT_F4HWN
                    RxBlinkLed = 1;
                    RxBlinkLedCounter = 0;
                    RxLine = line;
                    RxOnVfofrequency = frequency;
                    // if(!isMainVFO)
                    // {
                    //     RxBlink = 1;
                    // }
                    // else
                    // {
                    //     RxBlink = 0;
                    // }

                    // if (RxBlink == 0 || RxBlink == 1) {
                        if(gRxVfo->Modulation == MODULATION_AM)
                            GUI_DisplaySmallest("AIR", 10, RxLine == 0 ? 1 : 33, false, true);
                        else {
                            #ifdef ENABLE_FEAT_F4HWN_AUDIO
                                strcpy(String, gSubMenu_SET_AUD[gSetting_set_audio]);
                            #else
                                strcpy(String, "RX");
                            #endif
                            GUI_DisplaySmallest(String, 10, RxLine == 0 ? 1 : 33, false, true);
                        }

                        //UI_PrintStringSmallBold("RX", 8, 0, RxLine);
                    // }
#else
                    UI_PrintStringSmallBold("RX", 8, 0, line);
#endif
                }
#ifdef ENABLE_FEAT_F4HWN
                else {
                    if(RxBlinkLed == 1)
                        RxBlinkLed = 2;
                }
            }
            else {
                if(RxOnVfofrequency == frequency && !isMainOnly()) {
                    //UI_PrintStringSmallNormal(">>", 8, 0, line);
                    //memcpy(p_line0 + 14, BITMAP_VFO_Default, sizeof(BITMAP_VFO_Default));
                    GUI_DisplaySmallest(">>", 8, RxLine == 0 ? 1 : 33, false, true);
                }

                if(RxBlinkLed == 1)
                    RxBlinkLed = 2;
            }
#endif
        }

        if (IS_MR_CHANNEL(gEeprom.ScreenChannel[vfo_num]))
        {   // channel mode
            const unsigned int x = 1;
            const bool inputting = gInputBoxIndex != 0 && gEeprom.TX_VFO == vfo_num;
            if (!inputting || gScanStateDir != SCAN_OFF)
                sprintf(String, "%04u", gEeprom.ScreenChannel[vfo_num] + 1);
            else
                sprintf(String, "%.4s", INPUTBOX_GetAscii());  // show the input text

            //if (gSetting_set_gui) {
                UI_PrintStringSmallNormalInverse(String, x, 0, line + 1);
            /*
            }
            else
            {
                GUI_DisplaySmallest(String, x + 1, line == 0 ? 9 : 41, false, true);
                gFrameBuffer[line + 1][0] ^= 0x1C;
                gFrameBuffer[line + 1][1] ^= 0x3E;
                for (uint8_t i = 2; i < 21; i++) {
                    gFrameBuffer[line + 1][i] ^= 0x7F;
                }
                gFrameBuffer[line + 1][21] ^= 0x3E;
                gFrameBuffer[line + 1][22] ^= 0x1C;

            }
            */
        }
        else if (IS_FREQ_CHANNEL(gEeprom.ScreenChannel[vfo_num]))
        {   // frequency mode
            // show the frequency band number
            const unsigned int x = 2;
            const uint8_t f = 1 + gEeprom.ScreenChannel[vfo_num] - FREQ_CHANNEL_FIRST;
            const bool over1GHz = gEeprom.VfoInfo[vfo_num].pRX->Frequency >= _1GHz_in_KHz;

            sprintf(String, over1GHz ? "F%u+" : "F%u", f);
            //if (gSetting_set_gui) {
                UI_PrintStringSmallNormalInverse(String, x, 0, line + 1);
            /*
            }
            else
            {
                GUI_DisplaySmallest(String, x + 2, line == 0 ? 9 : 41, false, true);
                uint8_t g = 13;
                if(over1GHz)
                    g = 17;

                gFrameBuffer[line + 1][0] ^= 0x1C;
                gFrameBuffer[line + 1][1] ^= 0x3E;
                for (uint8_t i = 2; i < g; i++) {
                    gFrameBuffer[line + 1][i] ^= 0x7F;
                }
                gFrameBuffer[line + 1][g] ^= 0x3E;
                gFrameBuffer[line + 1][g + 1] ^= 0x1C;

            }
            */
        }
#ifdef ENABLE_NOAA
        else
        {
            if (gInputBoxIndex == 0 || gEeprom.TX_VFO != vfo_num)
            {   // channel number
                sprintf(String, "N%u", 1 + gEeprom.ScreenChannel[vfo_num] - NOAA_CHANNEL_FIRST);
            }
            else
            {   // user entering channel number
                sprintf(String, "N%u%u", '0' + gInputBox[0], '0' + gInputBox[1]);
            }
            UI_PrintStringSmallNormal(String, 7, 0, line + 1);
        }
#endif

        // ----------------------------------------

        enum VfoState_t state = VfoState[vfo_num];

#ifdef ENABLE_ALARM
        if (gCurrentFunction == FUNCTION_TRANSMIT && gAlarmState == ALARM_STATE_SITE_ALARM) {
            if (activeTxVFO == vfo_num)
                state = VFO_STATE_ALARM;
        }
#endif
        if (state != VFO_STATE_NORMAL)
        {
            if (state < ARRAY_SIZE(VfoStateStr))
                UI_PrintString(VfoStateStr[state], 35, 0, line, 8);
        }
        else if (gInputBoxIndex > 0 && IS_FREQ_CHANNEL(gEeprom.ScreenChannel[vfo_num]) && gEeprom.TX_VFO == vfo_num)
        {   // user entering a frequency
            const char * ascii = INPUTBOX_GetAscii();
            bool isGigaF = frequency>=_1GHz_in_KHz;
            sprintf(String, "%.*s.%.3s", 3 + isGigaF, ascii, ascii + 3 + isGigaF);
#ifdef ENABLE_BIG_FREQ
            if(!isGigaF) {
                // show the remaining 2 small frequency digits
                UI_PrintStringSmallNormal(String + 7, 113, 0, line + 1);
                String[7] = 0;
                // show the main large frequency digits
                UI_DisplayFrequency(String, 32, line, false);
            }
            else
#endif
            {
                // show the frequency in the main font
                UI_PrintString(String, 32, 0, line, 8);
            }

            continue;
        }
        else
        {
            if (gCurrentFunction == FUNCTION_TRANSMIT)
            {   // transmitting
                if (activeTxVFO == vfo_num)
                    frequency = gEeprom.VfoInfo[vfo_num].pTX->Frequency;
            }

            if (IS_MR_CHANNEL(gEeprom.ScreenChannel[vfo_num]))
            {   // it's a channel

                #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
                    if(gEeprom.MENU_LOCK == false) {
                #endif

                const ChannelAttributes_t* att = MR_GetChannelAttributes(gEeprom.ScreenChannel[vfo_num]);


                if(att->exclude == false)
                {
                    // show the scan list assigment symbols
                    const ChannelAttributes_t* att = MR_GetChannelAttributes(gEeprom.ScreenChannel[vfo_num]);

                    uint8_t countList = att->scanlist;
                    if(countList > MR_CHANNELS_LIST + 1) {
                        countList = 0;
                    }

                    const char *displayStr;
                    uint8_t xStart, xDisplay;

                    if (countList == MR_CHANNELS_LIST + 1) {
                        displayStr = "ALL";
                        xStart = 113;
                        xDisplay = 115;
                    } 
                    else if (countList == 0) {
                        displayStr = "OFF";
                        xStart = 113;
                        xDisplay = 115;
                    } 
                    else {
                        // List 1 to MR_CHANNELS_LIST
                        const char *name = gListName[countList - 1];
                        
                        // If name is empty/invalid, display number
                        if (IsEmptyName(name, sizeof(gListName[0]))) {
                            sprintf(String, "%02d", countList);
                            xStart = 117;  // 2-digit number aligned right
                            xDisplay = 119;
                        } 
                        else {
                            sprintf(String, "%.3s", name);
                            xStart = 113;  // 3-char name aligned left
                            xDisplay = 115;
                        }
                        displayStr = String;
                    }

                    GUI_DisplaySmallest(displayStr, xDisplay, line == 0 ? 1 : 33, false, true);

                    gFrameBuffer[line][xStart] ^= 0x3E;
                    for (uint8_t x = xStart + 1; x < 127; x++) {
                        gFrameBuffer[line][x] ^= 0x7F;
                    }
                    gFrameBuffer[line][127] ^= 0x3E;

                }
                else
                {
                    const char *displayStr = "EX";

                    uint8_t xStart = 117;
                    uint8_t xDisplay = 119;
                    
                    GUI_DisplaySmallest(displayStr, xDisplay, line == 0 ? 1 : 33, false, true);

                    gFrameBuffer[line][xStart] ^= 0x3E;
                    for (uint8_t x = xStart + 1; x < 127; x++) {
                        gFrameBuffer[line][x] ^= 0x7F;
                    }
                    gFrameBuffer[line][127] ^= 0x3E;
                }

                #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
                {
                    }
                }
                #endif

                // compander symbol
#ifndef ENABLE_BIG_FREQ
                if (att->compander)
                    memcpy(p_line0 + 120 + LCD_WIDTH, BITMAP_compand, sizeof(BITMAP_compand));
#else
                // TODO:  // find somewhere else to put the symbol
#endif

                switch (gEeprom.CHANNEL_DISPLAY_MODE)
                {
                    case MDF_FREQUENCY: // show the channel frequency
                        sprintf(String, "%3u.%05u", frequency / 100000, frequency % 100000);
#ifdef ENABLE_BIG_FREQ
                        if(frequency < _1GHz_in_KHz) {
                            // show the remaining 2 small frequency digits
                            UI_PrintStringSmallNormal(String + 7, 113, 0, line + 1);
                            String[7] = 0;
                            // show the main large frequency digits
                            UI_DisplayFrequency(String, 32, line, false);
                        }
                        else
#endif
                        {
                            // show the frequency in the main font
                            UI_PrintString(String, 32, 0, line, 8);
                        }

                        break;

                    case MDF_CHANNEL:   // show the channel number
                        sprintf(String, "CH-%04u", gEeprom.ScreenChannel[vfo_num] + 1);
                        UI_PrintString(String, 36, 0, line, 8);
                        break;

                    case MDF_NAME:      // show the channel name
                    case MDF_NAME_FREQ: // show the channel name and frequency

                        SETTINGS_FetchChannelName(String, gEeprom.ScreenChannel[vfo_num]);
                        if (String[0] == 0)
                        {   // no channel name, show the channel number instead
                            sprintf(String, "CH-%04u", gEeprom.ScreenChannel[vfo_num] + 1);
                        }

                        if (gEeprom.CHANNEL_DISPLAY_MODE == MDF_NAME) {
                            String[10] = 0;
                            UI_PrintString(String, 33, 0, line, 8);
                        }
                        else {
#ifdef ENABLE_FEAT_F4HWN
                            if (isMainOnly())
                            {
                                String[10] = 0;
                                UI_PrintString(String, 33, 0, line, 8);
                            }
                            else
                            {
                                if(activeTxVFO == vfo_num) {
                                    UI_PrintStringSmallBold(String, 32 + 4, 0, line);
                                }
                                else
                                {
                                    UI_PrintStringSmallNormal(String, 32 + 4, 0, line);     
                                }
                            }
#else
                            UI_PrintStringSmallBold(String, 32 + 4, 0, line);
#endif

#ifdef ENABLE_FEAT_F4HWN
                            if (isMainOnly())
                            {
                                sprintf(String, "%3u.%05u", frequency / 100000, frequency % 100000);
                                if(frequency < _1GHz_in_KHz) {
                                    // show the remaining 2 small frequency digits
                                    UI_PrintStringSmallNormal(String + 7, 113, 0, line + 4);
                                    String[7] = 0;
                                    // show the main large frequency digits
                                    UI_DisplayFrequency(String, 32, line + 3, false);
                                }
                                else
                                {
                                    // show the frequency in the main font
                                    UI_PrintString(String, 32, 0, line + 3, 8);
                                }
                            }
                            else
                            {
                                sprintf(String, "%03u.%05u", frequency / 100000, frequency % 100000);
                                UI_PrintStringSmallNormal(String, 32 + 4, 0, line + 1);
                            }
#else                           // show the channel frequency below the channel number/name
                            sprintf(String, "%03u.%05u", frequency / 100000, frequency % 100000);
                            UI_PrintStringSmallNormal(String, 32 + 4, 0, line + 1);
#endif
                        }

                        break;
                }
            }
            else
            {   // frequency mode
                sprintf(String, "%3u.%05u", frequency / 100000, frequency % 100000);

#ifdef ENABLE_BIG_FREQ
                if(frequency < _1GHz_in_KHz) {
                    // show the remaining 2 small frequency digits
                    UI_PrintStringSmallNormal(String + 7, 113, 0, line + 1);
                    String[7] = 0;
                    // show the main large frequency digits
                    UI_DisplayFrequency(String, 32, line, false);
                }
                else
#endif
                {
                    // show the frequency in the main font
                    UI_PrintString(String, 32, 0, line, 8);
                }

                // show the channel symbols
                const ChannelAttributes_t* att = MR_GetChannelAttributes(gEeprom.ScreenChannel[vfo_num]);
                if (att->compander)
#ifdef ENABLE_BIG_FREQ
                    memcpy(p_line0 + 120, BITMAP_compand, sizeof(BITMAP_compand));
#else
                    memcpy(p_line0 + 120 + LCD_WIDTH, BITMAP_compand, sizeof(BITMAP_compand));
#endif
            }
        }

        // ----------------------------------------

        {   // show the TX/RX level
            int8_t Level = -1;

            if (mode == VFO_MODE_TX)
            {   // TX power level
                /*
                switch (gRxVfo->OUTPUT_POWER)
                {
                    case OUTPUT_POWER_LOW1:     Level = 2; break;
                    case OUTPUT_POWER_LOW2:     Level = 2; break;
                    case OUTPUT_POWER_LOW3:     Level = 2; break;
                    case OUTPUT_POWER_LOW4:     Level = 2; break;
                    case OUTPUT_POWER_LOW5:     Level = 2; break;
                    case OUTPUT_POWER_MID:      Level = 4; break;
                    case OUTPUT_POWER_HIGH:     Level = 6; break;
                }

                if (gRxVfo->OUTPUT_POWER == OUTPUT_POWER_MID) {
                    Level = 4;
                } else if (gRxVfo->OUTPUT_POWER == OUTPUT_POWER_HIGH) {
                    Level = 6;
                } else {
                    Level = 2;
                }
                */

                uint8_t currentPower = gRxVfo->OUTPUT_POWER;

                if(currentPower == OUTPUT_POWER_USER)
                    Level = gSetting_set_pwr;
                else
                    Level = currentPower - 1;
            }
            else 
            if (mode == VFO_MODE_RX)
            {   // RX signal level
                #ifndef ENABLE_RSSI_BAR
                    // bar graph
                    if (gVFO_RSSI_bar_level[vfo_num] > 0)
                        Level = gVFO_RSSI_bar_level[vfo_num];
                #endif
            }
            if(Level >= 0)
                DrawSmallPowerBars(p_line1 + LCD_WIDTH, Level);
        }

        // ----------------------------------------

        String[0] = '\0';
        const VFO_Info_t *vfoInfo = &gEeprom.VfoInfo[vfo_num];

        // show the modulation symbol
        const char * s = "";
#ifdef ENABLE_FEAT_F4HWN
        const char * t = "";
#endif
        const ModulationMode_t mod = vfoInfo->Modulation;
        switch (mod){
            case MODULATION_FM: {
                const FREQ_Config_t *pConfig = (mode == VFO_MODE_TX) ? vfoInfo->pTX : vfoInfo->pRX;
                const unsigned int code_type = pConfig->CodeType;
#ifdef ENABLE_FEAT_F4HWN
                const char *code_list[] = {"", "CT", "DC", "DC"};
#else
                const char *code_list[] = {"", "CT", "DCS", "DCR"};
#endif
                if (code_type < ARRAY_SIZE(code_list))
                    s = code_list[code_type];
#ifdef ENABLE_FEAT_F4HWN
                if(gCurrentFunction != FUNCTION_TRANSMIT || activeTxVFO != vfo_num)
                    t = gModulationStr[mod];
#endif
                break;
            }
            default:
                t = gModulationStr[mod];
            break;
        }

#if ENABLE_FEAT_F4HWN
        const FREQ_Config_t *pConfig = (mode == VFO_MODE_TX) ? vfoInfo->pTX : vfoInfo->pRX;
        int8_t shift = 0;

        switch((int)pConfig->CodeType)
        {
            case 1:
            sprintf(String, "%u.%u", CTCSS_Options[pConfig->Code] / 10, CTCSS_Options[pConfig->Code] % 10);
            break;

            case 2:
            sprintf(String, "%03oN", DCS_Options[pConfig->Code]);
            break;

            case 3:
            sprintf(String, "%03oI", DCS_Options[pConfig->Code]);
            break;

            default:
            sprintf(String, "%d.%02uK", vfoInfo->StepFrequency / 100, vfoInfo->StepFrequency % 100);
            shift = -10;
        }

        if (gSetting_set_gui)
        {
            UI_PrintStringSmallNormal(s, LCD_WIDTH + 22, 0, line + 1);
            UI_PrintStringSmallNormal(t, LCD_WIDTH + 2, 0, line + 1);

            if (isMainOnly() && !gDTMF_InputMode)
            {
                if(shift == 0)
                {
                    UI_PrintStringSmallNormal(String, 2, 0, 6);
                }

                if((vfoInfo->StepFrequency / 100) < 100)
                {
                    sprintf(String, "%d.%02uK", vfoInfo->StepFrequency / 100, vfoInfo->StepFrequency % 100);
                }
                else
                {
                    sprintf(String, "%dK", vfoInfo->StepFrequency / 100);               
                }
                UI_PrintStringSmallNormal(String, 46, 0, 6);
            }
        }
        else
        {
            if ((s != NULL) && (s[0] != '\0')) {
                GUI_DisplaySmallest(s, 58, line == 0 ? 17 : 49, false, true);
            }

            if ((t != NULL) && (t[0] != '\0')) {
                GUI_DisplaySmallest(t, 3, line == 0 ? 17 : 49, false, true);
            }

            GUI_DisplaySmallest(String, 68 + shift, line == 0 ? 17 : 49, false, true);

            //sprintf(String, "%d.%02u", vfoInfo->StepFrequency / 100, vfoInfo->StepFrequency % 100);
            //GUI_DisplaySmallest(String, 91, line == 0 ? 2 : 34, false, true);
        }
#else
        UI_PrintStringSmallNormal(s, LCD_WIDTH + 24, 0, line + 1);
#endif

        if (state == VFO_STATE_NORMAL || state == VFO_STATE_ALARM)
        {   // show the TX power
            uint8_t currentPower = vfoInfo->OUTPUT_POWER % 8;
            uint8_t arrowPos = 19;
            bool userPower = false;

            if(currentPower == OUTPUT_POWER_USER)
            {
                currentPower = gSetting_set_pwr;
                userPower = true;
            }
            else
            {
                currentPower--;
                userPower = false;
            }

            if (gSetting_set_gui)
            {
                const char pwr_short[][3] = {"L1", "L2", "L3", "L4", "L5", "M", "H"};
                //sprintf(String, "%s", pwr_short[currentPower]);
                //UI_PrintStringSmallNormal(String, LCD_WIDTH + 42, 0, line + 1);
                UI_PrintStringSmallNormal(pwr_short[currentPower], LCD_WIDTH + 42, 0, line + 1);

                arrowPos = 38;
            }
            else
            {
                const char pwr_long[][5] = {"LOW1", "LOW2", "LOW3", "LOW4", "LOW5", "MID", "HIGH"};
                //sprintf(String, "%s", pwr_long[currentPower]);
                //GUI_DisplaySmallest(String, 24, line == 0 ? 17 : 49, false, true);
                GUI_DisplaySmallest(pwr_long[currentPower], 24, line == 0 ? 17 : 49, false, true);
            }

            if(userPower == true)
            {
                memcpy(p_line0 + 256 + arrowPos, BITMAP_PowerUser, sizeof(BITMAP_PowerUser));
            }
        }

        if (vfoInfo->freq_config_RX.Frequency != vfoInfo->freq_config_TX.Frequency)
        {   // show the TX offset symbol
            int i = vfoInfo->TX_OFFSET_FREQUENCY_DIRECTION % 3;

            #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
                const char dir_list[][2] = {"", "+", "-", "D"};

                if(gTxVfo->TX_OFFSET_FREQUENCY_DIRECTION != 0 && gTxVfo->pTX == &gTxVfo->freq_config_RX && !vfoInfo->FrequencyReverse)
                {
                    i = 3;
                }
            #else
                const char dir_list[][2] = {"", "+", "-"};
            #endif

#if ENABLE_FEAT_F4HWN
        if (gSetting_set_gui)
        {
            UI_PrintStringSmallNormal(dir_list[i], LCD_WIDTH + 60, 0, line + 1);
        }
        else
        {
            #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
            if(i == 3)
            {
                GUI_DisplaySmallest(dir_list[i], 43, line == 0 ? 17 : 49, false, true);
            }
            else
            {
            #endif
            UI_PrintStringSmallNormal(dir_list[i], LCD_WIDTH + 41, 0, line + 1);
            #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
            }
            #endif
        }
#else
            UI_PrintStringSmallNormal(dir_list[i], LCD_WIDTH + 54, 0, line + 1);
#endif
        }

        // show the TX/RX reverse symbol
        if (vfoInfo->FrequencyReverse)
#if ENABLE_FEAT_F4HWN
        {
            if (gSetting_set_gui)
            {
                UI_PrintStringSmallNormal("R", LCD_WIDTH + 68, 0, line + 1);
            }
            else
            {
                GUI_DisplaySmallest("R", 51, line == 0 ? 17 : 49, false, true);
            }
        }
#else
            UI_PrintStringSmallNormal("R", LCD_WIDTH + 62, 0, line + 1);
#endif

#if ENABLE_FEAT_F4HWN
        #ifdef ENABLE_FEAT_F4HWN_NARROWER
            bool narrower = 0;

            if(vfoInfo->CHANNEL_BANDWIDTH == BANDWIDTH_NARROW && gSetting_set_nfm == 1)
            {
                narrower = 1;
            }

            if (gSetting_set_gui)
            {
                const char *bandWidthNames[] = {"W", "N", "N+"};
                UI_PrintStringSmallNormal(bandWidthNames[vfoInfo->CHANNEL_BANDWIDTH + narrower], LCD_WIDTH + 80, 0, line + 1);
            }
            else
            {
                const char *bandWidthNames[] = {"WIDE", "NAR", "NAR+"};
                GUI_DisplaySmallest(bandWidthNames[vfoInfo->CHANNEL_BANDWIDTH + narrower], 91, line == 0 ? 17 : 49, false, true);
            }
        #else
            if (gSetting_set_gui)
            {
                const char *bandWidthNames[] = {"W", "N"};
                UI_PrintStringSmallNormal(bandWidthNames[vfoInfo->CHANNEL_BANDWIDTH], LCD_WIDTH + 80, 0, line + 1);
            }
            else
            {
                const char *bandWidthNames[] = {"WIDE", "NAR"};
                GUI_DisplaySmallest(bandWidthNames[vfoInfo->CHANNEL_BANDWIDTH], 91, line == 0 ? 17 : 49, false, true);
            }
        #endif
#else
        if (vfoInfo->CHANNEL_BANDWIDTH == BANDWIDTH_NARROW)
            UI_PrintStringSmallNormal("N", LCD_WIDTH + 70, 0, line + 1);
#endif

#ifdef ENABLE_DTMF_CALLING
        // show the DTMF decoding symbol
        if (vfoInfo->DTMF_DECODING_ENABLE || gSetting_KILLED)
            UI_PrintStringSmallNormal("DTMF", LCD_WIDTH + 78, 0, line + 1);
#endif

#ifndef ENABLE_FEAT_F4HWN
        // show the audio scramble symbol
        if (vfoInfo->SCRAMBLING_TYPE > 0 && gSetting_ScrambleEnable)
            UI_PrintStringSmallNormal("SCR", LCD_WIDTH + 106, 0, line + 1);
#endif

#ifdef ENABLE_FEAT_F4HWN
        /*
        if(isMainVFO)   
        {
            if(gMonitor)
            {
                sprintf(String, "%s", "MONI");
            }
            
            if (gSetting_set_gui)
            {
                if(!gMonitor)
                {
                    sprintf(String, "SQL%d", gEeprom.SQUELCH_LEVEL);
                }
                UI_PrintStringSmallNormal(String, LCD_WIDTH + 98, 0, line + 1);
            }
            else
            {
                if(!gMonitor)
                {
                    sprintf(String, "SQL%d", gEeprom.SQUELCH_LEVEL);
                }
                GUI_DisplaySmallest(String, 110, line == 0 ? 17 : 49, false, true);
            }
        }
        */
        if (isMainVFO) {
           if (gMonitor) {
                strcpy(String, "MONI");
           } else {
                sprintf(String, "SQL%d", gEeprom.SQUELCH_LEVEL);
           }

           if (gSetting_set_gui) {
                UI_PrintStringSmallNormal(String, LCD_WIDTH + 98, 0, line + 1);
           } else {
                GUI_DisplaySmallest(String, 110, line == 0 ? 17 : 49, false, true);
           }
        }
#endif
    }

display_main_after_vfo_loop:

#ifdef ENABLE_FEAT_F4HWN
    if (DualVfoMainFreqEntryScreen() && !(gEeprom.KEY_LOCK && gKeypadLocked > 0))
    {
        ST7565_BlitMainPerMode();
        return;
    }
#endif

#ifdef ENABLE_AGC_SHOW_DATA
#ifdef ENABLE_FEAT_F4HWN
    if (!isMainOnly() && !DualVfoShouldUseLegacyMain()) {
        /* new dual layout uses row 3 for top VFO detail */
    } else
#endif
    {
        center_line = CENTER_LINE_IN_USE;
        UI_MAIN_PrintAGC(false);
    }
#endif

    if (center_line == CENTER_LINE_NONE)
    {   // we're free to use the middle line

        const bool rx = FUNCTION_IsRx();

#ifdef ENABLE_FEAT_F4HWN_AUDIO_SCOPE
        if (gSetting_mic_bar && gCurrentFunction == FUNCTION_TRANSMIT) {
            // Reserve the line so no other element overwrites it.
            // Actual drawing is handled exclusively by the app.c timeslice.
            center_line = CENTER_LINE_AUDIO_SCOPE;
        }
        else
#endif
#ifdef ENABLE_AUDIO_BAR
        if (gSetting_mic_bar && gCurrentFunction == FUNCTION_TRANSMIT) {
#ifdef ENABLE_FEAT_F4HWN
            if (!isMainOnly() && !DualVfoShouldUseLegacyMain()) {
                /* New dual layout uses all framebuffer rows */
            } else
#endif
            {
                center_line = CENTER_LINE_AUDIO_BAR;
                UI_DisplayAudioBar();
            }
        }
        else
#endif

#if defined(ENABLE_AM_FIX) && defined(ENABLE_AM_FIX_SHOW_DATA)
        if (rx && gEeprom.VfoInfo[gEeprom.RX_VFO].Modulation == MODULATION_AM && gSetting_AM_fix)
        {
            if (gScreenToDisplay != DISPLAY_MAIN
#ifdef ENABLE_DTMF_CALLING
                || gDTMF_CallState != DTMF_CALL_STATE_NONE
#endif
                )
                return;

#ifdef ENABLE_FEAT_F4HWN
            if (!isMainOnly() && !DualVfoShouldUseLegacyMain()) {
                /* new dual layout uses full framebuffer */
            } else
#endif
            {
                center_line = CENTER_LINE_AM_FIX_DATA;
                AM_fix_print_data(gEeprom.RX_VFO, String);
                UI_PrintStringSmallNormal(String, 2, 0, 3);
            }
        }
        else
#endif

#ifdef ENABLE_RSSI_BAR
        if (rx) {
#ifdef ENABLE_FEAT_F4HWN
            if (!isMainOnly() && !DualVfoShouldUseLegacyMain()) {
                DisplayRSSIBar(false);
            } else
#endif
            {
                center_line = CENTER_LINE_RSSI;
                DisplayRSSIBar(false);
            }
        }
        else
#endif
        if (rx || gCurrentFunction == FUNCTION_FOREGROUND || gCurrentFunction == FUNCTION_POWER_SAVE)
        {
            #if 1
                if (gSetting_live_DTMF_decoder && gDTMF_RX_live[0] != 0 && gKeypadLocked == 0)
                {   // show live DTMF decode
                    const unsigned int len = strlen(gDTMF_RX_live);
                    const unsigned int idx = (len > (17 - 5)) ? len - (17 - 5) : 0;  // limit to last 'n' chars

                    if (gScreenToDisplay != DISPLAY_MAIN
#ifdef ENABLE_DTMF_CALLING
                        || gDTMF_CallState != DTMF_CALL_STATE_NONE
#endif
                        )
                        return;

                    center_line = CENTER_LINE_DTMF_DEC;

                    sprintf(String, "DTMF %s", gDTMF_RX_live + idx);
#ifdef ENABLE_FEAT_F4HWN
                    if (!isMainOnly() && !DualVfoShouldUseLegacyMain()) {
                        /* new dual panel: no spare row for live DTMF strip */
                    }
                    else if (isMainOnly())
                    {
                        UI_PrintStringSmallNormal(String, 2, 0, 5);
                    }
                    else
                    {
                        UI_PrintStringSmallNormal(String, 2, 0, 3);
                    }
#else
                    UI_PrintStringSmallNormal(String, 2, 0, 3);

#endif
                }
            #else
                if (gSetting_live_DTMF_decoder && gDTMF_RX_index > 0)
                {   // show live DTMF decode
                    const unsigned int len = gDTMF_RX_index;
                    const unsigned int idx = (len > (17 - 5)) ? len - (17 - 5) : 0;  // limit to last 'n' chars

                    if (gScreenToDisplay != DISPLAY_MAIN ||
                        gDTMF_CallState != DTMF_CALL_STATE_NONE)
                        return;

                    center_line = CENTER_LINE_DTMF_DEC;

                    sprintf(String, "DTMF %s", gDTMF_RX_live + idx);
                    UI_PrintStringSmallNormal(String, 2, 0, 3);
                }
            #endif

#ifdef ENABLE_SHOW_CHARGE_LEVEL
            else if (gChargingWithTypeC)
            {   // charging .. show the battery state
                if (gScreenToDisplay != DISPLAY_MAIN
#ifdef ENABLE_DTMF_CALLING
                    || gDTMF_CallState != DTMF_CALL_STATE_NONE
#endif
                    )
                    return;

#ifdef ENABLE_FEAT_F4HWN
                if (!isMainOnly() && !DualVfoShouldUseLegacyMain()) {
                    /* new dual layout uses full framebuffer */
                } else
#endif
                {
                    center_line = CENTER_LINE_CHARGE_DATA;

                    sprintf(String, "Charge %u.%02uV %u%%",
                        gBatteryVoltageAverage / 100, gBatteryVoltageAverage % 100,
                        BATTERY_VoltsToPercent(gBatteryVoltageAverage));
                    UI_PrintStringSmallNormal(String, 2, 0, 3);
                }
            }
#endif
        }
    }

#ifdef ENABLE_FEAT_F4HWN
    //#ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
    //if(gEeprom.MENU_LOCK == false)
    //{
    //#endif
    if (isMainOnly() && !gDTMF_InputMode)
    {
        sprintf(String, "VFO %s", activeTxVFO ? "B" : "A");
        GUI_DisplaySmallest(String, 107, 50, false, true);

        gFrameBuffer[6][105] ^= 0x7C;
        for (uint8_t x = 106; x < 127; x++) {
            gFrameBuffer[6][x] ^= 0xFE;
        }
        gFrameBuffer[6][127] ^= 0x7C;

        /*
        UI_PrintStringSmallBold(String, 92, 0, 6);
        for (uint8_t i = 92; i < 128; i++)
        {
            gFrameBuffer[6][i] ^= 0x7F;
        }
        */
    }
    //#ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
    //}
    //#endif
#endif

#ifdef ENABLE_FEAT_F4HWN
    ST7565_BlitMainPerMode();
#else
    ST7565_BlitFullScreen();
#endif
}
