# esp32s3-sparkbot-ai-chatbot
ESP32 AI桌上型機器人-SparkBot
<img width="1220" height="825" alt="image" src="https://github.com/user-attachments/assets/bf6ab6bc-ec0d-4f53-b88c-dc5fddd36221" />
<img width="897" height="661" alt="image" src="https://github.com/user-attachments/assets/19794953-543d-4a03-a10d-aec998eb4641" />

把 ESP-SparkBot 頭部原廠的「小智」韌體換成 **小柯 OpenAI 版**：直接說話 → OpenAI 語音轉文字 → GPT 回答 → OpenAI TTS 從喇叭講出來。1.54 吋螢幕顯示可愛表情、時鐘、農曆與對話文字；頭頂鏡頭可以拍照給 AI 看；接上履帶底座還能用語音控制移動與燈光。

---

## 一、系統安裝

### 1. 硬體

| 元件 | 說明 |
|---|---|
| 主控 | ESP-SparkBot 頭部：ESP32-S3-WROOM-1 N16R8（16MB Flash、8MB OPI PSRAM）|
| 螢幕 | 1.54" ST7789 240×240 |
| 音訊 | ES8311（喇叭 + 麥克風同一顆晶片）+ 功放 |
| 鏡頭 | OV2640 |
| 底座（選配）| 履帶底座，底部 4P 磁吸介面（5V / GPIO48 / GPIO38 / GND），需刷官方 `c2_tracked_chassis` 韌體 |

腳位已寫在 `config.example.h`，是板子固定的接線，不需要修改：

| 元件 | 腳位 |
|---|---|
| ST7789 | SCK=21、MOSI=47、CS=44、DC=43、無 RST |
| 背光 + 功放 | GPIO46（同一支腳）|
| ES8311 I2C | SDA=4、SCL=5（與鏡頭 SCCB 共用）|
| ES8311 I2S | MCLK=45、BCLK=39、WS=41、DOUT=42（喇叭）、DIN=40（麥克風）|
| OV2640 | XCLK=15、PCLK=13、VSYNC=6、HREF=7、D0~D7=11,9,8,10,12,18,17,16 |
| 底座 UART | TX=38、RX=48（115200）|
| 按鍵 | GPIO0（BOOT）|

### 2. 開發環境

- Arduino IDE 2.x
- 開發板：**esp32 by Espressif 3.x**（測試版本 3.3.12）
- 函式庫（程式庫管理員安裝）：**Adafruit GFX**、**Adafruit ST7735 and ST7789**、**ArduinoJson 7.x**

### 3. 設定 `config.h`

1. 把 `SparkBot_OpenAI/config.example.h` 複製成 `SparkBot_OpenAI/config.h`
2. 填入 Wi-Fi 名稱、密碼（**只支援 2.4GHz**）與 OpenAI API Key（https://platform.openai.com/api-keys）

也可以先不填：開機連不上 Wi-Fi 時，機器人會開熱點 `SparkBot-Setup`（密碼 `sparkbot123`），用手機連上後開 `http://192.168.4.1`，選擇 Wi-Fi 並輸入 API Key。

### 4. 編譯與上傳

用 Arduino IDE 開啟 `SparkBot_OpenAI/SparkBot_OpenAI.ino`，工具選單設定：

| 工具選單 | 設定 |
|---|---|
| 開發板 | ESP32S3 Dev Module |
| USB CDC On Boot | **Enabled** |
| USB Mode | Hardware CDC and JTAG |
| Flash Size | 16MB (128Mb) |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| PSRAM | **OPI PSRAM** |

用 USB-C 線接頭部，選擇對應的 COM 埠後上傳。首次編譯約需 10 分鐘。

命令列（arduino-cli + esptool）：

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,UploadSpeed=921600,CDCOnBoot=cdc,USBMode=hwcdc" --build-path build SparkBot_OpenAI
```

```bash
esptool --chip esp32s3 --port COM8 --after watchdog-reset write-flash 0x0 build/SparkBot_OpenAI.ino.merged.bin
```

### 5. 硬體測試（選用）

`config.h` 的 `HW_TEST_MODE` 改成 1 後上傳：開機會依序測試螢幕顏色、喇叭嗶聲、麥克風音量條、BOOT 鍵與拍照，確認硬體正常後再改回 0。

---

## 二、系統功能

| 功能 | 怎麼用 |
|---|---|
| 聲控聊天 | 直接說話，停頓一下就自動送出，小柯用台灣繁體中文回答並唸出來，不用按鍵 |
| 拍照看圖 | 說「拍照」「你看到什麼」「看得見我嗎」「這是什麼」「我手上拿什麼」，頭頂鏡頭拍照、螢幕顯示照片，AI 看完再回答 |
| 上網查詢 | 問天氣、新聞、股價、比賽結果等即時資訊，AI 會自己上網搜尋（OpenAI web search）再用口語摘要 |
| 履帶底座 | 「向前走」「後退」「左轉」「右轉」「停車」；「跳個舞」「燈光秀」「開燈」「關燈」「呼吸燈」「流水燈」「閃爍」|
| 唱歌 | 說「唱首歌」，從吳玉柱的 YouTube 創作歌單隨機選一首，顯示封面、歌名與 QR 碼，手機掃描就能播放；「更新歌單」重新下載 |
| 音量 | 「大聲一點」「小聲一點」，音量會保存 |
| 對話記憶 | 記得最近 10 則對話；「清除記憶」「重新開始」可清除 |
| 表情 | 依 AI 回答的心情顯示開心、愛心、驚訝、難過、生氣、眨眼、害羞；聆聽、思考、說話各有動畫；太久沒互動會想睡覺 |
| 時鐘與農曆 | 上方顯示日期、星期、農曆與時間，也可以問「今天農曆幾號」|
| 設定網頁 | 瀏覽器開螢幕左下角的網址或 `http://sparkbot.local`，可修改 Wi-Fi、API Key、YouTube 歌單網址 |
| BOOT 鍵 | 短按：功能說明（翻頁，最後一頁回到聊天）；按住：說話，放開送出（環境吵雜時用）；唱歌畫面時按：回到聊天 |

---

## 三、注意事項

1. **API Key 安全**：`config.h` 內含 Wi-Fi 密碼與 OpenAI API Key，已列在 `.gitignore`，**不要上傳或分享**。金鑰外流請立刻到 OpenAI 後台撤銷。
2. **使用費用**：語音辨識、對話、語音合成、看圖都會使用 OpenAI API 計費；上網搜尋另外計費。建議在 OpenAI 後台設定每月用量上限。
3. **Wi-Fi 只支援 2.4GHz**：手機熱點請切換成 2.4GHz 頻段。
4. **COM 埠會變動**：頭部用 ESP32-S3 內建 USB-JTAG，燒錄後埠號可能改變（例如 COM7 → COM8），上傳前請確認。上傳要用 `--after watchdog-reset`，否則可能停在下載模式。
5. **USB CDC On Boot 必須 Enabled**：螢幕用到 GPIO43/44（UART0），序列埠輸出必須走 USB。
6. **背光與功放共用 GPIO46**：程式讓它一直保持開啟，不支援調整背光亮度。
7. **鏡頭方向**：照片上下顛倒時，把 `config.h` 的 `CAMERA_FLIP` 改成 1（本機實測為 0）。鏡頭與音訊晶片共用 I2C，開機時會先設定 ES8311 再啟動鏡頭。
8. **聲控誤觸**：環境吵雜或常被電視、音樂誤觸時，調高 `VAD_MIN_RMS`；要喊很大聲才有反應時調低，或調整 `MIC_PGA_GAIN` / `MIC_SOFT_GAIN`。一路沒有停頓的聲音（音樂、電視）不會送出，並會自動提高門檻。
9. **履帶底座**：只接受短句指令（約 12 個字內），避免聊天內容誤觸；移動約 1.2 秒、轉向約 0.7 秒後自動停止。底座約 0.5 秒沒收到指令也會自動停車。請在桌面上使用時注意邊緣，避免摔落。
10. **還原原廠韌體**：更換前請先用 esptool 備份整顆 Flash（`read-flash 0 0x1000000`）。USB-JTAG 一次讀 16MB 可能中途斷線，可分段讀取，失敗的區段加 `--no-stub`。還原：

    ```bash
    esptool --chip esp32s3 --port COM8 --after watchdog-reset write-flash 0 sparkbot_original_16MB.bin
    ```

11. **歌曲版權**：歌單歌曲為吳玉柱詞曲創作，版權所有。

---

## 創意開發者

此系統是**吳玉柱先生**與 Claude AI 共同開發，有任何意見請聯絡 achir1015@gmail.com。
