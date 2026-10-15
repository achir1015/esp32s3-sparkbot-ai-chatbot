// ============================================================================
//  media.h — 雲端唱歌（MP3）、播 MV（.xkv）、攝影／錄影上傳 Google 雲端硬碟
//  由小柯 P4 版移植到 ESP32-S3：
//    - S3 只有 8MB PSRAM：MP3 邊下載邊播（不整首存下來）
//    - S3 沒有硬體 JPEG：MV 畫面用軟體解碼縮一半（640x352 → 320x176），取中間 240 寬
//    - 錄影直接用鏡頭輸出的 QVGA JPEG（不用重新編碼），聲音 8kHz μ-law，存成 AVI 上傳
//  本檔在 SparkBot_OpenAI.ino 中間被 #include，可以直接使用前面定義的全域變數與函式
// ============================================================================
#pragma once
#include "mp3dec.h"
#include "song_types.h"

// ---------------------------------------------------------------- 鏡頭 ----
// 鏡頭同時給拍照、攝影預覽、錄影使用，用 camLock 保護
SemaphoreHandle_t camLock = nullptr;

// 切換解析度（呼叫前要拿到 camLock）；換完丟掉兩張舊尺寸的畫面
static void camSetSize(framesize_t fs) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s || s->status.framesize == fs) return;
  s->set_framesize(s, fs);
  for (int i = 0; i < 2; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
  }
}

// 拍一張 JPEG，交給 fn 處理（fn 執行時畫面緩衝還有效）；回傳是否成功
template <typename F>
bool camCapture(framesize_t fs, F fn) {
  if (!cameraOK || !camLock) return false;
  xSemaphoreTake(camLock, portMAX_DELAY);
  camSetSize(fs);
  camera_fb_t *fb = esp_camera_fb_get();
  bool ok = fb && fb->format == PIXFORMAT_JPEG && fb->len > 0;
  if (ok) fn(fb->buf, fb->len);
  if (fb) esp_camera_fb_return(fb);
  xSemaphoreGive(camLock);
  return ok;
}

// ---------------------------------------------------- 照片上傳雲端硬碟 ----
static String urlEncode(const String &s) {
  String enc;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((uint8_t)c) || c == '_' || c == '.' || c == '-') enc += c;
    else { char h[4]; snprintf(h, sizeof(h), "%%%02X", (uint8_t)c); enc += h; }
  }
  return enc;
}

static String timeName(const char *prefix, const char *ext) {
  char buf[24];
  struct tm t;
  if (getLocalTime(&t, 10)) strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &t);
  else snprintf(buf, sizeof(buf), "%lu", (unsigned long)millis());
  return String(prefix) + buf + ext;
}

// Apps Script 存好檔後回 302；HTTPClient 自動轉址會用 POST 重送（Google 回 400），所以自己用 GET 讀結果
String gdriveResult(HTTPClient &http, int code, WiFiClientSecure &client) {
  if (code == 200) return http.getString();
  if (code != 302) return "";
  String loc = http.header("Location");
  http.end();
  if (loc.isEmpty()) return "";
  http.begin(client, loc);
  int c2 = http.GET();
  return c2 == 200 ? http.getString() : String("HTTP ") + c2;
}

bool gdriveReady() { return String(GDRIVE_SCRIPT_URL).indexOf("script.google.com") >= 0; }

struct PhotoUpload { uint8_t *jpg; size_t len; String name; };

void uploadTask(void *arg) {
  PhotoUpload *u = (PhotoUpload *)arg;
  size_t b64Len = 0;
  mbedtls_base64_encode(nullptr, 0, &b64Len, u->jpg, u->len);
  uint8_t *b64 = (uint8_t *)ps_malloc(b64Len + 1);
  if (b64 && mbedtls_base64_encode(b64, b64Len, &b64Len, u->jpg, u->len) == 0) {
    NetGuard ng;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(30000);
    http.begin(client, String(GDRIVE_SCRIPT_URL) + "?token=" GDRIVE_TOKEN "&name=" + urlEncode(u->name));
    http.addHeader("Content-Type", "text/plain");
    const char *keys[] = {"Location"};
    http.collectHeaders(keys, 1);
    int code = http.POST(b64, b64Len);
    String resp = gdriveResult(http, code, client);
    http.end();
    bool ok = resp.startsWith("ok");
    Serial.printf("照片上傳 %s → %d %s%s\n", u->name.c_str(), code, resp.substring(0, 40).c_str(), ok ? "（成功）" : "（失敗）");
  }
  free(b64);
  free(u->jpg);
  delete u;
  vTaskDelete(nullptr);
}

// 背景上傳一張照片（複製一份，不影響呼叫端）
void queuePhotoUpload(const uint8_t *jpg, size_t len) {
  if (!gdriveReady()) return;
  PhotoUpload *u = new PhotoUpload;
  u->jpg = (uint8_t *)ps_malloc(len);
  if (!u->jpg) { delete u; return; }
  memcpy(u->jpg, jpg, len);
  u->len = len;
  u->name = timeName("SparkBot_photo_", ".jpg");
  xTaskCreatePinnedToCore(uploadTask, "upload", 8192, u, 1, nullptr, 0);
}

// ------------------------------------------------- 下載環狀緩衝（PSRAM）----
// 網路 task 邊下載邊寫入；唱歌與 MV 從這裡讀
struct {
  uint8_t *buf = nullptr; size_t cap = 0;
  volatile size_t w = 0, r = 0;              // 累計寫入／讀出位元組
  volatile bool eof = true, stop = false, failed = false;
  volatile int total = -1;                   // 檔案大小（伺服器有給才知道）
  String id;
} dl;

void dlTask(void *) {
  NetGuard ng;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, "https://drive.usercontent.google.com/download?id=" + dl.id + "&export=download&confirm=t");
  int code = http.GET();
  if (code == 200) {
    dl.total = http.getSize();
    size_t got = 0;
    WiFiClient *st = http.getStreamPtr();
    uint32_t last = millis();
    while (!dl.stop && (dl.total < 0 || (int)got < dl.total) && millis() - last < 15000) {
      size_t space = dl.cap - (dl.w - dl.r);
      size_t a = st->available();
      if (!a || space < 4096) {
        if (!a && !http.connected()) break;
        delay(2);
        continue;
      }
      size_t wpos = dl.w % dl.cap;
      size_t n = min(min(a, space), dl.cap - wpos);
      n = st->readBytes(dl.buf + wpos, min(n, (size_t)16384));
      dl.w = dl.w + n;
      got += n;
      last = millis();
    }
  } else {
    Serial.printf("雲端下載失敗 %d\n", code);
    dl.failed = true;
  }
  http.end();
  dl.eof = true;
  vTaskDelete(nullptr);
}

bool dlStart(const String &id, size_t cap) {
  dl.buf = (uint8_t *)ps_malloc(cap);
  if (!dl.buf) return false;
  dl.cap = cap;
  dl.w = dl.r = 0; dl.total = -1;
  dl.eof = dl.stop = dl.failed = false;
  dl.id = id;
  xTaskCreatePinnedToCore(dlTask, "dl", 16384, nullptr, 3, nullptr, 0);
  return true;
}

void dlEnd() {
  dl.stop = true;
  while (!dl.eof) delay(10);
  free(dl.buf);
  dl.buf = nullptr;
}

bool dlDone() { return dl.eof && dl.w == dl.r; }

// 讀剛好 n bytes（等資料）；下載結束還不夠就回傳 false
bool dlRead(uint8_t *dst, size_t n) {
  size_t done = 0;
  while (done < n) {
    size_t avail = dl.w - dl.r;
    if (!avail) {
      if (dl.eof || dl.stop) return false;
      delay(2);
      continue;
    }
    size_t rpos = dl.r % dl.cap;
    size_t m = min(min(avail, n - done), dl.cap - rpos);
    memcpy(dst + done, dl.buf + rpos, m);
    dl.r = dl.r + m;
    done += m;
  }
  return true;
}

// 讀最多 n bytes（至少等到 1 byte 或下載結束）
size_t dlReadSome(uint8_t *dst, size_t n) {
  while (dl.w == dl.r && !dl.eof && !dl.stop) delay(2);
  size_t avail = dl.w - dl.r;
  size_t rpos = dl.r % dl.cap;
  size_t m = min(min(avail, n), dl.cap - rpos);
  memcpy(dst, dl.buf + rpos, m);
  dl.r = dl.r + m;
  return m;
}

// ------------------------------------------------------- 雲端資料夾清單 ----
std::vector<Mp3Song> mp3Songs, mvSongs;

// 讀公開雲端資料夾清單（embeddedfolderview），只留副檔名 ext 的檔案；回傳檔案數（失敗 -1）
int fetchDriveList(const char *folderUrl, const char *ext, std::vector<Mp3Song> &out) {
  String f = folderUrl;
  int k = f.indexOf("folders/");
  if (k >= 0) { f = f.substring(k + 8); int q = f.indexOf('?'); if (q >= 0) f = f.substring(0, q); }
  if (f.isEmpty()) return -1;
  NetGuard ng;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, "https://drive.google.com/embeddedfolderview?id=" + f);
  int code = http.GET();
  String html = code == 200 ? http.getString() : String("");
  http.end();
  if (code != 200) { Serial.printf("雲端清單讀取失敗 %d\n", code); return -1; }
  std::vector<Mp3Song> v;
  int pos = 0, el = strlen(ext);
  for (;;) {
    int a = html.indexOf("id=\"entry-", pos);
    if (a < 0) break;
    a += 10;
    int b = html.indexOf('"', a), t = html.indexOf("flip-entry-title\">", b);
    if (b < 0 || t < 0) break;
    t += 18;
    int e = html.indexOf('<', t);
    String id = html.substring(a, b), title = html.substring(t, e);
    title.replace("&amp;", "&"); title.replace("&#39;", "'"); title.replace("&quot;", "\"");
    String low = title; low.toLowerCase();
    if (low.endsWith(ext)) v.push_back({id, title.substring(0, title.length() - el)});
    pos = e;
  }
  xSemaphoreTake(songLock, portMAX_DELAY);
  out = v;
  xSemaphoreGive(songLock);
  Serial.printf("雲端清單 %s：%d 個\n", ext, (int)v.size());
  return v.size();
}

String normTitle(String s) {
  static const char *DROP[] = {" ", "　", "，", "。", "、", "：", "！", "？", "(", ")", "（", "）", "《", "》", "「", "」", "_", "-", ".", ",", ":", "!", "?"};
  for (auto d : DROP) s.replace(d, "");
  while (s.length() && isdigit((uint8_t)s[s.length() - 1])) s.remove(s.length() - 1);
  return s;
}

// 找使用者點名的歌：「唱千江有水千江月」→ 索引，找不到 -1（取最長的符合歌名）
int findRequested(const std::vector<Mp3Song> &list, const String &userText) {
  String u = normTitle(userText);
  int best = -1, bestLen = 0;
  xSemaphoreTake(songLock, portMAX_DELAY);
  for (size_t i = 0; i < list.size(); i++) {
    String t = normTitle(list[i].title);
    if (t.length() >= 6 && u.indexOf(t) >= 0 && (int)t.length() > bestLen) { best = i; bestLen = t.length(); }
  }
  xSemaphoreGive(songLock);
  return best;
}

// 用歌名找 YouTube 播放清單裡同一首（封面與 QR 碼用）
bool findYoutube(const String &title, String &ytId) {
  String t = normTitle(title);
  bool ok = false;
  xSemaphoreTake(songLock, portMAX_DELAY);
  for (auto &s : songs) {
    String y = normTitle(s.title);
    if (y.length() && (y.indexOf(t) >= 0 || t.indexOf(y) >= 0)) { ytId = s.id; ok = true; break; }
  }
  xSemaphoreGive(songLock);
  return ok;
}

// 隨機選一首：硬體亂數，最近播過的（記在 NVS，重開機也記得）不會馬上重複
int pickRandom(int n, const char *key) {
  if (n <= 0) return -1;
  prefs.begin("robot", false);
  String recent = prefs.getString(key, "");             // 例如 ",3,17,5,"
  int avoid = min(n / 2, 5);                               // 最近 5 首（歌少時取一半）不重複
  int idx = esp_random() % n;
  for (int k = 0; k < 50 && recent.indexOf("," + String(idx) + ",") >= 0; k++) idx = esp_random() % n;
  recent = "," + String(idx) + recent;
  for (int c = 0, i = 0; i < (int)recent.length(); i++)    // 只保留最近 avoid 筆
    if (recent[i] == ',' && ++c > avoid) { recent = recent.substring(0, i + 1); break; }
  prefs.putString(key, recent);
  prefs.end();
  return idx;
}

// ------------------------------------------------------------ 唱歌（MP3）----

// 邊下載邊解碼播放（阻塞到唱完或按鍵）；回傳 true = 按鍵停止
bool playMp3Stream() {
  HMP3Decoder h = MP3InitDecoder();
  if (!h) return false;
  static int16_t pcm[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];
  static int16_t out[MAX_NGRAN * MAX_NSAMP * 2 * 2];
  static uint8_t in[8192];
  uint8_t *p = in;
  int inLen = 0;
  uint32_t ramp = 0, samples = 0;
  bool stopped = false, wasPressed = buttonPressed();
  songPosMs = 0;
  songPlaying = true;
  for (;;) {
    bool b = buttonPressed();
    if (b && !wasPressed) { stopped = true; break; }               // 按鍵：停止
    wasPressed = b;
    if (inLen < 4096 && !dlDone()) {                               // 補資料
      memmove(in, p, inLen);
      p = in;
      inLen += dlReadSome(in + inLen, sizeof(in) - inLen);
    }
    if (inLen <= 0) break;
    int off = MP3FindSyncWord(p, inLen);
    if (off < 0) { inLen = 0; continue; }
    p += off; inLen -= off;
    int left = inLen;
    int err = MP3Decode(h, &p, &left, pcm, 0);
    inLen = left;
    if (err == ERR_MP3_INDATA_UNDERFLOW) { if (dlDone()) break; continue; }
    if (err) { if (err != ERR_MP3_MAINDATA_UNDERFLOW && inLen > 0) { p++; inLen--; } continue; }   // 壞影格：跳過
    MP3FrameInfo fi;
    MP3GetLastFrameInfo(h, &fi);
    int n = fi.outputSamps / max(1, fi.nChans);
    if (songDurMs == 0 && fi.bitrate > 0 && dl.total > 0) songDurMs = (uint32_t)((uint64_t)dl.total * 8000 / fi.bitrate);
    // 轉成 24kHz 立體聲（其他取樣率用最近鄰換算）
    int outN = fi.samprate == I2S_RATE ? n : (int)((int64_t)n * I2S_RATE / max(1, fi.samprate));
    outN = min(outN, (int)(sizeof(out) / 4));
    int vol = volumePct;
    int64_t acc = 0;
    for (int i = 0; i < outN; i++) {
      int si = fi.samprate == I2S_RATE ? i : (int)((int64_t)i * fi.samprate / I2S_RATE);
      int32_t s = fi.nChans == 2 ? ((int32_t)pcm[si * 2] + pcm[si * 2 + 1]) / 2 : pcm[si];
      s = s * vol / 100;
      const uint32_t RAMP = I2S_RATE / 5;                          // 開頭 0.2 秒淡入
      if (ramp < RAMP) s = s * (int32_t)ramp++ / (int32_t)RAMP;
      s = constrain(s, -32767, 32767);
      out[i * 2] = out[i * 2 + 1] = s;
      acc += abs(s);
    }
    speakLevel = (int)(acc / max(1, outN)) * 2;                    // 嘴巴跟著節奏開合
    sbI2S.write((uint8_t *)out, outN * 4);
    samples += outN;
    songPosMs = (uint32_t)((uint64_t)samples * 1000 / I2S_RATE);
  }
  memset(out, 0, sizeof(out));
  for (int i = 0; i < 4; i++) sbI2S.write((uint8_t *)out, sizeof(out));
  speakLevel = 0;
  songPlaying = false;
  MP3FreeDecoder(h);
  while (buttonPressed()) delay(10);                               // 等放開按鍵，避免接著打開說明
  return stopped;
}

void singYoutubeOnly();
void exitSong();

// 唱歌：有雲端 MP3 就直接唱出來；userText 有點歌名就唱那首；沒有 MP3 就退回 YouTube QR 碼
void singSong(const String &userText) {
  setUi(UI_THINK, "選歌中…", "");
  if (fetchDriveList(SONGS_MP3_FOLDER, ".mp3", mp3Songs) <= 0) { singYoutubeOnly(); return; }   // 每次重讀，雲端新增的歌馬上能唱
  int n = mp3Songs.size();
  int idx = findRequested(mp3Songs, userText);
  if (idx < 0) {
    idx = pickRandom(n, "recentMp3");
  }
  lastSong = idx;
  Mp3Song m = mp3Songs[idx];
  Serial.println("唱歌（MP3）：" + m.title);

  String ytId;
  bool hasYt = findYoutube(m.title, ytId);
  thumbOk = hasYt && fetchThumb(ytId);
  qrSize = 0;
  if (hasYt) {
    esp_qrcode_config_t qc = {};
    qc.display_func = qrCapture;
    qc.max_qrcode_version = 7;
    qc.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
    esp_qrcode_generate(&qc, ("https://youtu.be/" + ytId).c_str());
  }
  songDurMs = 0; songPosMs = 0;
  songLoading = true;
  setUi(UI_SONG, m.title, hasYt ? ytId : String(""));
  songSince = millis();
  if (speak("我來唱《" + m.title + "》給你聽！這是" SONG_COMPOSER "創作的歌。")) waitPlaybackDone();

  if (!dlStart(m.id, 1024 * 1024)) { songLoading = false; showError("記憶體不足，無法唱歌"); exitSong(); return; }
  uint32_t t0 = millis();
  while (dl.w < 48 * 1024 && !dl.eof && millis() - t0 < 10000) delay(20);   // 先緩衝一點
  songLoading = false;
  bool stopped = false;
  if (dl.failed || dl.w == 0) {
    dlEnd();
    speakExpr = EX_SAD;
    speakAndShow("這首歌下載失敗，請確認雲端資料夾已經設成知道連結的人都能檢視。");
  } else {
    stopped = playMp3Stream();
    dlEnd();
  }
  Serial.println(stopped ? "（唱歌：按鍵停止）" : "（唱歌：唱完了）");
  exitSong();
}

// 拍照：顯示在螢幕上、背景上傳雲端硬碟，並回傳給 GPT 用的 data URI（VGA 640x480）
String captureImageDataUri() {
  String uri;
  camCapture(FRAMESIZE_VGA, [&](const uint8_t *jpg, size_t len) {
    // 640x480 JPEG 縮一半解碼成 320x240，螢幕顯示中間 240 寬
    if (photoPix && jpg2rgb565(jpg, len, (uint8_t *)photoPix, JPG_SCALE_2X)) photoShow = true;
    queuePhotoUpload(jpg, len);
    size_t b64Len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64Len, jpg, len);
    uint8_t *b64 = (uint8_t *)ps_malloc(b64Len + 1);
    if (b64 && mbedtls_base64_encode(b64, b64Len, &b64Len, jpg, len) == 0) {
      b64[b64Len] = 0;
      uri.reserve(b64Len + 32);
      uri = "data:image/jpeg;base64,";
      uri += (const char *)b64;
    }
    free(b64);
    Serial.printf("拍照 JPEG %u bytes\n", (unsigned)len);
  });
  if (cameraActive) {                                  // 攝影模式中：預覽改回 QVGA
    xSemaphoreTake(camLock, portMAX_DELAY);
    camSetSize(FRAMESIZE_QVGA);
    xSemaphoreGive(camLock);
  }
  return uri;
}

// ------------------------------------------------------------- 播 MV ----
// .xkv（tools/make_xkv.py 由 MP4 轉成：MJPEG + MP3 交錯）
#define MV_PCM_CAP (I2S_RATE * 3)                 // 3 秒聲音緩衝
struct {
  int16_t *buf = nullptr;
  volatile size_t w = 0, r = 0;
  volatile uint32_t played = 0;                    // 已播出的樣本數 = 影音同步的時鐘
  volatile bool run = false, done = true;
} mvPcm;

void mvAudioTask(void *) {
  static int16_t out[480 * 2];
  uint32_t ramp = 0;
  while (mvPcm.run) {
    size_t avail = mvPcm.w - mvPcm.r;
    if (avail < 480) {                             // 網路跟不上：送靜音，時鐘不前進（畫面會等）
      memset(out, 0, sizeof(out));
      sbI2S.write((uint8_t *)out, sizeof(out));
      continue;
    }
    int vol = volumePct;
    int64_t acc = 0;
    for (int i = 0; i < 480; i++) {
      int32_t s = (int32_t)mvPcm.buf[(mvPcm.r + i) % MV_PCM_CAP] * vol / 100;
      const uint32_t RAMP = I2S_RATE / 5;
      if (ramp < RAMP) s = s * (int32_t)ramp++ / (int32_t)RAMP;
      s = constrain(s, -32767, 32767);
      out[i * 2] = out[i * 2 + 1] = s;
      acc += abs(s);
    }
    mvPcm.r = mvPcm.r + 480;
    speakLevel = (int)(acc / 480) * 2;
    sbI2S.write((uint8_t *)out, sizeof(out));
    mvPcm.played = mvPcm.played + 480;
  }
  memset(out, 0, sizeof(out));
  for (int i = 0; i < 4; i++) sbI2S.write((uint8_t *)out, sizeof(out));
  speakLevel = 0;
  mvPcm.done = true;
  vTaskDelete(nullptr);
}

// 顯示一張 MV 畫面：軟體解碼縮小（MV_FAST = 1 時縮到寬 160，解碼快約 3 倍）→ 放大／裁切成寬 240，垂直置中
static uint16_t *mvDec = nullptr, *mvLine = nullptr;
static int mvScale = 1, mvOW = 0, mvOH = 0, mvDW = 0, mvDH = 0;
static uint16_t mvXmap[SCREEN_W], mvYmap[SCREEN_H];
static uint32_t mvDecMs = 0, mvLcdMs = 0, mvCnt = 0;
bool mvShowFrame(const uint8_t *jpg, size_t len) {
  uint32_t t0 = millis();
  esp_jpeg_image_scale_t sc = mvScale == 8 ? JPG_SCALE_8X : mvScale == 4 ? JPG_SCALE_4X : mvScale == 2 ? JPG_SCALE_2X : JPG_SCALE_NONE;
  if (!jpg2rgb565(jpg, len, (uint8_t *)mvDec, sc)) return false;
  uint32_t t1 = millis();
  for (int y = 0; y < mvDH; y++) {
    const uint16_t *src = mvDec + mvYmap[y] * mvOW;
    uint16_t *dst = mvLine + y * SCREEN_W;
    for (int x = 0; x < SCREEN_W; x++) dst[x] = src[mvXmap[x]];
  }
  tft.drawRGBBitmap(0, (SCREEN_H - mvDH) / 2, mvLine, SCREEN_W, mvDH);
  mvDecMs += t1 - t0; mvLcdMs += millis() - t1;
  if (++mvCnt == 30) {
    Serial.printf("[MV] 解碼 %ums、顯示 %ums／張\n", (unsigned)(mvDecMs / 30), (unsigned)(mvLcdMs / 30));
    mvDecMs = mvLcdMs = mvCnt = 0;
  }
  return true;
}

int lastMv = -1;

void playMv(const String &userText) {
  setUi(UI_THINK, "找 MV 中…", "");
  if (fetchDriveList(SONGS_MV_FOLDER, ".xkv", mvSongs) <= 0) {
    speakExpr = EX_SAD;
    speakAndShow("我還沒有 MV 可以播，請把轉好的 MV 檔放到雲端的 MV 資料夾喔。");
    return;
  }
  int n = mvSongs.size();
  int idx = findRequested(mvSongs, userText);
  if (idx < 0) idx = pickRandom(n, "recentMv");
  lastMv = idx;
  Mp3Song m = mvSongs[idx];
  Serial.println("播 MV：" + m.title);
  if (speak("播放《" + m.title + "》的 MV！按一下按鍵就可以停止。")) waitPlaybackDone();

  uint8_t *chunk = (uint8_t *)ps_malloc(256 * 1024);
  mvPcm.buf = (int16_t *)ps_malloc(MV_PCM_CAP * 2);
  mvLine = (uint16_t *)ps_malloc(SCREEN_W * SCREEN_H * 2);
  if (!chunk || !mvPcm.buf || !mvLine || !dlStart(m.id, 1536 * 1024)) {
    free(chunk); free(mvPcm.buf); free(mvLine); mvPcm.buf = nullptr; mvLine = nullptr;
    showError("記憶體不足，無法播放 MV");
    return;
  }
  memset(mvLine, 0, SCREEN_W * SCREEN_H * 2);

  setUi(UI_VIDEO, m.title, "");                    // UI task 停止畫圖，畫面由這裡直接畫
  delay(60);
  cv->fillScreen(C_BG);
  u8f.setForegroundColor(C_GLOW);
  std::vector<String> tl = u8f.wrap("《" + m.title + "》", SCREEN_W - 8);
  if (tl.size()) u8f.drawUTF8((SCREEN_W - u8f.getUTF8Width(tl[0].c_str())) / 2, 112, tl[0].c_str());
  u8f.setForegroundColor(C_GRAY);
  u8f.drawUTF8((SCREEN_W - u8f.getUTF8Width("MV 載入中…")) / 2, 136, "MV 載入中…");
  tft.drawRGBBitmap(0, 0, cv->getBuffer(), SCREEN_W, SCREEN_H);

  uint8_t hdr[32];
  bool ok = dlRead(hdr, 32) && !memcmp(hdr, "XKV1", 4);
  int w = hdr[4] | hdr[5] << 8, h = hdr[6] | hdr[7] << 8, fps = hdr[8] | hdr[9] << 8;
  if (ok) {
    const int minW = MV_FAST ? 160 : SCREEN_W;                         // 解碼後的最小寬度
    mvScale = 1;
    while (mvScale < 8 && w / (mvScale * 2) >= minW) mvScale *= 2;
    mvOW = w / mvScale; mvOH = h / mvScale;
    // 顯示：寬度剛好 240（比 240 小就放大、比 240 大就取中間），高度等比例
    int srcW = min(mvOW, SCREEN_W);                                    // 比 240 窄：整張放大；比 240 寬：1:1 取中間
    int sx0 = (mvOW - srcW) / 2;
    mvDW = SCREEN_W;
    mvDH = min(SCREEN_H, mvOH * SCREEN_W / srcW);
    for (int x = 0; x < SCREEN_W; x++) mvXmap[x] = sx0 + x * srcW / SCREEN_W;
    for (int y = 0; y < mvDH; y++) mvYmap[y] = y * srcW / SCREEN_W;
    mvDecMs = mvLcdMs = mvCnt = 0;
    mvDec = (uint16_t *)ps_malloc(mvOW * mvOH * 2);
  }
  if (!ok || w <= 0 || w > 1920 || h <= 0 || h > 1080 || fps <= 0 || !mvDec) {
    dlEnd();
    free(chunk); free(mvPcm.buf); free(mvLine); free(mvDec);
    mvPcm.buf = nullptr; mvLine = nullptr; mvDec = nullptr;
    setUi(UI_IDLE);
    speakExpr = EX_SAD;
    speakAndShow(dl.failed ? "MV 下載失敗，請確認雲端資料夾已經設成知道連結的人都能檢視。" : "這個 MV 檔案格式不對，請用轉檔工具重新轉一次。");
    return;
  }
  uint32_t t0 = millis();
  while (dl.w < 300 * 1024 && !dl.eof && millis() - t0 < 8000) delay(20);   // 先緩衝一點再開播

  // 畫面上下黑邊 + 歌名、提示
  cv->fillScreen(C_BG);
  u8f.setForegroundColor(C_ORANGE);
  if (tl.size()) u8f.drawUTF8((SCREEN_W - u8f.getUTF8Width(tl[0].c_str())) / 2, 22, tl[0].c_str());
  u8f.setForegroundColor(C_GREEN);
  u8f.drawUTF8((SCREEN_W - u8f.getUTF8Width("按鍵：停止播放")) / 2, 234, "按鍵：停止播放");
  tft.drawRGBBitmap(0, 0, cv->getBuffer(), SCREEN_W, SCREEN_H);

  HMP3Decoder dec = MP3InitDecoder();
  static int16_t pcm[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];
  mvPcm.w = mvPcm.r = mvPcm.played = 0;
  mvPcm.run = true; mvPcm.done = false;
  xTaskCreatePinnedToCore(mvAudioTask, "mvAud", 4096, nullptr, 5, nullptr, 1);

  uint32_t frame = 0, shown = 0, dropped = 0;
  bool stopped = false, wasPressed = buttonPressed();
  auto pressed = [&]() { bool b = buttonPressed(); bool hit = b && !wasPressed; wasPressed = b; return hit; };
  for (;;) {
    if (pressed()) { stopped = true; break; }
    uint8_t ch[4];
    if (!dlRead(ch, 4)) break;
    size_t len = ch[1] | ch[2] << 8 | ch[3] << 16;
    if (len > 256 * 1024 || !dlRead(chunk, len)) break;
    if (ch[0] == 'A') {
      uint8_t *p = chunk;
      int left = len;
      if (MP3Decode(dec, &p, &left, pcm, 0) == ERR_MP3_NONE) {
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(dec, &fi);
        int ns = fi.outputSamps / max(1, fi.nChans);
        while (MV_PCM_CAP - (mvPcm.w - mvPcm.r) < (size_t)ns) {   // 聲音緩衝滿：等它播
          if (pressed()) { stopped = true; break; }
          delay(2);
        }
        if (stopped) break;
        for (int i = 0; i < ns; i++) mvPcm.buf[(mvPcm.w + i) % MV_PCM_CAP] = fi.nChans == 2 ? (pcm[i * 2] + pcm[i * 2 + 1]) / 2 : pcm[i];
        mvPcm.w = mvPcm.w + ns;
      }
    } else if (ch[0] == 'V') {
      uint32_t due = frame * 1000 / fps;            // 這張畫面該出現的時間（ms）
      frame++;
      uint32_t now = (uint32_t)((uint64_t)mvPcm.played * 1000 / I2S_RATE);
      if (now > due + 150) { dropped++; continue; } // 畫面落後太多：略過（S3 解碼較慢，會自動跳格）
      while (now + 5 < due) {                       // 太早：等聲音追上
        if (pressed()) { stopped = true; break; }
        delay(3);
        now = (uint32_t)((uint64_t)mvPcm.played * 1000 / I2S_RATE);
        if (mvPcm.w == mvPcm.r && dl.eof) break;
      }
      if (stopped) break;
      if (mvShowFrame(chunk, len)) shown++;
    }
  }
  if (!stopped) while (mvPcm.w - mvPcm.r >= 480 && !buttonPressed()) delay(20);   // 把剩下的聲音播完
  dlEnd();
  mvPcm.run = false;
  while (!mvPcm.done) delay(10);
  MP3FreeDecoder(dec);
  free(chunk); free(mvPcm.buf); free(mvLine); free(mvDec);
  mvPcm.buf = nullptr; mvLine = nullptr; mvDec = nullptr;
  Serial.printf("MV 結束：顯示 %u 張、略過 %u 張%s\n", (unsigned)shown, (unsigned)dropped, stopped ? "（按鍵停止）" : "");
  while (buttonPressed()) delay(10);
  setUi(UI_IDLE);
  flushMic(200);
  lastInteraction = millis();
}

// ------------------------------------------------------------ 攝影模式 ----
uint16_t *previewPix = nullptr;         // 預覽解碼緩衝 320x240

// UI task 呼叫：畫一張鏡頭預覽到畫布（320x240 取中間 240 寬）
bool drawCameraPreview() {
  if (!previewPix) previewPix = (uint16_t *)ps_malloc(320 * 240 * 2);
  if (!previewPix) return false;
  bool ok = false;
  if (recActive) {
    const uint8_t *j = recLastFrame;
    size_t n = recLastLen;
    ok = j && n && jpg2rgb565(j, n, (uint8_t *)previewPix, JPG_SCALE_NONE);
  } else {
    ok = camCapture(FRAMESIZE_QVGA, [&](const uint8_t *j, size_t n) {
      ok = jpg2rgb565(j, n, (uint8_t *)previewPix, JPG_SCALE_NONE);
    }) && ok;
  }
  if (!ok) return false;
  uint16_t *dst = cv->getBuffer();
  for (int y = 0; y < SCREEN_H; y++) memcpy(dst + y * SCREEN_W, previewPix + y * 320 + 40, SCREEN_W * 2);
  return true;
}

void startCamera(bool announce) {
  if (!cameraOK) {
    speakExpr = EX_SAD;
    speakAndShow("我的鏡頭沒有偵測到，沒辦法攝影喔。");
    return;
  }
  if (cameraActive && announce) {
    speakExpr = EX_HAPPY;
    speakAndShow("已經在攝影囉！要錄影就說「錄製」。");
    return;
  }
  if (announce) {
    speakExpr = EX_HAPPY;
    speakAndShow("好的，開啟鏡頭！要錄影就說「錄製」，按一下按鍵回到原畫面。");
  }
  cameraSince = millis();
  cameraActive = true;
  setUi(UI_IDLE);
  Serial.println("（攝影模式）");
}

void exitCamera() {
  while (buttonPressed()) delay(10);
  cameraActive = false;
  if (camLock) {                                       // 拍照給 AI 用 VGA
    xSemaphoreTake(camLock, portMAX_DELAY);
    camSetSize(FRAMESIZE_VGA);
    xSemaphoreGive(camLock);
  }
  setUi(UI_IDLE);
  flushMic(200);
  lastInteraction = millis();
  Serial.println("（離開攝影模式）");
}

// ---------------------------------------------------------------- 錄影 ----
struct {
  uint8_t *buf; size_t cap; volatile size_t len; size_t maxFrame;
  std::vector<uint32_t> off, sz;
  volatile bool run, done, full;
} recVid;

void recVideoTask(void *) {
  uint32_t lastShot = 0;
  while (recVid.run) {
    if (millis() - lastShot < 1000UL / REC_FPS) { delay(2); continue; }
    lastShot = millis();
    bool full = false;
    camCapture(FRAMESIZE_QVGA, [&](const uint8_t *jpg, size_t len) {
      if (recVid.len + len + 2 > recVid.cap) { full = true; return; }
      memcpy(recVid.buf + recVid.len, jpg, len);
      recVid.off.push_back(recVid.len);
      recVid.sz.push_back(len);
      recLastFrame = recVid.buf + recVid.len;
      recLastLen = len;
      recVid.len += (len + 1) & ~1u;
      recVid.maxFrame = max(recVid.maxFrame, len);
    });
    if (full) { recVid.full = true; break; }
  }
  recVid.done = true;
  vTaskDelete(nullptr);
}

bool isStopWord(const String &t) {
  if (containsAny(t, {"停止", "停下", "結束錄", "上傳", "不要錄了", "錄好了"})) return true;
  return t.length() <= 9 && t.indexOf("停") >= 0;        // 只說「停」「停！」
}

// 錄影時的聲音：獨立 task 持續讀麥克風，避免漏音
struct { int16_t *buf; size_t cap; volatile size_t n; volatile bool run; volatile bool done; } recAud;

void recAudioTask(void *) {
  while (recAud.run && recAud.n + FRAME <= recAud.cap) {
    readFrame(recAud.buf + recAud.n);
    recAud.n += FRAME;
  }
  recAud.done = true;
  vTaskDelete(nullptr);
}

// AVI（MJPEG 影像 + μ-law 聲音）邊產生邊 base64，直接當 HTTP 上傳內容（不用第二份記憶體）
struct Seg { const uint8_t *p; uint32_t n; };

class AviB64Stream : public Stream {
 public:
  std::vector<Seg> segs;
  size_t rawTotal = 0, b64Total = 0, b64Sent = 0;
  int lastPct = -1;
  size_t write(uint8_t) override { return 0; }
  int available() override { return b64Sent < b64Total ? (int)min((size_t)0x7FFFFFFF, b64Total - b64Sent) : 0; }
  int read() override { char c; return readBytes(&c, 1) == 1 ? (uint8_t)c : -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t readBytes(char *buf, size_t n) override {
    size_t k = 0;
    while (k < n) {
      if (outPos >= outLen && !refill()) break;
      size_t m = min(n - k, outLen - outPos);
      memcpy(buf + k, out + outPos, m);
      outPos += m; k += m;
    }
    b64Sent += k;
    int pct = b64Total ? (int)(b64Sent * 100 / b64Total) : 0;
    if (pct / 5 != lastPct / 5) {
      lastPct = pct;
      setUi(UI_THINK, "影片上傳中…", String(pct) + "%");
    }
    return k;
  }
 private:
  uint8_t out[1032];
  size_t outLen = 0, outPos = 0, seg = 0, segPos = 0;
  size_t rawRead(uint8_t *dst, size_t max) {
    size_t k = 0;
    while (k < max && seg < segs.size()) {
      size_t m = min(max - k, (size_t)segs[seg].n - segPos);
      if (segs[seg].p) memcpy(dst + k, segs[seg].p + segPos, m);
      else memset(dst + k, 0, m);
      k += m; segPos += m;
      if (segPos >= segs[seg].n) { seg++; segPos = 0; }
    }
    return k;
  }
  bool refill() {
    uint8_t raw[768];                                   // 3 的倍數：中間不會出現 '=' 補位
    size_t r = rawRead(raw, sizeof(raw));
    if (!r) return false;
    size_t o = 0;
    mbedtls_base64_encode(out, sizeof(out), &o, raw, r);
    outLen = o; outPos = 0;
    return true;
  }
};

// 16bit PCM → 8bit μ-law（G.711，電話音質，檔案只有 1/4）
static uint8_t linear2ulaw(int pcm) {
  const int BIAS = 0x84, CLIP = 32635;
  int sign = 0;
  if (pcm < 0) { sign = 0x80; pcm = -pcm; }
  if (pcm > CLIP) pcm = CLIP;
  pcm += BIAS;
  int exponent = 7;
  for (int mask = 0x4000; (pcm & mask) == 0 && exponent > 0; exponent--, mask >>= 1) {}
  int mantissa = (pcm >> (exponent + 3)) & 0x0F;
  return ~(sign | (exponent << 4) | mantissa);
}

static void put32(uint8_t *&p, uint32_t v) { memcpy(p, &v, 4); p += 4; }
static void put16(uint8_t *&p, uint16_t v) { memcpy(p, &v, 2); p += 2; }
static void putCC(uint8_t *&p, const char *cc) { memcpy(p, cc, 4); p += 4; }

#define REC_W 320
#define REC_H 240

// 錄影（說「停止」或按鍵結束）並上傳到 Google 雲端硬碟；回傳給使用者聽的結果
String recordVideo() {
  if (!cameraOK) return "我的鏡頭沒有偵測到，沒辦法錄影喔。";
  if (!gdriveReady()) return "還沒設定雲端硬碟上傳網址，請在 config.h 填好 Apps Script 網址，再叫我錄製喔。";
  if (!cameraActive) startCamera(false);

  // 借用語音播放緩衝區（錄影時不會說話），錄完再還回去
  free(tts.buf);
  tts.buf = nullptr;
  ttsCap = 0;
  // 可用記憶體依比例分給聲音（32KB/秒）與影像（約 100KB/秒）
  size_t freeBlk = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  size_t budget = freeBlk > 1536 * 1024 ? freeBlk - 768 * 1024 : 0;
  recAud.cap = min((size_t)MIC_SAMPLE_RATE * REC_SECONDS, budget / 4 / 2);
  recAud.buf = recAud.cap ? (int16_t *)ps_malloc(recAud.cap * 2) : nullptr;
  size_t vcap = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  vcap = vcap > 1024 * 1024 ? min(vcap - 512 * 1024, (size_t)6 * 1024 * 1024) : 0;
  uint8_t *vbuf = vcap ? (uint8_t *)ps_malloc(vcap) : nullptr;
  if (!recAud.buf || !vbuf) {
    free(recAud.buf); free(vbuf);
    recAud.buf = nullptr;
    tts.buf = (uint8_t *)ps_malloc(TTS_BUF_BYTES);
    ttsCap = tts.buf ? TTS_BUF_BYTES : 0;
    return "記憶體不夠，沒辦法錄影。";
  }
  Serial.printf("錄影：影像緩衝 %u KB，聲音 %u KB\n", (unsigned)(vcap / 1024), (unsigned)(recAud.cap * 2 / 1024));

  recVid.buf = vbuf; recVid.cap = vcap; recVid.len = 0; recVid.maxFrame = 0;
  recVid.off.clear(); recVid.sz.clear();
  recVid.off.reserve(REC_SECONDS * REC_FPS + 8);
  recVid.sz.reserve(REC_SECONDS * REC_FPS + 8);
  recVid.run = true; recVid.done = false; recVid.full = false;
  bool stopped = false, heardStop = false;

  flushMic(60);
  recAud.n = 0; recAud.run = true; recAud.done = false;
  recLastFrame = nullptr; recLastLen = 0;
  recStart = millis();
  recActive = true;
  setUi(UI_IDLE);
  xTaskCreatePinnedToCore(recAudioTask, "recAud", 4096, nullptr, 4, nullptr, 1);
  xTaskCreatePinnedToCore(recVideoTask, "recVid", 4096, nullptr, 3, nullptr, 1);
  Serial.println("（開始錄影，說「停止」結束）");

  // 一邊錄一邊聽：對錄到的聲音做語音偵測，一句話結束就送辨識，聽到「停止」就結束
  int thr = vadThreshold();
  size_t pos = 0, segStart = 0;
  bool inSpeech = false;
  int loud = 0, silentMs = 0, voicedMs = 0;
  bool btnPrev = buttonPressed();
  while (!heardStop && !stopped) {
    bool b = buttonPressed();
    if (b && !btnPrev) { stopped = true; break; }        // 按鍵：停止
    btnPrev = b;
    if (recVid.full || recAud.done || millis() - recStart > REC_SECONDS * 1000UL) break;   // 記憶體滿或到上限
    size_t avail = recAud.n;
    bool checkSeg = false;
    size_t segEnd = 0;
    while (pos + FRAME <= avail && !checkSeg) {
      int64_t sum = 0;
      const int16_t *f = recAud.buf + pos;
      for (int i = 0; i < FRAME; i++) sum += (int32_t)f[i] * f[i];
      int rms = (int)sqrtf((float)(sum / FRAME));
      micLevel = rms;
      if (!inSpeech) {
        loud = rms > thr ? loud + 1 : 0;
        if (loud >= 3) {
          inSpeech = true;
          segStart = pos > FRAME * 15 ? pos - FRAME * 15 : 0;   // 往前多留 0.3 秒
          silentMs = 0; voicedMs = 60;
        }
      } else {
        if (rms > thr * 0.7f) { silentMs = 0; voicedMs += 20; } else silentMs += 20;
        if (silentMs >= 700 || pos - segStart > (size_t)MIC_SAMPLE_RATE * 6) {   // 一句話結束（最長 6 秒）
          inSpeech = false;
          if (voicedMs >= 200) { checkSeg = true; segEnd = pos; }
        }
      }
      pos += FRAME;
    }
    if (checkSeg) {
      size_t n = min(segEnd - segStart, MAX_SAMPLES);
      memcpy(wavBuf + 44, recAud.buf + segStart, n * 2);
      String heard = speechToText(n);                    // 影像與聲音照常在背景錄
      Serial.println("（錄影中聽到：" + heard + "）");
      if (isStopWord(heard)) heardStop = true;
    }
    delay(20);
  }
  recVid.run = false;
  recAud.run = false;
  while (!recVid.done || !recAud.done) delay(5);
  uint32_t durMs = millis() - recStart;
  recActive = false;
  micLevel = 0;
  while (buttonPressed()) delay(10);

  std::vector<uint32_t> &off = recVid.off, &len = recVid.sz;
  size_t vlen = recVid.len, maxFrame = recVid.maxFrame;
  bool full = recVid.full || (recAud.done && recAud.n + FRAME > recAud.cap);
  size_t frames = off.size();
  size_t ulawN = recAud.n / 2;                           // 8kHz 樣本數 = μ-law 位元組數
  uint8_t *ulaw = (uint8_t *)recAud.buf;
  for (size_t i = 0; i < ulawN; i++)                     // 兩點平均降頻到 8kHz，再編成 μ-law（寫在原緩衝區前段）
    ulaw[i] = linear2ulaw(((int)recAud.buf[2 * i] + recAud.buf[2 * i + 1]) / 2);
  size_t audBytes = ulawN;
  if (recAud.n) durMs = (uint32_t)((uint64_t)recAud.n * 1000 / MIC_SAMPLE_RATE);   // 以聲音長度為準，影音才會同步
  Serial.printf("錄影結束：%u 張，%u ms，影像 %u KB%s%s\n", (unsigned)frames, (unsigned)durMs, (unsigned)(vlen / 1024),
                full ? "（記憶體滿）" : "", heardStop ? "（聽到停止）" : stopped ? "（按鍵停止）" : "");
  String result;
  if (frames < 5 || durMs < 1000) {
    result = "錄到的畫面太少，沒有存檔。";
  } else {
    // ---- 組 AVI ----
    bool hasAudio = audBytes > 0;
    uint32_t fpsRate = (uint32_t)((uint64_t)frames * 1000000ULL / durMs);   // scale = 1000
    uint8_t *chunkHdr = (uint8_t *)ps_malloc(frames * 16 + 8);
    uint8_t *idx = (uint8_t *)ps_malloc(frames * 32 + 8);
    static const uint8_t zero2[2] = {0, 0};
    AviB64Stream st;
    std::vector<Seg> movi;
    uint32_t moviSize = 4, idxCount = 0, audPos = 0, maxAud = 0;
    uint8_t *ip = idx;
    for (size_t i = 0; i < frames; i++) {
      uint8_t *h = chunkHdr + i * 16;
      uint8_t *hp = h;
      putCC(hp, "00dc"); put32(hp, len[i]);
      movi.push_back({h, 8});
      movi.push_back({vbuf + off[i], len[i]});
      if (len[i] & 1) movi.push_back({zero2, 1});
      putCC(ip, "00dc"); put32(ip, 0x10); put32(ip, moviSize); put32(ip, len[i]); idxCount++;
      moviSize += 8 + ((len[i] + 1) & ~1u);
      if (hasAudio) {
        uint32_t end = (uint32_t)((uint64_t)ulawN * (i + 1) / frames);
        uint32_t n = end - audPos;
        if (n) {
          putCC(hp, "01wb"); put32(hp, n);
          movi.push_back({h + 8, 8});
          movi.push_back({ulaw + audPos, n});
          if (n & 1) movi.push_back({zero2, 1});
          putCC(ip, "01wb"); put32(ip, 0x10); put32(ip, moviSize); put32(ip, n); idxCount++;
          moviSize += 8 + ((n + 1) & ~1u);
          audPos = end;
          maxAud = max(maxAud, n);
        }
      }
    }
    uint32_t idxSize = idxCount * 16;
    uint32_t hdrlSize = 4 + 64 + 124 + (hasAudio ? 100 : 0);
    uint32_t riffSize = 4 + (8 + hdrlSize) + (8 + moviSize) + (8 + idxSize);

    static uint8_t hdr[512];
    uint8_t *p = hdr;
    putCC(p, "RIFF"); put32(p, riffSize); putCC(p, "AVI ");
    putCC(p, "LIST"); put32(p, hdrlSize); putCC(p, "hdrl");
    putCC(p, "avih"); put32(p, 56);
    put32(p, (uint32_t)((uint64_t)durMs * 1000 / frames));          // 每張微秒
    put32(p, (uint32_t)((vlen + audBytes) * 1000ULL / durMs));      // 每秒最大位元組
    put32(p, 0); put32(p, 0x110);                                   // HASINDEX | ISINTERLEAVED
    put32(p, frames); put32(p, 0); put32(p, hasAudio ? 2 : 1);
    put32(p, maxFrame + 8); put32(p, REC_W); put32(p, REC_H);
    put32(p, 0); put32(p, 0); put32(p, 0); put32(p, 0);
    // 影像串流
    putCC(p, "LIST"); put32(p, 116); putCC(p, "strl");
    putCC(p, "strh"); put32(p, 56);
    putCC(p, "vids"); putCC(p, "MJPG"); put32(p, 0); put16(p, 0); put16(p, 0); put32(p, 0);
    put32(p, 1000); put32(p, fpsRate); put32(p, 0); put32(p, frames);
    put32(p, maxFrame); put32(p, 0xFFFFFFFF); put32(p, 0);
    put16(p, 0); put16(p, 0); put16(p, REC_W); put16(p, REC_H);
    putCC(p, "strf"); put32(p, 40);
    put32(p, 40); put32(p, REC_W); put32(p, REC_H); put16(p, 1); put16(p, 24);
    putCC(p, "MJPG"); put32(p, REC_W * REC_H * 3); put32(p, 0); put32(p, 0); put32(p, 0); put32(p, 0);
    if (hasAudio) {                                                // 聲音串流：8kHz 8bit 單聲道 μ-law
      putCC(p, "LIST"); put32(p, 92); putCC(p, "strl");
      putCC(p, "strh"); put32(p, 56);
      putCC(p, "auds"); put32(p, 0); put32(p, 0); put16(p, 0); put16(p, 0); put32(p, 0);
      put32(p, 1); put32(p, MIC_SAMPLE_RATE / 2); put32(p, 0); put32(p, ulawN);
      put32(p, maxAud); put32(p, 0xFFFFFFFF); put32(p, 1);
      put16(p, 0); put16(p, 0); put16(p, 0); put16(p, 0);
      putCC(p, "strf"); put32(p, 16);
      put16(p, 7); put16(p, 1); put32(p, MIC_SAMPLE_RATE / 2); put32(p, MIC_SAMPLE_RATE / 2); put16(p, 1); put16(p, 8);   // 7 = μ-law
    }
    putCC(p, "LIST"); put32(p, moviSize); putCC(p, "movi");
    st.segs.push_back({hdr, (uint32_t)(p - hdr)});
    for (auto &m : movi) st.segs.push_back(m);
    static uint8_t idxHdr[8];
    uint8_t *q = idxHdr;
    putCC(q, "idx1"); put32(q, idxSize);
    st.segs.push_back({idxHdr, 8});
    st.segs.push_back({idx, idxSize});
    st.rawTotal = 8 + riffSize;
    st.b64Total = (st.rawTotal + 2) / 3 * 4;
    Serial.printf("AVI %u KB（base64 %u KB）上傳中…\n", (unsigned)(st.rawTotal / 1024), (unsigned)(st.b64Total / 1024));

    // ---- 上傳 ----
    String fname = timeName("SparkBot_video_", ".avi");
    setUi(UI_THINK, "影片上傳中…", "0%");
    tts.buf = (uint8_t *)ps_malloc(160 * 1024);
    ttsCap = tts.buf ? 160 * 1024 : 0;
    if (tts.buf) speak(heardStop ? "好的，停止錄製，上傳中，請稍等喔！" : "錄製結束，上傳中，請稍等喔！");
    NetGuard ng;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(180000);
    http.begin(client, String(GDRIVE_SCRIPT_URL) + "?token=" GDRIVE_TOKEN "&type=video&name=" + urlEncode(fname));
    http.addHeader("Content-Type", "text/plain");
    const char *keys[] = {"Location"};
    http.collectHeaders(keys, 1);
    int code = http.sendRequest("POST", &st, st.b64Total);
    String resp = gdriveResult(http, code, client);
    http.end();
    Serial.printf("影片上傳 %s → %d %s\n", fname.c_str(), code, resp.substring(0, 40).c_str());
    if (resp.startsWith("ok"))                                   // Apps Script 存好檔會回覆「ok 檔案ID」
      result = String(full ? "記憶體滿了，先幫你存這一段。" : "") + "錄好了！" + String(durMs / 1000) + "秒的影片已經上傳到雲端硬碟囉。";
    else
      result = "影片上傳失敗（" + String(code) + "），請檢查雲端硬碟上傳網址。";
    free(chunkHdr);
    free(idx);
  }
  waitPlaybackDone();
  free(tts.buf);
  free(vbuf);
  free(recAud.buf);
  recAud.buf = nullptr;
  tts.buf = (uint8_t *)ps_malloc(TTS_BUF_BYTES);
  ttsCap = tts.buf ? TTS_BUF_BYTES : 0;
  if (!tts.buf) Serial.println("警告：語音緩衝區重新配置失敗");
  flushMic(200);
  lastInteraction = millis();
  return result;
}
