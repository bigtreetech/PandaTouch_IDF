# BIGTREETECH Panda Touch – Pinout Documentation

This pinout guide is written for **developers extending the board** and for **makers** who need to interface with the display, touch, or expansion headers.

## 1. Overview

- **MCU**: ESP32-S3 (dual-core, Xtensa LX7 @ up to 240 MHz, Wi-Fi 2.4 GHz, Bluetooth 5 LE, native USB OTG)
- **Memory**: 8 MB Octal PSRAM onboard
- **Display**: 7" TFT LCD, 800×480 resolution, RGB565 color, 16-bit parallel bus
- **Touch**: GT911 capacitive controller (I²C0 bus)
- **Connectivity**:
  - **USB-C**: flashing, serial debug, and power input
  - **USB-A Host**: OTG port for USB devices (flash drives, HID, etc.)
  - Dual I²C buses, Wi-Fi, Bluetooth
- **Power**: USB-C 5 V input with onboard regulators for 3.3 V and LCD backlight supply

---

## 2. External Connectors

### 2.1 USB-C (P1 – Flashing, Debug, Power)

The USB-C port is the **primary interface** for:

    - Powering the board (5 V input).
    - Flashing firmware (via CH340K USB-UART bridge).
    - Serial debug connection.

    | Pin | Name | Description                | ESP32 (Arduino) Pin |
    | --- | ---- | -------------------------- | ------------------- |
    | 1   | VBUS | +5V power in               | —                   |
    | 2   | D-   | USB data − (CH340K bridge) | Internally routed   |
    | 3   | D+   | USB data + (CH340K bridge) | Internally routed   |
    | 4   | GND  | Ground                     | —                   |

> 💡 Note: This port does **not** expose the ESP32-S3’s native USB OTG. It is tied to the **CH340K** for flashing and debug.

---

### 2.2 USB-A (U2 – OTG Host Port)

The USB-A port is connected to the **ESP32-S3 native USB OTG PHY** and is intended for plugging in **USB devices** (mass storage, HID, etc.).

    | Pin | Name | Description           | ESP32 (Arduino) Pin |
    | --- | ---- | --------------------- | ------------------- |
    | 1   | VBUS | +5V out to USB device | —                   |
    | 2   | D-   | USB native data −     | GPIO19              |
    | 3   | D+   | USB native data +     | GPIO20              |
    | 4   | GND  | Ground                | —                   |

> 💡 In **host mode**, this port provides power to external USB devices.  
> In **device mode**, the ESP32-S3 can present itself as a USB peripheral if configured in firmware.

---

### 2.3 LCD FPC (U1 – 50-pin TFT RGB)

This 50-pin FPC carries the **RGB parallel display signals**, backlight, and reset. The display runs in **DE mode** (HSYNC/VSYNC unused).

    | Signal    | ESP32 (Arduino) Pin | Notes                 |
    | --------- | ------------------- | --------------------- |
    | PCLK      | GPIO5               | Pixel clock           |
    | DE        | GPIO38              | Data enable           |
    | HSYNC     | —                   | Not routed            |
    | VSYNC     | —                   | Not routed            |
    | R3–R7     | GPIO6–10            | Red data bits         |
    | G2–G7     | GPIO11–16           | Green data bits       |
    | B3–B7     | GPIO17,18,48,47,39  | Blue data bits        |
    | Backlight | GPIO21              | PWM brightness (LEDC) |
    | Reset     | GPIO46              | LCD reset             |

---

### 2.4 Touch FPC (JP1 – GT911 I²C0)

The GT911 capacitive touch controller communicates via **I²C0** and uses dedicated reset/interrupt pins.

    | Pin | Name | Description     | ESP32 (Arduino) Pin |
    | --- | ---- | --------------- | ------------------- |
    | 1   | RST  | Touch reset     | GPIO41              |
    | 2   | INT  | Touch interrupt | GPIO40              |
    | 3   | SDA  | I²C0 data       | GPIO2               |
    | 4   | SCL  | I²C0 clock      | GPIO1               |
    | 5   | VCC  | 3V3 supply      | —                   |
    | 6   | GND  | Ground          | —                   |

---

### 2.5 Debug UART (via CH340K)

For flashing and debugging, the CH340K bridges USB-C to **UART0** on the ESP32-S3.

    | Signal | Description            | ESP32 (Arduino) Pin |
    | ------ | ---------------------- | ------------------- |
    | TXD    | UART TX to host        | GPIO43 (U0TXD)      |
    | RXD    | UART RX from host      | GPIO44 (U0RXD)      |
    | DTR    | Drives BOOT            | —                   |
    | RTS    | Drives CHIP_PU (reset) | —                   |

---

### 2.6 I²C Expansion Header (P2 – I²C1)

Secondary I²C bus exposed for external peripherals.

    | Pin | Name | Description | ESP32 (Arduino) Pin |
    | --- | ---- | ----------- | ------------------- |
    | 1   | SDA1 | I²C1 data   | GPIO4               |
    | 2   | SCL1 | I²C1 clock  | GPIO3               |
    | 3   | VCC  | 3V3 supply  | —                   |
    | 4   | GND  | Ground      | —                   |

---

## 3. Special Signals & Functions

- **USB Roles**

  - **USB-C** → Flashing, debug UART, power.
  - **USB-A** → Native USB OTG (host/device).

- **Backlight Control**

  - `GPIO21` → PWM brightness.
  - Power via `BL-VCC/BL-GND`.

- **Touch**

  - `GPIO41` → Touch reset.
  - `GPIO40` → Touch interrupt.

- **Expansion**
  - `GPIO3/4` → I²C1 for add-ons.
