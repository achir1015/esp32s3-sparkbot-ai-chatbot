# ESP-SparkBot AI 桌上型機器人（OpenAI 版）

把 ESP-SparkBot 頭部原廠的「小智」韌體換成 **小柯 OpenAI 版**：直接說話 → OpenAI 語音轉文字 → GPT 回答 → OpenAI TTS 從喇叭講出來；1.54 吋螢幕顯示可愛表情、時鐘、農曆和對話文字。頭頂鏡頭可以拍照給 AI 看，接上履帶底座還能用語音控制移動與燈光。

由小柯 OpenAI 版（`E:\AI聊天機器人組合套件ESP32-S3 N16R8開發板\XiaoKe_OpenAI`）移植，音訊驅動參考 ESP32-S3-BOX-3 版的 ES8311 設定。

## 硬體（ESP-SparkBot 頭部）

| 元件 | 說明 |
|---|---|
| 主控 | ESP32-S3-WROOM-1 N16R8（16MB Flash、8MB OPI PSRAM），USB-JTAG → COM7 |
| 螢幕 | 1.54" ST7789 240×240 |
| 音訊 | ES8311（喇叭 DAC + 類比麥克風 ADC 同一顆）+ 功放 |
| 鏡頭 | OV2640（DVP）|
| 底座 | 底部 4P 磁吸介面（5V / GPIO48 / GPIO38 / GND），UART 115200 |

腳位（取自 Espressif 官方 / xiaozhi `esp-sparkbot` 板定義，已寫在 `config.h`，不需要改）：

| 元件 | 腳位 |
|---|---|
| ST7789 | SCK=21、MOSI=47、CS=44、DC=43、無 RST |
| 背光 + 功放 | GPIO46（**同一支腳**，所以一直保持開啟）|
| ES8311 I2C | SDA=4、SCL=5（與鏡頭 SCCB 共用）|
| ES8311 I2S | MCLK=45、BCLK=39、WS=41、DOUT=42（喇叭）、DIN=40（麥克風）|
| OV2640 | XCLK=15、PCLK=13、VSYNC=6、HREF=7、D0~D7=11,9,8,10,12,18,17,16 |
| 底座 UART | TX=38、RX=48 |
| 按鍵 | GPIO0（BOOT）|

注意：螢幕 CS/DC 用到 GPIO43/44（UART0），所以 `Serial` 必須走 USB（`USB CDC On Boot: Enabled`）。

## 編譯與上傳

Arduino IDE 2.x + esp32 by Espressif **3.x**，函式庫：Adafruit GFX、Adafruit ST7735 and ST7789、ArduinoJson 7.x。

| 工具選單 | 設定 |
|---|---|
| 開發板 | ESP32S3 Dev Module |
| USB CDC On Boot | **Enabled** |
| USB Mode | Hardware CDC and JTAG |
| Flash Size | 16MB (128Mb) |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| PSRAM | **OPI PSRAM** |

命令列：

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,UploadSpeed=921600,CDCOnBoot=cdc,USBMode=hwcdc" --build-path build .
```

上傳（USB-JTAG 要用 watchdog-reset，hard-reset 會卡在下載模式）：

```bash
esptool --chip esp32s3 --port COM7 --after watchdog-reset write-flash 0x0 build/SparkBot_OpenAI.ino.merged.bin
```

## 設定 `config.h`

複製 `config.example.h` 成 `config.h`，填入 Wi-Fi（只支援 2.4GHz）與 OpenAI API Key。也可以不填：開機連不上時，機器人會開熱點 `SparkBot-Setup`（密碼 `sparkbot123`），用手機開 `http://192.168.4.1` 設定。

可調整：`MIC_PGA_GAIN` / `MIC_SOFT_GAIN`（錄音音量）、`VAD_*`（聲控靈敏度）、`TFT_ROTATION` / `TFT_INVERT`（螢幕方向、顏色）、`CAMERA_FLIP`（鏡頭上下顛倒）、`ENABLE_CAMERA`、`ENABLE_CHASSIS`、`HW_TEST_MODE`。

## 使用方式

| 操作 | 效果 |
|---|---|
| 直接說話 | 停頓一下就自動送出，小柯用繁體中文回答並唸出來 |
| 「你看到什麼」「這是什麼」「幫我看」 | 用頭頂鏡頭拍照，螢幕顯示照片，AI 看完再回答 |
| 「大聲一點」「小聲一點」 | 調整音量（會保存）|
| 「清除記憶」「重新開始」 | 忘掉先前的對話 |
| 天氣、新聞、股價、比賽結果… | 先上網搜尋（OpenAI web search）再用口語回答 |
| 「唱首歌」 | 從吳玉柱的 YouTube 創作歌單隨機選一首，顯示封面、歌名與 QR 碼 |
| 「更新歌單」 | 重新下載播放清單 |
| 「向前走」「後退」「左轉」「右轉」「停車」 | 接上履帶底座時移動（前後約 1.2 秒、轉向約 0.7 秒）|
| 「跳個舞」「燈光秀」「開燈」「關燈」「呼吸燈」「流水燈」 | 底座跳舞與燈光效果 |
| BOOT 鍵 | 短按：功能說明；按住：說話，放開送出；唱歌畫面時按：回到聊天 |
| 瀏覽器開螢幕左下角網址 / `http://sparkbot.local` | 修改 Wi-Fi、API Key、YouTube 歌單網址 |

底座指令只在短句（約 12 個字內）才會觸發，避免聊天內容誤觸。底座要刷官方 `c2_tracked_chassis` 韌體。

## 還原原廠小智韌體

原廠韌體完整備份在 `..\backup\sparkbot_original_16MB.bin`（2026-10-01 從 COM7 讀出）：

```bash
esptool --chip esp32s3 --port COM7 --after watchdog-reset write-flash 0 ../backup/sparkbot_original_16MB.bin
```

## 創意開發者

此系統是**吳玉柱先生**與 Claude AI 共同開發，有任何意見請聯絡 achir1015@gmail.com。
