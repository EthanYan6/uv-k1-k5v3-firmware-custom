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

#include "app/chFrScanner.h"
#ifdef ENABLE_FMRADIO
    #include "app/fm.h"
#endif
#include "app/scanner.h"
#include "bitmaps.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "settings.h"
#include "ui/battery.h"
#include "ui/helper.h"
#include "ui/ui.h"
#include "ui/status.h"
#include "radio.h"

#ifdef ENABLE_FEAT_F4HWN_RX_TX_TIMER
#ifndef ENABLE_FEAT_F4HWN_DEBUG
static void convertTime(uint8_t *line, uint8_t type)
{
    uint16_t t = (type == 0) ? (gTxTimerCountdown_500ms / 2) : (3600 - gRxTimerCountdown_500ms / 2);
    uint16_t m = t / 60;
    uint8_t s = (uint8_t)(t % 60);

    gStatusLine[0] = gStatusLine[7] = gStatusLine[14] = 0x00;

    char str[10];
    sprintf(str, "%02u:%02u", (unsigned)m, s);
    UI_PrintStringSmallBufferNormal(str, line);

    gUpdateStatus = true;
}
#endif
#endif

void UI_DisplayStatus()
{
    char str[8] = "";

    gUpdateStatus = false;
    memset(gStatusLine, 0, sizeof(gStatusLine));

#ifdef ENABLE_FEAT_F4HWN
    // 主页面 (MAIN ONLY): 定制顶部菜单栏
    if (gScreenToDisplay == DISPLAY_MAIN && !gAirCopyBootMode &&
        gEeprom.DUAL_WATCH == DUAL_WATCH_OFF && gEeprom.CROSS_BAND_RX_TX == CROSS_BAND_OFF) {
        uint8_t *line = gStatusLine;
        unsigned int x = 0;
        const uint8_t vfo = gEeprom.TX_VFO;
        const VFO_Info_t *pVfo = &gEeprom.VfoInfo[vfo];

        // 1. 天线图标
        memcpy(line + x, BITMAP_Antenna, sizeof(BITMAP_Antenna));
        x += 6;

        // 2. 5格信号条：向左 2 像素、向上 1 像素，条间 1 像素间隔
        x -= 2;  // 整体左移 2 像素
        uint8_t bars = 0;
        if (FUNCTION_IsRx()) {
            bars = (gVFO_RSSI_bar_level[vfo] * 5 + 5) / 6;
            if (bars > 5) bars = 5;
        }
        for (uint8_t i = 0; i < 5; i++) {
            uint8_t h = i + 1;  // 高度 1,2,3,4,5
            uint8_t mask = ((1u << h) - 1u) << (7 - h);  // 上移 1 像素：留顶 1 像素空
            if (i < bars)
                line[x + (unsigned int)i * 2u] |= mask;  // 每条 1 列，间隔 1 列
        }
        x += 10;  // 5 条 × 2（条+间隔）

        // 3. |->| 直频图标 (仅当 RX 频率 == TX 频率时显示)
        if (pVfo->freq_config_RX.Frequency == pVfo->freq_config_TX.Frequency) {
            GUI_DisplaySmallest("|->|", x, 1, true, true);
            x += 22;
        }

        // 4. 功率 (L1/L2/.../M/H)
        {
            const char *pwr[] = {"L1","L2","L3","L4","L5","M","H"};
            uint8_t idx = pVfo->OUTPUT_POWER;
            if (idx == OUTPUT_POWER_USER)
                idx = gSetting_set_pwr + 1;  // 1..7
            if (idx >= 1 && idx <= 7) {
                GUI_DisplaySmallest(pwr[idx - 1], x, 1, true, true);
                x += 14;
            }
        }

        // 5. 宽带/窄带 + 带宽
        {
            const char *bw = (pVfo->CHANNEL_BANDWIDTH == BANDWIDTH_WIDE) ? "W" : "N";
            GUI_DisplaySmallest(bw, x, 1, true, true);
            x += 6;
            sprintf(str, "%s", (pVfo->CHANNEL_BANDWIDTH == BANDWIDTH_WIDE) ? "25K" : "12K");
            GUI_DisplaySmallest(str, x, 1, true, true);
            x += 14;
        }

        // 6. 静噪值 (0-9)
        {
            uint8_t sq = (pVfo->SquelchOpenRSSIThresh * 9u + 255u) / 256u;
            if (sq > 9) sq = 9;
            sprintf(str, "%u", sq);
            GUI_DisplaySmallest(str, x, 1, true, true);
            x += 6;
        }

        // 7. 步进
        {
            sprintf(str, "%d.%02uK", pVfo->StepFrequency / 100, pVfo->StepFrequency % 100);
            GUI_DisplaySmallest(str, x, 1, true, true);
            x += 22;
        }

        // 8. 电池 (右侧)
        x = LCD_WIDTH - sizeof(BITMAP_BatteryLevel1) - 2;
        UI_DrawBattery(line + x, gBatteryDisplayLevel, gLowBatteryBlink);

        ST7565_BlitStatusLine();
        return;
    }
#endif

    uint8_t     *line = gStatusLine;
    unsigned int x    = 0;

#ifdef ENABLE_NOAA
    // NOAA indicator
    if (!(gScanStateDir != SCAN_OFF || SCANNER_IsScanning()) && gIsNoaaMode) { // NOASS SCAN indicator
        memcpy(line + x, BITMAP_NOAA, sizeof(BITMAP_NOAA));
    }
    // Power Save indicator
    else if (gCurrentFunction == FUNCTION_POWER_SAVE) {
        memcpy(line + x, gFontPowerSave, sizeof(gFontPowerSave));
    }
    x += 8;
#else
    // Power Save indicator
    if (gCurrentFunction == FUNCTION_POWER_SAVE) {
        memcpy(line + x, gFontPowerSave, sizeof(gFontPowerSave));
    }
    x += 8;
#endif

    unsigned int x1 = x;

#ifdef ENABLE_DTMF_CALLING
    if (gSetting_KILLED) {
        memset(line + x, 0xFF, 10);
        x1 = x + 10;
    }
    else
#endif
    { // SCAN indicator
        if (gScanStateDir != SCAN_OFF || SCANNER_IsScanning()) {
            if (IS_MR_CHANNEL(gNextMrChannel) && !SCANNER_IsScanning()) { // channel mode

                uint8_t end = 0;

                if(gEeprom.SCAN_LIST_DEFAULT == MR_CHANNELS_LIST + 1)
                {
                    if(gEeprom.SCAN_LIST_ENABLED==1) {
                        sprintf(str, "%s+", "ALL");
                        end = 19;
                    }
                    else
                    {
                        sprintf(str, "%s", "ALL");
                        end = 15;
                    }
                }
                else
                {
                    if(gEeprom.SCAN_LIST_ENABLED==1) {
                        sprintf(str, "%02d+", gEeprom.SCAN_LIST_DEFAULT);
                        end = 15;
                    }
                    else {
                        sprintf(str, "%02d", gEeprom.SCAN_LIST_DEFAULT);
                        end = 11;
                    }
                }

                GUI_DisplaySmallest(str, 2, 1, true, true);
                for (uint8_t x = 0; x < end; x++)
                {
                    gStatusLine[x] ^= 0x7F;
                }
            }
            else {  // frequency mode
                memcpy(line + x + 1, gFontS, sizeof(gFontS));
                //UI_PrintStringSmallBufferNormal("S", line + x + 1);
            }
            x1 = x + 10;
        }
    }
    x += 10;  // font character width

    #ifdef ENABLE_FEAT_F4HWN_DEBUG
        // Only for debug
        // Only for debug
        // Only for debug

        sprintf(str, "%d", gDebug);
        UI_PrintStringSmallBufferNormal(str, line + x + 1);
        x += 16;
    #else
        #ifdef ENABLE_VOICE
        // VOICE indicator
        if (gEeprom.VOICE_PROMPT != VOICE_PROMPT_OFF){
            memcpy(line + x, BITMAP_VoicePrompt, sizeof(BITMAP_VoicePrompt));
            x1 = x + sizeof(BITMAP_VoicePrompt);
        }
        x += sizeof(BITMAP_VoicePrompt);
        #endif

        if(!SCANNER_IsScanning()) {
        #ifdef ENABLE_FEAT_F4HWN_RX_TX_TIMER
            if(gCurrentFunction == FUNCTION_TRANSMIT && gSetting_set_tmr == true)
            {
                convertTime(line, 0);
            }
            else if(FUNCTION_IsRx() && gSetting_set_tmr == true)
            {
                convertTime(line, 1);
            }
            else
        #endif
            {
                #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
                if(gEeprom.MENU_LOCK == true) {
                    memcpy(line + x + 2, gFontRO, sizeof(gFontRO));
                }
                else
                {
                #endif
                    uint8_t dw = (gEeprom.DUAL_WATCH != DUAL_WATCH_OFF) + (gEeprom.CROSS_BAND_RX_TX != CROSS_BAND_OFF) * 2;
                    if(dw == 1 || dw == 3) { // DWR - dual watch + respond
                        if(gDualWatchActive)
                            memcpy(line + x + (dw==1?0:2), gFontDWR, sizeof(gFontDWR) - (dw==1?0:5));
                        else
                            memcpy(line + x + 3, gFontHold, sizeof(gFontHold));
                    }
                    else if(dw == 2) { // XB - crossband
                        memcpy(line + x + 2, gFontXB, sizeof(gFontXB));
                    }
                    else
                    {
                        if(!gAirCopyBootMode)
                            memcpy(line + x + 2, gFontMO, sizeof(gFontMO));
                    }
                #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
                }
                #endif
            }
        }
        x += sizeof(gFontDWR) + 3;
    #endif

#ifdef ENABLE_VOX
    // VOX indicator
    if (gEeprom.VOX_SWITCH) {
        memcpy(line + x, gFontVox, sizeof(gFontVox));
        x1 = x + sizeof(gFontVox) + 1;
    }
    x += sizeof(gFontVox) + 3;
#endif

#ifdef ENABLE_FEAT_F4HWN
    // PTT indicator
    if(!gAirCopyBootMode) {
        if (gSetting_set_ptt_session) {
            memcpy(line + x, gFontPttOnePush, sizeof(gFontPttOnePush));
            x1 = x + sizeof(gFontPttOnePush) + 1;
        }
        else
        {
            memcpy(line + x, gFontPttClassic, sizeof(gFontPttClassic));
            x1 = x + sizeof(gFontPttClassic) + 1;       
        }
    }
    x += sizeof(gFontPttClassic) + 3;
#endif

    x = MAX(x1, 69u);

    const void *src = NULL;   // Pointer to the font/bitmap to copy
    size_t size = 0;          // Size of the font/bitmap

    // Determine the source and size based on conditions
    if (gEeprom.KEY_LOCK) {
        src = gFontKeyLock;
        size = sizeof(gFontKeyLock);
    }
    else if (gWasFKeyPressed) {
        #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
        if (!gEeprom.MENU_LOCK) {
            src = gFontF;
            size = sizeof(gFontF);
        }
        #else
        src = gFontF;
        size = sizeof(gFontF);
        #endif
    }
    #ifdef ENABLE_FEAT_F4HWN
        else if (gMute) {
            src = gFontMute;
            size = sizeof(gFontMute);
        }
    #endif
    else if (gBackLight) {
        src = gFontLight;
        size = sizeof(gFontLight);
    }
    #ifdef ENABLE_FEAT_F4HWN_CHARGING_C
    else if (gChargingWithTypeC) {
        src = BITMAP_USB_C;
        size = sizeof(BITMAP_USB_C);
    }
    #endif

    // Perform the memcpy if a source was selected
    if (src) {
        memcpy(line + x + 1, src, size);
    }

    // Battery
    unsigned int x2 = LCD_WIDTH - sizeof(BITMAP_BatteryLevel1) - 0;

    UI_DrawBattery(line + x2, gBatteryDisplayLevel, gLowBatteryBlink);

    bool BatTxt = true;

    switch (gSetting_battery_text) {
        default:
        case 0:
            BatTxt = false;
            break;

        case 1:    // voltage
            const uint16_t voltage = (gBatteryVoltageAverage <= 999) ? gBatteryVoltageAverage : 999; // limit to 9.99V
            sprintf(str, "%u.%02u", voltage / 100, voltage % 100);
            break;

        case 2:     // percentage
            //gBatteryVoltageAverage = 999;
            sprintf(str, "%02u%%", BATTERY_VoltsToPercent(gBatteryVoltageAverage));
            break;
    }

    if (BatTxt) {
        x2 -= (7 * strlen(str));
        UI_PrintStringSmallBufferNormal(str, line + x2);
        /*
        uint8_t shift = (strlen(str) < 5) ? 92 : 88;
        GUI_DisplaySmallest(str, shift, 1, true, true);

        for (uint8_t i = shift - 2; i < 110; i++) {
            gStatusLine[i] ^= 0x7F; // invert
        }
        */
    }

    // **************

    ST7565_BlitStatusLine();
}
