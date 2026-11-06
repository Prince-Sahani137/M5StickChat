# Release notes

## v1.1.0 — Security and reliability update (2026-11-06)

### Security
- **Verified HTTPS.** All HTTPS requests (chat, model list, model scan) now check ollama.com's certificate against a pinned root CA instead of calling `setInsecure()`. New helper `tools/make_ca_header.py` generates `include/ollama_ca.h`; the build stops with a clear message until it exists.
- **Clock for TLS.** Certificate dates need a valid clock, so the stick syncs time automatically before HTTPS when needed.
- **Settings → Cloud → TLS check** (ON/OFF) as an emergency fallback.
- **API-key handling.** No serial output of secrets, masked display, and a new `SECURITY_AND_TESTING.md` documenting plain-text NVS storage, physical-access risk, key-rotation advice and why flash encryption was not enabled.
- **Better error classification.** A bare HTTP 403 is no longer treated as "not in the free tier"; 401 / permission errors now show **"Cloud rejected the API key"**.

### Reliability
- **NTP retries** every 20 s while the clock is unset (2 min if the RTC already has a time), using three time servers.
- **Deep sleep + alarms.** Alarms, timers, snooze and the running stopwatch now survive deep sleep: wall-clock times are stored in RTC memory and the chip wakes itself ~12 s early. Deep sleep is skipped if an event is under 40 s away or the clock is unset.
- **Settings validation.** Saved style, language, brightness, deep-sleep delay, screen-off time, alarm time, port and timezone are range-checked at boot and reset to safe defaults when invalid.
- **Safer model scanning.** Max 8 models per scan, pauses between probes, HTTP 429 backoff (10 min), temporary failures skipped for 30 min, no endless re-scan when every model is blocked.
- **Task and thread safety.** Request task creation is checked; flags shared with the request task are `std::atomic`; the reply text is behind a mutex.
- **Independent alerts.** Timers and alarms ring even while a reply streams, Wi-Fi connects or models are scanned.
- **Alarm snooze** has its own countdown and no longer overwrites a running timer.
- **Stopwatch** no longer keeps the screen on; it keeps counting while the screen sleeps.

### Memory
- Conversation history is capped by size (8 messages / ~3000 characters); stored replies are trimmed.
- Streaming redraw wraps only the visible tail; fewer `String` allocations.
- Free-heap check before every cloud request; heap statistics on the About screen.

### UI
- Boot, sleep and wake logo now hold for 2 seconds (constants at the top of `main.cpp`). The logo is drawn entirely in code.
- Battery icon with percentage and a red low state; low-battery warnings.
- Version label in the logo updated to v1.1.

### Upgrade notes
1. Copy the new `src/main.cpp`, then add `tools/`, `SECURITY_AND_TESTING.md` and this README.
2. Run `python3 tools/make_ca_header.py` (once) and rebuild.
3. Delete `logo.h`: it is a leftover bitmap that the firmware no longer uses (and it was third-party game artwork, so it should not be in the repository).
4. Run **Settings → Reset all settings** once to clear old saved values (for example misspelled model names such as `gpt-oss-20b`; the correct name is `gpt-oss:20b`).

### Known limitations
- Deep-sleep wake-ups for alarms/timers are new and should be tested on your device (steps in `SECURITY_AND_TESTING.md`).
- Stopwatch laps are not kept across deep sleep.
- The API key is stored unencrypted in flash.
- Local Ollama mode is unauthenticated HTTP.
- Voice/dictation is not included.

## v1.0.0 — First release

### AI chat
- Three-button on-screen keyboard (lowercase / uppercase / symbols) with SEND, DEL, SPACE.
- Local Ollama and Ollama Cloud, with automatic fallback and a **Models** browser.
- **Streaming replies** with hold-A to stop; scrollable reply viewer.
- Free-tier cloud model scan with cached results.
- **Reply style** (short / normal / detailed) and **language** (auto / English / Hinglish).
- **Prompt LIST key**: last 10 prompts and 8 presets.

### Home screen and tools
- Home screen clock/date with status chips; NTP sync, RTC persistence, timezone, 12/24-hour.
- **Tools:** timer (presets or custom up to 600 min, +1 min), stopwatch with laps, daily alarm with 5-minute snooze.

### Look and power
- Marauder-style menus and a code-drawn animated logo for boot / sleep / wake.
- Screen-off sleep, deep sleep, brightness control, battery icon and low-battery warning.
- Category Settings pages with option pickers; settings stored in NVS.
