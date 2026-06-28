# ESH - ESP32 Shell (v0.2)

A lightweight, minimal interactive shell, line editor, and full-screen visual text editor tailored for the ESP32 microcontroller. ESH bridges the gap between physical embedded hardware and classic Unix-like CLI workflows, featuring concurrent dual-screen output rendering (Serial Terminal + ST7735 TFT LCD), a dynamic current working directory (`cwd`), and automated filename path resolution.

---

## 🚀 Features

- **Dual Output Interface:** Simulates text terminal grids concurrently over standard Serial UART (115200 baud) and a 1.8" SPI ST7735 TFT display ($160 \times 128$ resolution).
- **Global Calibration Offset:** Integrated 2-pixel safe-zone padding (`TFT_OFFSET_X` / `TFT_OFFSET_Y`) across the entire OS layer to prevent character cropping on misaligned third-party TFT panels.
- **Dynamic Path Resolution:** Automatically resolves paths relative to the current working directory (`cd`) or routes absolute paths seamlessly.
- **Automated PATH Script Execution:** Runs `.esh` automation script files directly from the global root directory (`/`). Launching `hello` or `hello.esh` will parse and sequentially execute commands directly out of `/hello.esh`.
- **Micro Text Editor (`edit`):** A robust visual screen editor that queries terminal window dimensions dynamically using ANSI `CSI 19t` sequences, supports responsive full-screen UI updates with colorized ANSI blocks over Serial, text-wrapping, and line-continuation dash indicators on the TFT.
- **Standard Storage Operations:** Leverages native `LittleFS` block management for resilient on-flash asset management.

---

## 🔌 Hardware Configuration & Pinout

| Component | ESP32 Pin | Function |
| :--- | :--- | :--- |
| **TFT CS** | GPIO 26 | SPI Chip Select |
| **TFT DC** | GPIO 22 | Data / Command Select |
| **TFT RST** | GPIO 27 | Hardware Reset |
| **Joystick X** | GPIO 32 | Analog Horizontal Input |
| **Joystick Y** | GPIO 36 | Analog Vertical Input |
| **Joystick SW**| GPIO 25 | Digital Press Button (Pull-Up) |
| **Serial RX/TX**| Default | UART Terminal Communication (115200 Baud) |

---

## 🛠️ Command Reference

ESH includes a suite of lightweight commands out-of-the-box:

- `cd <dir>`: Change current directory context.
- `ls [dir]`: List contents of absolute or relative directories along with folder markers (`/`) and file capacities in bytes (`B`).
- `cat <file>`: Print the contents of a text file directly to the display screens.
- `touch <file>`: Instantiates an empty file or updates timestamps.
- `mkdir <dir>`: Generates a single-level folder partition on the flash filesystem.
- `rm <file>`: Removes a target file from storage.
- `rmdir <dir>`: Deletes an *empty* folder directory (strips trailing slashes automatically).
- `cp <src> <dst>`: Copies raw binary or string blocks from a source path to a destination path.
- `mv <src> <dst>`: Relocates or renames files.
- `echo [args]`: Echoes text input directly back to standard outputs.
- `df`: Inspect total, used, and free storage spaces available on `LittleFS`.
- `tee <file>`: Interactive line-by-line file writing stream editor.
- `edit <file>`: Opens the advanced full-screen visual text editor.
- `help`: Lists all available commands.

---

## 📝 Micro Text Editor (`edit`) Guide

When launching the interactive `edit` utility, the terminal framework re-allocates your console rows into a dedicated editing buffer layout. 

### Hotkeys and Controls
- **Arrow Keys (or Analog Joystick):** Moves the interactive cursor (`|` block) seamlessly up, down, left, and right through the character array.
- **Enter Key (`\n`):** Breaks the line at your current cursor position, shifting subsequent rows down.
- **Backspace (`0x7F`):** Removes characters to the left of the cursor.
- **`Ctrl + D`:** Saves the current memory workspace to the flash device using a truncated output cycle and exits cleanly.
- **`Ctrl + C`:** Aborts the editor instantly, discarding all unsaved data and restoring standard prompt interfaces.

### TFT Word Wrapping
Lines exceeding your display grid column boundaries are wrapped to the subsequent physical screen row on the TFT console layout without splitting text strings in the file. A subtle dash marker (`-`) is automatically rendered over the margin break using `tft.drawFastHLine()` to indicate wrapped line states. 

---

## 💾 Installation & Setup

1. Open your Arduino IDE or PlatformIO workspace.
2. Ensure you have the core dependencies installed:
   - `Adafruit_GFX` Library
   - `Adafruit_ST7735` Library
3. Copy the codebase into your sketch layout file (e.g., `esh.ino`).
4. Set your terminal emulator application (such as **PuTTY**, **Tera Term**, or **Miniterm**) to **115200 Baud**.
5. *Crucial:* Enable **Implicit CR in every LF** under your terminal emulator's preferences panel to ensure newlines (`\n`) register a proper carriage return (`\r`) alignment hook, preventing staircase text artifacts.
6. Compile, upload to your ESP32, and hit the **EN (Reset)** button to boot ESH!
