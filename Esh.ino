#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <vector>
#include <string>

#define TFT_CS         26
#define TFT_DC         22
#define TFT_RST        27
#define JOY_SW         25
#define JOY_X          32
#define JOY_Y          36
#define MAX_LINE       128
#define MAX_ARGS       16

#define TFT_WIDTH      160
#define TFT_HEIGHT     128
#define TFT_OFFSET_X   2
#define TFT_OFFSET_Y   2
#define CHAR_WIDTH     6
#define CHAR_HEIGHT    8

#define HISTORY_SIZE 10

char cmdHistory[HISTORY_SIZE][MAX_LINE];
int historyCount = 0;   // Total valid commands stored so far
int historyWriteIdx = 0; // Where the next command will be written
int historyViewIdx = -1; // Current active browse index (-1 means not browsing)

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

// Global Variables
struct EnvVar { std::string name; std::string value; };
std::vector<EnvVar> envTable;
char cwd[MAX_LINE] = "/";
int tftCursorX = TFT_OFFSET_X;
int tftCursorY = TFT_OFFSET_Y;

// Control Flow Structs
struct LoopState {
    long filePosition;    // Where the while loop line starts in the file
    std::string condition; // The raw condition string to re-evaluate
};

std::vector<LoopState> loopStack;
bool skipExecution = false; 
int skipDepth = 0; // Tracks nested ifs when skipping

// --- 1. CORE DEPENDENCIES REQUIRED BY ESHBIN ---
void shellPrint(const char* text) {
    Serial.print(text);
    while (*text) {
        char c = *text++;
        if (c == '\n') {
            tftCursorX = TFT_OFFSET_X; tftCursorY += CHAR_HEIGHT;
        } else if (c == '\r') {
            tftCursorX = TFT_OFFSET_X;
        } else if (c >= 32 && c <= 126) {
            if (tftCursorX + CHAR_WIDTH > TFT_WIDTH - TFT_OFFSET_X) {
                tft.drawFastHLine(TFT_WIDTH - TFT_OFFSET_X - 4, tftCursorY + (CHAR_HEIGHT / 2), 4, ST7735_BLUE);
                tftCursorX = TFT_OFFSET_X; tftCursorY += CHAR_HEIGHT;
            }
            if (tftCursorY + CHAR_HEIGHT > TFT_HEIGHT - TFT_OFFSET_Y) {
                tft.fillScreen(ST7735_BLACK); tftCursorY = TFT_OFFSET_Y; tftCursorX = TFT_OFFSET_X;
            }
            tft.drawChar(tftCursorX, tftCursorY, c, ST7735_WHITE, ST7735_BLACK, 1);
            tftCursorX += CHAR_WIDTH;
        }
    }
}

void shellPrintLn(const char* text) { shellPrint(text); shellPrint("\n"); }

void shellBackspace() {
    Serial.print("\b \b");
    if (tftCursorX >= TFT_OFFSET_X + CHAR_WIDTH) {
        tftCursorX -= CHAR_WIDTH;
    } else if (tftCursorY >= TFT_OFFSET_Y + CHAR_HEIGHT) {
        tftCursorY -= CHAR_HEIGHT;
        tftCursorX = TFT_WIDTH - TFT_OFFSET_X - CHAR_WIDTH;
    }
    tft.fillRect(tftCursorX, tftCursorY, CHAR_WIDTH, CHAR_HEIGHT, ST7735_BLACK);
}

void resolvePath(const char* src, char* dest, size_t destLen) {
    if (src[0] == '/') strncpy(dest, src, destLen);
    else {
        if (strcmp(cwd, "/") == 0) snprintf(dest, destLen, "/%s", src);
        else snprintf(dest, destLen, "%s/%s", cwd, src);
    }
    size_t len = strlen(dest);
    if (len > 1 && dest[len - 1] == '/') dest[len - 1] = '\0';
}

// --- 2. NOW INCLUDE EXTERNAL BINARIES safely ---
#include "eshbin.hpp"

// --- 3. CORE INTERNAL STATE BUILT-INS ---
void cmd_cd(int argc, char *argv[]) {
    if (argc < 2) { strcpy(cwd, "/"); shellPrintLn("Moved to /"); return; }
    char target[MAX_LINE];
    if (strcmp(argv[1], "..") == 0) {
        if (strcmp(cwd, "/") == 0) return;
        char* lastSlash = strrchr(cwd, '/');
        if (lastSlash == cwd) strcpy(cwd, "/");
        else *lastSlash = '\0';
        shellPrint("Moved to "); shellPrintLn(cwd); return;
    }
    resolvePath(argv[1], target, sizeof(target));
    File dir = LittleFS.open(target, "r");
    if (dir && dir.isDirectory()) { strncpy(cwd, target, sizeof(cwd)); shellPrint("Moved to "); shellPrintLn(cwd); }
    else { shellPrintLn("Error: Directory not found."); }
    if (dir) dir.close();
}

void cmd_set(int argc, char *argv[]) {
    if (argc < 3) { shellPrintLn("Usage: set <var> <val> [opt: +|- <val2>]"); return; }
    std::string varName = argv[1];
    std::string value = argv[2];

    // Check if the user is trying to perform math: set count $count + 1
    if (argc >= 5) {
        int val1 = atoi(argv[2]);
        std::string op = argv[3];
        int val2 = atoi(argv[4]);
        
        if (op == "+") value = std::to_string(val1 + val2);
        if (op == "-") value = std::to_string(val1 - val2);
    }

    // Update existing or insert new variable matching key signature
    for (auto& var : envTable) {
        if (var.name == varName) { var.value = value; return; }
    }
    envTable.push_back({varName, value});
}

void cmd_deset(int argc, char *argv[]) {
    if (argc < 2) { shellPrintLn("Usage: deset <var>"); return; }
    std::string vName = argv[1];
    for (auto it = envTable.begin(); it != envTable.end(); ++it) {
        if (it->name == vName) { envTable.erase(it); shellPrintLn("Deleted variable."); return; }
    }
    shellPrintLn("Error: Variable not found.");
}

void cmd_help(int argc, char *argv[]) {
    shellPrintLn("--- ESH Internal Shell Built-ins ---");
    shellPrintLn("help               - Show this interface");
    shellPrintLn("cd [dir]           - Change directory reference");
    shellPrintLn("set <var> <val>    - Assign environment value");
    shellPrintLn("deset <var>        - Drop environment value");
    shellPrintLn("init [FS_BARE]     - Re-format layout structure");
    shellPrintLn("edit <file>        - Edit file contents in visual mode");
    shellPrintLn("\n--- External Filesystem Commands ---");
    for (int i = 0; i < COMMAND_COUNT; i++) {
        shellPrint(CMDS[i].c_str()); shellPrint(" \t- "); shellPrintLn(USAGE[i].c_str());
    }
}

void cmd_edit(int argc, char *argv[]) {
    if (argc < 2) { 
        shellPrintLn("Usage: edit <file>"); 
        return; 
    }
    char path[MAX_LINE]; 
    resolvePath(argv[1], path, sizeof(path));

    std::vector<std::string> buffer;
    
    // Attempt to open file to read existing contents into buffer
    File f = LittleFS.open(path, "r");
    if (f && !f.isDirectory()) {
        while (f.available()) {
            String line = f.readStringUntil('\n');
            if (line.endsWith("\r")) line.remove(line.length() - 1);
            buffer.push_back(line.c_str());
        }
        f.close();
    }
    if (buffer.empty()) buffer.push_back(""); // Always start with at least one blank line

    int cursorX = 0, cursorY = 0;
    bool active = true;

    // Clear serial screen and display text-editor header
    Serial.print("\033[2J\033[H");
    Serial.println("--- VISUAL EDIT MODE: Ctrl+D to Save, Ctrl+C to Abort ---");
    
    // Initial display draw cycle
    tft.fillScreen(ST7735_BLACK);
    tftCursorX = TFT_OFFSET_X; tftCursorY = TFT_OFFSET_Y;
    for (int i = 0; i < (int)buffer.size(); i++) {
        if (i == cursorY) {
            std::string lineWithCursor = buffer[i];
            lineWithCursor.insert(cursorX, "|");
            shellPrintLn(lineWithCursor.c_str());
        } else {
            shellPrintLn(buffer[i].c_str());
        }
    }

    // Interactive Editor Input Loop
    while (active) {
        if (Serial.available()) {
            char c = Serial.read();
            bool structuralChange = false;
            
            if (c == 0x04) { // Ctrl+D: Commit changes to Flash block
                File out = LittleFS.open(path, "w");
                if (out) {
                    for (const auto& l : buffer) out.println(l.c_str());
                    out.close();
                    Serial.println("\n[System: Content written cleanly to LittleFS partition.]");
                }
                active = false;
                structuralChange = true;
            } 
            else if (c == 0x03) { // Ctrl+C: Safe Abort
                Serial.println("\n[System: Changes discarded cleanly.]");
                active = false;
                structuralChange = true;
            } 
            else if (c == 0x7F || c == 0x08) { // Backspace handling
                if (cursorX > 0) {
                    buffer[cursorY].erase(cursorX - 1, 1);
                    cursorX--;
                    structuralChange = true;
                } else if (cursorY > 0) {
                    cursorX = buffer[cursorY - 1].length();
                    buffer[cursorY - 1] += buffer[cursorY];
                    buffer.erase(buffer.begin() + cursorY);
                    cursorY--;
                    structuralChange = true;
                }
            } 
            else if (c == '\r' || c == '\n') { // Carriage Return / Newline splits
                std::string currentStr = buffer[cursorY];
                buffer[cursorY] = currentStr.substr(0, cursorX);
                buffer.insert(buffer.begin() + cursorY + 1, currentStr.substr(cursorX));
                cursorY++;
                cursorX = 0;
                structuralChange = true;
            } 
            else if (c == '\033') { // ANSI Arrow Escape Traps
                delay(2);
                if (Serial.read() == '[') {
                    char arrow = Serial.read();
                    if (arrow == 'A' && cursorY > 0) { cursorY--; cursorX = std::min(cursorX, (int)buffer[cursorY].length()); structuralChange = true; } // Up
                    if (arrow == 'B' && cursorY < (int)buffer.size() - 1) { cursorY++; cursorX = std::min(cursorX, (int)buffer[cursorY].length()); structuralChange = true; } // Down
                    if (arrow == 'C' && cursorX < (int)buffer[cursorY].length()) { cursorX++; structuralChange = true; } // Right
                    if (arrow == 'D' && cursorX > 0) { cursorX--; structuralChange = true; } // Left
                }
            } 
            else if (c >= 32 && c <= 126) { // Direct character insertions
                buffer[cursorY].insert(cursorX, 1, c);
                cursorX++;
                structuralChange = true;
            }
            
            // Re-render display canvas strictly when changes occur
            if (active && structuralChange) {
                Serial.print("\033[H\033[2J");
                Serial.println("--- VISUAL EDIT MODE: Ctrl+D to Save, Ctrl+C to Abort ---");
                
                tft.fillScreen(ST7735_BLACK);
                tftCursorX = TFT_OFFSET_X; tftCursorY = TFT_OFFSET_Y;
                
                for (int i = 0; i < (int)buffer.size(); i++) {
                    if (i == cursorY) {
                        std::string lineWithCursor = buffer[i];
                        lineWithCursor.insert(cursorX, "|");
                        shellPrintLn(lineWithCursor.c_str());
                    } else {
                        shellPrintLn(buffer[i].c_str());
                    }
                }
            }
        }
    }
    
    // Clear display to return back cleanly to shell prompt coordinates
    tft.fillScreen(ST7735_BLACK);
    tftCursorX = TFT_OFFSET_X; tftCursorY = TFT_OFFSET_Y;
}

void cmd_init(int argc, char *argv[]) {
    shellPrintLn("Formatting System Partition...");
    LittleFS.format();
    if (!LittleFS.begin(true)) { shellPrintLn("FS critical error!"); return; }

    LittleFS.mkdir("/bin");
    if (argc > 1 && strcmp(argv[1], "FS_BARE") == 0) {
        shellPrintLn("Initialized bare layout. No proxy hooks mapped.");
        return;
    }

    // AUTOMATED SYSTEM BLOCK GENERATION USING THE HPP METADATA ARRAYS
    for (int i = 0; i < COMMAND_COUNT; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/bin/%s.esh", CMDS[i].c_str());
        File f = LittleFS.open(path, "w");
        if (f) {
            f.print("ESH_INCODE(cmd_");
            f.print(CMDS[i]);
            f.print(")\n");
            f.close();
        }
    }
    shellPrintLn("System binaries generated automatically via eshbin framework.");
}

bool evaluateCondition(int argc, char* argv[]) {
    // Basic condition format: [if/while] <val1> <operator> <val2>
    if (argc < 4) return false;
    
    std::string val1 = argv[1];
    std::string op = argv[2];
    std::string val2 = argv[3];

    if (op == "==") return (val1 == val2);
    if (op == "!=") return (val1 != val2);
    
    // Numeric handling
    int num1 = atoi(val1.c_str());
    int num2 = atoi(val2.c_str());
    if (op == ">")  return (num1 > num2);
    if (op == "<")  return (num1 < num2);
    if (op == ">=") return (num1 >= num2);
    if (op == "<=") return (num1 <= num2);

    return false;
}

// --- 4. DYNAMIC DISPATCH ENGINE ---
bool tryExecuteInCode(const char* line, int argc, char* argv[]) {
    // Check if the script file starts with our special macro signature
    if (strstr(line, "ESH_INCODE(cmd_") == line) {
        char extractedName[32];
        
        // Extract just the raw base name (e.g., extracts "ls" from "ESH_INCODE(cmd_ls)")
        if (sscanf(line, "ESH_INCODE(cmd_%31[^)])", extractedName) == 1) {
            
            // Loop dynamically through your centralized CMDS configuration array
            for (int i = 0; i < COMMAND_COUNT; i++) {
                if (CMDS[i] == extractedName) {
                    // Dynamic Dispatch execution execution signature via function pointer!
                    CMD_FUNCTIONS[i](argc, argv);
                    return true;
                }
            }
            shellPrint("Error: Flash macro matched but executable pointer missing for: ");
            shellPrintLn(extractedName);
            return true;
        }
    }
    return false;
}
void executeLine(char* rawLine) {
    // --- PRE-PROCESSING: STRIP COMMENTS AND CLEAN ---
    std::string lineStr = rawLine;
    size_t commentPos = lineStr.find('#');
    if (commentPos != std::string::npos) {
        lineStr = lineStr.substr(0, commentPos);
    }
    
    // Trim trailing whitespace/newlines safely
    while (!lineStr.empty() && (lineStr.back() == ' ' || lineStr.back() == '\r' || lineStr.back() == '\n')) {
        lineStr.pop_back();
    }
    while (!lineStr.empty() && lineStr.front() == ' ') {
        lineStr.erase(0, 1);
    }
    
    if (lineStr.empty()) return; // Pure comment or empty line, skip instantly!

    // Reassign back to raw buffer for tokenization
    char cleanLine[MAX_LINE];
    strncpy(cleanLine, lineStr.c_str(), sizeof(cleanLine));
    std::string savedLine = cleanLine;

    char* argv[MAX_ARGS]; int argc = 0;
    char* token = strtok(cleanLine, " ");
    while (token != NULL && argc < MAX_ARGS) { argv[argc++] = token; token = strtok(NULL, " "); }
    if (argc == 0) return;

    // --- 1. CONTROL FLOW STRUCTURAL TOKENS ---
    if (strcmp(argv[0], "if") == 0) {
        if (skipExecution) { skipDepth++; return; }
        
        std::string substitutedArgs[MAX_ARGS];
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '$') {
                std::string varName = argv[i] + 1;
                for (const auto& var : envTable) if (var.name == varName) substitutedArgs[i] = var.value;
                if (!substitutedArgs[i].empty()) argv[i] = (char*)substitutedArgs[i].c_str();
            }
        }
        if (!evaluateCondition(argc, argv)) { skipExecution = true; skipDepth = 1; }
        return;
    }

    if (strcmp(argv[0], "else") == 0) {
        if (skipExecution && skipDepth == 1) { skipExecution = false; skipDepth = 0; }
        else if (!skipExecution) { skipExecution = true; skipDepth = 1; }
        return;
    }

    if (strcmp(argv[0], "endif") == 0) {
        if (skipExecution) { skipDepth--; if (skipDepth == 0) skipExecution = false; }
        return;
    }

    if (strcmp(argv[0], "while") == 0 || strcmp(argv[0], "endwhile") == 0) return; 

    if (skipExecution) return;

    // --- 2. VARIABLES SUBSTITUTION PIPELINE ---
    std::string substitutedArgs[MAX_ARGS];
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] == '$') {
            std::string varName = argv[i] + 1;
            for (const auto& var : envTable) if (var.name == varName) substitutedArgs[i] = var.value;
            if (!substitutedArgs[i].empty()) argv[i] = (char*)substitutedArgs[i].c_str();
        }
    }

    // High Priority Global Internal Built-ins
    if (strcmp(argv[0], "cd") == 0)    { cmd_cd(argc, argv); return; }
    if (strcmp(argv[0], "init") == 0)  { cmd_init(argc, argv); return; }
    if (strcmp(argv[0], "set") == 0)   { cmd_set(argc, argv); return; }
    if (strcmp(argv[0], "deset") == 0) { cmd_deset(argc, argv); return; }
    if (strcmp(argv[0], "help") == 0)  { cmd_help(argc, argv); return; }
    if (strcmp(argv[0], "edit") == 0)  { cmd_edit(argc, argv); return; }

    // External Macro Pointer Router
    char path[MAX_LINE];
    snprintf(path, sizeof(path), "/bin/%s.esh", argv[0]);
    File f = LittleFS.open(path, "r");
    if (!f) { resolvePath(argv[0], path, sizeof(path)); f = LittleFS.open(path, "r"); }
    if (!f) { shellPrint("ESH: Command not found: "); shellPrintLn(argv[0]); return; }

    char firstLine[64];
    int len = f.readBytesUntil('\n', firstLine, sizeof(firstLine)-1);
    firstLine[len] = '\0';
    if (len > 0 && firstLine[len - 1] == '\r') firstLine[len - 1] = '\0';
    
    if (tryExecuteInCode(firstLine, argc, argv)) { f.close(); return; }

    // --- 3. RUN SCRIPT LINES ---
    f.seek(0);
    while (f.available()) {
        long currentPos = f.position();
        char scriptLine[MAX_LINE];
        int sLen = f.readBytesUntil('\n', scriptLine, sizeof(scriptLine)-1);
        scriptLine[sLen] = '\0';
        
        // Inline cleaning for checking structural keywords
        std::string sStr = scriptLine;
        size_t cPos = sStr.find('#');
        if (cPos != std::string::npos) sStr = sStr.substr(0, cPos);
        while(!sStr.empty() && (sStr.back() == ' ' || sStr.back() == '\r' || sStr.back() == '\n')) sStr.pop_back();
        while(!sStr.empty() && sStr.front() == ' ') sStr.erase(0, 1);
        
        if (sStr.empty()) continue;

        char checkLine[MAX_LINE]; strcpy(checkLine, sStr.c_str());
        char* firstWord = strtok(checkLine, " ");
        if (firstWord == NULL) continue;

        if (strcmp(firstWord, "while") == 0) {
            if (!skipExecution) {
                LoopState newLoop = { currentPos, sStr };
                loopStack.push_back(newLoop);
                
                char parseLine[MAX_LINE]; strcpy(parseLine, sStr.c_str());
                char* wArgv[MAX_ARGS]; int wArgc = 0;
                char* wTok = strtok(parseLine, " ");
                while(wTok && wArgc < MAX_ARGS) { wArgv[wArgc++] = wTok; wTok = strtok(NULL, " "); }
                
                std::string wSub[MAX_ARGS];
                for (int i = 1; i < wArgc; i++) {
                    if (wArgv[i][0] == '$') {
                        std::string vN = wArgv[i] + 1;
                        for (const auto& var : envTable) if (var.name == vN) wSub[i] = var.value;
                        if (!wSub[i].empty()) wArgv[i] = (char*)wSub[i].c_str();
                    }
                }
                if (!evaluateCondition(wArgc, wArgv)) { skipExecution = true; skipDepth = 1; }
            } else {
                skipDepth++;
            }
            continue;
        }

        if (strcmp(firstWord, "endwhile") == 0) {
            if (skipExecution) {
                skipDepth--; if (skipDepth == 0) skipExecution = false;
            } else if (!loopStack.empty()) {
                LoopState activeLoop = loopStack.back();
                loopStack.pop_back();
                f.seek(activeLoop.filePosition);
            }
            continue;
        }

        // Execute cleaned line safely
        char execBuf[MAX_LINE];
        strcpy(execBuf, sStr.c_str());
        executeLine(execBuf);
    }
    f.close();
}

void setup() {
    Serial.begin(115200);
    pinMode(JOY_SW, INPUT_PULLUP);
    tft.initR(INITR_BLACKTAB); tft.setRotation(1); tft.fillScreen(ST7735_BLACK);
    
    if (!LittleFS.begin(true)) Serial.println("LittleFS Mount Failed.");
    envTable.reserve(20);
    
    shellPrintLn("ESH Core Booted cleanly. Type 'help' for directions.");
    shellPrint("ESH:"); shellPrint(cwd); shellPrint(" $ ");
}

void addCommandToHistory(const char* cmd) {
    if (strlen(cmd) == 0) return;
    
    // If it matches the last entered command exactly, don't duplicate it
    int lastIdx = (historyWriteIdx - 1 + HISTORY_SIZE) % HISTORY_SIZE;
    if (historyCount > 0 && strcmp(cmdHistory[lastIdx], cmd) == 0) {
        historyViewIdx = -1; // Reset view pointer
        return;
    }

    // Copy into the circular ring buffer slot
    strcpy(cmdHistory[historyWriteIdx], cmd);
    historyWriteIdx = (historyWriteIdx + 1) % HISTORY_SIZE;
    
    if (historyCount < HISTORY_SIZE) historyCount++;
    historyViewIdx = -1; // Reset view pointer to live prompt
}

void loop() {
    static char commandLineBuffer[MAX_LINE];
    static int bufferIndex = 0;
    static unsigned long lastJoyPoll = 0;

    // --- 1. HARDWARE OS CONTROL (JOYSTICK W/ 10-CMD HISTORY) ---
    if (millis() - lastJoyPoll > 180) { // Slightly increased delay for intentional stepping
        int xVal = analogRead(JOY_X);
        int yVal = analogRead(JOY_Y);
        bool actionTaken = false;

        // X-Axis: Left = Backspace, Right = Space
        if (xVal < 1000 && bufferIndex > 0) { 
            bufferIndex--; shellBackspace(); 
            actionTaken = true; 
        } else if (xVal > 3000 && bufferIndex < MAX_LINE - 1) { 
            commandLineBuffer[bufferIndex++] = ' ';
            shellPrint(" ");
            actionTaken = true;
        }

        // Y-Axis UP: Move Backward in History
        if (yVal > 3000 && historyCount > 0) {
            if (historyViewIdx == -1) {
                // Start browsing from the most recently added item
                historyViewIdx = (historyWriteIdx - 1 + HISTORY_SIZE) % HISTORY_SIZE;
            } else {
                // Calculate oldest available slot in circular buffer
                int oldestIdx = (historyWriteIdx - historyCount + HISTORY_SIZE) % HISTORY_SIZE;
                if (historyViewIdx != oldestIdx) {
                    historyViewIdx = (historyViewIdx - 1 + HISTORY_SIZE) % HISTORY_SIZE;
                }
            }
            // Wipe the prompt and output the historical line
            while(bufferIndex > 0) { bufferIndex--; shellBackspace(); } 
            strcpy(commandLineBuffer, cmdHistory[historyViewIdx]);
            bufferIndex = strlen(commandLineBuffer);
            shellPrint(commandLineBuffer);
            actionTaken = true;
        }

        // Y-Axis DOWN: Move Forward in History
        if (yVal < 1000 && historyCount > 0 && historyViewIdx != -1) {
            int mostRecentIdx = (historyWriteIdx - 1 + HISTORY_SIZE) % HISTORY_SIZE;
            if (historyViewIdx == mostRecentIdx) {
                // Reached the present day, clear text back to empty live prompt
                historyViewIdx = -1;
                while(bufferIndex > 0) { bufferIndex--; shellBackspace(); }
                commandLineBuffer[0] = '\0';
            } else {
                historyViewIdx = (historyViewIdx + 1) % HISTORY_SIZE;
                while(bufferIndex > 0) { bufferIndex--; shellBackspace(); } 
                strcpy(commandLineBuffer, cmdHistory[historyViewIdx]);
                bufferIndex = strlen(commandLineBuffer);
                shellPrint(commandLineBuffer);
            }
            actionTaken = true;
        }

        // Click: Enter / Execute Command
        if (digitalRead(JOY_SW) == LOW) {
            shellPrintLn(""); 
            commandLineBuffer[bufferIndex] = '\0';
            if (bufferIndex > 0) { 
                addCommandToHistory(commandLineBuffer); // Commit to history block
                executeLine(commandLineBuffer); 
                bufferIndex = 0; 
            } else {
                historyViewIdx = -1; // Reset view context if empty enter pressed
            }
            shellPrint("ESH:"); shellPrint(cwd); shellPrint(" $ ");
            actionTaken = true;
            while(digitalRead(JOY_SW) == LOW) delay(10); // Debounce physical switch
        }

        if (actionTaken) lastJoyPoll = millis();
    }

    // --- 2. SERIAL TERMINAL CONTROL ---
    if (Serial.available()) {
        char c = Serial.read();
        
        if (c == '\033') { // Trap and discard ANSI escape arrows from PuTTY
            delay(2); if (Serial.available() && Serial.read() == '[') { while (!Serial.available()); Serial.read(); }
            return;
        }
        
        if (c == '\n' || c == '\r') {
            shellPrintLn(""); commandLineBuffer[bufferIndex] = '\0';
            if (bufferIndex > 0) { 
                addCommandToHistory(commandLineBuffer); // Commit to history block
                executeLine(commandLineBuffer); 
                bufferIndex = 0; 
            } else {
                historyViewIdx = -1;
            }
            shellPrint("ESH:"); shellPrint(cwd); shellPrint(" $ ");
        } else if (c == 0x08 || c == 0x7F) {
            if (bufferIndex > 0) { bufferIndex--; shellBackspace(); }
        } else if (c >= 32 && c <= 126 && bufferIndex < MAX_LINE - 1) {
            commandLineBuffer[bufferIndex++] = c;
            char echoStr[2] = {c, '\0'}; shellPrint(echoStr);
            historyViewIdx = -1; // Typing breaks out of active history browsing
        }
    }
}