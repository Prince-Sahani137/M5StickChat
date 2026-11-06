# StickChat: security notes and test checklist

## 1. HTTPS certificate verification (one-time setup)
The firmware pins the root CA that signs ollama.com and refuses anything else.

    python3 tools/make_ca_header.py        # needs openssl + a normal system CA store
    pio run -t upload

This writes `include/ollama_ca.h`. Without that file the build stops with an error
(add `-DALLOW_INSECURE_TLS` to build_flags to skip checks, not recommended).
If ollama.com ever switches certificate authority, HTTPS fails with a TLS error until you
re-run the script. Settings > Cloud > TLS check can turn verification OFF as an emergency fallback.
Certificate dates are checked, so the clock must be set: the stick syncs it automatically
before HTTPS when it is not (needs WiFi).

## 2. API key: what is and is not protected
- The key is stored in the ESP32's NVS flash area in **plain text**, and also inside the firmware
  image if you put it in `DEF_API_KEY` (config.h). Anyone with the stick in hand and a USB cable can
  read it with `esptool read_flash`.
- The firmware never prints the key (no Serial output; CORE_DEBUG_LEVEL=0 must stay 0) and the
  Settings page only shows the last 4 characters.
- Do: use a dedicated key just for this stick; revoke it at ollama.com/settings/keys if the stick is
  lost; leave `DEF_API_KEY` empty and type the key in Settings; never commit config.h, ollama_ca.h
  is fine to commit (public certificate).
- Flash encryption (ESP-IDF eFuse feature) would protect the key at rest, but it is a one-way,
  hard-to-undo change that needs a custom build/flash flow. It is NOT enabled here and I have not
  tested it on the Plus2: read Espressif's flash-encryption guide first, and try it on a spare
  board, not your main one.

## 3. Deep sleep and alarms
- Alarms, timers, snooze and the stopwatch survive deep sleep: their wall-clock times are kept in
  RTC memory and the chip wakes itself about 12 s before the next alarm/timer.
- Deep sleep is skipped when an event is less than 40 s away or the clock is not set.
- Laps are not kept across deep sleep (running stopwatch time is).
Tests to run: (a) alarm 3 min ahead, deep sleep = 2 min, wait: it should wake and ring on time;
(b) 5 min timer, same; (c) start stopwatch, let it deep-sleep, press A: time should have kept running;
(d) snooze an alarm, let it sleep, check it rings again after 5 min.

## 4. Memory checks
About shows free heap, minimum free heap since boot and the biggest free block.
Test: send ~20 prompts in a row (mix of short/long replies, a few stopped with hold A, a few cloud
errors). Free heap should return to roughly the same value after each reply and "Heap min" should not
keep falling. If the biggest block drops under ~60 KB the stick clears the conversation, under ~45 KB
it asks you to restart.
