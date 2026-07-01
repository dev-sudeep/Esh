#ifndef ESHBIN_HPP
#define ESHBIN_HPP

#include <Arduino.h>
#include <LittleFS.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <vector>
#include <string>

// 1. Core Metadata Arrays for Automated Configuration
const int COMMAND_COUNT = 15; // Changed from 12

const String CMDS[COMMAND_COUNT] = {
    "ls", "cat", "touch", "mkdir", "rm", 
    "rmdir", "cp", "mv", "echo", "df", 
    "tee", "pin",
    "clear", "rect", "line" // New graphics commands
};

const String USAGE[COMMAND_COUNT] = {
    "Usage: ls [dir]",
    "Usage: cat <file>",
    "Usage: touch <file>",
    "Usage: mkdir <dir>",
    "Usage: rm <file>",
    "Usage: rmdir <dir>",
    "Usage: cp <src> <dst>",
    "Usage: mv <src> <dst>",
    "Usage: echo [text]",
    "Usage: df (Displays storage metrics)",
    "Usage: tee <file> (Stream input to file)",
    "Usage: pin mode|write|read <gpio> [val]",
    "Usage: clear (Resets the console screen)",
    "Usage: rect <x> <y> <w> <h> [fill_color_hex]",
    "Usage: line <x1> <y1> <x2> <y2> [color_hex]"
};

// 2. External Command Implementations

// 3. Graphics Extensions
void cmd_clear(int argc, char *argv[]) {
    tft.fillScreen(ST7735_BLACK);
    tftCursorX = 2; // Reset coordinate system variables
    tftCursorY = 2;
}

void cmd_rect(int argc, char *argv[]) {
    if (argc < 5) { shellPrintLn(USAGE[13].c_str()); return; }
    int x = atoi(argv[1]);
    int y = atoi(argv[2]);
    int w = atoi(argv[3]);
    int h = atoi(argv[4]);
    
    // Parse optional color (default to White if not supplied)
    uint16_t color = ST7735_WHITE;
    if (argc >= 6) color = (uint16_t)strtol(argv[5], NULL, 16);
    
    tft.fillRect(x, y, w, h, color);
}

void cmd_line(int argc, char *argv[]) {
    if (argc < 5) { shellPrintLn(USAGE[14].c_str()); return; }
    int x1 = atoi(argv[1]);
    int y1 = atoi(argv[2]);
    int x2 = atoi(argv[3]);
    int y2 = atoi(argv[4]);
    
    uint16_t color = ST7735_WHITE;
    if (argc >= 6) color = (uint16_t)strtol(argv[5], NULL, 16);
    
    tft.drawLine(x1, y1, x2, y2, color);
}

void cmd_ls(int argc, char *argv[]) {
    char target[128];
    if (argc > 1) resolvePath(argv[1], target, sizeof(target));
    else strcpy(target, cwd);

    File root = LittleFS.open(target, "r");
    if (!root || !root.isDirectory()) {
        shellPrintLn("Error: Cannot open target directory.");
        return;
    }

    File file = root.openNextFile();
    while (file) {
        shellPrint(file.name());
        if (file.isDirectory()) {
            shellPrintLn("/");
        } else {
            shellPrint(" \t[");
            shellPrint(String(file.size()).c_str());
            shellPrintLn(" B]");
        }
        file = root.openNextFile();
    }
    root.close();
}

void cmd_cat(int argc, char *argv[]) {
    if (argc < 2) { shellPrintLn(USAGE[1].c_str()); return; }
    char path[128]; resolvePath(argv[1], path, sizeof(path));
    File f = LittleFS.open(path, "r");
    if (!f || f.isDirectory()) { shellPrintLn("Error opening file."); return; }
    while (f.available()) {
        char buf[64];
        int numRead = f.readBytes(buf, sizeof(buf) - 1);
        buf[numRead] = '\0';
        shellPrint(buf);
    }
    shellPrintLn("");
    f.close();
}

void cmd_touch(int argc, char *argv[]) {
    if (argc < 2) { shellPrintLn(USAGE[2].c_str()); return; }
    char path[128]; resolvePath(argv[1], path, sizeof(path));
    File f = LittleFS.open(path, "w");
    if (!f) { shellPrintLn("Error writing file."); return; }
    f.close();
}

void cmd_mkdir(int argc, char *argv[]) {
    if (argc < 2) { shellPrintLn(USAGE[3].c_str()); return; }
    char path[128]; resolvePath(argv[1], path, sizeof(path));
    if (LittleFS.mkdir(path)) {
        shellPrint("Directory initialized at: "); shellPrintLn(path);
    } else { shellPrintLn("Error creating directory."); }
}

void cmd_rm(int argc, char *argv[]) {
    if (argc < 2) { shellPrintLn(USAGE[4].c_str()); return; }
    char path[128]; resolvePath(argv[1], path, sizeof(path));
    if (LittleFS.remove(path)) shellPrintLn("File deleted.");
    else shellPrintLn("Error removing file.");
}

void cmd_rmdir(int argc, char *argv[]) {
    if (argc < 2) { shellPrintLn(USAGE[5].c_str()); return; }
    char path[128]; resolvePath(argv[1], path, sizeof(path));
    if (LittleFS.rmdir(path)) shellPrintLn("Directory unlinked.");
    else shellPrintLn("Error unlinking directory.");
}

void cmd_cp(int argc, char *argv[]) {
    if (argc < 3) { shellPrintLn(USAGE[6].c_str()); return; }
    char srcPath[128], dstPath[128];
    resolvePath(argv[1], srcPath, sizeof(srcPath));
    resolvePath(argv[2], dstPath, sizeof(dstPath));
    File src = LittleFS.open(srcPath, "r");
    File dst = LittleFS.open(dstPath, "w");
    if (!src || !dst) {
        shellPrintLn("IO Layer Error on copy reference.");
        if (src) src.close(); if (dst) dst.close(); return;
    }
    uint8_t buffer[128];
    while (src.available()) {
        size_t bytes = src.read(buffer, sizeof(buffer));
        dst.write(buffer, bytes);
    }
    src.close(); dst.close();
}

void cmd_mv(int argc, char *argv[]) {
    if (argc < 3) { shellPrintLn(USAGE[7].c_str()); return; }
    char srcPath[128], dstPath[128];
    resolvePath(argv[1], srcPath, sizeof(srcPath));
    resolvePath(argv[2], dstPath, sizeof(dstPath));
    if (LittleFS.rename(srcPath, dstPath)) shellPrintLn("Resource relocated.");
    else shellPrintLn("File reallocation failure.");
}

void cmd_echo(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        shellPrint(argv[i]);
        if (i < argc - 1) shellPrint(" ");
    }
    shellPrintLn("");
}

void cmd_df(int argc, char *argv[]) {
    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();
    shellPrint("Storage Free Space:\nTotal: ");
    shellPrint(String(total).c_str());
    shellPrint(" B\nUsed:  ");
    shellPrint(String(used).c_str());
    shellPrint(" B\nFree:  ");
    shellPrint(String(total - used).c_str());
    shellPrintLn(" B");
}

void cmd_tee(int argc, char *argv[]) {
    if (argc < 2) { shellPrintLn(USAGE[10].c_str()); return; }
    char path[128]; resolvePath(argv[1], path, sizeof(path));
    File f = LittleFS.open(path, "w");
    if (!f) { shellPrintLn("Target invalid."); return; }
    shellPrintLn("Streaming inputs to flash block. [Empty line to exit]");
    while (true) {
        while (!Serial.available());
        String input = Serial.readStringUntil('\n');
        input.trim();
        if (input.length() == 0) break;
        f.println(input);
        shellPrintLn(">> Written");
    }
    f.close();
}

void cmd_pin(int argc, char *argv[]) {
    if (argc < 3) { shellPrintLn(USAGE[11].c_str()); return; }
    char *action = argv[1];
    int gpio = atoi(argv[2]);
    if (gpio < 0 || gpio > 39) { shellPrintLn("Error: Invalid Pin."); return; }

    if (strcmp(action, "mode") == 0 && argc >= 4) {
        if (strcmp(argv[3], "INPUT") == 0) pinMode(gpio, INPUT);
        else if (strcmp(argv[3], "OUTPUT") == 0) pinMode(gpio, OUTPUT);
        else if (strcmp(argv[3], "INPUT_PULLUP") == 0) pinMode(gpio, INPUT_PULLUP);
        shellPrintLn("Hardware state mapped.");
    } else if (strcmp(action, "write") == 0 && argc >= 4) {
        int state = (strcmp(argv[3], "HIGH") == 0 || strcmp(argv[3], "1") == 0) ? HIGH : LOW;
        digitalWrite(gpio, state);
        shellPrintLn("Logic signal deployed.");
    } else if (strcmp(action, "read") == 0) {
        shellPrintLn(digitalRead(gpio) == HIGH ? "1" : "0");
    }
}

// Define a clean type blueprint for our command functions
typedef void (*CmdFunction)(int, char**);

// Function pointer table mapped in the exact parallel index order of CMDS[]
const CmdFunction CMD_FUNCTIONS[COMMAND_COUNT] = {
    cmd_ls, cmd_cat, cmd_touch, cmd_mkdir, cmd_rm, 
    cmd_rmdir, cmd_cp, cmd_mv, cmd_echo, cmd_df, 
    cmd_tee, cmd_pin, cmd_clear, cmd_rect, cmd_line
};

#endif