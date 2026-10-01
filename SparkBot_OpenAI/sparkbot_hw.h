// ============================================================================
//  sparkbot_hw.h — ESP-SparkBot 頭部板載硬體
//    ES8311 一顆晶片同時負責喇叭（DAC）與麥克風（ADC），共用一組全雙工 I2S
//    錄音與播放都用 24kHz（OpenAI TTS pcm 固定 24kHz），錄音再降頻成 16kHz 上傳
// ============================================================================
#pragma once
#include <Wire.h>

#define ES8311_ADDR    0x18
#define I2S_RATE       24000

static bool i2cWrite(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
static int i2cRead(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(addr, (uint8_t)1) != 1) return -1;
  return Wire.read();
}
static bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// ---------------------------------------------------------------- ES8311 ----
// 從屬模式，MCLK = 256fs（24kHz → 6.144MHz），I2S 16-bit；DAC 播放 + ADC 類比麥克風
bool es8311Init(uint8_t pgaGain) {
  if (!i2cPresent(ES8311_ADDR)) { Serial.println("ES8311 位址 0x18 沒有回應"); return false; }
  uint8_t pga = 0x10 | (pgaGain > 10 ? 10 : pgaGain);   // bit4 = MIC1P/MIC1N 差動輸入
  const uint8_t seq[][2] = {
    {0x44, 0x08}, {0x44, 0x08},              // 加強 I2C 抗雜訊
    {0x00, 0x1F}, {0x00, 0x00},              // 重置
    {0x01, 0x30}, {0x02, 0x00}, {0x03, 0x10}, {0x16, 0x24}, {0x04, 0x10}, {0x05, 0x00},
    {0x0B, 0x00}, {0x0C, 0x00}, {0x10, 0x1F}, {0x11, 0x7F},
    {0x00, 0x80},                            // 從屬模式、開機
    {0x01, 0x3F},                            // 時脈取自 MCLK 腳，全部開啟
    {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xFF}, // BCLK / LRCK 分頻（從屬模式不使用）
    {0x09, 0x0C}, {0x0A, 0x0C},              // DAC / ADC 格式：I2S 16-bit
    {0x13, 0x10}, {0x1B, 0x0A}, {0x1C, 0x6A}, // ADC 高通濾波（去直流）
    {0x0D, 0x01}, {0x0E, 0x02}, {0x12, 0x00},
    {0x14, pga},                             // 麥克風類比增益
    {0x15, 0x40},
    {0x17, 0xBF},                            // ADC 音量 0dB（預設 0x00 = 幾乎靜音）
    {0x37, 0x08}, {0x45, 0x00},
    {0x32, 0xBF},                            // DAC 音量 0dB（實際音量用軟體調）
    {0x31, 0x00},                            // 取消靜音
  };
  bool ok = true;
  for (auto &r : seq) {
    bool w = false;
    for (int k = 0; k < 3 && !w; k++) { w = i2cWrite(ES8311_ADDR, r[0], r[1]); if (!w) delay(5); }
    if (!w) { Serial.printf("ES8311 寫入失敗 reg 0x%02X\n", r[0]); ok = false; }
    if (r[0] == 0x00 && r[1] == 0x1F) delay(20);
  }
  Serial.printf("ES8311 ID=%02X%02X\n", i2cRead(ES8311_ADDR, 0xFD), i2cRead(ES8311_ADDR, 0xFE));
  return ok;
}

// ---------------------------------------------------------- 履帶底座（UART）----
// 官方 c2_tracked_chassis 韌體的指令：
//   "x<轉向> y<前後>" 搖桿（-1.0~1.0，約 0.5 秒沒收到會自動停車）、"d1" 跳舞
//   燈光 "w2" 長亮、"w3" 閃爍、"w4" 慢呼吸、"w5" 快呼吸、"w6" 流水、"w7" 燈光秀、"w8" 關燈
HardwareSerial chassis(1);

void chassisInit() {
  chassis.begin(115200, SERIAL_8N1, SB_UART_RX, SB_UART_TX);
  chassis.print("w2");                      // 與原廠相同：開機燈光
}

void chassisSend(const char *cmd) {
  while (chassis.available()) chassis.read();   // 底座會把收到的指令回傳，直接丟掉
  chassis.print(cmd);
  Serial.printf("[底座] %s\n", cmd);
}

// 以搖桿方式移動一段時間後停止
void chassisMove(float x, float y, int ms) {
  char buf[32];
  snprintf(buf, sizeof(buf), "x%.1f y%.1f", x, y);
  uint32_t t0 = millis();
  while (millis() - t0 < (uint32_t)ms) {   // 持續送指令，避免底座逾時自動停車
    chassisSend(buf);
    delay(100);
  }
  chassisSend("x0.0 y0.0");
}
