# StickChat

**A pocket-sized AI chat client and productivity companion for the M5StickC Plus2, powered by Ollama.**

StickChat turns the M5StickC Plus2 into a tiny Wi-Fi-connected interface for chatting with models running on your laptop or through the Ollama Cloud API. It combines a compact, three-button chat interface with a clock, timer, stopwatch, and daily alarm.

> Built by Prince Sahani. The interface is inspired by the menu style of ESP32 Marauder.

## Features

### AI chat

- **On-device chat interface** — enter prompts using the three-button on-screen keyboard.
- **Local Ollama support** — connect to an Ollama server running on a laptop or another computer on your Wi-Fi network.
- **Ollama Cloud support** — optionally connect to `ollama.com` using an API key.
- **Local/cloud fallback** — use the preferred endpoint and fall back to the other configured endpoint when available.
- **Streaming responses** — display generated reply text as it arrives instead of waiting for the complete response.
- **Model browser** — fetch available model names from the local or cloud Ollama API and select a model on the device.
- **Cloud model availability scan** — test unclassified cloud models with a small request and cache the results for later selection.
- **Conversation context** — keep a short rolling conversation history during the current chat session.
- **Prompt shortcuts and history** — use prompt shortcuts or reuse recent prompts.
- **Reply length options** — choose short, normal, or detailed responses.
- **Language options** — choose automatic language, English, or Hinglish (Hindi written in English letters).
- **Cancel generation** — stop an in-progress AI response from the device.

### Clock and time

- **Digital home screen clock** — show the current time and date.
- **NTP time synchronization** — synchronize the clock over the internet.
- **Timezone setting** — configure the local timezone.
- **12/24-hour format** — choose how the time is displayed.
- **RTC persistence** — retain clock time across supported sleep and wake cycles.

### Timer, stopwatch, and alarm

- **Countdown timer** — use presets or set a custom duration from 1 to 600 minutes.
- **Timer adjustment** — add one minute while a timer is running.
- **Stopwatch** — start, pause, and reset elapsed time.
- **Lap recording** — record and review stopwatch laps (up to 20).
- **Daily alarm** — set an alarm time with an audible alert.
- **Snooze** — snooze an alarm for five minutes.

### Device interface and power

- **Compact reply viewer** — wrapped text with button-based scrolling.
- **Animated boot logo** — show the StickChat logo during startup.
- **Sleep/wake animations** — visual transitions for display sleep and wake.
- **Display controls** — adjust brightness and screen timeout.
- **Deep-sleep option** — configure the device's power-saving behavior.
- **Battery status** — show battery level and low-battery warnings.
- **Persistent settings** — save device settings using ESP32 Preferences/NVS flash.

## Hardware and software

### Hardware

- M5Stack **M5StickC Plus2**
- USB data cable
- A Wi-Fi network
- A laptop or computer running Ollama for local mode, or an Ollama Cloud API key for cloud mode

### Build environment

- PlatformIO Core / PlatformIO IDE
- Espressif32 Arduino framework
- Libraries declared in `platformio.ini`, including:
  - `M5Unified`
  - `ArduinoJson` 7

The project environment is `m5stickc-plus2`.

## How it works

1. The M5StickC Plus2 joins the configured Wi-Fi network.
2. For local mode, it sends an Ollama-compatible `POST /api/chat` request to `http://HOST:PORT/api/chat`.
3. For cloud mode, it sends the request to `https://ollama.com/api/chat` with a bearer API key.
4. The response is streamed to the device and displayed in the reply viewer.

The local Ollama server provides `/api/tags`, which StickChat uses to list available models. The cloud model list is fetched from `https://ollama.com/api/tags`.

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

Open `src/config.h` and set the first-boot defaults:

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
| `DEF_API_KEY` | Ollama Cloud API key; use a placeholder if you are not using cloud |
| `DEF_CLOUD_MODEL` | Default cloud model name |
| `DEF_USE_CLOUD` | `1` enables cloud fallback; `0` disables it |

`config.h` provides **first-boot defaults**. After the device has saved a setting to NVS, changing the macro in `config.h` does not necessarily override the saved value. To change settings after first boot, use the device's **Settings** menu, or choose **Reset all settings** to clear saved values and reload the compiled defaults.

#### Configure settings on the device

Use the device's Settings menu to configure Wi-Fi, the laptop server, cloud access, display, and power options. The device also provides clock/time and chat preference options, including response length and language.

The cloud API key is stored in the device's NVS preferences. See the security notes below.

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

Use the relevant LAN address in `DEF_HOST` or set it in the device's laptop-server settings. Keep the Ollama port restricted to your trusted LAN; do not expose port `11434` directly to the public internet.

Test the endpoint from another device on the same network:

```bash
curl http://YOUR_HOST_IP:11434/api/tags
```

You should receive JSON containing the available models.

### 5. Set up Ollama Cloud (optional)

1. Sign in to [Ollama](https://ollama.com/).
2. Create an API key in [Ollama API keys](https://ollama.com/settings/keys).
3. On StickChat, open the cloud settings.
4. Enter the API key and desired cloud model.
5. Enable cloud fallback if you want cloud to be available when local Ollama cannot be reached.

Cloud model availability and free-tier access can change. The built-in scan is a point-in-time test using a small request; it is not a guarantee of future availability or unlimited free usage. Scanning can take time and may encounter account or rate limits.

## Build and flash

Connect the M5StickC Plus2 over USB, then from the project root:

```bash
pio run
```

Upload the firmware:

```bash
pio run --target upload
```

If your device appears on a different port, specify it explicitly:

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
| **A** (front button) | Select / confirm / press the highlighted on-screen key | Back / backspace; screen-specific action |
| **B** (side button) | Move down / move right | Page down / move left |
| **PWR** (side button) | Move up / move to the next keyboard row | Page up / move to the previous keyboard row |

On the home screen:

- **A** opens the main menu.
- **B** opens chat.
- **PWR** opens settings.

The on-screen keyboard includes a **SEND** action. Prompt shortcuts and recent prompts are available when configured.

## Using StickChat

1. Power on the M5StickC Plus2.
2. Wait for it to connect to Wi-Fi.
3. Open **Chat** from the home screen or main menu.
4. Use the buttons to enter a prompt and select **SEND**.
5. Watch the response stream onto the display.
6. Read the reply; use the navigation buttons to scroll and continue the conversation.
7. Choose **New chat** to clear the current conversation context.

### Model selection

Choose **Models** from the main menu. StickChat checks which endpoints are reachable, fetches their model lists, and lets you choose a local or cloud model when available.

For cloud models, the scanner tests models that have not yet been classified and stores the results. A model's availability can change, and a successful scan does not guarantee future access.

### Clock and productivity tools

Open **Tools** from the main menu to access the clock-related tools:

- **Clock** — view or configure time-related options and synchronize time.
- **Timer** — start a preset or custom countdown.
- **Stopwatch** — measure elapsed time and record laps.
- **Alarm** — configure a daily alarm and snooze an alert.

The exact options shown depend on the current firmware configuration.

## Configuration and data storage

- Compiled first-boot defaults live in `src/config.h`.
- Runtime settings are stored using ESP32 `Preferences` (NVS).
- Recent prompt history is persisted in NVS.
- The active conversation history is held in RAM and is cleared when starting a new chat or rebooting.
- Clock/time state uses the device's available timekeeping and persistence facilities.

## Repository layout

```text
M5StickChat/
├── platformio.ini
├── extra_script.py
├── logo.h
├── README.md
├── LICENSE
└── src/
    ├── config.h
    └── main.cpp
```

## Troubleshooting

**Wi-Fi does not connect**
- Check the SSID and password in the device's Wi-Fi settings.
- Confirm the network is 2.4 GHz compatible and reachable by the device.
- Reboot the device after changing network settings if needed.

**Laptop/local model is unreachable**
- Confirm the laptop and M5StickC Plus2 are on the same LAN.
- Confirm Ollama is listening on the LAN interface and port `11434`.
- Check the host IP in the laptop-server settings.
- Check firewall rules on the laptop.

**No cloud models / cloud requests fail**
- Check the API key and cloud fallback setting.
- Verify the selected model is currently available to your Ollama account.
- A cloud API rate limit or plan restriction can prevent a model from working.

**Clock does not synchronize**
- Check Wi-Fi connectivity and confirm the device can reach the internet.
- Try the manual clock-sync option again.

**Upload permission denied**
- Check the port with `ls /dev/ttyACM* /dev/ttyUSB*`.
- Check your serial permissions and serial-access group membership.

**Settings do not change after editing `config.h`**
- Runtime values already saved in NVS take precedence. Use the Settings menu or reset all settings to reload compiled defaults.

## Security notes and known limitations

- The local Ollama connection uses plain HTTP and has no authentication in this firmware. Use it only on a trusted LAN and firewall the port.
- The current HTTPS implementation calls `setInsecure()`, which disables TLS certificate verification. HTTPS traffic is encrypted, but the firmware does not authenticate the server certificate. This should be improved before relying on it for sensitive conversations.
- The cloud API key is stored in ESP32 NVS preferences; this firmware does not provide encrypted secret storage.
- Cloud model scanning sends small test requests and may consume account usage or encounter rate limits.
- Deep-sleep behavior may be limited while time-based features such as alarms are enabled.
- This firmware targets the M5StickC Plus2 configuration in `platformio.ini`; other M5Stick models are not guaranteed to work without changes.

## License

This project is licensed under the MIT License.
See the [LICENSE](LICENSE) file for details.
