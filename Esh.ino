// ESH - ESP32 Shell v0.2
// Pinout: TFT CS=26 DC=22 RST=27 | Joystick X=32 Y=36 SW=25
// Serial 115200 baud for full keyboard input
// LittleFS for filesystem storage

#pragma GCC diagnostic ignored "-fpermissive"

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <LittleFS.h>

// ---- Pin definitions ----
#define TFT_CS   26
#define TFT_RST  27
#define TFT_DC   22
#define JOY_X    32
#define JOY_Y    36
#define JOY_SW   25

// ---- TFT text grid (textSize=1: 6x8 per char, 160x128 screen) ----
#define TFT_CHAR_W  6
#define TFT_CHAR_H  8
#define TFT_COLS    26
#define TFT_ROWS    16

// ---- Shell limits ----
#define MAX_LINE   128
#define MAX_ARGS    16
#define ESH_EXT    ".esh"

// ---- Key codes ----
#define KEY_LEFT   0x11
#define KEY_RIGHT  0x12
#define KEY_UP     0x13
#define KEY_DOWN   0x14

// ---- Display Offscreen Corrections ----
#define TFT_OFFSET_X  2   // Global hardware X alignment padding
#define TFT_OFFSET_Y  3   // Global hardware Y alignment padding

#define ST7735_DARKGREY  0x4208  // Custom 16-bit RGB565 dark grey

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

int tftCol = 0;
int tftRow = 0;
volatile bool esh_killed = false;
bool joystickConnected = false;
static unsigned long lastJoyTime = 0;

// Global Working Directory
char cwd[MAX_LINE] = "/";

// ================================================================
// Line editor
// ================================================================

struct LineEditor {
    char buf[MAX_LINE];
    int  len;
    int  cur;
    int  tftLineRow;
    int  promptLen;
};

// ================================================================
// Path Management Helper
// ================================================================

void resolvePath(const char *input, char *output, size_t maxLen) {
    if (input[0] == '/') {
        // Absolute path input
        strncpy(output, input, maxLen - 1);
        output[maxLen - 1] = 0;
    } else {
        // Relative path input -> prepend current working directory
        if (strcmp(cwd, "/") == 0) {
            snprintf(output, maxLen, "/%s", input);
        } else {
            snprintf(output, maxLen, "%s/%s", cwd, input);
        }
    }
}

// ================================================================
// Joystick detection (stability-based, reused from arcade project)
// ================================================================

bool detectJoystick() {
    int minX = 4095, maxX = 0, minY = 4095, maxY = 0;
    for (int i = 0; i < 20; i++) {
        int x = analogRead(JOY_X);
        int y = analogRead(JOY_Y);
        if (x < minX) minX = x; if (x > maxX) maxX = x;
        if (y < minY) minY = y; if (y > maxY) maxY = y;
        delay(5);
    }
    int spreadX = maxX - minX;
    int spreadY = maxY - minY;
    Serial.print("Joystick probe X spread=");
    Serial.print(spreadX);
    Serial.print(" Y spread=");
    Serial.println(spreadY);
    return (spreadX < 100 && spreadY < 100);
}

// ================================================================
// Dual output: Serial + TFT
// ================================================================

void shellPutChar(char c) {
    Serial.write(c);
    if (c == '\n') {
        tftCol = 0;
        tftRow++;
        if (tftRow >= TFT_ROWS) {
            tft.fillScreen(ST7735_BLACK);
            tftRow = 0;
        }
    } else if (c == '\r') {
        tftCol = 0;
    } else {
        tft.drawChar( TFT_OFFSET_X + (tftCol * TFT_CHAR_W), TFT_OFFSET_Y + (tftRow * TFT_CHAR_H),
                     c, ST7735_WHITE, ST7735_BLACK, 1);
        tftCol++;
        if (tftCol >= TFT_COLS) {
            tftCol = 0;
            tftRow++;
            if (tftRow >= TFT_ROWS) {
                tft.fillScreen(ST7735_BLACK);
                tftRow = 0;
            }
        }
    }
}

void shellPrint(const char *s) {
    while (*s) shellPutChar(*s++);
}

void shellPrintLn(const char *s) {
    shellPrint(s);
    shellPutChar('\n');
}

void shellPrintInt(int n) {
    char buf[16];
    itoa(n, buf, 10);
    shellPrint(buf);
}


void le_init(LineEditor *le, int promptLen) {
    memset(le->buf, 0, sizeof(le->buf));
    le->len = 0;
    le->cur = 0;
    le->promptLen = promptLen;
}

void le_redrawTFT(LineEditor *le) {
    int startX = le->promptLen * TFT_CHAR_W;
    tft.fillRect(TFT_OFFSET_X + startX, 
                 TFT_OFFSET_Y + (le->tftLineRow * TFT_CHAR_H),
                 (TFT_COLS - le->promptLen) * TFT_CHAR_W, 
                 TFT_CHAR_H,
                 ST7735_BLACK);
    int col = le->promptLen;
    for (int i = 0; i < le->len; i++) {
        if (i == le->cur) {
            tft.drawChar(col * TFT_CHAR_W, le->tftLineRow * TFT_CHAR_H,
                         '|', ST7735_CYAN, ST7735_BLACK, 1);
            col++;
        }
        tft.drawChar(col * TFT_CHAR_W, le->tftLineRow * TFT_CHAR_H,
                     le->buf[i], ST7735_WHITE, ST7735_BLACK, 1);
        col++;
    }
    if (le->cur == le->len) {
        tft.drawChar(col * TFT_CHAR_W, le->tftLineRow * TFT_CHAR_H,
                     '|', ST7735_CYAN, ST7735_BLACK, 1);
    }
}

void le_redrawSerial(LineEditor *le) {
    for (int i = le->cur; i < le->len; i++)
        Serial.write(le->buf[i]);
    Serial.print("\x1B[K");
    int charsAfter = le->len - le->cur;
    if (charsAfter > 0) {
        Serial.print("\x1B[");
        Serial.print(charsAfter);
        Serial.print("D");
    }
}

void le_insert(LineEditor *le, char c) {
    if (le->len >= MAX_LINE - 1) return;
    memmove(&le->buf[le->cur + 1], &le->buf[le->cur], le->len - le->cur);
    le->buf[le->cur] = c;
    le->len++;
    le->cur++;
    Serial.write(c);
    le_redrawSerial(le);
    le_redrawTFT(le);
}

void le_backspace(LineEditor *le) {
    if (le->cur == 0) return;
    memmove(&le->buf[le->cur - 1], &le->buf[le->cur], le->len - le->cur);
    le->len--;
    le->cur--;
    le->buf[le->len] = 0;
    Serial.print("\x1B[D");
    Serial.print("\x1B[K");
    le_redrawSerial(le);
    le_redrawTFT(le);
}

void le_left(LineEditor *le) {
    if (le->cur == 0) return;
    le->cur--;
    Serial.print("\x1B[D");
    le_redrawTFT(le);
}

void le_right(LineEditor *le) {
    if (le->cur >= le->len) return;
    le->cur++;
    Serial.print("\x1B[C");
    le_redrawTFT(le);
}

// ================================================================
// Input Parsing & Key Management
// ================================================================

int readKey() {
    if (Serial.available()) {
        int c = Serial.read();
        if (c == 0x1B) {
            delay(5);
            if (Serial.available() && Serial.read() == '[') {
                delay(5);
                if (Serial.available()) {
                    int code = Serial.read();
                    if (code == 'A') return KEY_UP;
                    if (code == 'B') return KEY_DOWN;
                    if (code == 'C') return KEY_RIGHT;
                    if (code == 'D') return KEY_LEFT;
                }
            }
            return 0x1B;
        }
        if (c == 127 || c == 8) return 0x7F;
        if (c == '\r') return '\n';
        return c;
    }

    if (joystickConnected && millis() - lastJoyTime > 200) {
        int x = analogRead(JOY_X);
        int y = analogRead(JOY_Y);
        if (x < 1500) { lastJoyTime = millis(); return KEY_LEFT;  }
        if (x > 2500) { lastJoyTime = millis(); return KEY_RIGHT; }
        if (y < 1500) { lastJoyTime = millis(); return KEY_UP;    }
        if (y > 2500) { lastJoyTime = millis(); return KEY_DOWN;  }
    }
    return 0;
}

int waitKey() {
    int k;
    while ((k = readKey()) == 0);
    return k;
}

int readLine(char *buf, int maxlen, const char *prompt) {
    LineEditor le;
    le_init(&le, strlen(prompt)); // Clear variables first

    // Print prompt. If prompt is long, shellPrint automatically increments tftRow
    shellPrint(prompt); 
    
    // NOW capture the absolute row the text cursor is resting on
    le.tftLineRow = tftRow; 

    while (true) {
        int k = waitKey();
        if (esh_killed) return -1;

        if (k == 0x03) {
            esh_killed = true;
            shellPrintLn("^C");
            return -1;
        }
        if (k == 0x04) {
            if (le.len == 0) return -2;
            continue;
        }
        if (k == '\n') {
            shellPutChar('\n'); 
            memcpy(buf, le.buf, le.len);
            buf[le.len] = 0;
            return le.len;
        }
        if (k == 0x7F)           { le_backspace(&le); continue; }
        if (k == KEY_LEFT)       { le_left(&le);      continue; }
        if (k == KEY_RIGHT)      { le_right(&le);     continue; }
        if (k == KEY_UP || k == KEY_DOWN) continue;
        if (k >= 0x20 && k < 0x7F && le.len < maxlen - 1)
            le_insert(&le, (char)k);
    }
}
int parseArgs(char *line, char *argv[], int maxargs) {
    int argc = 0;
    char *p = line;
    while (*p && argc < maxargs) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    return argc;
}

// ================================================================
// Built-in commands
// ================================================================

void cmd_cd(int argc, char *argv[]) {
    char target[MAX_LINE];
    if (argc < 2) {
        strcpy(target, "/");
    } else {
        resolvePath(argv[1], target, sizeof(target));
    }

    // Strip trailing slash unless it's just root "/"
    size_t len = strlen(target);
    if (len > 1 && target[len - 1] == '/') {
        target[len - 1] = 0;
    }

    // Verify target exists and is a directory
    File dir = LittleFS.open(target, "r");
    if (!dir || !dir.isDirectory()) {
        shellPrint("cd: no such directory: ");
        shellPrintLn(target);
        if (dir) dir.close();
        return;
    }
    dir.close();

    strncpy(cwd, target, sizeof(cwd) - 1);
    cwd[sizeof(cwd) - 1] = 0;
}

// ================================================================
// Micro Text Editor (Visual Screen Editor)
// ^D to save and exit, ^C to abort without saving
// ================================================================

#define EDIT_MAX_ROWS 14

void cmd_edit(int argc, char *argv[]) {
    if (argc < 2) { shellPrintLn("Usage: edit <file>"); return; }
    char path[MAX_LINE];
    resolvePath(argv[1], path, sizeof(path));

    // 1. Query Terminal Size over Serial
    while(Serial.available()) Serial.read();
    Serial.print("\x1B[19t");
    Serial.flush();

    int serialRows = EDIT_MAX_ROWS;
    int serialCols = TFT_COLS;

    unsigned long startMilli = millis();
    while (millis() - startMilli < 300) {
        if (Serial.available() >= 5) {
            if (Serial.read() == 0x1B && Serial.read() == '[') {
                if (Serial.read() == '9' && Serial.read() == ';') {
                    serialRows = Serial.parseInt();
                    if (Serial.read() == ';') {
                        serialCols = Serial.parseInt();
                    }
                    break;
                }
            }
        }
    }

    int maxEditRows = (serialRows - 2 > EDIT_MAX_ROWS) ? EDIT_MAX_ROWS : (serialRows - 2);
    if (maxEditRows < 5) maxEditRows = 5; 
    int maxEditCols = (serialCols < TFT_COLS) ? serialCols : TFT_COLS;

    char text[EDIT_MAX_ROWS][TFT_COLS + 1];
    int totalRows = 0;
    memset(text, 0, sizeof(text));

    if (LittleFS.exists(path)) {
        File f = LittleFS.open(path, "r");
        while (f.available() && totalRows < maxEditRows) {
            int len = f.readBytesUntil('\n', text[totalRows], maxEditCols - 1);
            text[totalRows][len] = 0;
            if (len > 0 && text[totalRows][len - 1] == '\r') {
                text[totalRows][len - 1] = 0;
            }
            totalRows++;
        }
        f.close();
    }

    if (totalRows == 0) totalRows = 1;

    int curR = 0;
    int curC = 0;

    auto refreshUI = [&]() {
        // --- Redraw TFT Screen Layout ---
        tft.fillScreen(ST7735_BLACK);
        
        // 1. Header (Row 0)
        tft.fillRect(0, 0, 160, TFT_CHAR_H, ST7735_BLUE);
        for(int i=0; i<TFT_COLS; i++) {
            char hc = (i < 7) ? "^D:Save"[i] : (i < 15) ? " ^C:Exit"[i-7] : ' ';
            tft.drawChar((i * TFT_CHAR_W), TFT_OFFSET_Y, hc, ST7735_WHITE, ST7735_BLUE, 1);
        }

        // 2. Draw TFT Body with Dynamic Wrapping Tracking
        int currentTftVisualRow = 1; 
        int targetCursorVisualRow = 1;
        int targetCursorVisualCol = 0;

        int safeScreenWidth = 160 - (TFT_OFFSET_X * 2);

        // Max wrap limit per line to prevent overrunning your layout
        int wrapWidth = safeScreenWidth / TFT_CHAR_W;
        if (wrapWidth > TFT_COLS) wrapWidth = TFT_COLS;

        for (int r = 0; r < totalRows; r++) {
            int len = strlen(text[r]);
            
            if (len == 0) {
                // If this is the active row, capture cursor coordinates
                if (r == curR) {
                    targetCursorVisualRow = currentTftVisualRow;
                    targetCursorVisualCol = 0;
                }
                currentTftVisualRow++;
                continue;
            }

            for (int c = 0; c < len; c++) {
                int visualCol = c % wrapWidth;
                int subLine = c / wrapWidth;

                // Handle cursor geometry tracking
                if (r == curR && c == curC) {
                    targetCursorVisualRow = currentTftVisualRow + subLine;
                    targetCursorVisualCol = visualCol;
                }

                // If we reach the end of a wrapped block segment, render your long custom break dash
                if (c > 0 && visualCol == 0) {
                    int prevLineRow = currentTftVisualRow + subLine - 1;
                    int dashX = TFT_OFFSET_X + (wrapWidth * TFT_CHAR_W) + 2;
                    int dashY = TFT_OFFSET_Y + (prevLineRow * TFT_CHAR_H) + (TFT_CHAR_H / 2);
                    tft.drawFastHLine(dashX, dashY, 6, ST7735_DARKGREY); // Draws the line break indicator dash
                }

                // Paint wrapped character data onto screen
                if ((currentTftVisualRow + subLine) < 15) {
                    tft.drawChar(TFT_OFFSET_X + (visualCol * TFT_CHAR_W), 
                                 TFT_OFFSET_Y + ((currentTftVisualRow + subLine) * TFT_CHAR_H), 
                                 text[r][c], ST7735_WHITE, ST7735_BLACK, 1);
                }
            }

            // Capture trailing end-of-string cursor locations
            if (r == curR && curC == len) {
                targetCursorVisualRow = currentTftVisualRow + (len / wrapWidth);
                targetCursorVisualCol = len % wrapWidth;
            }

            // Increment line index stepping factor
            currentTftVisualRow += (len / wrapWidth) + 1;
        }

        // 3. Draw TFT Bottom Status Bar (Row 15 remains fixed)
        tft.fillRect(0, 15 * TFT_CHAR_H, 160, TFT_CHAR_H, ST7735_DARKGREY);
        char status[24];
        snprintf(status, sizeof(status), "Row:%d Col:%d", curR + 1, curC + 1);
        for(int i=0; status[i] != 0; i++) {
            tft.drawChar(TFT_OFFSET_X + (i * TFT_CHAR_W), TFT_OFFSET_Y + (15 * TFT_CHAR_H), status[i], ST7735_WHITE, ST7735_DARKGREY, 1);
        }

        // 4. Highlight Wrapped TFT Cursor Location safely
        if (targetCursorVisualRow < 15) {
            char cursorChar = (curC < strlen(text[curR])) ? text[curR][curC] : ' ';
            tft.drawChar(TFT_OFFSET_X + (targetCursorVisualCol * TFT_CHAR_W), 
                         TFT_OFFSET_Y + (targetCursorVisualRow * TFT_CHAR_H), 
                         cursorChar, ST7735_BLACK, ST7735_CYAN, 1);
        }

        // --- Redraw Serial Terminal Layout (Untouched raw buffer fallback) ---
        Serial.print("\x1B[2J\x1B[H"); 
        
        Serial.print("\x1B[37;44m");
        char serialHeader[MAX_LINE];
        snprintf(serialHeader, maxEditCols, " ESH Editor: %s (^D Save, ^C Exit)", argv[1]);
        Serial.print(serialHeader);
        Serial.print("\x1B[K\x1B[0m\r\n");

        for (int r = 0; r < maxEditRows; r++) {
            if (r < totalRows) {
                Serial.print(text[r]);
            }
            Serial.print("\x1B[K\r\n");
        }
        for (int r = totalRows; r < maxEditRows; r++) {
            Serial.print("~\x1B[K\r\n");
        }

        Serial.print("\x1B[37;100m");
        Serial.print(status);
        Serial.print("\x1B[K\x1B[0m");

        // Maintain expected continuous line coordinates on serial link terminal
        Serial.print("\x1B["); Serial.print(curR + 2); Serial.print(";"); Serial.print(curC + 1); Serial.print("H");
    };

    refreshUI();

    while (true) {
        int k = waitKey();

        if (k == 0x03) { // ^C
            Serial.print("\x1B[2J\x1B[H"); 
            shellPrintLn("Editor closed. Changes discarded.");
            return;
        }

        if (k == 0x04) { // ^D
            File f = LittleFS.open(path, "w");
            if (!f) { 
                Serial.print("\x1B[2J\x1B[H");
                shellPrintLn("Error: Could not save file!"); 
                return; 
            }
            for (int r = 0; r < totalRows; r++) {
                if (r == totalRows - 1 && strlen(text[r]) == 0) break;
                f.print(text[r]);
                f.print('\n');
            }
            f.close();
            Serial.print("\x1B[2J\x1B[H");
            shellPrintLn("File successfully written!");
            return;
        }

        if (k == KEY_UP && curR > 0) {
            curR--;
            if (curC > strlen(text[curR])) curC = strlen(text[curR]);
            refreshUI(); continue;
        }
        if (k == KEY_DOWN) {
            if (curR < totalRows - 1) {
                curR++;
                if (curC > strlen(text[curR])) curC = strlen(text[curR]);
                refreshUI(); continue;
            } else if (totalRows < maxEditRows) {
                totalRows++;
                curR++;
                curC = 0;
                refreshUI(); continue;
            }
        }
        if (k == KEY_LEFT && curC > 0) {
            curC--; refreshUI(); continue;
        }
        if (k == KEY_RIGHT && curC < strlen(text[curR]) && curC < maxEditCols - 1) {
            curC++; refreshUI(); continue;
        }

        if (k == 0x7F) { // Backspace
            int len = strlen(text[curR]);
            if (curC > 0) {
                memmove(&text[curR][curC - 1], &text[curR][curC], len - curC + 1);
                curC--;
                refreshUI();
            }
            continue;
        }

        if (k == '\n') { // Enter split logic
            if (totalRows < maxEditRows) {
                for (int i = totalRows; i > curR + 1; i--) {
                    strcpy(text[i], text[i - 1]);
                }
                strcpy(text[curR + 1], &text[curR][curC]);
                text[curR][curC] = 0;
                totalRows++;
                curR++;
                curC = 0;
                refreshUI();
            }
            continue;
        }

        if (k >= 0x20 && k < 0x7F) {
            int len = strlen(text[curR]);
            if (len < maxEditCols - 1) {
                memmove(&text[curR][curC + 1], &text[curR][curC], len - curC + 1);
                text[curR][curC] = (char)k;
                curC++;
                refreshUI();
            }
        }
    }
}
void cmd_ls(int argc, char *argv[]) {
    char path[MAX_LINE];
    resolvePath((argc > 1) ? argv[1] : cwd, path, sizeof(path));

    File dir = LittleFS.open(path);
    if (!dir || !dir.isDirectory()) {
        shellPrintLn("ls: not a directory");
        return;
    }
    File f = dir.openNextFile();
    while (f) {
        if (esh_killed) return;
        shellPrint(f.name());
        if (f.isDirectory()) {
            shellPutChar('/');
        } else {
            shellPrint("  ");
            shellPrintInt((int)f.size());
            shellPrint("B");
        }
        shellPutChar('\n');
        f = dir.openNextFile();
    }
}

void cmd_cat(int argc, char *argv[]) {
    if (argc < 2) { shellPrintLn("Usage: cat <file>"); return; }
    char path[MAX_LINE];
    resolvePath(argv[1], path, sizeof(path));

    File f = LittleFS.open(path, "r");
    if (!f) { shellPrint("cat: cannot open "); shellPrintLn(path); return; }
    while (f.available()) {
        if (esh_killed) break;
        shellPutChar((char)f.read());
    }
    f.close();
}

void cmd_touch(int argc, char *argv[]) {
    if (argc < 2) { shellPrintLn("Usage: touch <file>"); return; }
    char path[MAX_LINE];
    resolvePath(argv[1], path, sizeof(path));

    File f = LittleFS.open(path, "a");
    if (!f) { shellPrint("touch: failed: "); shellPrintLn(path); return; }
    f.close();
}

void cmd_mkdir(int argc, char *argv[]) {
    if (argc < 2) { shellPrintLn("Usage: mkdir <dir>"); return; }
    char path[MAX_LINE];
    resolvePath(argv[1], path, sizeof(path));

    if (!LittleFS.mkdir(path)) {
        shellPrint("mkdir: failed: "); shellPrintLn(path);
    }
}

void cmd_rm(int argc, char *argv[]) {
    if (argc < 2) { shellPrintLn("Usage: rm <file>"); return; }
    char path[MAX_LINE];
    resolvePath(argv[1], path, sizeof(path));

    if (!LittleFS.remove(path)) {
        shellPrint("rm: failed: "); shellPrintLn(path);
    }
}

void cmd_rmdir(int argc, char *argv[]) {
    if (argc < 2) { shellPrintLn("Usage: rmdir <dir>"); return; }
    char path[MAX_LINE];
    resolvePath(argv[1], path, sizeof(path));

    if (!LittleFS.rmdir(path)) {
        shellPrint("rmdir: failed: "); shellPrintLn(path);
    }
}

void cmd_echo(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) shellPutChar(' ');
        shellPrint(argv[i]);
    }
    shellPutChar('\n');
}

void cmd_df(int argc, char *argv[]) {
    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    shellPrint("Total: "); shellPrintInt((int)total); shellPrintLn("B");
    shellPrint("Used:  "); shellPrintInt((int)used);  shellPrintLn("B");
    shellPrint("Free:  "); shellPrintInt((int)(total - used)); shellPrintLn("B");
}

void cmd_cp(int argc, char *argv[]) {
    if (argc < 3) { shellPrintLn("Usage: cp <src> <dst>"); return; }
    char srcPath[MAX_LINE];
    char dstPath[MAX_LINE];
    resolvePath(argv[1], srcPath, sizeof(srcPath));
    resolvePath(argv[2], dstPath, sizeof(dstPath));

    File src = LittleFS.open(srcPath, "r");
    if (!src) { shellPrint("cp: cannot open "); shellPrintLn(srcPath); return; }
    File dst = LittleFS.open(dstPath, "w");
    if (!dst) {
        shellPrint("cp: cannot write "); shellPrintLn(dstPath);
        src.close(); return;
    }
    uint8_t buf[64];
    while (src.available()) {
        if (esh_killed) break;
        int n = src.read(buf, sizeof(buf));
        dst.write(buf, n);
    }
    src.close();
    dst.close();
}

void cmd_mv(int argc, char *argv[]) {
    if (argc < 3) { shellPrintLn("Usage: mv <src> <dst>"); return; }
    cmd_cp(argc, argv);
    if (!esh_killed) {
        char srcPath[MAX_LINE];
        resolvePath(argv[1], srcPath, sizeof(srcPath));
        LittleFS.remove(srcPath);
    }
}

void cmd_tee(int argc, char *argv[]) {
    if (argc < 2) { shellPrintLn("Usage: tee <file>"); return; }
    char path[MAX_LINE];
    resolvePath(argv[1], path, sizeof(path));

    File f = LittleFS.open(path, "w");
    if (!f) { shellPrint("tee: cannot open "); shellPrintLn(path); return; }

    shellPrint("tee > ");
    shellPrint(path);
    shellPrintLn(" (^D save+exit, ^C abort)");

    int totalBytes = 0;
    while (true) {
        LineEditor le;
        le_init(&le, 0);

        while (true) {
            int k = waitKey();
            if (k == 0x03) {
                f.close();
                esh_killed = true;
                shellPrintLn("\n^C");
                return;
            }
            if (k == 0x04) {
                if (le.len > 0) {
                    f.write((uint8_t*)le.buf, le.len);
                    f.write((uint8_t)'\n');
                    totalBytes += le.len + 1;
                    shellPutChar('\n');
                }
                f.close();
                shellPrintInt(totalBytes);
                shellPrint(" bytes written to ");
                shellPrintLn(path);
                return;
            }
            if (k == '\n') {
                le.buf[le.len] = '\n';
                f.write((uint8_t*)le.buf, le.len + 1);
                totalBytes += le.len + 1;
                shellPutChar('\n');
                tftRow++;
                tftCol = 0;
                break;
            }
            if (k == 0x7F)           { le_backspace(&le); continue; }
            if (k == KEY_LEFT)       { le_left(&le);      continue; }
            if (k == KEY_RIGHT)      { le_right(&le);     continue; }
            if (k == KEY_UP || k == KEY_DOWN) continue;
            if (k >= 0x20 && k < 0x7F && le.len < MAX_LINE - 1)
                le_insert(&le, (char)k);
        }
    }
}

void cmd_help(int argc, char *argv[]) {
    shellPrintLn("ESH v0.2 - ESP32 Shell");
    shellPrintLn("Commands:");
    shellPrintLn("  cd <dir>   ls [dir]");
    shellPrintLn("  cat <file> touch <file>");
    shellPrintLn("  mkdir <dir> rm <file>");
    shellPrintLn("  rmdir <dir> cp <src> <dst>");
    shellPrintLn("  mv <src> <dst> echo [args]");
    shellPrintLn("  df         tee <file>");
    shellPrintLn("  <script>   (runs /<script>.esh)");
}

// ================================================================
// ESH script runner
// ================================================================

void execLine(char *line); 

void runScript(const char *path) {
    File f = LittleFS.open(path, "r");
    if (!f) { shellPrint("esh: cannot open "); shellPrintLn(path); return; }
    char line[MAX_LINE];
    int i = 0;
    while (f.available() && !esh_killed) {
        char c = (char)f.read();
        if (c == '\n' || c == '\r') {
            line[i] = 0;
            if (i > 0) execLine(line);
            i = 0;
        } else if (i < MAX_LINE - 1) {
            line[i++] = c;
        }
    }
    if (i > 0 && !esh_killed) {
        line[i] = 0;
        execLine(line);
    }
    f.close();
}

// ================================================================
// Command dispatch
// ================================================================

void execLine(char *line) {
    if (line[0] == '#' || line[0] == 0) return;

    char copy[MAX_LINE];
    strncpy(copy, line, MAX_LINE - 1);
    copy[MAX_LINE - 1] = 0;

    char *argv[MAX_ARGS];
    int argc = parseArgs(copy, argv, MAX_ARGS);
    if (argc == 0) return;

    esh_killed = false;

    if      (strcmp(argv[0], "cd")    == 0) cmd_cd(argc, argv);
    else if (strcmp(argv[0], "ls")    == 0) cmd_ls(argc, argv);
    else if (strcmp(argv[0], "cat")   == 0) cmd_cat(argc, argv);
    else if (strcmp(argv[0], "touch") == 0) cmd_touch(argc, argv);
    else if (strcmp(argv[0], "mkdir") == 0) cmd_mkdir(argc, argv);
    else if (strcmp(argv[0], "rm")    == 0) cmd_rm(argc, argv);
    else if (strcmp(argv[0], "rmdir") == 0) cmd_rmdir(argc, argv);
    else if (strcmp(argv[0], "echo")  == 0) cmd_echo(argc, argv);
    else if (strcmp(argv[0], "df")    == 0) cmd_df(argc, argv);
    else if (strcmp(argv[0], "cp")    == 0) cmd_cp(argc, argv);
    else if (strcmp(argv[0], "mv")    == 0) cmd_mv(argc, argv);
    else if (strcmp(argv[0], "tee")   == 0) cmd_tee(argc, argv);
    else if (strcmp(argv[0], "edit")  == 0) cmd_edit(argc, argv);
    else if (strcmp(argv[0], "help")  == 0) cmd_help(argc, argv);
    else {
        // Global PATH handler looking directly inside the root directory '/'
        char scriptPath[MAX_LINE];
        size_t nameLen = strlen(argv[0]);
        bool hasExt = (nameLen > 4 && strcmp(&argv[0][nameLen - 4], ESH_EXT) == 0);

        if (hasExt) {
            snprintf(scriptPath, sizeof(scriptPath), "/%s", argv[0]);
        } else {
            snprintf(scriptPath, sizeof(scriptPath), "/%s%s", argv[0], ESH_EXT);
        }

        if (LittleFS.exists(scriptPath)) {
            runScript(scriptPath);
        } else {
            shellPrint("esh: command not found: ");
            shellPrintLn(argv[0]);
        }
    }
}

// ================================================================
// Setup and main loop
// ================================================================

void setup() {
    Serial.begin(115200);

    pinMode(JOY_SW, INPUT_PULLUP);
    pinMode(JOY_X,  INPUT);
    pinMode(JOY_Y,  INPUT);

    joystickConnected = detectJoystick();
    Serial.print("Joystick: ");
    Serial.println(joystickConnected ? "YES" : "NO (serial-only mode)");

    tft.initR(INITR_BLACKTAB);
    tft.setRotation(1);
    tft.fillScreen(ST7735_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(ST7735_WHITE, ST7735_BLACK);

    if (!LittleFS.begin(true)) {
        shellPrintLn("LittleFS mount failed!");
        while (1) delay(1000);
    }

    shellPrintLn("ESH v0.2 - ESP32 Shell");
    shellPrintLn("Type 'help' for commands");
    shellPutChar('\n');
}

void loop() {
    char line[MAX_LINE];
    char prompt[MAX_LINE + 8];
    
    // Format dynamic prompt using current working directory 
    snprintf(prompt, sizeof(prompt), "%s> ", cwd);

    int r = readLine(line, MAX_LINE, prompt);
    if (r < 0) {
        esh_killed = false;
        return;
    }
    if (r == 0) return; 
    execLine(line);
}