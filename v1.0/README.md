# Inference Firmware

This project contains only the on-device inference path for the ESP32.

## Responsibilities

- Read raw accelerometer windows from the ADXL362.
- Compute the small feature set used by the TinyML baseline detector.
- Evaluate the generated baseline model header.
- Report baseline vs deviation on Serial.

## Files

- `src/main.cpp` - sensor loop and window processing.
- `src/feature_extraction.cpp` - feature computation.
- `src/feature_extraction.h` - feature data structure and API.
- `include/tinyml_baseline_model.h` - generated model thresholds and decision rule.

## Build

```bash
cd inference
pio run
```

## Notes

This project does not upload data to the server and does not contain data gathering or persistence code.
