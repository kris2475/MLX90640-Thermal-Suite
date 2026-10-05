# Teensy 3.5 MLX90640 Thermal Suite & Adaptive ML Room Monitor

A professional embedded firmware suite designed for the **Teensy 3.5** and the **MLX90640 32x24 IR array**. It combines real-time thermal frame streaming over high-speed serial (compatible with companion Python GUI applications) with edge-based machine learning anomaly detection, local OLED telemetry visualization, hardware RTC timestamping, and built-in SD card CSV logging.

---

## Executive Summary

This firmware bridges low-level hardware sensing with automated intelligence. Upon boot, the Teensy automatically scans the I2C bus for an OLED display, syncs time via its hardware RTC, initializes the built-in micro-SD card for batch logging, and fires up the MLX90640 infrared sensor. 

The system operates across three distinct machine learning phases—**Warmup**, **Baseline Learning**, and **Active Adaptive Monitoring**—using Welford's streaming algorithm and Exponentially Weighted Moving Averages (EWMA) to detect thermal anomalies in real time without manual threshold configuration.

---

## Key Features

* **High-Speed Frame Streaming:** Streams full 768-value temperature frames (`FRAME:val,val,...`) over Serial at **460800 baud** to host computer GUIs.
* **Adaptive ML Anomaly Detection:** Dynamically learns normal environmental thermal bounds and tracks deviations using Z-score statistics.
* **Burst-Mode SD Logging:** Automatically writes timestamped metrics (`DateTime, MaxTemp, MinTemp, AvgTemp, AnomalyScore, Event`) to a CSV file on the built-in SD card when anomalies or batch limits are reached.
* **Dual-View OLED Interface:** Rotates automatically between a downsampled 16x12 live thermal block preview and live machine learning metrics.
* **Hardware RTC Integration:** Timestamps all logged events accurately utilizing the Teensy 3.5 onboard RTC.

## Standalone Edge ML IoT Architecture

Designed to operate entirely **standalone** without requiring a host computer for core intelligence, the Teensy 3.5 firmware acts as an autonomous IoT monitoring node. By combining local hardware sensing, edge machine learning, and onboard persistent storage, it functions independently in remote or mission-critical environments:

* **Autonomous Operation:** Runs untethered, relying on its onboard hardware Real-Time Clock (RTC) to maintain precise timestamps and local micro-SD storage for persistent event tracking.
* **Edge Intelligence:** Evaluates thermal frames locally in real-time, completely bypassing the need for cloud connectivity or external computing power for anomaly detection.
* **Optional Hybrid Telemetry:** While fully standalone, it retains high-speed serial streaming capabilities to interface with companion Python GUI applications when live desktop visualization is desired.

---

---
## Adaptive ML Anomaly Engine & Welford's Algorithm

The firmware implements an edge-based machine learning pipeline that monitors thermal environments without hardcoded safety thresholds. It operates through a three-stage state machine:

1. **Warmup (`5s`):** Stabilizes sensor readings and internal thermal bias upon boot.
2. **Baseline Learning (`60s`):** Uses **Welford's streaming algorithm** (`updateWelfordStats`) to compute online running means (`meanMaxT`, `meanAvgT`) and sum of squared differences (`M2MaxT`, `M2AvgT`) frame-by-frame. This memory-efficient approach calculates exact variances without needing to store historical arrays in RAM.
3. **Active Monitoring:** Continuously computes Z-scores (`zMax`, `zAvg`) against an **Exponentially Weighted Moving Average (EWMA)** sliding distribution (`EWMA_ALPHA = 0.02f`). 

### Anomaly Detection & SD Logging
* **Consecutive Debouncing:** Requires a raw anomaly score ($\sqrt{z_{max}^2 + z_{avg}^2} > 3.5$) for two consecutive frames to confirm an event, avoiding false positives.
* **Burst-Mode Diagnostics:** Triggers a 30-second high-frequency logging window upon event detection.
* **SD Card Batch Logging:** Collects telemetry metrics (`DateTime`, `MaxTemp`, `MinTemp`, `AvgTemp`, `AnomalyScore`, `Event`) in a RAM buffer (`BUFFER_SIZE = 20`) and flushes them to `thermal_ml_log.csv` on the built-in micro-SD card every 10 minutes or instantly during burst events.
---

## Hardware Requirements

* **Teensy 3.5** Microcontroller (features built-in SD card holder and hardware RTC).
* **MLX90640 32x24 IR Array Sensor** (Breakout board).
* **SSD1306 OLED Display** (128x64 resolution, I2C address `0x3C` or `0x3D`).
* Micro-SD Card (formatted to FAT/FAT32 for event logging).

---

## Pin Connections

| Component | Component Pin | Teensy 3.5 Pin | Notes |
| :--- | :--- | :--- | :--- |
| **MLX90640 / OLED** | VCC / VIN | **3.3V** | Must use 3.3V power rail (do not use 5V). |
| **MLX90640 / OLED** | GND | **GND** | Common ground. |
| **I2C Bus** | SDA | **Pin 18 (SDA0)** | Primary I2C data line. |
| **I2C Bus** | SCL | **Pin 19 (SCL0)** | Primary I2C clock line. |
| **Built-in SD** | Built-in Slot | `BUILTIN_SDCARD` | Native SD interface on Teensy 3.5. |

---
