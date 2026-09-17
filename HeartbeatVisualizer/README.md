# MR60BHA2 Heartbeat Visualiser

Upload `HeartbeatVisualizer.ino` to a **Seeed Studio XIAO ESP32C6 / MR60BHA2** kit. It drives the kit's integrated WS2812 RGB LED on `D1`.

Install these libraries before compiling:

- **Adafruit NeoPixel** (Arduino Library Manager)
- **Seeed Arduino mmWave** (Seeed's [mmWave library](https://github.com/Seeed-Projects/Seeed-mmWave-library))

In Arduino IDE, select **XIAO_ESP32C6**, choose its USB port, and upload. Serial Monitor is 115200 baud.

## Calibration and behaviour

Edit `HEART_RATE_REDUCTION_BPM` near the top of the sketch to change the default 10 BPM correction. The sensor distance value is treated as centimetres, so only complete readings from **20 cm to 130 cm** (0.20–1.30 m) are used. The lower limit accommodates short-range distance underestimation. A small orange startup flash confirms that the integrated D1 LED is working immediately after reset/upload.

The first in-zone distance reading immediately starts a temporary **Category 2** pattern (850 ms). The first three valid heart-rate readings select the real category (their median category is used); if it differs, the repeat period transitions over three seconds. One or two anomalous readings cannot change an established category: later changes need four consecutive valid readings, then transition over three seconds. Two consecutive out-of-zone distance readings turn the LED off. Distance and heart-rate packets arrive separately, so the last in-zone distance remains valid until a newer distance packet is received; this keeps the LED rhythm continuous between sensor samples.

The pattern is bright orange for 100 ms, off for 70 ms, smaller orange for 100 ms, then a dim orange glow until the period ends:

| Corrected heart rate | Category | Pattern period |
| --- | ---: | ---: |
| 50–60 BPM | 1 | 1050 ms |
| 61–80 BPM | 2 | 850 ms |
| 81–100 BPM | 3 | 666 ms |
| 101–180 BPM | 4 | 450 ms |

The MR60BHA2 library reports the distance/heart-rate solution for the sensor-selected target. The sketch rejects it unless that reported distance is within the 20–130 cm zone.

This is a visualisation project, not a medical device.
