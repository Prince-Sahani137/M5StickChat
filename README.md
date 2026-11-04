# StickChat

**A tiny Wi-Fi AI chat client for the M5StickC Plus2, powered by Ollama.**

StickChat turns the M5StickC Plus2 into a pocket-sized interface for chatting with an Ollama model running on your laptop or with a model available through the Ollama cloud API. It uses the device's 240 × 135 display and three-button controls, with a compact menu-driven interface.

> Built by Prince Sahani. The interface is inspired by the menu style of ESP32 Marauder.

## Features

- **On-device chat interface** — enter prompts using the three-button on-screen keyboard.
- **Local Ollama support** — send chat requests to an Ollama server running on a laptop or another computer on your Wi-Fi network.
- **Ollama cloud support** — optionally connect to `ollama.com` with an API key.
- **Local/cloud fallback** — try the preferred route and fall back to the other route when available.
- **Model browser** — fetch model names from the local or cloud Ollama API and select the model on the device.
- **Cloud model availability scan** — test unclassified cloud models with a tiny one-token request and remember models that appear available or blocked for the current account.
- **Conversation context** — retain a short rolling chat history during the current chat session.
- **Prompt shortcuts and history** — choose common prompt prefixes or reuse recent prompts.
- **Persistent settings** — Wi-Fi, server, model, cloud, and display settings are saved in ESP32 NVS flash.
- **Power controls** — adjustable brightness, display timeout, and optional deep sleep.
- **Compact reply viewer** — wrapped text with button-based scrolling.

## Hardware and software

### Hardware

- M5Stack **M5StickC Plus2**
- USB data cable
- A Wi-Fi network shared by the device and the computer running Ollama (for local mode)

### Build environment

- PlatformIO Core / PlatformIO IDE
- Espressif32 Arduino framework
- Libraries are declared in `platformio.ini` and installed by PlatformIO:
  - `M5Unified`
  - `ArduinoJson` 7

The project environment is `m5stickc-plus2`.

## How it works

1. The M5StickC Plus2 joins the configured Wi-Fi network.
2. For local mode, it sends an Ollama-compatible `POST /api/chat` request to `http://HOST:PORT/api/chat`.
3. For cloud mode, it sends the request to `https://ollama.com/api/chat` with a bearer API key.
4. The response's `message.content` is displayed on the device.

The local Ollama server also provides `/api/tags`, which StickChat uses to list available models. The cloud model list is fetched from `https://ollama.com/api/tags`.

## Getting started

### 1. Clone the repository

```bash
git clone https://github.com/Prince-Sahani137/M5StickChat.git
cd M5StickChat
```

### 2. Install PlatformIO on Linux

One option is to install PlatformIO Core in a Python virtual environment:

```bash
sudo apt update
sudo apt install -y python3 python3-venv python3-pip git
python3 -m venv ~/.venvs/platformio
source ~/.venvs/platformio/bin/activate
pip install -U platformio
```

Check that it is installed:

```bash
pio --version
```

If you open a new terminal later, activate the environment again:

```bash
source ~/.venvs/platformio/bin/activate
```

### 3. Configure the device

Open `src/config.h` and set the defaults:

```cpp
#define DEF_SSID  "YOUR_WIFI_SSID"
#define DEF_PASS  "YOUR_WIFI_PASSWORD"

#define DEF_HOST  "YOUR_HOST_IP"
#define DEF_PORT  11434
#define DEF_MODEL "YOUR_LOCAL_OLLAMA_MODEL"

#define DEF_API_KEY "YOUR_OLLAMA_API_KEY"
#define DEF_CLOUD_MODEL "gpt-oss:20b"
#define DEF_USE_CLOUD 1
```

What each setting means:

| Setting | Purpose |
|---|---|
| `DEF_SSID` | Wi-Fi network name |
| `DEF_PASS` | Wi-Fi password |
| `DEF_HOST` | LAN IP address of the computer running Ollama |
| `DEF_PORT` | Ollama API port; normally `11434` |
| `DEF_MODEL` | Exact local model name shown by `ollama list` |
| `DEF_API_KEY` | Ollama cloud API key; leave as the placeholder if not using cloud |
| `DEF_CLOUD_MODEL` | Cloud model name to use (optional you can choose the model by the models menu too)|
| `DEF_USE_CLOUD` | `1` enables cloud fallback; `0` disables it |

`config.h` provides **first-boot defaults**. After the device has saved a setting to NVS, changing the macro in `config.h` does not necessarily override the saved value. To change settings after first boot, use the device's **Settings** menu, or choose **Reset all settings** to clear saved values and reload the compiled defaults.

#### Configure settings on the device

From the home menu, open **Settings**:

- **WiFi** — edit SSID and password.
- **Laptop server** — edit the Ollama host IP, port, and local model.
- **Cloud (ollama.com)** — enable/disable cloud fallback, set the cloud model, and enter the API key.
- **Display & power** — set brightness, screen-off timeout, and deep-sleep timeout.
- **Reset all settings** — clear saved preferences and reload defaults.

The API key is stored in the device's NVS preferences.

### 4. Set up local Ollama (optional)

Install Ollama on your laptop/server and download at least one model. Then check:

```bash
ollama list
```

By default, Ollama listens on localhost. To allow a device on your trusted local network to reach it, start Ollama listening on the LAN interface:

```bash
OLLAMA_HOST=0.0.0.0:11434 ollama serve
```

If Ollama is already running as a system service, configure the service environment instead of starting a second server. The exact method depends on how Ollama was installed.

Find the laptop's LAN IP address:

```bash
hostname -I
```

Use the relevant LAN address as `DEF_HOST` or set it in **Settings → Laptop server**. Keep the Ollama port restricted to your trusted LAN; do not expose port `11434` directly to the public internet.

Test the endpoint from another device on the same network:

```bash
curl http://YOUR_HOST_IP:11434/api/tags
```

You should receive JSON containing the available models.

### 5. Set up Ollama cloud (optional)

1. Sign in to [Ollama](https://ollama.com/).
2. Create an API key in [Ollama API keys](https://ollama.com/settings/keys).
3. On the StickChat device, open **Settings → Cloud (ollama.com)**.
4. Enter the API key and desired cloud model.
5. Enable **Fallback** if you want cloud to be available when local Ollama cannot be reached.

Cloud model availability and free-tier access can change. The built-in scan is only a point-in-time test using a very small request; it is not a guarantee of future availability or unlimited free usage.

## Build and flash

Connect the M5StickC Plus2 over USB, then from the project root:

```bash
pio run
```

Upload the firmware:

```bash
pio run --target upload
```

The supplied `platformio.ini` currently sets `/dev/ttyACM0` as the upload port. If your device appears on a different port, specify it explicitly:

```bash
pio run --target upload --upload-port /dev/ttyUSB0
```

Find the detected serial device with:

```bash
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

On Linux, if permission is denied, add your user to the serial-access group and log out/in:

```bash
sudo usermod -aG dialout "$USER"
```

Some distributions use a different group or udev rule. Reconnect the board after changing permissions.

To open the serial monitor:

```bash
pio device monitor --baud 115200
```

## Controls

The device is used in landscape orientation.

| Control | Short click | Hold |
|---|---|---|
| **A** (front button) | Select / press the highlighted on-screen key | Back / backspace; in the reply viewer, return to the chat menu |
| **B** (side button) | Move down / move right | Page down / move left |
| **PWR** (side button) | Move up / move to the next keyboard row | Page up / move to the previous keyboard row |

The on-screen keyboard has a **SEND** action. It also provides prompt shortcuts / recent prompts when available.

## Using StickChat

1. Power on the M5StickC Plus2.
2. Wait for it to connect to Wi-Fi.
3. From the **StickChat** menu, select **Chat**.
4. Use the buttons to enter a prompt and select **SEND**.
5. Wait while the selected Ollama endpoint generates a response.
6. Read the reply; use B/PWR to scroll and A to continue the conversation.
7. Select **New chat** from the home menu to clear the current conversation context.

### Model selection

Choose **Models** from the home menu. StickChat checks which endpoints are reachable, fetches their model lists, and lets you choose a local or cloud model when available. Cloud model scanning may make one small API request per unclassified model, so it can take time and may encounter account or rate limits.

## Configuration and data storage

- Compiled defaults live in `src/config.h`.
- Runtime settings are stored using ESP32 `Preferences` (NVS) under the namespace `stickchat`.
- Recent prompt history is persisted in NVS.
- The active conversation history is held in RAM and is cleared when starting a new chat or rebooting.

## Repository layout

```text
StickChat/
├── platformio.ini
├── extra_script.py
├── logo.h
└── src/
    ├── config.h
    └── main.cpp
```


## Troubleshooting

**Wi-Fi does not connect**
- Check the SSID and password in **Settings → WiFi**.
- Confirm the network is 2.4 GHz compatible and reachable by the device.
- Reboot the device after changing network settings if needed.

**Laptop/local model is unreachable**
- Confirm the laptop and M5StickC Plus2 are on the same LAN.
- Confirm Ollama is listening on the LAN interface and port `11434`.
- Check the host IP in **Settings → Laptop server**.
- Check firewall rules on the laptop.

**No cloud models / cloud requests fail**
- Check the API key and cloud fallback setting.
- Verify the selected model is currently available to your Ollama account.
- A cloud API rate limit or plan restriction can prevent a model from working.

**Upload permission denied**
- Check the port with `ls /dev/ttyACM* /dev/ttyUSB*`.
- Check your serial permissions and `dialout` group membership.

**Settings do not change after editing `config.h`**
- Runtime values already saved in NVS take precedence. Use the Settings menu or reset all settings to reload compiled defaults.

## Security notes and known limitations

- The local Ollama connection uses plain HTTP and has no authentication in this firmware. Use it only on a trusted LAN and firewall the port.
- The current HTTPS implementation calls `setInsecure()`, which disables TLS certificate verification. HTTPS traffic is encrypted, but the firmware does not authenticate the server certificate. This should be improved before relying on it for sensitive conversations.
- The chat system prompt asks for concise replies (under 80 words) suitable for the small display.
- This firmware targets the M5StickC Plus2 configuration in `platformio.ini`; other M5Stick models are not guaranteed to work without changes.

## License

This project is licensed under the MIT License.
See the [LICENSE](LICENSE) file for details.
