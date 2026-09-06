# 🚨 Edge TinyML Fall Detector on ESP32-S3

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0+-blue.svg?logo=espressif)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/)
[![Target](https://img.shields.io/badge/Hardware-ESP32--S3%20N16R8-red.svg)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Framework](https://img.shields.io/badge/AI-TensorFlow%20Lite%20Micro-orange.svg?logo=tensorflow)](https://www.tensorflow.org/lite/microcontrollers)
[![Protocol](https://img.shields.io/badge/Protocol-MQTT%20v3.1.1-brightgreen.svg?logo=eclipse-mosquitto)](https://mosquitto.org/)
[![Platform](https://img.shields.io/badge/OS-Linux%20%7C%20macOS-lightgrey.svg)]()
[![Standard](https://img.shields.io/badge/Standard-C%2B%2B17%20%2F%20C11-blue.svg)]()

A high-performance, real-time edge fall detection system deployed on the **ESP32-S3** dual-core microcontroller. The system samples a 6-axis **MPU-6050** IMU at 100 Hz, runs a 1D-CNN floating-point model using **TensorFlow Lite Micro** in PSRAM, signals instant local status via an **addressable WS2812 RGB LED**, and streams live detection telemetry over **WiFi via MQTT**.

---

## 📑 Table of Contents
- [System Architecture](#-system-architecture)
- [Key Features](#-key-features)
- [Hardware & Pinout](#-hardware--pinout)
- [Directory Structure](#-directory-structure)
- [Prerequisites & Installation](#-prerequisites--installation)
  - [Linux (Ubuntu/Debian)](#linux-ubuntudebian)
  - [macOS (Homebrew)](#macos-homebrew)
- [Mosquitto MQTT Broker Setup](#-mosquitto-mqtt-broker-setup)
- [Configuration & Flashing](#-configuration--flashing)
- [Live Telemetry Stream](#-live-telemetry-stream)
- [RGB Indicator Reference](#-rgb-indicator-reference)
- [Native Host Tests (CI/PC)](#-native-host-tests-cipc)

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
               |  +---------------------+                     | (~520 ms)  | |
WS2812 RGB <----- |  RGB Indicator Task |<-- State Queue -----+            | |
  (Onboard)    |  +---------------------+                     +------+-----+ |
               |                                                     |       |
               |  +---------------------+                            |       |
               |  |  WiFi STA & MQTT    |<------ Result / Error -----+       |
               |  |  (Non-blocking)     |                                    |
               |  +----------+----------+                                    |
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
- **Robust Sensor Data Hygiene:**
  - Strict interval validation ($5000\,\mu\text{s} \le \Delta t \le 15000\,\mu\text{s}$).
  - Automatic detection of dropped I2C reads and sequence gaps.
  - Rejection of invalid data (NaN/Inf) with automatic window reset.
- **Optimized Memory Allocation:**
  - Reserves a 256 KiB tensor arena in Octal PSRAM at 80 MHz, keeping internal SRAM free for network buffers and FreeRTOS queues.
- **Resilient Network & MQTT Client:**
  - WiFi STA mode with automatic event-driven reconnect.
  - Non-blocking MQTT publishing: telemetry is streamed if connected, but network downtime never stalls local fall detection or LED alerts.
  - Configured with MQTT **Last Will and Testament (LWT)** to detect device disconnection instantly.
- **Addressable RGB Visualizer:**
  - Real-time WS2812 LED feedback driven by hardware RMT peripheral.

---

## 🔌 Hardware & Pinout

### Recommended Hardware
- **MCU:** ESP32-S3-DevKitC-1 (N16R8: 16 MB Flash, 8 MB Octal PSRAM)
- **Sensor:** MPU-6050 6-Axis Accelerometer & Gyroscope module
- **Indicator:** Onboard WS2812 Addressable RGB LED

### Wiring Table

| MPU-6050 Pin | ESP32-S3 Pin | Description |
|---|---|---|
| **VCC** | **3.3V** | Power supply (3.3V) |
| **GND** | **GND** | Ground reference |
| **SDA** | **GPIO 8** (Default) | I2C Data line (configurable in menuconfig) |
| **SCL** | **GPIO 9** (Default) | I2C Clock line (400 kHz Fast-Mode) |

> **Note on RGB LED GPIO:**  
> - ESP32-S3-DevKitC-1 **v1.0** typically connects the onboard RGB LED to **GPIO 48**.  
> - ESP32-S3-DevKitC-1 **v1.1** routes it to **GPIO 38**.  
> You can toggle this setting in `idf.py menuconfig` -> `MPU6050 Fall Detection Configuration`.

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
│   ├── Kconfig.projbuild       # Configuration menu for pins, WiFi & MQTT
│   ├── idf_component.yml       # Pinned dependencies (TFLite Micro, LED Strip, MQTT)
│   ├── app/                    # High-level application & task management
│   │   ├── main.c              # System bootloader, task initializers
│   │   ├── fall_detection.cc   # Core 1 inference loop & window evaluator
│   │   ├── fall_detection.h
│   │   └── imu_sample.h        # Inter-core binary sample structure
│   ├── drivers/                # Hardware peripheral drivers
│   │   ├── mpu6050.c / .h      # I2C driver & physical unit conversion
│   │   └── fall_indicator.c / .h # WS2812 RMT LED driver
│   ├── network/                # Connectivity & Telemetry
│   │   ├── wifi_station.c / .h # Auto-reconnecting WiFi STA
│   │   └── mqtt_reporter.c / .h # MQTT client with JSON payload formatter
│   └── model/                  # Machine Learning artifacts & inference engine
│       ├── model_data.cc / .h  # 1D-CNN weights (224 KB flatbuffer)
│       ├── scaler_data.h       # Feature scaling parameters (mean/scale)
│       ├── model_input.cc / .h # Sliding window & feature transformation
│       └── model_runtime.cc / .h # TFLite Micro runtime & kernel resolver
└── tests/                      # Native host test suite (runs on PC without hardware)
    ├── CMakeLists.txt
    └── model_checks.cc
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

2. **Configure WiFi and MQTT:**
   Open the configuration menu:
   ```bash
   idf.py menuconfig
   ```
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

```text
fall-detector/online {"status":"online"}
fall-detector/result {"status":"NORMAL","score":0.0034,"time_ms":521}
fall-detector/result {"status":"NORMAL","score":0.0041,"time_ms":522}
fall-detector/result {"status":"FALL","score":0.6404,"time_ms":518}
fall-detector/result {"status":"NORMAL","score":0.1362,"time_ms":520}
```

### MQTT Topics Reference

| Topic | Payload Format | QoS | Description |
|---|---|---|---|
| `fall-detector/result` | `{"status":"NORMAL"|"FALL", "score":0.00, "time_ms":520}` | 1 | Emitted every prediction window (~1s) |
| `fall-detector/error` | `{"error":"<Description>"}` | 1 | Emitted on hardware read drop or timing fault |
| `fall-detector/online` | `{"status":"online"}` or `{"status":"offline"}` | 0 / 1 | Connection status (LWT sets `offline` upon disconnect) |

---

## 💡 RGB Indicator Reference

The onboard addressable RGB LED reflects the system's operational state in real-time:

| Color | State | Description |
|---|---|---|
| 🔵 **Dim Blue** | **Warmup / Init** | Device booting, connecting to WiFi, or filling initial 200-sample window. |
| ⚫ **Off** | **Normal** | Valid sensor data; fall score is below threshold (`< 0.55`). |
| 🔴 **Red** | **Fall Detected** | Model detected a fall event (`score >= 0.55`). Updates dynamically. |
| 🟠 **Amber** | **Fault / Error** | I2C bus error, timing jitter, or no inference result for > 5 seconds. |

---

## 🧪 Native Host Tests (CI/PC)

You can validate the sliding window math, feature scaling, and TensorFlow Lite Micro inference directly on your Linux or macOS machine without physical hardware:

```bash
# Configure and build native tests
PATH=/usr/bin:/bin cmake -S tests -B /tmp/mpu6050-host-checks -G Ninja
PATH=/usr/bin:/bin cmake --build /tmp/mpu6050-host-checks

# Run test suite
ctest --test-dir /tmp/mpu6050-host-checks --output-on-failure
```

---

## 📄 License
This project is open source and available under the [MIT License](LICENSE).
