# Teensy 3.5 MLX90640 Thermal Suite & Adaptive ML Room Monitor

A professional embedded firmware suite designed for the **Teensy 3.5** and the **MLX90640 32x24 IR array**. It combines real-time thermal frame streaming over high-speed serial (compatible with companion Python GUI applications) with edge-based machine learning anomaly detection, local OLED telemetry visualization, hardware RTC timestamping, and built-in SD card CSV logging.

---

## Contents

1. [Executive Summary](#executive-summary)
2. [Key Features](#key-features)
3. [Hardware Requirements](#hardware-requirements)
4. [Pin Connections](#pin-connections)
5. [Software Dependencies](#software-dependencies)
6. [Adaptive ML Anomaly Engine](#adaptive-ml-anomaly-engine)
7. [Embedded Firmware Source](#embedded-firmware-source)

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
