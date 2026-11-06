// StickChat: type prompts on an M5StickC Plus2 and send them to Ollama over WiFi.
// UI is Marauder-style: title bar, boxed menu rows, 3-button navigation.
// Boot logo on startup, activity-based auto sleep (display off)
//
// Buttons (landscape):
//   A  (front)  click = select / press key      hold = back / backspace
//   B  (side)   click = down / right            hold = page down / left
//   PWR (side)  click = up / next key row       hold = page up / prev row

#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <freertos/semphr.h>
#include <atomic>
#include <time.h>
#include <sys/time.h>
#include <vector>
#include <utility>
#include "config.h"

static M5Canvas cv(&M5.Display);
static Preferences prefs;
static String cSsid, cPass, cHost, cModel;
static int cPort;
static String cloudApiKey, cCloudModel;
static String freeOk, freeBad;   // cached ',name,name,' lists of cloud models that work / are blocked on the free tier
static bool preferCloud = false;  // set when you pick a model from the cloud list
static const int LOGO_BOOT_MS = 2000, LOGO_SLEEP_MS = 2000, LOGO_WAKE_MS = 2000;  // logo duration (ms)
static int cTzMin = 330;       // timezone offset from UTC in minutes (330 = India, UTC+5:30)
static bool cH12 = false;      // 12-hour clock
static int cStyle = 1, cLang = 0;   // reply length (0 short,1 normal,2 detailed), language (0 auto,1 English,2 Hinglish)
static int cBright = 110, cDeepMin = 5;   // screen brightness, deep-sleep delay in minutes (0 = off)
static std::vector<String> pHist;           // recent prompts, newest first
// timer / stopwatch / alarm state
static bool tmActive = false;  static uint32_t tmEnd = 0, tmTotalMs = 0;  static String tmLabel = "Timer";
static bool swRun = false;     static uint32_t swStart = 0, swAccum = 0;  static std::vector<uint32_t> swLaps;
static bool alOn = false;      static int alH = 7, alM = 0, alLastKey = -1;
static bool snActive = false;  static uint32_t snEnd = 0;   // alarm snooze has its own countdown, separate from the timer
static bool useCloudFallback = false;
static std::vector<std::pair<String, String>> hist;

static const uint16_t BG = 0x0000, FG = 0xFFFF, ACC = 0x07FF, DIM = 0x4208;

// Power management
static uint32_t lastActivity = 0;
static uint32_t SLEEP_TIMEOUT = 30000;  // screen-off delay (ms), changeable in Settings
static bool displaySleeping = false;

// Forward declarations
static bool ensureWifi();
static int quickPick(String& text);
static void logoOut(int totalMs);
static void checkAlerts();

// Deep sleep: ~10 uA-class state. Wake = button A, then a normal reboot.
static void goDeepSleep() {
  if (!displaySleeping) logoOut(LOGO_SLEEP_MS);
  M5.Display.setBrightness(0);
  M5.Display.sleep();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  while (M5.BtnA.isPressed()) { M5.update(); delay(10); }   // don't wake instantly
  gpio_hold_en(GPIO_NUM_4);          // Plus2 keeps itself powered via GPIO4
  gpio_deep_sleep_hold_en();
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_37, 0);  // button A, active low
  esp_deep_sleep_start();
}

static void maybeDeepSleep() {
  if (tmActive || alOn || swRun || snActive) return;   // keep running timers/alarms alive
  if (cDeepMin > 0 && millis() - lastActivity > (uint32_t)cDeepMin * 60000UL) goDeepSleep();
}

// ---------- logo (drawn with primitives, no bitmap in flash) ----------
static void fadeBrightness(int from, int to, int ms) {
  for (int t = 0; t < ms; t += 20) {
    M5.Display.setBrightness(from + (to - from) * t / ms);
    delay(20);
  }
  M5.Display.setBrightness(to);
}

static void logoBase() {
  auto& g = M5.Display;
  g.fillScreen(BG);
  const int bx = 24, by = 30, bw = 76, bh = 50;
  g.fillRoundRect(bx, by, bw, bh, 11, ACC);
  g.fillRoundRect(bx + 4, by + 4, bw - 8, bh - 8, 8, BG);
  g.fillTriangle(bx + 14, by + bh - 3, bx + 14, by + bh + 15, bx + 32, by + bh - 3, ACC);
  g.fillTriangle(bx + 18, by + bh - 5, bx + 18, by + bh + 8, bx + 29, by + bh - 5, BG);
  g.fillRect(112, 70, 106, 2, ACC);
  g.setTextDatum(top_left);
  g.setTextSize(1);
  g.setTextColor(0x8410, BG);
  g.drawString("pocket AI chat", 112, 76);
  g.drawString("v1.0", 206, 122);
}

static void logoDots(int active) {
  for (int i = 0; i < 3; i++) M5.Display.fillCircle(62 + i * 18, 55, 5, i == active ? ACC : DIM);
}

static void logoTitle(int n) {   // typewriter: first n letters of "StickChat"
  auto& g = M5.Display;
  g.setTextDatum(top_left);
  g.setTextSize(2);
  g.fillRect(112, 46, 108, 18, BG);
  if (n > 0) {
    g.setTextColor(FG, BG);
    g.drawString(String("Stick").substring(0, min(n, 5)), 112, 48);
  }
  if (n > 5) {
    g.setTextColor(ACC, BG);
    g.drawString(String("Chat").substring(0, n - 5), 112 + 60, 48);
  }
}

// Fade the logo in and keep it up for totalMs. typing = boot animation (title types out).
static void logoIn(bool typing, int totalMs) {
  uint32_t t0 = millis();
  M5.Display.setBrightness(0);
  logoBase();
  logoTitle(typing ? 0 : 9);
  logoDots(0);
  fadeBrightness(0, cBright, 250);
  if (typing) for (int n = 1; n <= 9; n++) { logoTitle(n); logoDots(n % 3); delay(70); }
  int k = 0;
  while (millis() - t0 < (uint32_t)totalMs) { logoDots(k++ % 3); delay(90); }
}

// Show the logo for totalMs, then fade the screen to black (call before the panel is turned off).
static void logoOut(int totalMs) {
  uint32_t t0 = millis();
  M5.Display.setBrightness(cBright);
  logoBase();
  logoTitle(9);
  int k = 0;
  while (millis() - t0 < (uint32_t)(totalMs - 350)) { logoDots(k++ % 3); delay(90); }
  fadeBrightness(cBright, 0, 350);
}

// Screen-off sleep: logo, fade, panel + WiFi off. The last UI frame stays in `cv`.
static void sleepScreen() {
  logoOut(LOGO_SLEEP_MS);
  displaySleeping = true;
  M5.Display.sleep();
  WiFi.mode(WIFI_OFF);
}

// Wake without the logo (alarms and timers).
static void wakeQuiet() {
  if (!displaySleeping) return;
  M5.Display.wakeup();
  M5.Display.setBrightness(cBright);
  displaySleeping = false;
}

// Wake: logo flash, then put the last UI frame back. WiFi reconnects in the background.
static void wakeScreen() {
  if (!displaySleeping) return;
  M5.Display.wakeup();
  logoIn(false, LOGO_WAKE_MS);
  displaySleeping = false;
  cv.pushSprite(0, 0);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.begin(cSsid.c_str(), cPass.c_str());
}

// Header status text: link mode + battery
static String statusStr() {
  String s = WiFi.status() == WL_CONNECTED
               ? ((useCloudFallback && cloudApiKey.length() >= 20 && preferCloud) ? "CLD" : "LAN")
               : "off";
  return s;
}

// ---------- input ----------
enum Btn { NONE, A_CLICK, A_HOLD, B_CLICK, B_HOLD, P_CLICK, P_HOLD };

static Btn poll() {
  M5.update();
  if (M5.BtnA.wasHold()) return A_HOLD;
  if (M5.BtnA.wasClicked()) return A_CLICK;
  if (M5.BtnB.wasHold()) return B_HOLD;
  if (M5.BtnB.wasClicked()) return B_CLICK;
  if (M5.BtnPWR.wasHold()) return P_HOLD;
  if (M5.BtnPWR.wasClicked()) return P_CLICK;
  return NONE;
}

static Btn waitBtn() {
  Btn b;
  uint32_t startTime = millis();
  do {
    b = poll();
    maybeDeepSleep();
    checkAlerts();
    // Check for sleep timeout during wait
    if (millis() - startTime > SLEEP_TIMEOUT && !displaySleeping) {
      sleepScreen();

      while (true) {
        M5.update();
        if (M5.BtnA.wasClicked() || M5.BtnB.wasClicked() || M5.BtnPWR.wasClicked()) {
          break;
        }
        maybeDeepSleep();
        checkAlerts();
        delay(10);
      }

      wakeScreen();
      startTime = millis();
    }
    delay(10);
  } while (b == NONE);
  lastActivity = millis();
  return b;
}

// ---------- drawing helpers ----------
static void header(const String& t, const String& r = "") {
  cv.fillRect(0, 0, 240, 14, ACC);
  cv.setTextColor(BG, ACC);
  cv.setTextSize(1);
  cv.setTextDatum(top_left);
  cv.drawString(t, 4, 3);
  int rx = 236;
  cv.setTextDatum(top_right);
  if (r.length()) { cv.drawString(r, 236, 3); rx = 236 - (int)r.length() * 6 - 8; }
  int b = M5.Power.getBatteryLevel();
  if (b >= 0) {
    if (b > 100) b = 100;
    int ix = rx - 20;
    cv.drawRect(ix, 3, 18, 9, BG);
    cv.fillRect(ix + 18, 5, 2, 5, BG);
    cv.fillRect(ix + 2, 5, (b * 14) / 100, 5, b <= 15 ? 0xF800 : BG);   // red when low
    String pct = String(b) + "%";
    cv.drawString(pct, ix - 3, 3);
    rx = ix - 3 - (int)pct.length() * 6 - 6;
  }
  cv.drawString(statusStr(), rx, 3);
  cv.setTextDatum(top_left);
}

static void footer(const String& t) {
  cv.fillRect(0, 123, 240, 12, DIM);
  cv.setTextColor(FG, DIM);
  cv.setTextSize(1);
  cv.setTextDatum(top_left);
  cv.drawString(t, 3, 125);
}

static void wrapText(const String& s, int cols, std::vector<String>& out) {
  String line, word;
  auto flushWord = [&]() {
    if (word.length() == 0) return;
    if ((int)(line.length() + (line.length() ? 1 : 0) + word.length()) <= cols) {
      if (line.length()) line += ' ';
      line += word;
    } else {
      if (line.length()) out.push_back(line);
      while ((int)word.length() > cols) {
        out.push_back(word.substring(0, cols));
        word = word.substring(cols);
      }
      line = word;
    }
    word = "";
  };
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\n') { flushWord(); out.push_back(line); line = ""; }
    else if (c == ' ') flushWord();
    else word += c;
  }
  flushWord();
  if (line.length()) out.push_back(line);
}

static String clean(const String& s) {
  String o;
  o.reserve(s.length());
  for (size_t i = 0; i < s.length();) {
    uint8_t c = s[i];
    if (c < 0x80) { o += (char)c; i++; continue; }
    int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
    if (c == 0xE2 && i + 2 < s.length() && (uint8_t)s[i + 1] == 0x80) {
      uint8_t d = s[i + 2];
      if (d == 0x98 || d == 0x99) o += '\'';
      else if (d == 0x9C || d == 0x9D) o += '"';
      else if (d == 0x93 || d == 0x94) o += '-';
      else if (d == 0xA6) o += "...";
      else if (d == 0xA2) o += '*';
      else o += '?';
    } else o += '?';
    i += len;
  }
  o.replace("**", "");
  o.replace("`", "");
  o.replace("\r", "");
  return o;
}

static void busyScreen(const String& msg, const String& sub = "") {
  wakeScreen();
  cv.fillSprite(BG);
  header("StickChat");
  int a = (millis() / 3) % 360;
  cv.fillArc(120, 56, 16, 22, a, a + 90, ACC);
  cv.setTextColor(FG, BG);
  cv.setTextSize(1.5);
  cv.setTextDatum(top_center);
  cv.drawString(msg, 120, 90);
  cv.setTextSize(1);
  cv.setTextColor(0x8410, BG);
  cv.drawString(sub, 120, 110);
  cv.setTextDatum(top_left);
  cv.pushSprite(0, 0);
}

static void message(const String& title, const String& text) {
  wakeScreen();
  std::vector<String> lines;
  wrapText(clean(text), 38, lines);
  cv.fillSprite(BG);
  header(title);
  cv.setTextColor(FG, BG);
  cv.setTextSize(1);
  cv.setTextDatum(top_left);
  for (int i = 0; i < 12 && i < (int)lines.size(); i++) cv.drawString(lines[i], 3, 18 + i * 9);
  footer("A: ok");
  cv.pushSprite(0, 0);
  while (waitBtn() != A_CLICK) {}
}

// ---------- menu ----------
static int menu(const String& title, const std::vector<String>& items, int sel = 0) {
  int n = items.size(), top = 0;
  if (n == 0) return -1;
  while (true) {
    wakeScreen();
    if (sel < top) top = sel;
    if (sel >= top + 5) top = sel - 4;
    cv.fillSprite(BG);
    header(title, String(sel + 1) + "/" + String(n));
    for (int i = 0; i < 5 && top + i < n; i++) {
      int y = 17 + i * 21;
      bool on = (top + i == sel);
      if (on) { cv.fillRoundRect(3, y, 234, 19, 3, ACC); cv.setTextColor(BG, ACC); }
      else { cv.drawRoundRect(3, y, 234, 19, 3, DIM); cv.setTextColor(FG, BG); }
      cv.setTextSize(1.5);
      cv.setTextDatum(middle_left);
      cv.drawString(items[top + i].substring(0, 24), 10, y + 10);
    }
    cv.setTextDatum(top_left);
    footer("A:ok  B:down  PWR:up  holdA:back");
    cv.pushSprite(0, 0);
    switch (waitBtn()) {
      case B_CLICK: sel = (sel + 1) % n; break;
      case P_CLICK: sel = (sel - 1 + n) % n; break;
      case A_CLICK: return sel;
      case A_HOLD: return -1;
      default: break;
    }
  }
}

// ---------- on-screen keyboard ----------
static bool textInput(const String& title, String& text, const char* okLabel, bool mask = false, bool allowList = false) {
  static const char* L[3][4] = {
    {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm.,?"},
    {"1234567890", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM.,?"},
    {"!@#$%^&*()", "-_=+[]{}<>", "~`|\\/:;\"'%", "?,.!#&*()\\"},
  };
  int layer = 0, row = 0, col = 0;
  const int nsp = allowList ? 6 : 5, okIdx = allowList ? 4 : 3;

  auto cell = [&](int x, int y, int w, const String& lab, bool on) {
    if (on) { cv.fillRoundRect(x, y, w, 16, 3, ACC); cv.setTextColor(BG, ACC); }
    else { cv.drawRoundRect(x, y, w, 16, 3, DIM); cv.setTextColor(FG, BG); }
    cv.setTextDatum(middle_center);
    cv.drawString(lab, x + w / 2, y + 8);
  };

  while (true) {
    wakeScreen();
    cv.fillSprite(BG);
    header(title, String(text.length()));

    String shown = text;
    if (mask) { shown = ""; for (size_t i = 0; i < text.length(); i++) shown += '*'; }
    std::vector<String> lines;
    for (int i = 0; i < (int)shown.length(); i += 39) lines.push_back(shown.substring(i, i + 39));
    if (lines.empty() || lines.back().length() == 39) lines.push_back("");
    int n = lines.size(), start = max(0, n - 3);
    cv.setTextSize(1);
    cv.setTextColor(FG, BG);
    cv.setTextDatum(top_left);
    for (int i = start; i < n; i++) cv.drawString(lines[i], 3, 17 + (i - start) * 8);
    cv.drawString("_", 3 + lines.back().length() * 6, 17 + (n - 1 - start) * 8);

    cv.setTextSize(1.5);
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 10; c++) {
        String s = String(L[layer][r][c]);
        cell(c * 24 + 1, 42 + r * 18, 22, s, r == row && c == col);
      }
    cv.setTextSize(1);
    const char* lay = layer == 0 ? "abc" : layer == 1 ? "ABC" : "#+=";
    const char* sp[6] = { lay, "SPACE", "DEL", allowList ? "LIST" : okLabel, allowList ? okLabel : "BACK", "BACK" };
    int cw = 240 / nsp;
    for (int c = 0; c < nsp; c++) cell(c * cw + 1, 42 + 4 * 18, cw - 2, sp[c], row == 4 && c == col);
    cv.setTextDatum(top_left);
    cv.pushSprite(0, 0);

    int oldRow = row;
    int rc = (row == 4) ? nsp : 10;
    switch (waitBtn()) {
      case B_CLICK: col = (col + 1) % rc; break;
      case B_HOLD:  col = (col - 1 + rc) % rc; break;
      case P_CLICK: row = (row + 1) % 5; break;
      case P_HOLD:  row = (row + 4) % 5; break;
      case A_HOLD:  if (text.length()) text.remove(text.length() - 1); break;
      case A_CLICK:
        if (row < 4) {
          if (text.length() < 300) text += L[layer][row][col];
          if (layer == 1) layer = 0;
        } else if (col == 0) layer = (layer + 1) % 3;
        else if (col == 1) { if (text.length() < 300) text += ' '; }
        else if (col == 2) { if (text.length()) text.remove(text.length() - 1); }
        else if (allowList && col == 3) {
          if (quickPick(text) == 2) { row = 4; col = okIdx; }  // re-sending a recent prompt: jump to SEND
        }
        else if (col == okIdx) return true;
        else return false;
        break;
      default: break;
    }
    if (row != oldRow) {
      if (row == 4) col = col * nsp / 10;
      else if (oldRow == 4) col = min(9, col * 10 / nsp);
    }
  }
}

// ---------- config ----------
static void loadCfg() {
  prefs.begin("stickchat", false);
  cSsid = prefs.getString("ssid", DEF_SSID);
  cPass = prefs.getString("pass", DEF_PASS);
  cHost = prefs.getString("host", DEF_HOST);
  cModel = prefs.getString("model", DEF_MODEL);
  cPort = prefs.getInt("port", DEF_PORT);
  cloudApiKey = prefs.getString("apikey", DEF_API_KEY);
  cCloudModel = prefs.getString("cmodel", DEF_CLOUD_MODEL);
  freeOk = prefs.getString("fok", ",");
  freeBad = prefs.getString("fbad", ",");
  preferCloud = prefs.getBool("prefcloud", false);
  useCloudFallback = prefs.getBool("usecloud", DEF_USE_CLOUD);
  cBright = prefs.getInt("bright", 110);
  cDeepMin = prefs.getInt("dsleep", 5);
  cTzMin = prefs.getInt("tz", 330);
  cStyle = prefs.getInt("style", 1);
  cLang = prefs.getInt("lang", 0);
  alOn = prefs.getBool("alon", false);
  alH = prefs.getInt("alh", 7);
  alM = prefs.getInt("alm", 0);
  cH12 = prefs.getBool("h12", false);
  SLEEP_TIMEOUT = prefs.getUInt("scroff", 30000);
  pHist.clear();
  String ph = prefs.getString("phist", "");
  for (int st = 0; st < (int)ph.length();) {
    int e = ph.indexOf('\n', st);
    if (e < 0) e = ph.length();
    if (e > st) pHist.push_back(ph.substring(st, e));
    st = e + 1;
  }
}

static void addPromptHistory(const String& p) {
  for (size_t i = 0; i < pHist.size(); i++)
    if (pHist[i] == p) { pHist.erase(pHist.begin() + i); break; }
  pHist.insert(pHist.begin(), p);
  while (pHist.size() > 10) pHist.pop_back();
  String j;
  for (auto& x : pHist) { if (j.length() + x.length() > 1500) break; j += x + "\n"; }
  prefs.putString("phist", j);
}

static const char* QUICK[] = {
  "Explain simply: ", "Summarize: ", "Fix this: ", "Translate to Hindi: ",
  "Translate to English: ", "Give me ideas for ", "Write a short reply to: ", "What is "
};

// Returns 0 = nothing picked, 1 = preset added in front of your text, 2 = recent prompt loaded
static int quickPick(String& text) {
  std::vector<String> items, real;
  std::vector<int> kind;
  for (auto& h : pHist) { items.push_back("< " + h); real.push_back(h); kind.push_back(2); }
  for (auto q : QUICK) { items.push_back(String("+ ") + q); real.push_back(q); kind.push_back(1); }
  int i = menu("Prompts", items);
  if (i < 0) return 0;
  if (kind[i] == 2) { text = real[i]; return 2; }
  text = real[i] + text;
  if (text.length() > 300) text = text.substring(0, 300);
  return 1;
}

// ---------- clock ----------
// System time (UTC) is kept by the ESP32; the Plus2's RTC chip keeps it across deep sleep / power-off.
static bool ntpStarted = false, ntpDone = false;

static bool timeValid() { return time(nullptr) > 1700000000; }

static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = y - era * 400;
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int)doe - 719468;
}

static void loadRtc() {   // RTC holds UTC
  if (!M5.Rtc.isEnabled()) return;
  auto dt = M5.Rtc.getDateTime();
  if (dt.date.year < 2024) return;
  time_t t = (time_t)(daysFromCivil(dt.date.year, dt.date.month, dt.date.date) * 86400 +
                      dt.time.hours * 3600 + dt.time.minutes * 60 + dt.time.seconds);
  struct timeval tv = {t, 0};
  settimeofday(&tv, NULL);
}

static void saveRtc() {
  if (!M5.Rtc.isEnabled() || !timeValid()) return;
  time_t t = time(nullptr);
  struct tm u;
  gmtime_r(&t, &u);
  m5::rtc_datetime_t dt;
  dt.date.year = u.tm_year + 1900;
  dt.date.month = u.tm_mon + 1;
  dt.date.date = u.tm_mday;
  dt.date.weekDay = u.tm_wday;
  dt.time.hours = u.tm_hour;
  dt.time.minutes = u.tm_min;
  dt.time.seconds = u.tm_sec;
  M5.Rtc.setDateTime(dt);
}

static bool localTm(struct tm* out) {
  if (!timeValid()) return false;
  time_t t = time(nullptr) + (time_t)cTzMin * 60;
  gmtime_r(&t, out);
  return true;
}

// Called from the home screen: starts SNTP once WiFi is up, writes the RTC when a sync completes.
static void ntpTick() {
  if (WiFi.status() == WL_CONNECTED && !ntpStarted) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    ntpStarted = true;
  }
  if (ntpStarted && !ntpDone && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
    saveRtc();
    ntpDone = true;
  }
}

static bool syncClockNow() {
  if (!ensureWifi()) return false;
  esp_sntp_stop();   // renamed in newer Arduino-ESP32 (IDF 5)
  sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  ntpStarted = true;
  uint32_t t0 = millis();
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && millis() - t0 < 8000) {
    busyScreen("Syncing clock", "pool.ntp.org");
    checkAlerts();
    delay(40);
  }
  if (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) return false;
  saveRtc();
  ntpDone = true;
  return true;
}

static String tzLabel(int mins) {
  char b[16];
  int a = abs(mins);
  if (a % 60) snprintf(b, sizeof b, "UTC%c%d:%02d", mins < 0 ? '-' : '+', a / 60, a % 60);
  else snprintf(b, sizeof b, "UTC%c%d", mins < 0 ? '-' : '+', a / 60);
  return String(b);
}

// ---------- settings (category pages -> option pages) ----------
// Option page: shows all choices, current one marked with ">", returns chosen index or -1.
static int pickOption(const String& title, const std::vector<String>& opts, int cur) {
  std::vector<String> items;
  for (int k = 0; k < (int)opts.size(); k++) items.push_back((k == cur ? "> " : "  ") + opts[k]);
  return menu(title, items, cur < 0 ? 0 : cur);
}

static void wake() { wakeScreen(); }

static void settingsClock() {
  static const int tzV[] = {-480, -300, 0, 60, 120, 180, 240, 330, 345, 360, 420, 480, 540, 600};
  int sel = 0;
  while (true) {
    wake();
    sel = menu("Clock", {"Timezone: " + tzLabel(cTzMin), "Format: " + String(cH12 ? "12-hour" : "24-hour"), "Sync time now", "Back"}, sel);
    if (sel < 0 || sel == 3) return;
    if (sel == 0) {
      int cur = -1; for (int k = 0; k < 14; k++) if (tzV[k] == cTzMin) cur = k;
      int r = pickOption("Timezone", {"UTC-8 (US Pacific)", "UTC-5 (US Eastern)", "UTC+0 (London)", "UTC+1 (Paris)", "UTC+2 (Cairo)",
                                      "UTC+3 (Moscow)", "UTC+4 (Dubai)", "UTC+5:30 (India)", "UTC+5:45 (Nepal)", "UTC+6 (Dhaka)",
                                      "UTC+7 (Bangkok)", "UTC+8 (Singapore)", "UTC+9 (Japan)", "UTC+10 (Sydney)"}, cur);
      if (r >= 0) { cTzMin = tzV[r]; prefs.putInt("tz", cTzMin); }
    } else if (sel == 1) {
      int r = pickOption("Time format", {"24-hour", "12-hour (AM/PM)"}, cH12 ? 1 : 0);
      if (r >= 0) { cH12 = (r == 1); prefs.putBool("h12", cH12); }
    } else {
      bool ok = syncClockNow();
      message("Clock", ok ? "Time synced from the internet." : "Sync failed. Check WiFi and try again.");
    }
  }
}

static void settingsReply() {
  static const char* LEN[] = {"Short", "Normal", "Detailed"};
  static const char* LNG[] = {"Auto", "English", "Hinglish"};
  int sel = 0;
  while (true) {
    wake();
    sel = menu("Reply style", {"Length: " + String(LEN[cStyle]), "Language: " + String(LNG[cLang]), "Back"}, sel);
    if (sel < 0 || sel == 2) return;
    if (sel == 0) {
      int r = pickOption("Reply length", {"Short (~40 words)", "Normal (~80 words)", "Detailed (~200 words)"}, cStyle);
      if (r >= 0) { cStyle = r; prefs.putInt("style", cStyle); hist.clear(); }
    } else {
      int r = pickOption("Reply language", {"Auto (match my prompt)", "English", "Hinglish (Hindi in English letters)"}, cLang);
      if (r >= 0) { cLang = r; prefs.putInt("lang", cLang); hist.clear(); }
    }
  }
}

static void settingsWifi() {
  int sel = 0;
  while (true) {
    wake();
    String pw = ""; for (size_t i = 0; i < cPass.length(); i++) pw += '*';
    sel = menu("WiFi", {"SSID: " + cSsid, "Password: " + pw, "Back"}, sel);
    if (sel < 0 || sel == 2) return;
    String v;
    if (sel == 0) { v = cSsid; if (textInput("WiFi SSID", v, "OK")) { cSsid = v; prefs.putString("ssid", v); WiFi.disconnect(); } }
    else { v = cPass; if (textInput("WiFi password", v, "OK")) { cPass = v; prefs.putString("pass", v); WiFi.disconnect(); } }
  }
}

static void settingsServer() {
  int sel = 0;
  while (true) {
    wake();
    sel = menu("Laptop server", {"Host: " + cHost, "Port: " + String(cPort), "Model: " + cModel, "Back"}, sel);
    if (sel < 0 || sel == 3) return;
    String v;
    if (sel == 0) { v = cHost; if (textInput("Ollama host IP", v, "OK")) { cHost = v; prefs.putString("host", v); } }
    else if (sel == 1) { v = String(cPort); if (textInput("Ollama port", v, "OK")) { cPort = v.toInt() ? v.toInt() : DEF_PORT; prefs.putInt("port", cPort); } }
    else { v = cModel; if (textInput("Local model name", v, "OK")) { cModel = v; prefs.putString("model", v); hist.clear(); } }
  }
}

static void settingsCloud() {
  int sel = 0;
  while (true) {
    wake();
    String key = cloudApiKey.length() > 4 ? String("****") + cloudApiKey.substring(cloudApiKey.length() - 4)
                                          : String(cloudApiKey.length() ? "****" : "(none)");
    sel = menu("Cloud (ollama.com)",
               {"Fallback: " + String(useCloudFallback ? "ON" : "OFF"), "Cloud model: " + cCloudModel, "API key: " + key, "Back"}, sel);
    if (sel < 0 || sel == 3) return;
    String v;
    if (sel == 0) {
      int r = pickOption("Cloud fallback", {"ON  (use cloud when laptop is off)", "OFF (laptop only)"}, useCloudFallback ? 0 : 1);
      if (r >= 0) { useCloudFallback = (r == 0); prefs.putBool("usecloud", useCloudFallback); }
    } else if (sel == 1) {
      v = cCloudModel; if (textInput("Cloud model name", v, "OK")) { cCloudModel = v; prefs.putString("cmodel", v); hist.clear(); }
    } else {
      v = cloudApiKey; if (textInput("API key", v, "OK")) { cloudApiKey = v; prefs.putString("apikey", v); }
    }
  }
}

static void settingsDisplay() {
  static const int brightV[] = {40, 80, 120, 180};
  static const int deepV[] = {0, 2, 5, 10};
  static const uint32_t offV[] = {15000, 30000, 60000, 120000};
  int sel = 0;
  while (true) {
    wake();
    String off = SLEEP_TIMEOUT >= 60000 ? String(SLEEP_TIMEOUT / 60000) + " min" : String(SLEEP_TIMEOUT / 1000) + " s";
    sel = menu("Display & power",
               {"Brightness: " + String(cBright), "Screen off: " + off,
                "Deep sleep: " + (cDeepMin ? String(cDeepMin) + " min" : String("off")), "Back"}, sel);
    if (sel < 0 || sel == 3) return;
    if (sel == 0) {
      int cur = -1; for (int k = 0; k < 4; k++) if (brightV[k] == cBright) cur = k;
      int r = pickOption("Brightness", {"Low (40)", "Medium (80)", "High (120)", "Max (180)"}, cur);
      if (r >= 0) { cBright = brightV[r]; prefs.putInt("bright", cBright); M5.Display.setBrightness(cBright); }
    } else if (sel == 1) {
      int cur = -1; for (int k = 0; k < 4; k++) if (offV[k] == SLEEP_TIMEOUT) cur = k;
      int r = pickOption("Screen off after", {"15 seconds", "30 seconds", "1 minute", "2 minutes"}, cur);
      if (r >= 0) { SLEEP_TIMEOUT = offV[r]; prefs.putUInt("scroff", SLEEP_TIMEOUT); }
    } else {
      int cur = -1; for (int k = 0; k < 4; k++) if (deepV[k] == cDeepMin) cur = k;
      int r = pickOption("Deep sleep after idle", {"Off", "2 minutes", "5 minutes", "10 minutes"}, cur);
      if (r >= 0) { cDeepMin = deepV[r]; prefs.putInt("dsleep", cDeepMin); }
    }
  }
}

static void settings() {
  int sel = 0;
  while (true) {
    wake();
    sel = menu("Settings", {"WiFi", "Laptop server", "Cloud (ollama.com)", "Display & power", "Clock", "Reply style", "Reset all settings", "Back"}, sel);
    if (sel < 0 || sel == 7) return;
    switch (sel) {
      case 0: settingsWifi(); break;
      case 1: settingsServer(); break;
      case 2: settingsCloud(); break;
      case 3: settingsDisplay(); break;
      case 4: settingsClock(); break;
      case 5: settingsReply(); break;
      case 6: {
        int r = pickOption("Reset all settings?", {"No, keep them", "Yes, reset to defaults"}, 0);
        if (r == 1) { prefs.clear(); loadCfg(); M5.Display.setBrightness(cBright); message("Reset", "Done"); }
        break;
      }
    }
  }
}

// ---------- tools: timer, stopwatch, alarm ----------
static String two(int v) { return v < 10 ? "0" + String(v) : String(v); }

// Full-screen flashing alert with beeps. Returns after a button press (or 60 s).
static void ringAlert(const String& title, const String& sub, bool allowSnooze) {
  wakeQuiet();
  M5.Display.setBrightness(cBright);
  const size_t SZ = 240 * 135 * 2;
  uint8_t* snap = (uint8_t*)malloc(SZ);       // save the current screen so we can put it back
  if (snap) memcpy(snap, cv.getBuffer(), SZ);
  uint32_t t0 = millis(), lastBeep = 0;
  int k = 0;
  bool snooze = false;
  while (millis() - t0 < 60000) {
    M5.update();
    bool a = M5.BtnA.wasClicked();
    if (a || M5.BtnB.wasClicked() || M5.BtnPWR.wasClicked() || M5.BtnA.wasHold()) { snooze = allowSnooze && a; break; }
    bool on = ((millis() - t0) / 400) % 2 == 0;
    cv.fillSprite(on ? ACC : BG);
    cv.setTextColor(on ? BG : ACC, on ? ACC : BG);
    cv.setTextDatum(middle_center);
    cv.setTextSize(2.5);
    cv.drawString(title, 120, 40);
    cv.setTextSize(2);
    cv.drawString(sub, 120, 80);
    cv.setTextDatum(top_left);
    footer(allowSnooze ? "A: snooze 5 min   B: stop" : "any button: stop");
    cv.pushSprite(0, 0);
    if (millis() - lastBeep > 500) { M5.Speaker.tone((k++ % 2) ? 2400 : 1800, 220); lastBeep = millis(); }
    delay(30);
  }
  M5.Speaker.stop();
  lastActivity = millis();
  if (snooze) { snActive = true; snEnd = millis() + 300000; }
  if (snap) { memcpy(cv.getBuffer(), snap, SZ); free(snap); cv.pushSprite(0, 0); }
}

static void checkAlerts() {
  static bool busy = false;
  if (busy) return;
  if (tmActive && (int32_t)(millis() - tmEnd) >= 0) {
    busy = true;
    tmActive = false;
    ringAlert(tmLabel + " done!", "", false);
    busy = false;
  }
  if (snActive && (int32_t)(millis() - snEnd) >= 0) {
    busy = true;
    snActive = false;
    ringAlert("ALARM", "Snooze over", true);
    busy = false;
  }
  struct tm t;
  if (alOn && localTm(&t) && t.tm_hour == alH && t.tm_min == alM) {
    int key = t.tm_yday * 1440 + t.tm_hour * 60 + t.tm_min;
    if (key != alLastKey) {
      alLastKey = key;
      busy = true;
      ringAlert("ALARM", two(alH) + ":" + two(alM), true);
      busy = false;
    }
  }
}

static bool timeSpinner(const String& title, int& h, int& m) {
  int field = 0;
  while (true) {
    wake();
    cv.fillSprite(BG);
    header(title);
    cv.setTextDatum(top_left);
    cv.setTextSize(4);
    cv.setTextColor(field == 0 ? BG : FG, field == 0 ? ACC : BG);
    cv.drawString(two(h), 45, 35);
    cv.setTextColor(FG, BG);
    cv.drawString(":", 93, 35);
    cv.setTextColor(field == 1 ? BG : FG, field == 1 ? ACC : BG);
    cv.drawString(two(m), 117, 35);
    footer(field == 0 ? "B:+  PWR:-  A:next  holdA:cancel" : "B:+  PWR:-  A:save  holdA:cancel");
    cv.pushSprite(0, 0);
    switch (waitBtn()) {
      case B_CLICK: if (field == 0) h = (h + 1) % 24; else m = (m + 1) % 60; break;
      case P_CLICK: if (field == 0) h = (h + 23) % 24; else m = (m + 59) % 60; break;
      case B_HOLD:  if (field == 0) h = (h + 6) % 24; else m = (m + 10) % 60; break;
      case P_HOLD:  if (field == 0) h = (h + 18) % 24; else m = (m + 50) % 60; break;
      case A_CLICK: if (field == 0) field = 1; else return true; break;
      case A_HOLD:  return false;
      default: break;
    }
  }
}

static void timerPage() {
  if (!tmActive) {
    static const int mins[] = {1, 2, 3, 5, 10, 15, 20, 30, 45, 60};
    std::vector<String> opts;
    for (int k = 0; k < 10; k++) opts.push_back(String(mins[k]) + " min");
    opts.push_back("Custom (type minutes)");
    int r = pickOption("Timer", opts, -1);
    if (r < 0) return;
    uint32_t ms;
    if (r == 10) {
      String v;
      if (!textInput("Minutes (1-600)", v, "OK")) return;
      int mm = v.toInt();
      if (mm < 1 || mm > 600) { message("Timer", "Enter 1 to 600 minutes."); return; }
      ms = (uint32_t)mm * 60000UL;
    } else ms = (uint32_t)mins[r] * 60000UL;
    tmActive = true; tmTotalMs = ms; tmEnd = millis() + ms; tmLabel = "Timer";
    lastActivity = millis();
  }
  while (true) {
    wake();
    if (!tmActive) return;
    int32_t left = (int32_t)(tmEnd - millis());
    if (left < 0) left = 0;
    int secs = (left + 999) / 1000;
    cv.fillSprite(BG);
    header(tmLabel);
    cv.setTextDatum(top_left);
    cv.setTextSize(5);
    cv.setTextColor(FG, BG);
    cv.drawString(two(secs / 60) + ":" + two(secs % 60), 45, 30);
    int w = tmTotalMs ? (int)((uint64_t)(tmTotalMs - left) * 200 / tmTotalMs) : 0;
    cv.drawRect(20, 90, 200, 10, DIM);
    cv.fillRect(20, 90, w, 10, ACC);
    footer("A:stop  B:+1 min  PWR:back");
    cv.pushSprite(0, 0);
    Btn b = poll();
    if (b != NONE) lastActivity = millis();
    if (b == A_CLICK) { tmActive = false; return; }
    if (b == B_CLICK) { tmEnd += 60000; tmTotalMs += 60000; }
    if (b == P_CLICK || b == A_HOLD) return;
    checkAlerts();
    if (millis() - lastActivity > SLEEP_TIMEOUT) return;
    delay(50);
  }
}

static uint32_t swElapsed() { return swRun ? swAccum + (millis() - swStart) : swAccum; }

static String fmtSw(uint32_t ms) {
  uint32_t t = ms / 100;   // tenths
  uint32_t s = t / 10;
  return two(s / 60) + ":" + two(s % 60) + "." + String(t % 10);
}

static void stopwatchPage() {
  while (true) {
    wake();
    uint32_t e = swElapsed();
    uint32_t tenths = (e / 100) % 10, secs = e / 1000;
    cv.fillSprite(BG);
    header("Stopwatch");
    cv.setTextDatum(top_left);
    cv.setTextSize(4);
    cv.setTextColor(FG, BG);
    cv.drawString(two(secs / 60) + ":" + two(secs % 60), 48, 20);
    cv.setTextSize(2);
    cv.setTextColor(ACC, BG);
    cv.drawString("." + String(tenths), 48 + 120, 38);
    cv.setTextSize(1);
    cv.setTextColor(0x8410, BG);
    int shown = 0;
    for (int i = (int)swLaps.size() - 1; i >= 0 && shown < 4; i--, shown++)
      cv.drawString("#" + String(i + 1) + "  " + fmtSw(swLaps[i]), 60, 66 + shown * 12);
    footer(swRun ? "A:stop  B:lap  holdA:back" : "A:start  PWR:reset  holdA:back");
    cv.pushSprite(0, 0);
    Btn b = poll();
    if (b != NONE) lastActivity = millis();
    if (b == A_CLICK) {
      if (swRun) { swAccum += millis() - swStart; swRun = false; }
      else { swStart = millis(); swRun = true; }
    }
    if (b == B_CLICK && swRun && swLaps.size() < 20) swLaps.push_back(swElapsed());
    if (b == P_CLICK && !swRun) { swAccum = 0; swLaps.clear(); }
    if (b == A_HOLD) return;
    checkAlerts();
    if (millis() - lastActivity > SLEEP_TIMEOUT) return;   // back to home; the stopwatch keeps counting and the screen can sleep
    delay(50);
  }
}

static void alarmPage() {
  int sel = 0;
  while (true) {
    wake();
    sel = menu("Alarm", {"Alarm: " + String(alOn ? "ON" : "OFF"), "Time: " + two(alH) + ":" + two(alM), "Back"}, sel);
    if (sel < 0 || sel == 2) return;
    if (sel == 0) {
      int r = pickOption("Alarm", {"ON  (daily)", "OFF"}, alOn ? 0 : 1);
      if (r >= 0) {
        alOn = (r == 0);
        if (!alOn) snActive = false;
        prefs.putBool("alon", alOn);
        alLastKey = -1;
        if (alOn && !timeValid()) message("Alarm", "Clock is not set yet.\nSettings > Clock > Sync.");
      }
    } else {
      int h = alH, m = alM;
      if (timeSpinner("Alarm time", h, m)) {
        alH = h; alM = m;
        prefs.putInt("alh", alH);
        prefs.putInt("alm", alM);
        alLastKey = -1;
      }
    }
  }
}

static void toolsMenu() {
  int sel = 0;
  while (true) {
    wake();
    String t = "Timer";
    if (tmActive) { int s = max(0, (int)((int32_t)(tmEnd - millis()) / 1000)); t += ": " + two(s / 60) + ":" + two(s % 60); }
    String w = "Stopwatch";
    if (swRun) w += ": running";
    String a = "Alarm: " + String(alOn ? two(alH) + ":" + two(alM) + " ON" : String("off"));
    sel = menu("Tools", {t, w, a, "Back"}, sel);
    if (sel < 0 || sel == 3) return;
    if (sel == 0) timerPage();
    else if (sel == 1) stopwatchPage();
    else alarmPage();
  }
}

// ---------- network ----------
static bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);   // modem sleep between beacons
  WiFi.begin(cSsid.c_str(), cPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    busyScreen("Connecting WiFi", cSsid);
    checkAlerts();
    delay(40);
  }
  return WiFi.status() == WL_CONNECTED;
}

struct Job {
  String body, bodyCloud, reply, err;
  // flags are shared between the UI task and the request task: std::atomic gives proper ordering
  std::atomic<bool> usedCloud{false}, cloudBlocked{false}, cloudAuth{false}, done{false}, cancel{false};
  SemaphoreHandle_t mtx;   // guards `reply`
  Job() { mtx = xSemaphoreCreateMutex(); }
  ~Job() { if (mtx) vSemaphoreDelete(mtx); }
  void append(const String& t) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (reply.length() < 4000) reply += t;
    xSemaphoreGive(mtx);
  }
  void resetReply() { xSemaphoreTake(mtx, portMAX_DELAY); reply = ""; xSemaphoreGive(mtx); }
  String snapshot() { xSemaphoreTake(mtx, portMAX_DELAY); String c = reply; xSemaphoreGive(mtx); return c; }
};

// Receives the decoded response body as it arrives; every line is one JSON chunk from Ollama.
class ChatSink : public Stream {
 public:
  Job* j;
  String line, error;
  explicit ChatSink(Job* job) : j(job) {}
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, size_t n) override {
    if (j->cancel) return 0;   // aborts the transfer
    for (size_t i = 0; i < n; i++) {
      char c = (char)buf[i];
      if (c == '\n') { handle(); line = ""; }
      else if (c != '\r' && line.length() < 3000) line += c;
    }
    return n;
  }
  void handle() {
    if (line.length() == 0 || line[0] != '{') return;
    JsonDocument d;
    if (deserializeJson(d, line)) return;
    if (d["error"].is<const char*>()) { error = d["error"].as<String>(); return; }
    const char* c = d["message"]["content"] | "";
    if (c[0]) j->append(String(c));
  }
};

// One streaming chat POST. Text is appended to j->reply as it arrives. Returns true on success, else fills `err`.
static bool postChat(const String& url, const String& body, bool tls, const String& key,
                     uint16_t timeoutMs, Job* j, String& err) {
  WiFiClientSecure secure;   // declared BEFORE http so it outlives it
  HTTPClient http;
  if (tls) {
    secure.setInsecure();
    if (!http.begin(secure, url)) { err = "bad url"; return false; }
  } else if (!http.begin(url)) { err = "bad url"; return false; }
  http.setConnectTimeout(10000);
  http.setTimeout(timeoutMs);
  http.addHeader("Content-Type", "application/json");
  if (key.length()) http.addHeader("Authorization", "Bearer " + key);
  int code = http.POST((uint8_t*)body.c_str(), body.length());
  if (code <= 0) { err = http.errorToString(code); http.end(); return false; }
  if (code != 200) {
    String resp = http.getString();
    http.end();
    err = "HTTP " + String(code) + ": " + resp.substring(0, 80);
    return false;
  }
  ChatSink sink(j);
  int r = http.writeToStream(&sink);
  http.end();
  if (sink.line.length()) sink.handle();
  if (sink.error.length()) { err = sink.error; return false; }
  if (j->cancel) return true;
  String got = j->snapshot();
  if (got.length() == 0) {
    if (r < 0) { err = "stream error " + String(r); return false; }
    j->append("(empty reply)");
  }
  return true;
}

// Fast TCP probe so an unreachable laptop costs ~1.5 s, not a 10 s HTTP timeout.
static bool localUp() {
  WiFiClient c;
  bool ok = c.connect(cHost.c_str(), cPort, 1500);
  c.stop();
  return ok;
}

static bool cloudReady() { return useCloudFallback && cloudApiKey.length() >= 20; }

// Auth problems (bad / missing key, no permission) must not be confused with "model not in free tier".
static bool looksAuthError(const String& err) {
  String low = err;
  low.toLowerCase();
  return err.startsWith("HTTP 401") || low.indexOf("unauthorized") >= 0 || low.indexOf("api key") >= 0 ||
         low.indexOf("authenticat") >= 0 || low.indexOf("invalid token") >= 0 || low.indexOf("permission") >= 0;
}

static bool looksBlocked(const String& err) {
  if (looksAuthError(err)) return false;
  String low = err;
  low.toLowerCase();
  return low.indexOf("free tier") >= 0 || low.indexOf("subscription") >= 0 || low.indexOf("upgrade") >= 0 ||
         low.indexOf("not included") >= 0 || err.startsWith("HTTP 402");   // a bare 403 is NOT treated as blocked
}

static void doRequest(Job* j) {
  String localErr, cloudErr;
  bool cloudFirst = cloudReady() && preferCloud;
  for (int pass = 0; pass < 2; pass++) {
    bool doCloud = (pass == 0) ? cloudFirst : !cloudFirst;
    if (doCloud) {
      if (!cloudReady()) { cloudErr = useCloudFallback ? "no API key" : "cloud is OFF"; continue; }
      j->usedCloud = true;
      j->resetReply();
      if (postChat("https://ollama.com/api/chat", j->bodyCloud, true, cloudApiKey, 60000, j, cloudErr)) return;
      if (looksBlocked(cloudErr)) j->cloudBlocked = true;
      else if (looksAuthError(cloudErr)) j->cloudAuth = true;
    } else {
      j->usedCloud = false;
      if (!localUp()) { localErr = "laptop unreachable"; continue; }
      String url = "http://" + cHost + ":" + String(cPort) + "/api/chat";
      j->resetReply();
      if (postChat(url, j->body, false, "", 30000, j, localErr)) return;
    }
  }
  j->err = "Local: " + (localErr.length() ? localErr : String("-")) + "\nCloud: " + (cloudErr.length() ? cloudErr : String("-"));
}

static void ollamaTask(void* p) {
  Job* j = (Job*)p;
  doRequest(j);
  j->done = true;
  vTaskDelete(NULL);
}

static String systemPrompt() {
  String s = "You reply on a tiny 240x135 pixel screen. Plain ASCII text only, no markdown, no emojis. ";
  static const char* len[] = {"Keep replies under 40 words.", "Keep replies under 80 words.", "Give a thorough answer, up to about 200 words."};
  s += len[constrain(cStyle, 0, 2)];
  if (cLang == 1) s += " Always reply in English.";
  else if (cLang == 2) s += " Reply in Hinglish: Hindi written in English letters, never Devanagari script.";
  return s;
}

static String buildBody(const String& prompt, const String& model) {
  JsonDocument d;
  d["model"] = model;
  d["stream"] = true;
  JsonArray m = d["messages"].to<JsonArray>();
  JsonObject s = m.add<JsonObject>();
  s["role"] = "system";
  s["content"] = systemPrompt();
  for (auto& h : hist) {
    JsonObject o = m.add<JsonObject>();
    o["role"] = h.first;
    o["content"] = h.second;
  }
  JsonObject u = m.add<JsonObject>();
  u["role"] = "user";
  u["content"] = prompt;
  String out;
  serializeJson(d, out);
  return out;
}

static bool listHas(const String& l, const String& n) { return l.indexOf("," + n + ",") >= 0; }
static void listAdd(String& l, const String& n, const char* key) {
  if (listHas(l, n) || l.length() > 1500) return;
  l += n + ",";
  prefs.putString(key, l);
}
static void listDel(String& l, const String& n, const char* key) {
  int i = l.indexOf("," + n + ",");
  if (i < 0) return;
  l.remove(i, n.length() + 1);
  prefs.putString(key, l);
}

static bool fetchTags(const String& url, bool tls, const String& key, std::vector<String>& names, String& err) {
  WiFiClientSecure secure;
  HTTPClient http;
  if (tls) {
    secure.setInsecure();
    if (!http.begin(secure, url)) { err = "bad url"; return false; }
  } else if (!http.begin(url)) { err = "bad url"; return false; }
  http.setConnectTimeout(8000);
  http.setTimeout(15000);
  if (key.length()) http.addHeader("Authorization", "Bearer " + key);
  int code = http.GET();
  if (code != 200) { err = code <= 0 ? http.errorToString(code) : "HTTP " + String(code); http.end(); return false; }
  String resp = http.getString();
  http.end();
  JsonDocument d;
  if (deserializeJson(d, resp)) { err = "bad JSON"; return false; }
  for (JsonObject m : d["models"].as<JsonArray>()) {
    String n = m["name"].as<String>();
    if (n.length() == 0) n = m["model"].as<String>();
    if (n.length()) names.push_back(n);
  }
  if (names.empty()) { err = "empty list"; return false; }
  return true;
}

// Ollama publishes no per-model free-tier list, so test each model with a 1-token request.
// 1 = works, 0 = blocked on this plan, -1 = unknown (rate limit, timeout, server error)
static int probeModel(const String& name, String& why) {
  JsonDocument d;
  d["model"] = name;
  d["stream"] = false;
  d["options"]["num_predict"] = 1;
  JsonObject u = d["messages"].to<JsonArray>().add<JsonObject>();
  u["role"] = "user";
  u["content"] = "hi";
  String body;
  serializeJson(d, body);

  WiFiClientSecure secure;
  HTTPClient http;
  secure.setInsecure();
  if (!http.begin(secure, "https://ollama.com/api/chat")) { why = "bad url"; return -1; }
  http.setConnectTimeout(8000);
  http.setTimeout(30000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + cloudApiKey);
  int code = http.POST((uint8_t*)body.c_str(), body.length());
  String resp = code > 0 ? http.getString() : String("");
  http.end();
  if (code == 200) return 1;
  String err = code <= 0 ? "net error" : "HTTP " + String(code) + ": " + resp.substring(0, 80);
  if (code > 0 && looksBlocked(err)) return 0;
  why = err;
  return -1;
}

static void scanFreeModels(const std::vector<String>& names) {
  int todo = 0;
  for (auto& n : names) if (!listHas(freeOk, n) && !listHas(freeBad, n)) todo++;
  if (todo == 0) { message("Scan", "Nothing new to test."); return; }
  setCpuFrequencyMhz(160);
  int k = 0;
  String stopWhy;
  for (auto& n : names) {
    if (listHas(freeOk, n) || listHas(freeBad, n)) continue;
    k++;
    for (int f = 0; f < 3; f++) busyScreen("Testing " + String(k) + "/" + String(todo), n + "  (hold A: stop)");
    String why;
    checkAlerts();
    int r = probeModel(n, why);
    if (r == 1) listAdd(freeOk, n, "fok");
    else if (r == 0) listAdd(freeBad, n, "fbad");
    else if (why.indexOf("429") >= 0) { stopWhy = "Usage limit hit. Try later."; break; }
    else if (looksAuthError(why)) { stopWhy = "API key rejected.\nCheck Settings > Cloud."; break; }
    M5.update();
    if (M5.BtnA.isPressed()) { stopWhy = "Stopped."; break; }
  }
  setCpuFrequencyMhz(80);
  lastActivity = millis();
  message("Scan done", stopWhy.length() ? stopWhy : "Free models saved.");
}

static void pickModel() {
  if (!ensureWifi()) { message("WiFi failed", "Check Settings."); return; }
  busyScreen("Checking servers", cHost);

  bool up = localUp();
  bool useCloud;
  if (up && cloudReady()) {
    int c = menu("Models from", {"Laptop (local)", "Cloud (free tier)"}, preferCloud ? 1 : 0);
    if (c < 0) return;
    useCloud = (c == 1);
  } else if (up) {
    useCloud = false;
  } else if (cloudReady()) {
    useCloud = true;
  } else {
    message("No models", String("Laptop unreachable") + (useCloudFallback ? "\nCloud: add API key" : "\nCloud fallback is OFF"));
    return;
  }

  std::vector<String> names;
  String err;
  busyScreen("Fetching models", useCloud ? "ollama.com" : cHost);
  setCpuFrequencyMhz(160);
  bool ok = useCloud ? fetchTags("https://ollama.com/api/tags", true, cloudApiKey, names, err)
                     : fetchTags("http://" + cHost + ":" + String(cPort) + "/api/tags", false, "", names, err);
  setCpuFrequencyMhz(80);
  if (!ok) { message("No models", err); return; }

  const String& cur = useCloud ? cCloudModel : cModel;
  bool autoScanned = false;
  while (true) {
    std::vector<String> items, real;
    if (useCloud) { items.push_back("* Scan free models *"); real.push_back(""); }
    for (auto& n : names) {                       // confirmed-working first
      if (useCloud && !listHas(freeOk, n)) continue;
      items.push_back((n == cur ? "> " : "") + n);
      real.push_back(n);
    }
    if (useCloud)
      for (auto& n : names) {                     // untested next; blocked ones are hidden
        if (listHas(freeOk, n) || listHas(freeBad, n)) continue;
        items.push_back("? " + n);
        real.push_back(n);
      }
    int sel = 0;
    for (size_t k = 0; k < real.size(); k++) if (real[k] == cur) { sel = k; break; }
    if (useCloud) {
      int untested = 0;
      bool anyOk = false;
      for (auto& n : names) {
        if (listHas(freeOk, n)) anyOk = true;
        else if (!listHas(freeBad, n)) untested++;
      }
      if (!autoScanned && !anyOk && untested > 0) {   // first visit, nothing confirmed yet: test once
        autoScanned = true;
        scanFreeModels(names);
        continue;
      }
      if (items.size() == 1) {                         // everything blocked (or no models): do NOT scan again
        message("No free models", "Every model is blocked on\nyour plan, or none were\nfound. Try again later.");
        return;
      }
    }
    int i = menu(useCloud ? "Free cloud models" : "Local models", items, sel);
    if (i < 0) return;
    if (useCloud && i == 0) { scanFreeModels(names); continue; }
    String n = real[i];
    n.replace(" ", "");
    if (useCloud) { cCloudModel = n; prefs.putString("cmodel", n); }
    else { cModel = n; prefs.putString("model", n); }
    preferCloud = useCloud;
    prefs.putBool("prefcloud", preferCloud);
    hist.clear();
    return;
  }
}

// ---------- chat ----------
static bool viewer(const String& txt) {
  std::vector<String> lines;
  wrapText(clean(txt), 39, lines);
  const int V = 11;
  int n = lines.size(), top = 0;
  while (true) {
    wakeScreen();
    int maxTop = max(0, n - V);
    top = constrain(top, 0, maxTop);
    cv.fillSprite(BG);
    header("Reply", String(top + 1) + "/" + String(max(1, n)));
    cv.setTextColor(FG, BG);
    cv.setTextSize(1);
    cv.setTextDatum(top_left);
    for (int i = 0; i < V && top + i < n; i++) cv.drawString(lines[top + i], 3, 17 + i * 9);
    footer("B:dn PWR:up A:reply holdA:menu");
    cv.pushSprite(0, 0);
    switch (waitBtn()) {
      case B_CLICK: top += 3; break;
      case P_CLICK: top -= 3; break;
      case B_HOLD:  top += V; break;
      case P_HOLD:  top -= V; break;
      case A_CLICK: return true;
      case A_HOLD:  return false;
      default: break;
    }
  }
}

static void drawStream(const String& txt) {
  std::vector<String> lines;
  wrapText(clean(txt), 39, lines);
  const int V = 11;
  int n = lines.size(), top = max(0, n - V);
  cv.fillSprite(BG);
  header("Replying...");
  cv.setTextColor(FG, BG);
  cv.setTextSize(1);
  cv.setTextDatum(top_left);
  for (int i = 0; i < V && top + i < n; i++) cv.drawString(lines[top + i], 3, 17 + i * 9);
  footer("hold A: stop");
  cv.pushSprite(0, 0);
}

static void chat() {
  bool viaCloud = false;
  if (ensureWifi()) viaCloud = cloudReady() && (preferCloud || !localUp());
  while (true) {
    wakeScreen();
    String p;
    String t = String(viaCloud ? "Cloud: " : "Local: ") + (viaCloud ? cCloudModel : cModel);
    if (!textInput(t.substring(0, 34), p, "SEND", false, true)) return;
    if (p.length() == 0) continue;
    if (!ensureWifi()) { message("WiFi failed", "Check Settings."); return; }

    Job* j = new Job();
    if (!j || !j->mtx) { delete j; message("Error", "Out of memory.\nTry again."); continue; }
    j->body = buildBody(p, cModel);
    j->bodyCloud = buildBody(p, cCloudModel);
    setCpuFrequencyMhz(160);   // TLS handshake is slow at 80 MHz
    if (xTaskCreate(ollamaTask, "ollama", 24576, j, 1, NULL) != pdPASS) {   // TLS needs the bigger stack
      setCpuFrequencyMhz(80);
      delete j;
      message("Error", "Could not start the request\n(not enough free memory).\nTry again.");
      continue;
    }
    while (!j->done) {
      checkAlerts();   // timers/alarms keep working while the AI request runs in its own task
      M5.update();
      if (M5.BtnA.wasHold()) j->cancel = true;
      String cur = j->snapshot();
      if (cur.length() == 0) busyScreen(j->usedCloud ? "Cloud..." : "Thinking...", j->usedCloud ? cCloudModel : cModel);
      else drawStream(cur);
      delay(60);
    }
    setCpuFrequencyMhz(80);
    lastActivity = millis();
    String reply = j->reply, err = j->err;
    if (j->cancel && reply.length() == 0) reply = "(stopped)";
    bool blocked = j->cloudBlocked, authErr = j->cloudAuth, byCloud = j->usedCloud;
    delete j;

    if (blocked) { listAdd(freeBad, cCloudModel, "fbad"); listDel(freeOk, cCloudModel, "fok"); }
    if (err.length()) {
      message("Error", blocked ? "Not in free tier:\n" + cCloudModel + "\nPick another in Models."
                       : authErr ? "Cloud rejected the API key.\nSettings > Cloud > API key.\n\n" + err
                       : err);
      continue;
    }
    viaCloud = byCloud;
    if (byCloud) { listAdd(freeOk, cCloudModel, "fok"); listDel(freeBad, cCloudModel, "fbad"); }
    addPromptHistory(p);
    hist.push_back({"user", p});
    hist.push_back({"assistant", reply});
    while (hist.size() > 8) hist.erase(hist.begin(), hist.begin() + 2);
    if (!viewer(reply)) return;
  }
}

static void about() {
  wakeScreen();
  String s = "WiFi: " + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "off") +
             "\nHost: " + cHost + ":" + String(cPort) +
             "\nModel: " + cModel +
             "\nCloud model: " + cCloudModel +
             "\nCloud: " + String(useCloudFallback ? "ON" : "OFF") +
             "\nBattery: " + String(M5.Power.getBatteryLevel()) + "%";
  message("About", s);
  message("Credits", "StickChat\nMy first ever build!\n\nMade possible with the help of\nClaude by Anthropic.\nThank you!\n\nUI inspired by ESP32 Marauder.");
}

// ---------- boot logo ----------
static void showBootLogo() {
  logoIn(true, LOGO_BOOT_MS);
}

// ---------- sleep ----------
static void checkAutoSleep() {
  if (millis() - lastActivity > SLEEP_TIMEOUT && !displaySleeping) sleepScreen();
}

// ---------- home screen ----------

static void drawHome() {
  static const char* DOW[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char* MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  cv.fillSprite(BG);
  header("StickChat");

  struct tm t;
  bool ok = localTm(&t);
  String hm = "--:--", ap = "";
  if (ok) {
    int h = t.tm_hour;
    if (cH12) { ap = h >= 12 ? "PM" : "AM"; h = h % 12; if (h == 0) h = 12; }
    hm = two(h) + ":" + two(t.tm_min);
  }
  cv.setTextDatum(top_left);
  cv.setTextSize(4);
  cv.setTextColor(FG, BG);
  cv.drawString(hm, 45, 20);
  cv.setTextSize(2);
  cv.setTextColor(ACC, BG);
  if (ok) cv.drawString(two(t.tm_sec), 171, 36);
  cv.setTextSize(1);
  cv.setTextColor(0x8410, BG);
  if (ap.length()) cv.drawString(ap, 171, 22);

  cv.setTextDatum(top_center);
  cv.setTextSize(1.5);
  cv.setTextColor(FG, BG);
  String date = ok ? String(DOW[t.tm_wday]) + " " + two(t.tm_mday) + " " + MON[t.tm_mon] + " " + String(t.tm_year + 1900)
                   : String("Clock not set");
  cv.drawString(date, 120, 60);
  cv.drawFastHLine(10, 82, 220, DIM);

  bool cl = useCloudFallback && cloudApiKey.length() >= 20 && preferCloud;
  String model = String(cl ? "Cloud: " : "Local: ") + (cl ? cCloudModel : cModel);
  String last = pHist.empty() ? String("No prompts yet") : "Last: " + pHist[0];
  cv.setTextDatum(top_left);
  cv.setTextSize(1);
  cv.setTextColor(ACC, BG);
  cv.drawString(model.substring(0, 38), 4, 88);
  cv.setTextColor(0x8410, BG);
  cv.drawString(last.substring(0, 38), 4, 101);
  String chips;
  if (tmActive) { int sc = max(0, (int)((int32_t)(tmEnd - millis()) / 1000)); chips += "T " + two(sc / 60) + ":" + two(sc % 60) + "  "; }
  if (swRun) { uint32_t es = swElapsed() / 1000; chips += "SW " + two(es / 60) + ":" + two(es % 60) + "  "; }
  if (snActive) { int sc = max(0, (int)((int32_t)(snEnd - millis()) / 1000)); chips += "Z " + two(sc / 60) + ":" + two(sc % 60) + "  "; }
  if (alOn) chips += "AL " + two(alH) + ":" + two(alM);
  if (chips.length()) { cv.setTextColor(ACC, BG); cv.drawString(chips, 4, 112); }
  else if (!ok) cv.drawString("Settings > Clock > Sync", 4, 112);
  footer("A:menu   B:chat   PWR:settings");
}

static void batteryCheck() {
  static uint32_t lastChk = 0, lastWarn = 0;
  if (millis() - lastChk < 30000) return;
  lastChk = millis();
  int b = M5.Power.getBatteryLevel();
  if (b < 0 || b > 100) return;
  if (b > 20) { lastWarn = 0; return; }
  if (b > 15) return;
  uint32_t gap = b <= 5 ? 120000 : 300000;
  if (lastWarn != 0 && millis() - lastWarn < gap) return;
  lastWarn = millis();
  M5.Speaker.tone(1500, 150);
  delay(220);
  M5.Speaker.tone(1500, 150);
  message(b <= 5 ? "Battery critical" : "Low battery", String(b) + "% left.\nCharge the stick soon.");
}

// Returns 0 = menu, 1 = chat, 2 = settings
static int home() {
  bool first = true;
  time_t lastT = 0;
  uint32_t lastMs = 0;
  while (true) {
    Btn b = poll();
    if (displaySleeping) {
      if (b != NONE) { wakeScreen(); lastActivity = millis(); first = true; }
      else { maybeDeepSleep(); checkAlerts(); delay(20); }
      continue;
    }
    if (b != NONE) lastActivity = millis();
    if (b == A_CLICK) return 0;
    if (b == B_CLICK) return 1;
    if (b == P_CLICK) return 2;
    ntpTick();
    checkAlerts();
    batteryCheck();
    time_t now = time(nullptr);
    if (first || now != lastT || millis() - lastMs > 2000) {
      drawHome();
      cv.pushSprite(0, 0);
      first = false;
      lastT = now;
      lastMs = millis();
    }
    checkAutoSleep();
    maybeDeepSleep();
    delay(20);
  }
}

// ---------- main ----------
void setup() {
  gpio_deep_sleep_hold_dis();   // release GPIO4 hold if we are waking from deep sleep
  gpio_hold_dis(GPIO_NUM_4);
  auto cfg = M5.config();
  M5.begin(cfg);
  setCpuFrequencyMhz(80);       // idle at 80 MHz; raised to 160 only for HTTPS
  M5.Speaker.setVolume(200);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(110);
  cv.setColorDepth(16);
  cv.createSprite(240, 135);
  M5.BtnA.setHoldThresh(400);
  M5.BtnB.setHoldThresh(400);
  M5.BtnPWR.setHoldThresh(400);

  loadCfg();
  loadRtc();
  showBootLogo();

  M5.Display.fillScreen(BG);
  header("StickChat");
  cv.setTextSize(1);
  cv.setTextColor(FG, BG);
  cv.setTextDatum(top_left);
  cv.drawString("Connecting to WiFi...", 4, 20);
  cv.pushSprite(0, 0);
  delay(500);

  WiFi.mode(WIFI_STA);
  WiFi.begin(cSsid.c_str(), cPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    cv.fillRect(0, 20, 240, 20, BG);
    cv.drawString("Connecting...", 4, 20);
    cv.pushSprite(0, 0);
    M5.update();
    delay(500);
  }

  cv.fillRect(0, 20, 240, 20, BG);
  if (WiFi.status() == WL_CONNECTED) {
    cv.drawString("Connected: " + WiFi.localIP().toString(), 4, 20);
    cv.drawString("Host: " + cHost, 4, 35);
    cv.pushSprite(0, 0);
    delay(1500);
  } else {
    cv.drawString("WiFi failed", 4, 20);
    cv.pushSprite(0, 0);
    delay(1500);
  }

  cv.fillSprite(BG);
  cv.pushSprite(0, 0);
  lastActivity = millis();
}

void loop() {
  int act = home();
  if (act == 1) { chat(); return; }
  if (act == 2) { settings(); return; }

  int i = menu("Menu", {"Chat", "New chat", "Models", "Tools", "Settings", "Sync clock", "About"});
  switch (i) {
    case 0: chat(); break;
    case 1: hist.clear(); chat(); break;
    case 2: pickModel(); break;
    case 3: toolsMenu(); break;
    case 4: settings(); break;
    case 5: message("Clock", syncClockNow() ? "Time synced from the internet." : "Sync failed. Check WiFi and try again."); break;
    case 6: about(); break;
    default: break;
  }
}
