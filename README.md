# CYD Todoist Dashboard

Arduino-framework firmware for the ESP32-2432S028 "Cheap Yellow Display" that shows your top 5 Todoist tasks on the built-in 2.8" ILI9341 display.

## Features

- Uses `TFT_eSPI` for the 320x240 TFT.
- Uses `ArduinoJson` to parse Todoist Sync API responses.
- Uses the CYD's XPT2046 resistive touch panel for tap-to-refresh.
- Fetches `items` and `projects` from Todoist and displays the top 5 active tasks.
- Auto-refreshes every 5 minutes.
- Shows the current local time and Wi-Fi status in the header.

## Board Wiring Used

These pin assignments match the ESP32-2432S028 schematic:

- TFT `SCLK=14`, `MOSI=13`, `MISO=12`, `CS=15`, `DC=2`, `RST=-1`, backlight `21`
- Touch `CLK=25`, `MOSI=32`, `MISO=39`, `CS=33`, `IRQ=36`

## Setup

1. Install PlatformIO.
2. Copy `include/Secrets.h.example` to `include/Secrets.h`.
3. Fill in:
   - `WIFI_SSID`
   - `WIFI_PASSWORD`
   - `TODOIST_API_TOKEN`
   - `TIMEZONE_POSIX`
4. Build and upload:

```bash
pio run -t upload
pio device monitor
```

## How Task Ranking Works

Tasks are sorted with this display order:

1. Tasks with a due date first
2. Earliest due date first
3. Higher Todoist priority first
4. Lower `child_order` first

If you want a different ordering rule later, the sort is centralized in [src/main.cpp](/home/timp/Documents/CYD-TODOIST/src/main.cpp).

## Todoist API Notes

This firmware uses Todoist's current Sync endpoint:

- `POST https://api.todoist.com/api/v1/sync`

It requests:

- `sync_token=*`
- `resource_types=["items","projects"]`

## Notes

- The HTTPS client uses `setInsecure()` for simplicity on-device.
- Failed auto-refreshes are rate-limited before retrying so the dashboard stays responsive if Wi-Fi, NTP, or Todoist is temporarily unavailable.
- If `include/Secrets.h` is missing, the firmware still builds and shows an on-screen setup message.
- `.gitignore` excludes `include/Secrets.h`, `.pio/`, and `.venv/` so local secrets and build output stay out of version control.
