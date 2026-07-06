# Conversitions IoT Agent

Agentic AI-based Medical Support System for Cardiovascular Diseases using IoMT Sensors.

This folder contains the evaluation artifacts for the IoT Agent used in an agentic AI-based medical support system for cardiovascular disease monitoring with IoMT sensors.

## Evaluation Report

The main report is [evaluation_report.md](evaluation_report.md). It summarizes an independent verification and validation run across 37 scenarios covering:

- General IoT troubleshooting and data acquisition
- ECG signal quality issues
- Bluetooth connectivity problems
- Electrode placement and detachment scenarios

### Summary

- Total scenarios: 37
- Overall pass rate: 97.3% (36/37)

- State consistency: 100%

## Repository Contents

- Scenario JSON files for C, ECG, Bluetooth, and electrode test cases
- `evaluation_report.md` with the final analysis and conclusions
- `evaluations/` with per-scenario evaluation outputs
- `ESP-32-code.ino` Arduino firmware for the ECG device

## Arduino ECG Device Code

The `ESP-32-code.ino` sketch is the ECG acquisition firmware for the ESP32-based sensor device paired with the AD8232 module. It samples the ECG signal at 500 Hz, applies filtering and QRS detection, packages data for BLE transmission, and includes signal-quality checks plus adaptive heartbeat timing logic.

Key behaviors in the sketch include:

- 500 Hz sampling with a 2 ms sampling interval
- Baseline wander removal and 50 Hz notch filtering
- Pan-Tompkins-style QRS detection with adaptive refractory timing
- BLE packet formatting for ECG sample streaming
- Signal-quality reporting and electrode-contact handling

## Notes

The `.gitignore` file excludes common Python virtual environment folders and cache directories so local development files are not committed.
