// ============================================================================
//  config.example.h — 設定範本（複製成 config.h 再填入自己的值；沒有 config.h 時自動使用本檔）
//  ⚠ config.h 含 API Key，請勿上傳到 GitHub 或分享給他人
//  Wi-Fi 與 API Key 也可以開機後用手機開啟螢幕上的網址修改
// ============================================================================
#pragma once

// ---------------------------------------------------------------- Wi-Fi ----
#define WIFI_SSID       "你的WiFi名稱"
#define WIFI_PASSWORD   "你的WiFi密碼"

// --------------------------------------------------------------- OpenAI ----
#define OPENAI_API_KEY  "sk-請填入你的OpenAI金鑰"

#define STT_MODEL       "gpt-4o-mini-transcribe"   // 或 "whisper-1"
#define CHAT_MODEL      "gpt-4o-mini"
#define TTS_MODEL       "gpt-4o-mini-tts"          // 或 "tts-1"
#define TTS_VOICE       "nova"                     // alloy / ash / coral / echo / fable / nova / onyx / sage / shimmer
#define TTS_INSTRUCTIONS "用可愛、活潑、溫暖的台灣口音中文說話，語氣像貼心的小朋友，語速稍快。"

#define ROBOT_NAME      "小柯"
#define MAX_HISTORY_MSGS 10        // 記住最近幾則對話（user+assistant 各算 1 則）
#define CHAT_MAX_TOKENS  300

// ------------------------------------------------------------ 上網搜尋 ----
// 1 = 問天氣、新聞、股價等即時資訊時，AI 會自己決定上網搜尋再回答（OpenAI web_search，搜尋會另外計費）
#define ENABLE_WEB_SEARCH 1
#define SEARCH_CITY       "Taipei"   // 搜尋預設地區（英文城市名）
#define SEARCH_CITY_ZH    "台北"     // 沒講地點時以這裡為準

// ------------------------------------------------------------- 功能開關 ----
#define HW_TEST_MODE    0          // 1 = 開機只跑硬體測試（螢幕/喇叭/麥克風/鏡頭/按鍵），先確認硬體用
#define ENABLE_CAMERA   1          // 1 = 說「你看到什麼」「這是什麼」會用頭頂鏡頭拍照給 AI 看
#define ENABLE_CHASSIS  1          // 1 = 接上履帶底座時可以說「向前走」「跳個舞」「開燈」

// ------------------------------------------------------------ 唱歌（YouTube 歌單）----
// 說「唱首歌」會從這個播放清單隨機選一首，顯示封面、歌名與 QR 碼（手機掃描在 YouTube 播放）
#define YT_PLAYLIST_URL   "https://www.youtube.com/playlist?list=PLMiXt5EIkXI0"
#define SONG_COMPOSER     "吳玉柱"   // 詞曲創作者（版權所有）
#define SONG_SHOW_SEC     180        // 唱歌畫面停留秒數（按鍵可提早回到聊天）

// ------------------------------------------------------------ 聲控（免按鍵）----
#define VOICE_ACTIVATION  1        // 1 = 直接說話就會開始聆聽；0 = 只用按鍵（按住 BOOT 說話）
#define VAD_MIN_RMS       250      // 觸發錄音的最低音量（環境吵、常誤觸就調高；喊很大聲才有反應就調低）
#define VAD_RATIO         3.0      // 音量需高於背景噪音幾倍才觸發
#define VAD_SILENCE_MS    900      // 停頓多久視為說完
#define VAD_MIN_SPEECH_MS 400      // 少於這個長度的聲音視為雜音
#define SLEEPY_AFTER_SEC  90       // 多久沒互動表情變想睡

// ------------------------------------------------------------------ 音訊 ----
#define MAX_RECORD_SEC   12
#define DEFAULT_VOLUME   70        // 0~100
#define MIC_PGA_GAIN     10        // ES8311 麥克風類比增益 0~10（x3dB，10 = 30dB）
#define MIC_SOFT_GAIN    2         // 數位增益倍數（錄音太小聲調大）

// ------------------------------------------------------------------ 螢幕 ----
#define TFT_ROTATION     2         // 0~3（2 = 與原廠相同方向，offset 0）
#define TFT_INVERT       1         // IPS 面板要反相；顏色黑白顛倒就改 0
#define CAMERA_FLIP      0         // 鏡頭畫面上下顛倒就改 1（本機實測 0）

// ================================================================ 腳位 ====
//  ESP-SparkBot 頭部（ESP32-S3-WROOM-1 N16R8），取自 Espressif 官方 / xiaozhi esp-sparkbot 板定義
//  這些是板子上固定的接線，不需要修改
// ---------------------------------------------------------------------------
#define SB_I2C_SDA      4          // ES8311 與鏡頭 SCCB 共用
#define SB_I2C_SCL      5
#define SB_I2S_MCLK     45
#define SB_I2S_BCLK     39
#define SB_I2S_WS       41
#define SB_I2S_DOUT     42         // → ES8311 DAC（喇叭）
#define SB_I2S_DIN      40         // ← ES8311 ADC（麥克風）
#define SB_PA_BL        46         // 功放開關與螢幕背光共用同一支腳，必須一直保持 HIGH
#define SB_LCD_SCK      21
#define SB_LCD_MOSI     47
#define SB_LCD_CS       44
#define SB_LCD_DC       43
#define SB_UART_TX      38         // 底部磁吸介面 → 履帶底座（UART 115200）
#define SB_UART_RX      48
#define PIN_BUTTON      0          // BOOT 鍵（按下為 LOW）

// OV2640 鏡頭（DVP）
#define CAM_PIN_XCLK    15
#define CAM_PIN_PCLK    13
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_D0      11
#define CAM_PIN_D1      9
#define CAM_PIN_D2      8
#define CAM_PIN_D3      10
#define CAM_PIN_D4      12
#define CAM_PIN_D5      18
#define CAM_PIN_D6      17
#define CAM_PIN_D7      16
