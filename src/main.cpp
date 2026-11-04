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
static int cBright = 110, cDeepMin = 5;   // screen brightness, deep-sleep delay in minutes (0 = off)
static std::vector<String> pHist;           // recent prompts, newest first
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

// Deep sleep: ~10 uA-class state. Wake = button A, then a normal reboot.
static void goDeepSleep() {
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
  if (cDeepMin > 0 && millis() - lastActivity > (uint32_t)cDeepMin * 60000UL) goDeepSleep();
}

// Header status text: link mode + battery
static String statusStr() {
  String s = WiFi.status() == WL_CONNECTED
  ? ((useCloudFallback && cloudApiKey.length() >= 20 && preferCloud) ? "CLD" : "LAN")
  : "off";
  int b = M5.Power.getBatteryLevel();
  if (b >= 0) s += " " + String(b) + "%";
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
    // Check for sleep timeout during wait
    if (millis() - startTime > SLEEP_TIMEOUT && !displaySleeping) {
      displaySleeping = true;
      M5.Display.sleep();
      WiFi.mode(WIFI_OFF);

      while (true) {
        M5.update();
        if (M5.BtnA.wasClicked() || M5.BtnB.wasClicked() || M5.BtnPWR.wasClicked()) {
          break;
        }
        maybeDeepSleep();
        delay(10);
      }

      M5.Display.wakeup();
      WiFi.mode(WIFI_STA);
      ensureWifi();
      displaySleeping = false;
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
  displaySleeping = false;
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
  displaySleeping = false;
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
    if (displaySleeping) {
      M5.Display.wakeup();
      displaySleeping = false;
    }
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
    if (displaySleeping) {
      M5.Display.wakeup();
      displaySleeping = false;
    }
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

// ---------- settings (category pages -> option pages) ----------
// Option page: shows all choices, current one marked with ">", returns chosen index or -1.
static int pickOption(const String& title, const std::vector<String>& opts, int cur) {
  std::vector<String> items;
  for (int k = 0; k < (int)opts.size(); k++) items.push_back((k == cur ? "> " : "  ") + opts[k]);
  return menu(title, items, cur < 0 ? 0 : cur);
}

static void wake() { if (displaySleeping) { M5.Display.wakeup(); displaySleeping = false; } }

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
    sel = menu("Settings", {"WiFi", "Laptop server", "Cloud (ollama.com)", "Display & power", "Reset all settings", "Back"}, sel);
    if (sel < 0 || sel == 5) return;
    switch (sel) {
      case 0: settingsWifi(); break;
      case 1: settingsServer(); break;
      case 2: settingsCloud(); break;
      case 3: settingsDisplay(); break;
      case 4: {
        int r = pickOption("Reset all settings?", {"No, keep them", "Yes, reset to defaults"}, 0);
        if (r == 1) { prefs.clear(); loadCfg(); M5.Display.setBrightness(cBright); message("Reset", "Done"); }
        break;
      }
    }
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
    delay(40);
  }
  return WiFi.status() == WL_CONNECTED;
}

struct Job {
  String body, bodyCloud, reply, err;
  volatile bool usedCloud = false;
  volatile bool cloudBlocked = false;
  volatile bool done = false;
};

// One chat POST. Returns true and fills `reply` on success, else fills `err`.
static bool postChat(const String& url, const String& body, bool tls, const String& key,
                     uint16_t timeoutMs, String& reply, String& err) {
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
  String resp = http.getString();
  http.end();
  if (code != 200) { err = "HTTP " + String(code) + ": " + resp.substring(0, 80); return false; }
  JsonDocument d;
  if (deserializeJson(d, resp)) { err = "bad JSON"; return false; }
  reply = d["message"]["content"].as<String>();
  if (reply.length() == 0) reply = "(empty reply)";
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

                     static bool looksBlocked(const String& err) {
                       String low = err;
                       low.toLowerCase();
                       return low.indexOf("free tier") >= 0 || low.indexOf("subscription") >= 0 || low.indexOf("upgrade") >= 0 ||
                       err.startsWith("HTTP 402") || err.startsWith("HTTP 403");
                     }

                     static void doRequest(Job* j) {
                       String localErr, cloudErr;
                       bool cloudFirst = cloudReady() && preferCloud;
                       for (int pass = 0; pass < 2; pass++) {
                         bool doCloud = (pass == 0) ? cloudFirst : !cloudFirst;
                         if (doCloud) {
                           if (!cloudReady()) { cloudErr = useCloudFallback ? "no API key" : "cloud is OFF"; continue; }
                           j->usedCloud = true;
                           if (postChat("https://ollama.com/api/chat", j->bodyCloud, true, cloudApiKey, 60000, j->reply, cloudErr)) return;
                             if (looksBlocked(cloudErr)) j->cloudBlocked = true;
                         } else {
                           j->usedCloud = false;
                           if (!localUp()) { localErr = "laptop unreachable"; continue; }
                           String url = "http://" + cHost + ":" + String(cPort) + "/api/chat";
                           if (postChat(url, j->body, false, "", 30000, j->reply, localErr)) return;
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

                     static String buildBody(const String& prompt, const String& model) {
                       JsonDocument d;
                       d["model"] = model;
                       d["stream"] = false;
                       JsonArray m = d["messages"].to<JsonArray>();
                       JsonObject s = m.add<JsonObject>();
                       s["role"] = "system";
                       s["content"] = SYSTEM_PROMPT;
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
                         int r = probeModel(n, why);
                         if (r == 1) listAdd(freeOk, n, "fok");
                         else if (r == 0) listAdd(freeBad, n, "fbad");
                         else if (why.indexOf("429") >= 0) { stopWhy = "Usage limit hit. Try later."; break; }
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
                         if (useCloud && items.size() == 1) { scanFreeModels(names); continue; }  // first run: scan automatically
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
                         if (displaySleeping) {
                           M5.Display.wakeup();
                           displaySleeping = false;
                         }
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

                     static void chat() {
                       bool viaCloud = false;
                       if (ensureWifi()) viaCloud = cloudReady() && (preferCloud || !localUp());
                       while (true) {
                         if (displaySleeping) { M5.Display.wakeup(); displaySleeping = false; }
                         String p;
                         String t = String(viaCloud ? "Cloud: " : "Local: ") + (viaCloud ? cCloudModel : cModel);
                         if (!textInput(t.substring(0, 34), p, "SEND", false, true)) return;
                         if (p.length() == 0) continue;
                         if (!ensureWifi()) { message("WiFi failed", "Check Settings."); return; }

                         Job* j = new Job();
                         j->body = buildBody(p, cModel);
                         j->bodyCloud = buildBody(p, cCloudModel);
                         setCpuFrequencyMhz(160);   // TLS handshake is slow at 80 MHz
                         xTaskCreate(ollamaTask, "ollama", 24576, j, 1, NULL);  // TLS needs the bigger stack
                         while (!j->done) busyScreen(j->usedCloud ? "Cloud..." : "Thinking...", j->usedCloud ? cCloudModel : cModel);
                         setCpuFrequencyMhz(80);
                         lastActivity = millis();
                         String reply = j->reply, err = j->err;
                         bool blocked = j->cloudBlocked, byCloud = j->usedCloud;
                         delete j;

                         if (blocked) { listAdd(freeBad, cCloudModel, "fbad"); listDel(freeOk, cCloudModel, "fok"); }
                         if (err.length()) {
                           message("Error", blocked ? "Not in free tier:\n" + cCloudModel + "\nPick another in Models." : err);
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
                       if (displaySleeping) { M5.Display.wakeup(); displaySleeping = false; }
                       String s = "WiFi: " + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "off") +
                       "\nHost: " + cHost + ":" + String(cPort) +
                       "\nModel: " + cModel +
                       "\nCloud model: " + cCloudModel +
                       "\nCloud: " + String(useCloudFallback ? "ON" : "OFF") +
                       "\nBattery: " + String(M5.Power.getBatteryLevel()) + "%";
                       message("About", s);
                       message("Credits", "Prince sahani\nMy first ever build!\n\nUI inspired by ESP32 Marauder.");
                     }

                     // ---------- boot logo ----------
                     static void showBootLogo() {
                       M5.Display.fillScreen(ACC);
                       M5.Display.setBrightness(110);
                       cv.fillSprite(ACC);
                       cv.setTextColor(BG, ACC);
                       cv.setTextSize(2);
                       cv.setTextDatum(middle_center);
                       cv.drawString("StickChat", 120, 50);
                       cv.setTextSize(1);
                       cv.drawString("Loading...", 120, 85);
                       cv.pushSprite(0, 0);

                       delay(1500);

                       M5.Display.setBrightness(100);
                       delay(200);
                       M5.Display.setBrightness(110);
                     }

                     // ---------- sleep ----------
                     static void checkAutoSleep() {
                       if (millis() - lastActivity > SLEEP_TIMEOUT && !displaySleeping) {
                         displaySleeping = true;
                         M5.Display.sleep();
                         WiFi.mode(WIFI_OFF);
                       }
                     }

                     // ---------- main ----------
                     void setup() {
                       gpio_deep_sleep_hold_dis();   // release GPIO4 hold if we are waking from deep sleep
                       gpio_hold_dis(GPIO_NUM_4);
                       auto cfg = M5.config();
                       M5.begin(cfg);
                       setCpuFrequencyMhz(80);       // idle at 80 MHz; raised to 160 only for HTTPS
                       M5.Display.setRotation(1);
                       M5.Display.setBrightness(110);
                       cv.setColorDepth(16);
                       cv.createSprite(240, 135);
                       M5.BtnA.setHoldThresh(400);
                       M5.BtnB.setHoldThresh(400);
                       M5.BtnPWR.setHoldThresh(400);

                       showBootLogo();
                       loadCfg();
                       M5.Display.setBrightness(cBright);

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
                       checkAutoSleep();

                       int i = menu("StickChat", {"Chat", "New chat", "Models", "Settings", "About"});
                       switch (i) {
                         case 0: chat(); break;
                         case 1: hist.clear(); chat(); break;
                         case 2: pickModel(); break;
                         case 3: settings(); break;
                         case 4: about(); break;
                         default: break;
                       }
                     }
