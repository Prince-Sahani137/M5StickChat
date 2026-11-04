#pragma once

// Defaults only. Everything here can be edited on the stick
// (Settings menu) and is then stored in flash (NVS), overriding these values.

// WiFi credentials
#define DEF_SSID  "YOUR_WIFI_SSID"
#define DEF_PASS  "YOUR_WIFI_PASSWORD"

// Ollama local server (laptop on same WiFi)
#define DEF_HOST  "YOUR_HOST_IP"
#define DEF_PORT  11434 //default
#define DEF_MODEL "YOUR_MODEL"   // model name as your LOCAL Ollama knows it

// Cloud fallback (Ollama API key for ollama.com)
// Get your key at: https://ollama.com/settings/keys
#define DEF_API_KEY "YOUR_OLLAMA_API_KEY"
#define DEF_CLOUD_MODEL "YOUR_MODEL"   // name on ollama.com (colon, not dash)
#define DEF_USE_CLOUD 1  // 0=off, 1=on (cloud fallback enabled by default)

#define SYSTEM_PROMPT "You reply on a tiny 240x135 screen. Be concise: under 80 words, plain text, no markdown."
