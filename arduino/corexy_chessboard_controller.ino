/*
  CoreXY Arduino Motion Controller + CHESSBOARD MAPPING + SAFE ROUTING
  + GRAVEYARDS (PT/PB) + CAPTURE (with magnet, optional DRY)
  RAM-SAFE for Arduino UNO: NO String class, all Serial text in FLASH via F().

  JETSON-READY PROTOCOL:
  - Commands arrive as single lines terminated by '\n'
  - Arduino replies with:
      ACK
      DONE
      ERR <message>
      DBG <message>     (optional debug/info lines, safe to ignore from Jetson)

  Important behavior:
  - SUCCESS/FAILURE is reported ONLY at command level
  - Internal sub-moves do NOT print "OK"
  - STOP interrupts motion loop, forces magnet OFF, and invalidates current position
  - After STOP, do HOME / re-zero before continuing
*/

#include <Arduino.h>
#include <math.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

#define STEP_A 2
#define DIR_A  3
#define STEP_B 5
#define DIR_B  6

// ======= MAGNET =======
#define MAGNET_PIN 9

const float MAGNET_TOL_MM = 3.0f;
const uint16_t MAGNET_DELAY_MS = 200;
const bool MAGNET_ACTIVE_HIGH = true;

inline void magnetOnRaw()  { digitalWrite(MAGNET_PIN, MAGNET_ACTIVE_HIGH ? HIGH : LOW); }
inline void magnetOffRaw() { digitalWrite(MAGNET_PIN, MAGNET_ACTIVE_HIGH ? LOW  : HIGH); }

volatile bool magnetIsOn = false;
volatile bool stopRequested = false;
volatile bool positionKnown = true;

inline void magnetOn()  { magnetOnRaw();  magnetIsOn = true;  }
inline void magnetOff() { magnetOffRaw(); magnetIsOn = false; }

// ======= CALIBRATION / LIMITS =======
const float STEPS_PER_MM = 20.13f;
const float MAX_X_MM = 460.0f;
const float MAX_Y_MM = 670.0f;

// Speed: smaller = faster
volatile int pulseDelayUs = 400;

// Position tracking (in steps)
long curX_steps = 0;
long curY_steps = 0;

// ======= BOARD MAPPING =======
const float SQ_MM  = 50.0f;
const float GAP_MM = 25.0f;

const bool ORIGIN_IS_CENTER = true;
float originX_offset_mm = 0.0f;
float originY_offset_mm = 0.0f;

// ======= GRAVEYARD PAWN SLOT OCCUPANCY =======
uint8_t ptPawnMask = 0; // PT row0 A..H (captured black pawns)
uint8_t pbPawnMask = 0; // PB row1 A..H (captured white pawns)

// ======= LAST ERROR =======
static char lastErrorMsg[96] = "";

// ======= SERIAL LINE BUFFER =======
static char lineBuf[96];
static uint8_t lineLen = 0;

// ===================== PROTOCOL HELPERS =====================
void setLastError(const char *msg) {
  if (!msg) msg = "UNKNOWN";
  strncpy(lastErrorMsg, msg, sizeof(lastErrorMsg) - 1);
  lastErrorMsg[sizeof(lastErrorMsg) - 1] = '\0';
}

void clearLastError() {
  lastErrorMsg[0] = '\0';
}

void sendAck() {
  Serial.println(F("ACK"));
}

void sendDone() {
  Serial.println(F("DONE"));
}

void sendErrMsg(const char *msg) {
  Serial.print(F("ERR "));
  Serial.println(msg ? msg : "UNKNOWN");
}

void sendLastError() {
  if (lastErrorMsg[0]) sendErrMsg(lastErrorMsg);
  else sendErrMsg("UNKNOWN");
}

void dbgMsg(const __FlashStringHelper *msg) {
  Serial.print(F("DBG "));
  Serial.println(msg);
}

void dbgText(const char *msg) {
  Serial.print(F("DBG "));
  Serial.println(msg ? msg : "");
}

// ===================== LOW LEVEL HELPERS =====================
inline void pulsePin(uint8_t pin) {
  digitalWrite(pin, HIGH);
  delayMicroseconds(pulseDelayUs);
  digitalWrite(pin, LOW);
  delayMicroseconds(pulseDelayUs);
}

long mmToSteps(float mm) { return (long)lroundf(mm * STEPS_PER_MM); }
float stepsToMm(long steps) { return ((float)steps) / STEPS_PER_MM; }

inline float curX_mm() { return stepsToMm(curX_steps); }
inline float curY_mm() { return stepsToMm(curY_steps); }

bool withinLimits(float x_mm, float y_mm) {
  if (x_mm < 0.0f || x_mm > MAX_X_MM) return false;
  if (y_mm < 0.0f || y_mm > MAX_Y_MM) return false;
  return true;
}

bool isAtXY(float x, float y, float tol = MAGNET_TOL_MM) {
  float dx = fabs(curX_mm() - x);
  float dy = fabs(curY_mm() - y);
  return (dx <= tol) && (dy <= tol);
}

void printPos() {
  Serial.print(F("DBG POS mm: X="));
  Serial.print(stepsToMm(curX_steps), 2);
  Serial.print(F(" Y="));
  Serial.print(stepsToMm(curY_steps), 2);
  Serial.print(F(" | steps: X="));
  Serial.print(curX_steps);
  Serial.print(F(" Y="));
  Serial.println(curY_steps);
}

// ===================== COREXY MOTION =====================
// Returns true if completed, false if STOP interrupted.
bool moveXY_steps(long xSteps, long ySteps) {
  long aSteps = xSteps + ySteps;
  long bSteps = xSteps - ySteps;

  bool dirA = (aSteps >= 0) ? HIGH : LOW;
  bool dirB = (bSteps >= 0) ? HIGH : LOW;

  unsigned long nA = (unsigned long)labs(aSteps);
  unsigned long nB = (unsigned long)labs(bSteps);

  digitalWrite(DIR_A, dirA);
  digitalWrite(DIR_B, dirB);
  delay(5);

  unsigned long nMax = (nA > nB) ? nA : nB;
  unsigned long errA = 0, errB = 0;

  for (unsigned long i = 0; i < nMax; i++) {
    if (stopRequested) {
      magnetOff();
      positionKnown = false;
      setLastError("STOPPED_REHOME_REQUIRED");
      stopRequested = false;
      return false;
    }

    errA += nA;
    errB += nB;

    if (errA >= nMax) { pulsePin(STEP_A); errA -= nMax; }
    if (errB >= nMax) { pulsePin(STEP_B); errB -= nMax; }
  }
  return true;
}

// ===================== ABSOLUTE / RELATIVE MOVE =====================
bool moveTo_mm(float x_mm, float y_mm) {
  if (!positionKnown) {
    setLastError("POSITION_UNKNOWN_REHOME_REQUIRED");
    return false;
  }

  if (!withinLimits(x_mm, y_mm)) {
    setLastError("OUT_OF_SOFT_LIMITS");
    return false;
  }

  long targetX = mmToSteps(x_mm);
  long targetY = mmToSteps(y_mm);

  long dx = targetX - curX_steps;
  long dy = targetY - curY_steps;

  if (!moveXY_steps(dx, dy)) {
    if (!lastErrorMsg[0]) setLastError("MOTION_INTERRUPTED");
    return false;
  }

  curX_steps = targetX;
  curY_steps = targetY;
  return true;
}

bool moveRel_mm(float dx_mm, float dy_mm) {
  float newX = stepsToMm(curX_steps) + dx_mm;
  float newY = stepsToMm(curY_steps) + dy_mm;
  return moveTo_mm(newX, newY);
}

// ===================== STRING HELPERS =====================
void trimInPlace(char *s) {
  if (!s) return;

  while (*s && isspace((unsigned char)*s)) {
    memmove(s, s + 1, strlen(s));
  }

  int n = (int)strlen(s);
  while (n > 0 && isspace((unsigned char)s[n - 1])) {
    s[n - 1] = '\0';
    n--;
  }
}

void toUpperInPlace(char *s) {
  while (*s) {
    *s = (char)toupper((unsigned char)*s);
    s++;
  }
}

// ===================== BOARD MAPPING =====================
int fileToIndex(char fileChar) {
  if (fileChar >= 'a' && fileChar <= 'h') fileChar = (char)(fileChar - 'a' + 'A');
  if (fileChar < 'A' || fileChar > 'H') return -1;
  return (int)(fileChar - 'A');
}

char indexToFile(int idx) {
  if (idx < 0) idx = 0;
  if (idx > 7) idx = 7;
  return (char)('A' + idx);
}

float boardBaseY_mm() {
  float originCenterAdjust = ORIGIN_IS_CENTER ? (SQ_MM / 2.0f) : 0.0f;
  float baseY = (2.0f * SQ_MM) + GAP_MM + (SQ_MM / 2.0f) - originCenterAdjust;
  return baseY + originY_offset_mm;
}

float boardTopY_mm() {
  return boardBaseY_mm() + (7.0f * SQ_MM);
}

// Letters (file) => Y squares (A bottom -> H top)
// Numbers (rank) => X squares (1 right -> 8 left) so invert X
bool squareToXY(char fileChar, int rank, float &x_mm, float &y_mm) {
  int fi = fileToIndex(fileChar);
  if (fi < 0) return false;
  if (rank < 1 || rank > 8) return false;

  float centerShift = ORIGIN_IS_CENTER ? 0.0f : (SQ_MM / 2.0f);
  float originCenterAdjust = ORIGIN_IS_CENTER ? (SQ_MM / 2.0f) : 0.0f;

  float baseY = (2.0f * SQ_MM) + GAP_MM + (SQ_MM / 2.0f) - originCenterAdjust;

  int ySquares = fi;         // A..H => 0..7 upward
  int xSquares = 8 - rank;   // 1..8 => 7..0

  x_mm = (xSquares * SQ_MM) + centerShift + originX_offset_mm;
  y_mm = baseY + (ySquares * SQ_MM) + originY_offset_mm;
  return true;
}

// Bottom parking: row 0 majors, row 1 pawns
bool bottomParkingToXY(int row, char fileChar, float &x_mm, float &y_mm) {
  int fi = fileToIndex(fileChar);
  if (fi < 0) return false;
  if (row < 0 || row > 1) return false;

  float centerShift = ORIGIN_IS_CENTER ? 0.0f : (SQ_MM / 2.0f);
  float yBase = ORIGIN_IS_CENTER ? 0.0f : (SQ_MM / 2.0f);

  x_mm = (fi * SQ_MM) + centerShift + originX_offset_mm;
  y_mm = yBase + (row * SQ_MM) + originY_offset_mm;
  return true;
}

// Top parking: row 0 pawns, row 1 majors
bool topParkingToXY(int row, char fileChar, float &x_mm, float &y_mm) {
  int fi = fileToIndex(fileChar);
  if (fi < 0) return false;
  if (row < 0 || row > 1) return false;

  float centerShift = ORIGIN_IS_CENTER ? 0.0f : (SQ_MM / 2.0f);
  float originCenterAdjust = ORIGIN_IS_CENTER ? (SQ_MM / 2.0f) : 0.0f;

  float topRow0 = (2.0f * SQ_MM) + GAP_MM + (8.0f * SQ_MM) + GAP_MM + (SQ_MM / 2.0f) - originCenterAdjust;

  x_mm = (fi * SQ_MM) + centerShift + originX_offset_mm;
  y_mm = topRow0 + (row * SQ_MM) + originY_offset_mm;
  return true;
}

void printBoardParams() {
  Serial.println(F("DBG BOARD PARAMS:"));
  Serial.print(F("DBG   SQ_MM=")); Serial.println(SQ_MM, 2);
  Serial.print(F("DBG   GAP_MM=")); Serial.println(GAP_MM, 2);
  Serial.print(F("DBG   Offsets (mm): Xoff=")); Serial.print(originX_offset_mm, 2);
  Serial.print(F(" Yoff=")); Serial.println(originY_offset_mm, 2);

  float x, y;
  squareToXY('A', 1, x, y);
  Serial.print(F("DBG   A1 center -> X=")); Serial.print(x, 2); Serial.print(F(" Y=")); Serial.println(y, 2);
  squareToXY('H', 8, x, y);
  Serial.print(F("DBG   H8 center -> X=")); Serial.print(x, 2); Serial.print(F(" Y=")); Serial.println(y, 2);

  bottomParkingToXY(0, 'A', x, y);
  Serial.print(F("DBG   Bottom parking row0 A -> X=")); Serial.print(x, 2); Serial.print(F(" Y=")); Serial.println(y, 2);
  topParkingToXY(1, 'H', x, y);
  Serial.print(F("DBG   Top parking row1 H -> X=")); Serial.print(x, 2); Serial.print(F(" Y=")); Serial.println(y, 2);

  Serial.print(F("DBG   boardBaseY=")); Serial.println(boardBaseY_mm(), 2);
  Serial.print(F("DBG   boardTopY="));  Serial.println(boardTopY_mm(), 2);
}

// ===================== SAFE ROUTING =====================
float laneBelowY(float yCenter) {
  float originCenterAdjust = ORIGIN_IS_CENTER ? (SQ_MM / 2.0f) : 0.0f;
  float baseY = (2.0f * SQ_MM) + GAP_MM + (SQ_MM / 2.0f) - originCenterAdjust;

  float rel = yCenter - (baseY + originY_offset_mm);
  int k = (int)lroundf(rel / SQ_MM);
  float yLane = (baseY + originY_offset_mm) + (k * SQ_MM) - (SQ_MM / 2.0f);
  return yLane;
}

float chooseVerticalLaneX(float xCenter) {
  float leftLane  = xCenter - (SQ_MM / 2.0f);
  float rightLane = xCenter + (SQ_MM / 2.0f);

  bool leftOK  = (leftLane  >= 0.0f && leftLane  <= MAX_X_MM);
  bool rightOK = (rightLane >= 0.0f && rightLane <= MAX_X_MM);

  if (leftOK) return leftLane;
  if (rightOK) return rightLane;
  return xCenter;
}

bool moveSafeSquares(char f1, int r1, char f2, int r2) {
  float x1, y1, x2, y2;
  if (!squareToXY(f1, r1, x1, y1)) { setLastError("BAD_FROM_SQUARE"); return false; }
  if (!squareToXY(f2, r2, x2, y2)) { setLastError("BAD_TO_SQUARE"); return false; }

  if (!moveTo_mm(x1, y1)) return false;

  float yLane1 = laneBelowY(y1);
  if (!moveTo_mm(x1, yLane1)) return false;

  float xLane = chooseVerticalLaneX(x2);
  if (!moveTo_mm(xLane, yLane1)) return false;

  float yLane2 = laneBelowY(y2);
  if (!moveTo_mm(xLane, yLane2)) return false;

  if (!moveTo_mm(x2, yLane2)) return false;
  if (!moveTo_mm(x2, y2)) return false;

  return true;
}

// ======= ALWAYS SAFE XY (including graveyards) =======
const float SAFE_GAP_BOTTOM_Y = 25.0f;
const float SAFE_GAP_TOP_Y    = 525.0f;

bool magnetOnOnlyIfAt(float x, float y) {
  if (!moveTo_mm(x, y)) return false;
  delay(300);

  if (!isAtXY(x, y)) {
    setLastError("MAGNET_ON_BLOCKED_NOT_AT_TARGET");
    return false;
  }

  magnetOn();
  dbgMsg(F("MAG ON"));
  delay(MAGNET_DELAY_MS);
  return true;
}

bool magnetOffOnlyIfAt(float x, float y) {
  if (!moveTo_mm(x, y)) return false;

  if (!isAtXY(x, y)) {
    setLastError("MAGNET_OFF_BLOCKED_NOT_AT_TARGET");
    return false;
  }

  magnetOff();
  dbgMsg(F("MAG OFF"));
  delay(MAGNET_DELAY_MS);
  return true;
}

float pickSafeCorridorY(float yTarget) {
  float b0 = boardBaseY_mm();
  float b8 = boardTopY_mm();

  if (yTarget < b0) return SAFE_GAP_BOTTOM_Y;
  if (yTarget > b8) return SAFE_GAP_TOP_Y;

  return laneBelowY(yTarget);
}

bool moveSafeToXY(float xTarget, float yTarget) {
  if (!withinLimits(xTarget, yTarget)) {
    setLastError("SAFE_TARGET_OUT_OF_SOFT_LIMITS");
    return false;
  }

  if (fabs(curX_mm() - xTarget) <= MAGNET_TOL_MM && fabs(curY_mm() - yTarget) <= MAGNET_TOL_MM) {
    return true;
  }

  float x0 = curX_mm();
  float y0 = curY_mm();

  if (fabs(x0) < 0.01f && fabs(y0) < 0.01f) {
    if (!moveTo_mm(0.0f, SAFE_GAP_BOTTOM_Y)) return false;
    x0 = curX_mm();
    y0 = curY_mm();
  }

  float yCorr0 = pickSafeCorridorY(y0);
  float yCorrT = pickSafeCorridorY(yTarget);
  float xLane = chooseVerticalLaneX(xTarget);

  if (xLane < 0.0f) xLane = 0.0f;
  if (xLane > MAX_X_MM) xLane = MAX_X_MM;
  if (yCorr0 < 0.0f) yCorr0 = 0.0f;
  if (yCorrT < 0.0f) yCorrT = 0.0f;

  if (!moveTo_mm(x0, yCorr0)) return false;
  if (!moveTo_mm(xLane, yCorr0)) return false;

  if (fabs(yCorrT - yCorr0) > 0.01f) {
    if (!moveTo_mm(xLane, yCorrT)) return false;
  }

  if (!moveTo_mm(xTarget, yCorrT)) return false;
  if (!moveTo_mm(xTarget, yTarget)) return false;

  return true;
}

// safe only when carrying
bool smartMoveToXY(float x, float y) {
  if (magnetIsOn) return moveSafeToXY(x, y);
  return moveTo_mm(x, y);
}

// ===================== GRAVEYARD SLOT LOGIC =====================
bool allocTopPawnSlot(int &row, char &fileChar) {
  row = 0;
  for (int i = 0; i < 8; i++) {
    if ((ptPawnMask & (1 << i)) == 0) {
      ptPawnMask |= (1 << i);
      fileChar = indexToFile(i);
      return true;
    }
  }
  return false;
}

bool allocBottomPawnSlot(int &row, char &fileChar) {
  row = 1;
  for (int i = 0; i < 8; i++) {
    if ((pbPawnMask & (1 << i)) == 0) {
      pbPawnMask |= (1 << i);
      fileChar = indexToFile(i);
      return true;
    }
  }
  return false;
}

bool majorSlotTop(char type, int idx12, int &row, char &fileChar) {
  row = 1;
  switch (type) {
    case 'R': fileChar = (idx12 == 1) ? 'H' : 'A'; return true;
    case 'N': fileChar = (idx12 == 1) ? 'G' : 'B'; return true;
    case 'B': fileChar = (idx12 == 1) ? 'F' : 'C'; return true;
    case 'Q': if (idx12 != 1) return false; fileChar = 'D'; return true;
    case 'K': if (idx12 != 1) return false; fileChar = 'E'; return true;
  }
  return false;
}

bool majorSlotBottom(char type, int idx12, int &row, char &fileChar) {
  row = 0;
  switch (type) {
    case 'R': fileChar = (idx12 == 1) ? 'A' : 'H'; return true;
    case 'N': fileChar = (idx12 == 1) ? 'B' : 'G'; return true;
    case 'B': fileChar = (idx12 == 1) ? 'C' : 'F'; return true;
    case 'Q': if (idx12 != 1) return false; fileChar = 'D'; return true;
    case 'K': if (idx12 != 1) return false; fileChar = 'E'; return true;
  }
  return false;
}

bool parsePieceToken(const char *tok, char &bw, char &type, int &idx12) {
  if (!tok || !tok[0] || !tok[1]) return false;

  char c0 = tok[0];
  char c1 = tok[1];

  if (c0 != 'b' && c0 != 'B' && c0 != 'w' && c0 != 'W') return false;
  bw = (c0 == 'b' || c0 == 'B') ? 'b' : 'w';

  type = c1;
  if (!(type == 'P' || type == 'R' || type == 'N' || type == 'B' || type == 'Q' || type == 'K')) return false;

  idx12 = 1;
  if (tok[2]) {
    int n = atoi(tok + 2);
    idx12 = (n == 2) ? 2 : 1;
  }
  return true;
}

bool graveyardSlotForCaptured(const char *pieceTok, bool &useTopPT, int &row, char &fileChar) {
  char bw, type;
  int idx12;
  if (!parsePieceToken(pieceTok, bw, type, idx12)) return false;

  if (bw == 'b') {
    useTopPT = true;
    if (type == 'P') return allocTopPawnSlot(row, fileChar);
    return majorSlotTop(type, idx12, row, fileChar);
  } else {
    useTopPT = false;
    if (type == 'P') return allocBottomPawnSlot(row, fileChar);
    return majorSlotBottom(type, idx12, row, fileChar);
  }
}

void printGraveyard() {
  Serial.print(F("DBG GRV PT pawns row0 mask="));
  Serial.println(ptPawnMask, BIN);
  Serial.print(F("DBG GRV PB pawns row1 mask="));
  Serial.println(pbPawnMask, BIN);
}

void clearGraveyard() {
  ptPawnMask = 0;
  pbPawnMask = 0;
}

// ===================== HIGH LEVEL CHESS MOVES =====================
bool captureMove(char fFrom, int rFrom, char fTo, int rTo, const char *capturedPieceTok, bool dryRun) {
  magnetOff();
  dbgMsg(F("MAG OFF (start CAP)"));

  bool useTopPT;
  int gyRow;
  char gyFile;

  if (!graveyardSlotForCaptured(capturedPieceTok, useTopPT, gyRow, gyFile)) {
    setLastError("BAD_CAPTURED_PIECE_TOKEN");
    return false;
  }

  float xTo, yTo, xFrom, yFrom;
  if (!squareToXY(fTo, rTo, xTo, yTo))   { setLastError("BAD_TO_SQUARE"); return false; }
  if (!squareToXY(fFrom, rFrom, xFrom, yFrom)) { setLastError("BAD_FROM_SQUARE"); return false; }

  float xGy, yGy;
  if (useTopPT) {
    if (!topParkingToXY(gyRow, gyFile, xGy, yGy)) { setLastError("BAD_TOP_GRAVEYARD_SLOT"); return false; }
  } else {
    if (!bottomParkingToXY(gyRow, gyFile, xGy, yGy)) { setLastError("BAD_BOTTOM_GRAVEYARD_SLOT"); return false; }
  }

  if (!smartMoveToXY(xTo, yTo)) return false;

  if (!dryRun) {
    if (!magnetOnOnlyIfAt(xTo, yTo)) return false;
  } else {
    dbgMsg(F("DRY skip MAG ON (captured)"));
  }

  if (!smartMoveToXY(xGy, yGy)) return false;

  if (!dryRun) {
    if (!magnetOffOnlyIfAt(xGy, yGy)) return false;
  } else {
    dbgMsg(F("DRY skip MAG OFF (drop captured)"));
  }

  if (!smartMoveToXY(xFrom, yFrom)) return false;

  if (!dryRun) {
    if (!magnetOnOnlyIfAt(xFrom, yFrom)) return false;
  } else {
    dbgMsg(F("DRY skip MAG ON (own piece)"));
  }

  if (!moveSafeSquares(fFrom, rFrom, fTo, rTo)) return false;

  if (!dryRun) {
    if (!magnetOffOnlyIfAt(xTo, yTo)) return false;
  } else {
    dbgMsg(F("DRY skip MAG OFF (drop own piece)"));
  }

  return true;
}

bool pieceMove(char fFrom, int rFrom, char fTo, int rTo, bool dryRun) {
  magnetOff();
  dbgMsg(F("MAG OFF (start PMV)"));

  float xFrom, yFrom, xTo, yTo;
  if (!squareToXY(fFrom, rFrom, xFrom, yFrom)) { setLastError("BAD_FROM_SQUARE"); return false; }
  if (!squareToXY(fTo, rTo, xTo, yTo)) { setLastError("BAD_TO_SQUARE"); return false; }

  if (!moveTo_mm(xFrom, yFrom)) return false;

  if (!dryRun) {
    if (!magnetOnOnlyIfAt(xFrom, yFrom)) return false;
  } else {
    dbgMsg(F("DRY skip MAG ON (pick)"));
  }

  if (!moveSafeSquares(fFrom, rFrom, fTo, rTo)) return false;

  if (!dryRun) {
    if (!magnetOffOnlyIfAt(xTo, yTo)) return false;
  } else {
    dbgMsg(F("DRY skip MAG OFF (drop)"));
  }

  return true;
}

// ===================== PARSING HELPERS =====================
bool parseSquareToken(const char *tok, char &fileChar, int &rank) {
  if (!tok || !tok[0] || !tok[1]) return false;

  fileChar = tok[0];
  if (fileChar >= 'a' && fileChar <= 'h') fileChar = (char)(fileChar - 'a' + 'A');
  if (fileChar < 'A' || fileChar > 'H') return false;

  rank = atoi(tok + 1);
  if (rank < 1 || rank > 8) return false;

  return true;
}

void showHelp() {
  Serial.println(F("DBG Commands:"));
  Serial.println(F("DBG   ?                         -> help"));
  Serial.println(F("DBG   P                         -> print position"));
  Serial.println(F("DBG   Z                         -> set current position as (0,0)"));
  Serial.println(F("DBG   H                         -> go home (0,0)"));
  Serial.println(F("DBG   STOP                      -> emergency stop motion"));
  Serial.println(F("DBG   C                         -> go center of main board"));
  Serial.println(F("DBG   B                         -> print board mapping params"));
  Serial.println(F("DBG   S <us>                    -> set pulse delay (150..3000)"));
  Serial.println(F("DBG   G <x_mm> <y_mm>           -> move absolute"));
  Serial.println(F("DBG   R <dx_mm> <dy_mm>         -> move relative"));
  Serial.println(F("DBG   SQ <A1..H8>               -> go to square center"));
  Serial.println(F("DBG   MV <sq1> <sq2>            -> FAST direct move sq1->sq2"));
  Serial.println(F("DBG   SMV <sq1> <sq2>           -> SAFE routing sq1->sq2"));
  Serial.println(F("DBG   PMV <sq1> <sq2> [DRY]     -> pick at sq1, carry to sq2, drop"));
  Serial.println(F("DBG   PB <row 0/1> <file A..H>  -> bottom parking slot"));
  Serial.println(F("DBG   PT <row 0/1> <file A..H>  -> top parking slot"));
  Serial.println(F("DBG   GRV                       -> print graveyard pawn masks"));
  Serial.println(F("DBG   CLRGRV                    -> clear graveyard pawn masks"));
  Serial.println(F("DBG   MAG 0/1                   -> magnet OFF/ON"));
  Serial.println(F("DBG   CAP <from> <to> <piece> [DRY] -> capture"));
  Serial.println(F("DBG   PING                      -> reply DONE"));
}

bool executeCommand(char *line) {
  clearLastError();

  char work[96];
  strncpy(work, line, sizeof(work) - 1);
  work[sizeof(work) - 1] = '\0';

  char *cmd = strtok(work, " ");
  if (!cmd) {
    setLastError("EMPTY_COMMAND");
    return false;
  }

  char cmdUp[16];
  strncpy(cmdUp, cmd, sizeof(cmdUp) - 1);
  cmdUp[sizeof(cmdUp) - 1] = '\0';
  toUpperInPlace(cmdUp);

  // ===== HELP / INFO =====
  if (strcmp(cmdUp, "?") == 0) {
    showHelp();
    return true;
  }

  if (strcmp(cmdUp, "PING") == 0) {
    return true;
  }

  if (strcmp(cmdUp, "P") == 0) {
    printPos();
    return true;
  }

  if (strcmp(cmdUp, "B") == 0) {
    printBoardParams();
    return true;
  }

  if (strcmp(cmdUp, "GRV") == 0) {
    printGraveyard();
    return true;
  }

  if (strcmp(cmdUp, "CLRGRV") == 0) {
    clearGraveyard();
    return true;
  }

  // ===== POSITION CONTROL =====
  if (strcmp(cmdUp, "Z") == 0) {
    curX_steps = 0;
    curY_steps = 0;
    positionKnown = true;
    return true;
  }

  if (strcmp(cmdUp, "H") == 0) {
    positionKnown = true;
    return moveTo_mm(0.0f, 0.0f);
  }

  if (strcmp(cmdUp, "STOP") == 0) {
    stopRequested = true;
    return true;
  }

  if (strcmp(cmdUp, "C") == 0) {
    float xA1, yA1, xH8, yH8;
    if (!squareToXY('A', 1, xA1, yA1)) { setLastError("MAP_FAIL_A1"); return false; }
    if (!squareToXY('H', 8, xH8, yH8)) { setLastError("MAP_FAIL_H8"); return false; }
    return moveTo_mm((xA1 + xH8) / 2.0f, (yA1 + yH8) / 2.0f);
  }

  // ===== SPEED =====
  if (strcmp(cmdUp, "S") == 0) {
    char *t1 = strtok(NULL, " ");
    if (!t1) { setLastError("USAGE_S_<US>"); return false; }

    int us = atoi(t1);
    if (us < 150) us = 150;
    if (us > 3000) us = 3000;
    pulseDelayUs = us;
    return true;
  }

  // ===== RAW XY =====
  if (strcmp(cmdUp, "G") == 0) {
    char *t1 = strtok(NULL, " ");
    char *t2 = strtok(NULL, " ");
    if (!t1 || !t2) { setLastError("USAGE_G_<X>_<Y>"); return false; }

    float x = (float)atof(t1);
    float y = (float)atof(t2);
    return moveTo_mm(x, y);
  }

  if (strcmp(cmdUp, "R") == 0) {
    char *t1 = strtok(NULL, " ");
    char *t2 = strtok(NULL, " ");
    if (!t1 || !t2) { setLastError("USAGE_R_<DX>_<DY>"); return false; }

    float dx = (float)atof(t1);
    float dy = (float)atof(t2);
    return moveRel_mm(dx, dy);
  }

  // ===== BOARD MOVES =====
  if (strcmp(cmdUp, "SQ") == 0) {
    char *t1 = strtok(NULL, " ");
    if (!t1) { setLastError("USAGE_SQ_<A1..H8>"); return false; }

    char f; int r;
    if (!parseSquareToken(t1, f, r)) { setLastError("BAD_SQUARE"); return false; }

    float x, y;
    if (!squareToXY(f, r, x, y)) { setLastError("BAD_SQUARE_MAPPING"); return false; }
    return moveTo_mm(x, y);
  }

  if (strcmp(cmdUp, "MV") == 0) {
    char *t1 = strtok(NULL, " ");
    char *t2 = strtok(NULL, " ");
    if (!t1 || !t2) { setLastError("USAGE_MV_<SQ1>_<SQ2>"); return false; }

    char f1, f2; int r1, r2;
    if (!parseSquareToken(t1, f1, r1) || !parseSquareToken(t2, f2, r2)) {
      setLastError("BAD_SQUARES");
      return false;
    }

    float x1, y1, x2, y2;
    if (!squareToXY(f1, r1, x1, y1) || !squareToXY(f2, r2, x2, y2)) {
      setLastError("BAD_SQUARE_MAPPING");
      return false;
    }

    if (!moveTo_mm(x1, y1)) return false;
    if (!moveTo_mm(x2, y2)) return false;
    return true;
  }

  if (strcmp(cmdUp, "SMV") == 0) {
    char *t1 = strtok(NULL, " ");
    char *t2 = strtok(NULL, " ");
    if (!t1 || !t2) { setLastError("USAGE_SMV_<SQ1>_<SQ2>"); return false; }

    char f1, f2; int r1, r2;
    if (!parseSquareToken(t1, f1, r1) || !parseSquareToken(t2, f2, r2)) {
      setLastError("BAD_SQUARES");
      return false;
    }

    return moveSafeSquares(f1, r1, f2, r2);
  }

  if (strcmp(cmdUp, "PMV") == 0) {
    char *tFrom = strtok(NULL, " ");
    char *tTo   = strtok(NULL, " ");
    char *t3    = strtok(NULL, " ");

    if (!tFrom || !tTo) { setLastError("USAGE_PMV_<FROM>_<TO>_[DRY]"); return false; }

    bool dryRun = false;
    if (t3) {
      char tmp[8];
      strncpy(tmp, t3, sizeof(tmp) - 1);
      tmp[sizeof(tmp) - 1] = '\0';
      toUpperInPlace(tmp);
      if (strcmp(tmp, "DRY") == 0) dryRun = true;
    }

    char fFrom, fTo; int rFrom, rTo;
    if (!parseSquareToken(tFrom, fFrom, rFrom) || !parseSquareToken(tTo, fTo, rTo)) {
      setLastError("BAD_SQUARES");
      return false;
    }

    return pieceMove(fFrom, rFrom, fTo, rTo, dryRun);
  }

  if (strcmp(cmdUp, "CAP") == 0) {
    char *tFrom = strtok(NULL, " ");
    char *tTo   = strtok(NULL, " ");
    char *tPc   = strtok(NULL, " ");
    char *t4    = strtok(NULL, " ");

    if (!tFrom || !tTo || !tPc) { setLastError("USAGE_CAP_<FROM>_<TO>_<PIECE>_[DRY]"); return false; }

    bool dryRun = false;
    if (t4) {
      char tmp[8];
      strncpy(tmp, t4, sizeof(tmp) - 1);
      tmp[sizeof(tmp) - 1] = '\0';
      toUpperInPlace(tmp);
      if (strcmp(tmp, "DRY") == 0) dryRun = true;
    }

    char fFrom, fTo; int rFrom, rTo;
    if (!parseSquareToken(tFrom, fFrom, rFrom) || !parseSquareToken(tTo, fTo, rTo)) {
      setLastError("BAD_SQUARES");
      return false;
    }

    return captureMove(fFrom, rFrom, fTo, rTo, tPc, dryRun);
  }

  // ===== PARKING =====
  if (strcmp(cmdUp, "PB") == 0) {
    char *t1 = strtok(NULL, " ");
    char *t2 = strtok(NULL, " ");
    if (!t1 || !t2) { setLastError("USAGE_PB_<ROW>_<FILE>"); return false; }

    int row = atoi(t1);
    char fileC = t2[0];
    if (fileC >= 'a' && fileC <= 'h') fileC = (char)(fileC - 'a' + 'A');

    float x, y;
    if (!bottomParkingToXY(row, fileC, x, y)) { setLastError("BAD_PB_SLOT"); return false; }
    return moveTo_mm(x, y);
  }

  if (strcmp(cmdUp, "PT") == 0) {
    char *t1 = strtok(NULL, " ");
    char *t2 = strtok(NULL, " ");
    if (!t1 || !t2) { setLastError("USAGE_PT_<ROW>_<FILE>"); return false; }

    int row = atoi(t1);
    char fileC = t2[0];
    if (fileC >= 'a' && fileC <= 'h') fileC = (char)(fileC - 'a' + 'A');

    float x, y;
    if (!topParkingToXY(row, fileC, x, y)) { setLastError("BAD_PT_SLOT"); return false; }
    return moveTo_mm(x, y);
  }

  // ===== MAGNET =====
  if (strcmp(cmdUp, "MAG") == 0) {
    char *t1 = strtok(NULL, " ");
    if (!t1) { setLastError("USAGE_MAG_0_OR_1"); return false; }

    int v = atoi(t1);
    if (v == 1) {
      magnetOn();
      dbgMsg(F("MAG ON (manual)"));
    } else {
      magnetOff();
      dbgMsg(F("MAG OFF (manual)"));
    }
    return true;
  }

  setLastError("UNKNOWN_COMMAND");
  return false;
}

// ===================== SETUP / LOOP =====================
void setup() {
  pinMode(STEP_A, OUTPUT);
  pinMode(DIR_A, OUTPUT);
  pinMode(STEP_B, OUTPUT);
  pinMode(DIR_B, OUTPUT);

  pinMode(MAGNET_PIN, OUTPUT);
  magnetOff();

  Serial.begin(115200);
  delay(300);

  Serial.println(F("DBG CoreXY controller ready"));
  Serial.print(F("DBG STEPS_PER_MM=")); Serial.println(STEPS_PER_MM, 4);
  Serial.print(F("DBG Limits: X 0..")); Serial.print(MAX_X_MM);
  Serial.print(F(" mm, Y 0..")); Serial.print(MAX_Y_MM); Serial.println(F(" mm"));
  Serial.print(F("DBG pulseDelayUs=")); Serial.println(pulseDelayUs);
  Serial.print(F("DBG MAGNET_PIN=D")); Serial.println(MAGNET_PIN);
  Serial.println(F("READY"));
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;

    if (c == '\n') {
      lineBuf[lineLen] = '\0';
      lineLen = 0;

      trimInPlace(lineBuf);
      if (lineBuf[0] == '\0') return;

      sendAck();

      bool ok = executeCommand(lineBuf);

      if (ok) sendDone();
      else sendLastError();

      return;
    }

    if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    } else {
      lineLen = 0;
      setLastError("LINE_TOO_LONG");
      sendErrMsg("LINE_TOO_LONG");
    }
  }
}