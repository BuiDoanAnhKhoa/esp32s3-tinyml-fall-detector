# 🚨 Edge TinyML Fall Detector on ESP32-S3

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0+-blue.svg?logo=espressif)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/)
[![Target](https://img.shields.io/badge/Hardware-ESP32--S3%20N16R8-red.svg)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Framework](https://img.shields.io/badge/AI-TensorFlow%20Lite%20Micro-orange.svg?logo=tensorflow)](https://www.tensorflow.org/lite/microcontrollers)
[![Protocol](https://img.shields.io/badge/Protocol-MQTT%20v3.1.1-brightgreen.svg?logo=eclipse-mosquitto)](https://mosquitto.org/)
[![Platform](https://img.shields.io/badge/OS-Linux%20%7C%20macOS-lightgrey.svg)]()
[![Standard](https://img.shields.io/badge/Standard-C%2B%2B17%20%2F%20C11-blue.svg)]()

A real-time edge fall detection system deployed on the **ESP32-S3** dual-core microcontroller. The system samples a 6-axis **MPU-6050** IMU at 100 Hz, runs a fully quantized **INT8 1D-CNN** (accelerometer-only, 3 features) using **TensorFlow Lite Micro and ESP-NN**, signals local status via an **addressable WS2812 RGB LED**, and streams detection telemetry over **WiFi via MQTT**. The tensor arena uses PSRAM by default, with internal RAM selectable for benchmarking. A hardware **sleep/wake toggle button** allows pausing and resuming sensor capture on demand.

---

## 📑 Table of Contents
- [System Architecture](#-system-architecture)
- [Key Features](#-key-features)
- [Hardware \& Pinout](#-hardware--pinout)
- [Directory Structure](#-directory-structure)
- [Prerequisites \& Installation](#-prerequisites--installation)
  - [Linux (Ubuntu/Debian)](#linux-ubuntudebian)
  - [macOS (Homebrew)](#macos-homebrew)
- [Mosquitto MQTT Broker Setup](#-mosquitto-mqtt-broker-setup)
- [Configuration \& Flashing](#-configuration--flashing)
- [Live Telemetry Stream](#-live-telemetry-stream)
- [Sleep / Wake Button](#-sleep--wake-button)
- [Capture MPU Samples on Your Computer](#capture-mpu-samples-on-your-computer)
- [RGB Indicator Reference](#-rgb-indicator-reference)
- [Native Host Tests (CI/PC)](#-native-host-tests-cipc)
- [INT8 Model and Benchmarking](docs/model.md)

---

## 🏗 System Architecture

```text
               +-------------------------------------------------------------+
               |                       ESP32-S3 (Dual Core)                  |
               |                                                             |
               |  [ Core 0 ]                                  [ Core 1 ]     |
               |  +---------------------+                     +------------+ |
MPU-6050 ------>  |  100 Hz Sensor Task |                     |            | |
 (I2C 400kHz)  |  |  Acquisition & Jitter|--- Sample Queue -->|  Edge AI   | |
               |  |  Validation Checks  |   (256 samples)     |  TFLite    | |
               |  +---------------------+                     |  Micro     | |
               |                                              |  1D-CNN    | |
               |  +---------------------+                     |   INT8     | |
WS2812 RGB <----- |  RGB Indicator Task |<-- State Queue -----+            | |
  (Onboard)    |  +---------------------+                     +------+-----+ |
               |                                                     |       |
               |  +---------------------+                            |       |
               |  |  WiFi STA & MQTT    |<------ Result / Error -----+       |
               |  |  (Non-blocking)     |                                    |
               |  +----------+----------+                                    |
               |             |                                               |
               |  +---------------------+                                    |
Button -------->  | Sleep/Wake Toggle   |--- LED + MQTT status updates       |
  (GPIO 0)     |  | (Debounced ISR)     |                                    |
               |  +---------------------+                                    |
               +-------------|-----------------------------------------------+
                             | WiFi (TCP Port 1883)
                             v
               +-----------------------------+
               |  PC: Mosquitto MQTT Broker  |
               +--------------+--------------+
                              |
                              v
               +-----------------------------+
               |  Terminal: mosquitto_sub    |
               |  (Live JSON Telemetry)      |
               +-----------------------------+
```

---

## ✨ Key Features

- **Asynchronous Dual-Core Pipeline:**
  - **Core 0:** Handles high-priority 100 Hz I2C sampling (`vTaskDelayUntil`) and addressable RGB updates.
  - **Core 1:** Dedicated to preprocessing (standardization, feature scaling, 200-sample sliding window) and neural network inference.
- **Accelerometer-Only 1D-CNN Model:**
  - Uses only 3 input features (AccX, AccY, AccZ) — no gyroscope required for inference.
  - Input shape `[1,600]`: 200 samples × 3 axes, stored as 600 INT8 bytes. The preprocessing ring buffer retains 600 standardized floats.
  - Input is standardized and quantized; the INT8 output is dequantized to a probability before applying the exported threshold (approximately 0.20).
  - All 6 axes are still captured and available for serial streaming.
- **Robust Sensor Data Hygiene:**
  - Strict interval validation ($5000\,\mu\text{s} \le \Delta t \le 15000\,\mu\text{s}$).
  - Automatic detection of dropped I2C reads and sequence gaps.
  - Rejection of invalid acceleration data (NaN/Inf) with automatic window reset. Gyroscope values are unused by the model.
- **Optimized Memory Allocation:**
  - Reserves a 256 KiB tensor arena in Octal PSRAM at 80 MHz, keeping internal SRAM free for network buffers and FreeRTOS queues.
  - Configurable arena size and placement, with startup reporting actual usage. ESP-NN optimized kernels and 240 MHz CPU operation are enabled in project defaults.
- **Resilient Network & MQTT Client:**
  - WiFi STA mode with automatic event-driven reconnect.
  - **Offline Mode:** If you are running on batteries or do not need network connectivity, you can disable the WiFi/MQTT stack entirely to save power. Uncheck `Enable WiFi and MQTT` under `WiFi and MQTT Configuration` in `idf.py menuconfig`.
  - Non-blocking MQTT publishing: telemetry is streamed if connected, but network downtime never stalls local fall detection or LED alerts.
  - Configured with MQTT **Last Will and Testament (LWT)** to detect device disconnection instantly.
  - **5-second cooldown** after a FALL detection to avoid redundant MQTT messages.
- **Sleep / Wake Toggle Button:**
  - A single button press puts the device into low-power sleep mode: the MPU6050 enters hardware sleep, inference stops, and the LED shows dim cyan.
  - A second press wakes everything back up with a clean warmup cycle.
  - MQTT status messages indicate the current mode (`active` or `sleep`).
- **Addressable RGB Visualizer:**
  - Real-time WS2812 LED feedback driven by hardware RMT peripheral.

---

## 🔌 Hardware & Pinout

### Recommended Hardware
- **MCU:** ESP32-S3-DevKitC-1 (N16R8: 16 MB Flash, 8 MB Octal PSRAM)
- **Sensor:** MPU-6050 6-Axis Accelerometer & Gyroscope module
- **Indicator:** Onboard WS2812 Addressable RGB LED
- **Button:** Momentary push button (or use the onboard BOOT button on GPIO 0)

### Wiring Table

| MPU-6050 Pin | ESP32-S3 Pin | Description |
|---|---|---|
| **VCC** | **3.3V** | Power supply (3.3V) |
| **GND** | **GND** | Ground reference |
| **SDA** | **GPIO 8** (Default) | I2C Data line (configurable in menuconfig) |
| **SCL** | **GPIO 9** (Default) | I2C Clock line (400 kHz Fast-Mode) |

| Button Pin | ESP32-S3 Pin | Description |
|---|---|---|
| **One leg** | **GPIO 0** (Default) | Sleep/wake toggle (configurable in menuconfig) |
| **Other leg** | **GND** | Ground reference |

> **Note on RGB LED GPIO:**  
> - ESP32-S3-DevKitC-1 **v1.0** typically connects the onboard RGB LED to **GPIO 48**.  
> - ESP32-S3-DevKitC-1 **v1.1** routes it to **GPIO 38**.  
> You can toggle this setting in `idf.py menuconfig` -> `MPU6050 Fall Detection Configuration`.

> **Note on Sleep Button:**  
> GPIO 0 is the **BOOT** button on most ESP32-S3-DevKitC boards and can be reused for this purpose. An internal pull-up is enabled; the button connects to GND. If you prefer a different GPIO, change it in `idf.py menuconfig` -> `MPU6050 Fall Detection Configuration`.

---

## 📁 Directory Structure

Organized into a clean, domain-driven modular structure:

```text
├── CMakeLists.txt              # Root build script
├── partitions.csv              # Custom 3 MiB app partition table
├── sdkconfig.defaults          # Default hardware, PSRAM & network configuration
├── .gitignore                  # Excludes build output, venv, and component caches
├── main/
│   ├── CMakeLists.txt          # Component registration & include paths
│   ├── Kconfig.projbuild       # Configuration menu for pins, WiFi, MQTT & sleep button
│   ├── idf_component.yml       # Pinned dependencies (TFLite Micro, LED Strip, MQTT)
│   ├── app/                    # High-level application & task management
│   │   ├── main.c              # System bootloader, task initializers
│   │   ├── fall_detection.cc   # Core 1 inference loop & window evaluator
│   │   ├── fall_detection.h
│   │   ├── imu_sample.h        # Inter-core binary sample structure
│   │   └── imu_stream.c / .h   # Optional buffered serial sample output
│   ├── drivers/                # Hardware peripheral drivers
│   │   ├── mpu6050.c / .h      # I2C driver, physical unit conversion & sleep/wake
│   │   ├── fall_indicator.c / .h # WS2812 RMT LED driver
│   │   └── sleep_button.c / .h # GPIO button driver for sleep/wake toggle
│   ├── network/                # Connectivity & Telemetry
│   │   ├── wifi_station.c / .h # Auto-reconnecting WiFi STA
│   │   └── mqtt_reporter.c / .h # MQTT client with JSON payload formatter
│   └── model/                  # Active INT8 artifacts & inference engine
│       ├── model_data.cc / .h  # INT8 1D-CNN (120,752-byte flatbuffer)
│       ├── scaler_data.h       # Scaling, quantization and detection threshold
│       ├── model_input.cc / .h # Sliding window & feature transformation
│       ├── quantized_ops.cc / .h # Quantized padding fix for pinned TFLM kernel
│       └── model_runtime.cc / .h # TFLite Micro runtime & kernel resolver
├── models/
│   └── source/                 # Original model export; not compiled
│       ├── fall_model_int8.tflite
│       └── scaler_data.h       # Matching exported preprocessing parameters
├── docs/
│   └── model.md                # Model provenance, preparation & benchmarking
├── tools/
│   ├── capture_mpu.py           # USB serial to CSV recorder
│   ├── prepare_int8_model.py   # Freeze exported intermediate shapes for TFLM
│   ├── generate_model_fixtures.py # Desktop INT8 regression fixture generator
│   ├── model_requirements.txt # Optional fixture-generation dependencies
│   └── requirements.txt        # Python dependency (pyserial)
└── tests/                      # Native host test suite (runs on PC without hardware)
    ├── CMakeLists.txt
    ├── model_checks.cc
    ├── model_fixtures.h        # Checked-in NumPy/LiteRT reference inputs & outputs
    └── test_capture_mpu.py     # Parser and simulated serial capture tests
```

---

## 📦 Prerequisites & Installation

### Linux (Ubuntu/Debian)

1. **Install Build Tools & Mosquitto:**
   ```bash
   sudo apt update
   sudo apt install -y git wget flex bison gperf python3 python3-pip python3-venv \
                       cmake ninja-build ccache libffi-dev libssl-dev dfu-util \
                       libusb-1.0-0 mosquitto mosquitto-clients
   ```

2. **Grant Serial Port Permissions:**
   ```bash
   sudo usermod -a -G dialout $USER
   # Log out and log back in for changes to take effect
   ```

3. **Install ESP-IDF (v6.0 or higher recommended):**
   ```bash
   mkdir -p ~/esp && cd ~/esp
   git clone -b v6.0.2 --recursive https://github.com/espressif/esp-idf.git
   cd esp-idf && ./install.sh esp32s3
   ```

---

### macOS (Homebrew)

1. **Install Build Tools & Mosquitto:**
   ```bash
   brew install cmake ninja dfu-util mosquitto
   ```

2. **Install ESP-IDF:**
   ```bash
   mkdir -p ~/esp && cd ~/esp
   git clone -b v6.0.2 --recursive https://github.com/espressif/esp-idf.git
   cd esp-idf && ./install.sh esp32s3
   ```

3. **Locate Serial Device on macOS:**
   Connect your ESP32-S3 and check:
   ```bash
   ls /dev/cu.usb*
   # Typically /dev/cu.usbmodem* or /dev/cu.wchusbserial*
   ```

---

## 📡 Mosquitto MQTT Broker Setup

To allow the ESP32 to connect over your local WiFi network, Mosquitto must be configured to accept remote connections:

### 1. Enable Remote Listening

#### On Linux:
```bash
sudo bash -c 'printf "listener 1883\nallow_anonymous true\n" > /etc/mosquitto/conf.d/remote.conf'
sudo systemctl restart mosquitto
```
*(If UFW firewall is active: `sudo ufw allow 1883/tcp`)*

#### On macOS:
Edit `/opt/homebrew/etc/mosquitto/mosquitto.conf` (or `/usr/local/etc/mosquitto/mosquitto.conf` on Intel):
```text
listener 1883
allow_anonymous true
```
Then start the service:
```bash
brew services restart mosquitto
```

### 2. Verify Port is Open
```bash
# On Linux:
ss -tulpn | grep 1883
# On macOS:
netstat -an | grep 1883
```
You should see it listening on `0.0.0.0:1883` or `*:1883`.

### 3. Find Your PC's Local IP Address
- **Linux:** `hostname -I | awk '{print $1}'`
- **macOS:** `ipconfig getifaddr en0`

---

## 🚀 Configuration & Flashing

1. **Activate ESP-IDF Environment:**
   ```bash
   . $HOME/esp/esp-idf/export.sh
   ```

2. **Configure WiFi, MQTT and Hardware:**
   Open the configuration menu:
   ```bash
   idf.py menuconfig
   ```
   Navigate to **MPU6050 Fall Detection Configuration**:
   - Set **I2C SDA / SCL GPIO** pins (default: 8, 9)
   - Set **RGB LED GPIO** (default: 48; use 38 for DevKitC v1.1)
   - Set **Sleep/Wake toggle button GPIO** (default: 0 = BOOT button)
   - Set **Model working memory** (default: 256 KiB)
   - Keep **Model working memory location → PSRAM** for the initial INT8 run; see [the model guide](docs/model.md) for internal RAM comparisons.
   
   Navigate to **WiFi and MQTT Configuration**:
   - Set **WiFi SSID** (your network name)
   - Set **WiFi Password**
   - Set **MQTT Broker IP Address** (your computer's IP found above)
   - Set **MQTT Broker Port** (`1883`)

3. **Build the Project:**
   ```bash
   idf.py build
   ```

4. **Flash to Target & Open Monitor:**
   - **Linux:**
     ```bash
     idf.py -p /dev/ttyACM0 flash monitor
     ```
   - **macOS:**
     ```bash
     idf.py -p /dev/cu.usbmodem1101 flash monitor
     ```

*(Exit monitor anytime with `Ctrl + ]`)*

---

## 📊 Live Telemetry Stream

In a separate terminal on your computer, subscribe to all fall detector topics:

```bash
mosquitto_sub -t "fall-detector/#" -v
```

### Example Output Stream

The following payloads illustrate the format. The `time_ms` values are examples,
not measured INT8 performance; actual values are reported by the device.

```text
fall-detector/online {"status":"online","mode":"active"}
fall-detector/result {"status":"NORMAL","score":0.0117,"time_ms":123,"mode":"active"}
fall-detector/result {"status":"NORMAL","score":0.0000,"time_ms":123,"mode":"active"}
fall-detector/result {"status":"FALL","score":0.9609,"time_ms":123,"mode":"active"}
fall-detector/online {"status":"online","mode":"sleep"}
fall-detector/online {"status":"online","mode":"active"}
fall-detector/result {"status":"NORMAL","score":0.0117,"time_ms":123,"mode":"active"}
```

### MQTT Topics Reference

| Topic | Payload Format | QoS | Description |
|---|---|---|---|
| `fall-detector/result` | `{"status":"NORMAL"\|"FALL", "score":0.00, "time_ms":123, "mode":"active"}` | 1 | Measured total prediction time in ms. Emitted every prediction window (~1 s). Suppressed for 5 s after a FALL. |
| `fall-detector/error` | `{"error":"<Description>"}` | 1 | Emitted on hardware read drop or timing fault |
| `fall-detector/online` | `{"status":"online", "mode":"active"\|"sleep"}` | 0 / 1 | Published on boot, reconnect, and sleep/wake toggle (retained) |
| `fall-detector/online` | `{"status":"offline"}` | 1 | LWT: broker publishes this when the device disconnects unexpectedly |

---

## 🔘 Sleep / Wake Button

A momentary push button on **GPIO 0** (default: the onboard BOOT button) toggles the device between active sensing and low-power sleep mode.

### Button Press → Sleep Mode
1. MPU6050 sensor enters hardware sleep (I2C SLEEP bit set)
2. Sampling and inference tasks stop processing
3. LED turns **dim cyan** (device is alive but idle)
4. MQTT publishes `{"status":"online","mode":"sleep"}`

### Button Press Again → Wake Up
1. MPU6050 wakes from hardware sleep
2. Sample queue is cleared for a fresh start
3. LED turns **blue** (warmup — collecting 200 samples)
4. MQTT publishes `{"status":"online","mode":"active"}`
5. After ~2 seconds, normal fall detection resumes

> **Debounce:** The button ISR enforces a 250 ms debounce window to prevent double-triggers.

> **GPIO Configuration:** Change the button pin in `idf.py menuconfig` → `MPU6050 Fall Detection Configuration` → `Sleep/Wake toggle button GPIO`. The pin uses an internal pull-up; wire the button between the GPIO and GND.

---

## Capture MPU Samples on Your Computer

The Python recorder saves the six sensor axes sampled by the ESP32 at 100 Hz.
It uses the board's **UART USB connection**; WiFi and an MQTT broker are not
needed for capture. MQTT's existing result messages contain predictions, not
individual sensor samples.

### 1. Enable streaming and flash the firmware

Activate your installed ESP-IDF environment, then run `idf.py menuconfig`:

- Under **MPU6050 Fall Detection Configuration**, enable **Stream MPU samples to
  the UART console** (`CONFIG_MPU_SERIAL_STREAM`). It is disabled by default.
- Under **Component config → ESP-STDIO → Channel for console output**, select
  **Custom UART**. Keep **UART0**, **TX GPIO43**, and **RX GPIO44** for this ESP32-S3
  board. Set **UART console baud rate** to **460800**
  (`CONFIG_ESP_CONSOLE_UART_BAUDRATE`). In the installed ESP-IDF 6.0.2, the baud
  setting is editable only with Custom UART selected. Older IDF versions may put
  console settings under ESP System Settings.
- Streaming requires at least 230400 baud; the project's normal 115200 baud
  console setting is insufficient for this stream with adequate log headroom.

Build and flash, substituting the device path found in step 2:

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

If your ESP32-S3 has separate UART and native USB connectors, use the UART
connector. Device names vary; `/dev/ttyUSB0` is only an example. Close
`idf.py monitor` (Ctrl+]) and other serial readers before recording.

Streaming uses a separate 64-sample queue and a lower-priority output task.
Sensor acquisition never waits for serial output. Queue overflow drops capture
samples, visible as sequence gaps; capturing does not consume the inference queue.
The stream contains physical values before model scaling, not raw register counts.

### 2. Set up and run the recorder

From the project root:

```bash
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r tools/requirements.txt
python3 tools/capture_mpu.py --list-ports

# Record for 60 seconds (use your actual serial device).
python3 tools/capture_mpu.py --port /dev/ttyUSB0 --duration 60 --output captures/walk.csv

# Record until Ctrl+C, with an automatically named CSV file.
python3 tools/capture_mpu.py --port /dev/ttyUSB0
```

The recorder defaults to 460800 baud. If you selected another supported firmware
baud rate, pass the same value using `--baud`. It creates output directories and
refuses to overwrite existing files. Generated `captures/` and `.venv/` directories
are ignored by Git. Python 3.10 or newer is required.

### CSV format and capture health

```csv
host_time_utc,sequence,device_timestamp_us,acc_x_g,acc_y_g,acc_z_g
2026-09-10T03:00:00.123456+00:00,123,4567890,0.012,-0.004,1.002
```

- `host_time_utc`: when Python processes the received record; USB buffering can
  delay or group records. Use the device timestamp for sample intervals.
- `device_timestamp_us`: ESP32 microseconds since boot, recorded immediately
  before the I2C read. It restarts when the board reboots.
- `sequence`: unsigned 32-bit counter advancing on each acquisition attempt,
  including failed sensor reads. It wraps naturally.
- Acceleration is in **g**.

The firmware's versioned line format is
`MPU1,sequence,timestamp_us,ax,ay,az`. The recorder skips console logs,
rejects malformed/non-finite records, and handles lines split across serial reads.
It prints capture statistics every five seconds and at exit. `missing` counts
sequence gaps between received samples; `timing_gaps` counts device intervals
outside 5–15 ms. Missing samples can reflect I2C errors or dropped output; samples
before the first received record cannot be counted. Restarts and duplicates are
reported, and their valid records are preserved in the CSV.

CSV buffers are flushed every second and on a clean stop. Disconnects stop capture
with an error while preserving written data. There is no automatic reconnection;
start another recording after reconnecting. `--duration` starts when the port opens,
including any boot wait. No valid samples for 30 seconds causes an error; change
that limit with `--timeout 60`. A recording with zero samples also exits with an
error. Boot can include up to 15 seconds waiting for WiFi, even for serial capture.

If no samples arrive, check that the streaming firmware was flashed, the UART USB
connector and baud rate match, and the MPU initializes. On Linux, permission
errors may require membership in `dialout` as described above. Opening a serial
device may reset some boards; the recorder deasserts DTR/RTS before opening to
reduce this, but adapter behavior varies.

Run the Python tests (Linux serial integration uses a pseudo-terminal, so no
board is needed):

```bash
python3 -m unittest discover -s tests -p 'test_capture_mpu.py' -v
```

---

## 💡 RGB Indicator Reference

The onboard addressable RGB LED reflects the system's operational state in real-time:

| Color | State | Description |
|---|---|---|
| 🔵 **Dim Blue** | **Warmup / Init** | Device booting, connecting to WiFi, or filling initial 200-sample window. |
| ⚫ **Off** | **Normal** | Valid sensor data; dequantized score is below the exported threshold (approximately 0.20). |
| 🔴 **Red** | **Fall Detected** | Dequantized score is at or above the exported threshold. Holds for 5 seconds for visibility. |
| 🟠 **Amber** | **Fault / Error** | I2C bus error, timing jitter, or no inference result for > 5 seconds. |
| 🩵 **Dim Cyan** | **Sleep Mode** | Device is connected but sensor capture is paused (button toggled). |

---

## 🧪 Native Host Tests (CI/PC)

You can validate sliding windows, feature scaling, INT8 rounding/clipping, tensor
metadata, and TensorFlow Lite Micro inference directly on your Linux or macOS
machine without physical hardware. Four synthetic reference windows check all
600 quantized input bytes and exact output agreement with desktop LiteRT:

```bash
# Configure and build native tests
PATH=/usr/bin:/bin cmake -S tests -B /tmp/mpu6050-host-checks -G Ninja
PATH=/usr/bin:/bin cmake --build /tmp/mpu6050-host-checks

# Run test suite
ctest --test-dir /tmp/mpu6050-host-checks --output-on-failure
```

The insufficient-arena test deliberately exercises an allocation error. A
`Failed to allocate` diagnostic in verbose test output is expected when the
suite finishes with `100% tests passed`.

The checked-in reference fixtures need no Python ML packages to run. See
[the model guide](docs/model.md) for export preparation, fixture regeneration,
compatibility corrections, and device benchmarking.

---

## 📄 License
This project is open source and available under the [MIT License](LICENSE).
