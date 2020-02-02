
//------------------------------------------------------------------------------
// Makelangelo - firmware for various robot kinematic models
// dan@marginallycelver.com 2013-12-26
// Please see http://www.github.com/MarginallyClever/makelangeloFirmware for more information.
//------------------------------------------------------------------------------


//------------------------------------------------------------------------------
// INCLUDES
//------------------------------------------------------------------------------
#include "configure.h"
#include "motor.h"
#include "SDcard.h"
#include "LCD.h"
#include "eeprom.h"

#include <SPI.h>  // pkm fix for Arduino 1.5

#include "Vector3.h"
#include "sdcard.h"


//------------------------------------------------------------------------------
// GLOBALS
//------------------------------------------------------------------------------

// robot UID
int robot_uid = 0;

// description of the machine's position
Axis axies[NUM_AXIES];

// length of belt when weights hit limit switch
float calibrateRight  = 1011.0;
float calibrateLeft   = 1011.0;

// plotter position.
float feed_rate = DEFAULT_FEEDRATE;
float acceleration = DEFAULT_ACCELERATION;
float step_delay;

char absolute_mode = 1; // absolute or incremental programming mode?

// Serial comm reception
char serialBuffer[MAX_BUF + 1]; // Serial buffer
int sofar;                      // Serial buffer progress
long last_cmd_time;             // prevent timeouts
long line_number = 0;           // make sure commands arrive in order
uint8_t lastGcommand = -1;

uint8_t current_tool = 0;

#if MACHINE_STYLE == SIXI
uint32_t reportDelay = 0;
#endif

#ifdef HAS_WIFI

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
const char* SSID_NAME = WIFI_SSID_NAME;
const char* SSID_PASS = WIFI_SSID_PASS;
WiFiUDP port;
unsigned int localPort = 9999;

#endif  // HAS_WIFI




//------------------------------------------------------------------------------
// METHODS
//------------------------------------------------------------------------------




void findStepDelay() {
  step_delay = 1000000.0f / (DEFAULT_FEEDRATE / MM_PER_STEP);
}


// returns angle of dy/dx as a value from 0...2PI
float atan3(float dy, float dx) {
  float a = atan2(dy, dx);
  if (a < 0) a = (PI * 2.0) + a;
  return a;
}


/**
   feed rate is given in units/min and converted to cm/s
*/
void setFeedRate(float v1) {
  if ( feed_rate != v1 ) {
    feed_rate = v1;
    if (feed_rate < MIN_FEEDRATE) feed_rate = MIN_FEEDRATE;
#ifdef VERBOSE
    Serial.print(F("F="));
    Serial.println(feed_rate);
#endif
  }
}


/**
   @param delay in microseconds
*/
void pause(const long us) {
  delay(us / 1000);
  delayMicroseconds(us % 1000);
}


/**
   M101 Annn Tnnn Bnnn
   Change axis A limits to max T and min B.
   look for change to dimensions in command, apply and save changes.
*/
void parseLimits() {
  int axisNumber = parseNumber('A', -1);
  if (axisNumber >= 0 && axisNumber < NUM_AXIES) {
    float newT = parseNumber('T', axies[axisNumber].limitMax);
    float newB = parseNumber('B', axies[axisNumber].limitMin);
    boolean changed = false;

    if (!equalEpsilon(axies[axisNumber].limitMax, newT)) {
      axies[axisNumber].limitMax = newT;
      changed = true;
    }
    if (!equalEpsilon(axies[axisNumber].limitMin, newB)) {
      axies[axisNumber].limitMin = newB;
      changed = true;
    }
    if (changed == true) {
      saveLimits();
    }
  }
  printConfig();
}


/**
   Test that IK(FK(A))=A
*/
void testKinematics() {
  long A[NUM_MOTORS], i, j;
  float axies1[NUM_AXIES];
  float axies2[NUM_AXIES];

  for (i = 0; i < 3000; ++i) {
    for (j = 0; j < NUM_AXIES; ++j) {
      axies1[j] = random(axies[j].limitMin, axies[j].limitMax);
    }

    IK(axies1, A);
    FK(A, axies2);

    for (j = 0; j < NUM_AXIES; ++j) {
      Serial.print('\t');
      Serial.print(AxisNames[j]);
      Serial.print(axies1[j]);
    }
    for (j = 0; j < NUM_MOTORS; ++j) {
      Serial.print('\t');
      Serial.print(MotorNames[j]);
      Serial.print(A[j]);
    }
    for (j = 0; j < NUM_AXIES; ++j) {
      Serial.print('\t');
      Serial.print(AxisNames[j]);
      Serial.print('\'');
      Serial.print(axies2[j]);
    }
    for (j = 0; j < NUM_AXIES; ++j) {
      Serial.print(F("\td"));
      Serial.print(AxisNames[j]);
      Serial.print('=');
      Serial.print(axies2[j] - axies1[j]);
    }
    Serial.println();
  }
}


/**
   Split long moves into sub-moves if needed.
   @input pos NUM_AXIES floats describing destination coordinates
   @input new_feed_rate speed to travel along arc
*/
void lineSafe(float *pos, float new_feed_rate_mms) {
  // Remember the feed rate.  This value will be used whenever no feedrate is given in a command, so it MUST be saved BEFORE the dial adjustment.
  // otherwise the feedrate will slowly fall or climb as new commands are processed.
  feed_rate = new_feed_rate_mms;
  
#ifdef HAS_LCD
  // use LCD to adjust speed while drawing
  new_feed_rate_mms *= (float)speed_adjust * 0.01f;
#endif

  // split up long lines to make them straighter
  float delta[NUM_AXIES];
  float startPos[NUM_AXIES];
  float lenSquared = 0;
  uint8_t i;
  for (i = 0; i < NUM_AXIES; ++i) {
    startPos[i] = axies[i].pos;
    delta[i] = pos[i] - startPos[i];
    lenSquared += sq(delta[i]);
  }

#if MACHINE_STYLE == POLARGRAPH
  if(delta[0]==0 && delta[1]==0) {
    // only moving Z, don't split the line.
    motor_line(pos, new_feed_rate_mms, abs(delta[2]));
    return;
  }
#endif

  float len_mm = sqrt(lenSquared);
  if(abs(len_mm)<0.000001f) return;

  const float seconds = len_mm / new_feed_rate_mms;
  uint16_t segments = seconds * SEGMENTS_PER_SECOND;
  if(segments<1) segments=1;
  
  const float inv_segments = 1.0f / float(segments);
  const float segment_len_mm = len_mm * inv_segments;
  
  for(i = 0; i < NUM_AXIES; ++i) delta[i] *= inv_segments;
  
  while(--segments) {
    for(i = 0; i < NUM_AXIES; ++i) startPos[i] += delta[i];
    
    motor_line(startPos, new_feed_rate_mms,segment_len_mm);
  }

  // guarantee we stop exactly at the destination (no rounding errors).
  motor_line(pos, new_feed_rate_mms,segment_len_mm);

//  Serial.print("P");  Serial.println(movesPlanned());
}


/**
   This method assumes the limits have already been checked.
   This method assumes the start and end radius match.
   This method assumes arcs are not >180 degrees (PI radians)
   @input cx center of circle x value
   @input cy center of circle y value
   @input destination point where movement ends
   @input dir - ARC_CW or ARC_CCW to control direction of arc
   @input new_feed_rate speed to travel along arc
*/
void arc(float cx, float cy, float *destination, char clockwise, float new_feed_rate) {
  // get radius
  float dx = axies[0].pos - cx;
  float dy = axies[1].pos - cy;
  float sr = sqrt(dx * dx + dy * dy);

  // find angle of arc (sweep)
  float sa = atan3(dy, dx);
  float ea = atan3(destination[1] - cy, destination[0] - cx);
  float er = sqrt(dx * dx + dy * dy);

  float da = ea - sa;
  if (clockwise != 0 && da < 0) ea += 2 * PI;
  else if (clockwise == 0 && da > 0) sa += 2 * PI;
  da = ea - sa;
  float dr = er - sr;

  // get length of arc
  // float circ=PI*2.0*radius;
  // float len=theta*circ/(PI*2.0);
  // simplifies to
  float len1 = abs(da) * sr;
  float len = sqrt( len1 * len1 + dr * dr ); // mm

  int i, segments = ceil( len );

  float n[NUM_AXIES], scale;
  float a, r;
#if NUM_AXIES>2
  float sz = axies[2].pos;
  float z = destination[2];
#endif

  for (i = 0; i <= segments; ++i) {
    // interpolate around the arc
    scale = ((float)i) / ((float)segments);

    a = ( da * scale ) + sa;
    r = ( dr * scale ) + sr;

    n[0] = cx + cos(a) * r;
    n[1] = cy + sin(a) * r;
#if NUM_AXIES>2
    n[2] = ( z - sz ) * scale + sz;
#endif
    // send it to the planner
    lineSafe(n, new_feed_rate);
  }
}


/**
   Instantly move the virtual plotter position.  Does not check if the move is valid.
*/
void teleport(float *pos) {
  wait_for_empty_segment_buffer();

  int i;
  for (i = 0; i < NUM_AXIES; ++i) {
    axies[i].pos = pos[i];
  }

  long steps[NUM_MOTORS + NUM_SERVOS];
  IK(pos, steps);
  motor_set_step_count(steps);
}


void sayBuildDateAndTime() {
  Serial.print("Built ");  
  Serial.println(__DATE__ " " __TIME__);
}


/**
   M100
   Print a helpful message to serial.  The first line must never be changed to play nice with the JAVA software.
*/
void help() {
  Serial.print(F("\n\nHELLO WORLD! "));
  sayModelAndUID();
  sayFirmwareVersionNumber();
  sayBuildDateAndTime();
  Serial.println(F("Please see http://makelangelo.com/ for more information."));
  Serial.println(F("Try these (with a newline): G00,G01,G02,G03,G04,G28,G90,G91,G92,M18,M101,M100,M114"));
#ifdef HAS_WIFI
  // Print the IP address
  Serial.print("Use this URL to connect: http://");
  Serial.print(WiFi.softAPIP());
  Serial.print(':');
  Serial.print(localPort);
  Serial.println('/');
#endif  // HAS_WIFI
#ifdef HAS_LCD
  Serial.println(F("Has LCD"));
#endif
}


void sayModelAndUID() {
  Serial.print(F("I AM "));
  Serial.print(MACHINE_STYLE_NAME);
  Serial.print(F(" #"));
  Serial.println(robot_uid);
}


/**
   D5
   report current firmware version
*/
void sayFirmwareVersionNumber() {
  char versionNumber = loadVersion();

  Serial.print(F("Firmware v"));
  Serial.println(versionNumber, DEC);
}


/**
   M114
   Print the X,Y,Z, feedrate, acceleration, and home position
*/
void where() {
  wait_for_empty_segment_buffer();

  int i;
  for (i = 0; i < NUM_AXIES; ++i) {
    Serial.print(AxisNames[i]);
    Serial.print(axies[i].pos);
    Serial.print(' ');
  }

  Serial.print(F("F"));
  Serial.print(feed_rate);
  Serial.print(F("mm/s"));

  Serial.print(F(" A"));
  Serial.println(acceleration);
}


/**
   D23 report home values
*/
void reportHome() {
  Serial.print(F("D23 "));
  for (int i = 0; i < NUM_AXIES; ++i) {
    Serial.print(AxisNames[i]);
    Serial.print(axies[i].homePos);
    Serial.print(' ');
  }
  Serial.println();
}


/**
   Print the machine limits to serial.
*/
void printConfig() {
  int i;

  Serial.print(F("("));

  for (i = 0; i < NUM_AXIES; ++i) {
    Serial.print(axies[i].limitMin);
    if (i < NUM_AXIES - 1)  Serial.print(',');
  }

  Serial.print(F(") - ("));

  for (i = 0; i < NUM_AXIES; ++i) {
    Serial.print(axies[i].limitMax);
    if (i < NUM_AXIES - 1)  Serial.print(',');
  }

  Serial.print(F(")\n"));
}


/**
   M6 [Tnnn]
   Change the currently active tool
*/
void toolChange(int tool_id) {
  if (tool_id < 0) tool_id = 0;
  if (tool_id >= NUM_TOOLS) tool_id = NUM_TOOLS - 1;
  current_tool = tool_id;
}


/**
   Look for character /code/ in the buffer and read the float that immediately follows it.
   @return the value found.  If nothing is found, /val/ is returned.
   @input code the character to look for.
   @input val the return value if /code/ is not found.
*/
float parseNumber(char code, float val) {
  char *ptr = serialBuffer; // start at the beginning of buffer
  char *finale = serialBuffer + sofar;
  for (ptr = serialBuffer; ptr < finale; ++ptr) { // walk to the end
    if (*ptr == ';') break;
    if (toupper(*ptr) == code) { // if you find code on your walk,
      return atof(ptr + 1); // convert the digits that follow into a float and return it
    }
  }
  return val;  // end reached, nothing found, return default val.
}


/**
   @return 1 if the character is found in the serial buffer, 0 if it is not found.
*/
char hasGCode(char code) {
  char *ptr = serialBuffer; // start at the beginning of buffer
  char *finale = serialBuffer + sofar;
  for (ptr = serialBuffer; ptr < finale; ++ptr) { // walk to the end
    if (*ptr == ';') break;
    if (toupper(*ptr) == code) { // if you find code on your walk,
      return 1;  // found
    }
  }
  return 0;  // not found
}

/**
   G4 [Snn] [Pnn]
   Wait S milliseconds and P seconds.
*/
void parseDwell() {
  wait_for_empty_segment_buffer();
  float delayTime = parseNumber('S', 0) + parseNumber('P', 0) * 1000.0f;
  pause(delayTime);
}


/**
   G0-G1 [Xnnn] [Ynnn] [Znnn] [Unnn] [Vnnn] [Wnnn] [Ann] [Fnn]
   straight lines.  distance in mm.
*/
void parseLine() {
  acceleration = parseNumber('A', acceleration);
  acceleration = min(max(acceleration, (float)MIN_ACCELERATION), (float)MAX_ACCELERATION);
  float f = parseNumber('F', feed_rate);
  f = min(max(f, (float)MIN_FEEDRATE), (float)MAX_FEEDRATE);

  int i;
  float pos[NUM_AXIES];
  for (i = 0; i < NUM_AXIES; ++i) {
    float p = axies[i].pos;
    pos[i] = parseNumber(AxisNames[i], (absolute_mode ? p : 0)) + (absolute_mode ? 0 : p);
  }

  lineSafe( pos, f );
}


/**
   G2-G3 [Xnnn] [Ynnn] [Ann] [Fnn] [Inn] [Jnn]
   arcs in the XY plane
   @param clockwise (G2) 1 for cw, (G3) 0 for ccw
*/
void parseArc(int clockwise) {
  acceleration = parseNumber('A', acceleration);
  acceleration = min(max(acceleration, (float)MIN_ACCELERATION), (float)MAX_ACCELERATION);
  float f = parseNumber('F', feed_rate);
  f = min(max(f, (float)MIN_FEEDRATE), (float)MAX_FEEDRATE);

  int i;
  float pos[NUM_AXIES];
  for (i = 0; i < NUM_AXIES; ++i) {
    float p = axies[i].pos;
    pos[i] = parseNumber(AxisNames[i], (absolute_mode ? p : 0)) + (absolute_mode ? 0 : p);
  }

    float p0 = axies[0].pos;
    float p1 = axies[1].pos;
  arc(parseNumber('I', (absolute_mode ? p0 : 0)) + (absolute_mode ? 0 : p0),
      parseNumber('J', (absolute_mode ? p1 : 0)) + (absolute_mode ? 0 : p1),
      pos,
      clockwise,
      f );
}


/**
   G92 [Xnnn] [Ynnn] [Znnn] [Unnn] [Vnnn] [Wnnn]
   Teleport mental position
*/
void parseTeleport() {
  int i;
  float pos[NUM_AXIES];
  for (i = 0; i < NUM_AXIES; ++i) {
    float p = axies[i].pos;
    pos[i] = parseNumber(AxisNames[i], (absolute_mode ? p : 0)) + (absolute_mode ? 0 : p);
  }
  teleport(pos);
}


/**
   @return 1 if CRC ok or not present, 0 if CRC check fails.
*/
char checkLineNumberAndCRCisOK() {
  // is there a line number?
  long cmd = parseNumber('N', -1);
  if (cmd != -1 && serialBuffer[0] == 'N') { // line number must appear first on the line
    if ( cmd != line_number ) {
      // wrong line number error
      Serial.print(F("BADLINENUM "));
      Serial.println(line_number);
      return 0;
    }

    // is there a checksum?
    int i;
    for (i = strlen(serialBuffer) - 1; i >= 0; --i) {
      if (serialBuffer[i] == '*') {
        break;
      }
    }

    if (i >= 0) {
      // yes.  is it valid?
      char checksum = 0;
      int c;
      for (c = 0; c < i; ++c) {
        checksum ^= serialBuffer[c];
      }
      c++; // skip *
      int against = strtod(serialBuffer + c, NULL);
      if ( checksum != against ) {
        Serial.print(F("BADCHECKSUM "));
        Serial.println(line_number);
        return 0;
      }
    } else {
      Serial.print(F("NOCHECKSUM "));
      Serial.println(line_number);
      return 0;
    }

    // remove checksum
    serialBuffer[i] = 0;

    line_number++;
  }

  return 1;  // ok!
}


/**
   M117 [string]
   Display string on the LCD panel.  Command is ignored if there is no LCD panel.
*/
void parseMessage() {
#ifdef HAS_LCD
  // find M
  uint16_t i = 0;
  // skip "N*** M117 "
  while(serialBuffer[i]!='M') ++i;
  i+=5;
  // anything left?
  if (i >= strlen(serialBuffer)) {
    // no message
    LCD_setStatusMessage(0);
    return;
  }
  
  // read in any remaining message
  char message[M117_MAX_LEN];
  char *buf = serialBuffer + i;
  i = 0;
  while (isPrintable(*buf) && (*buf) != '\r' && (*buf) != '\n' && i < M117_MAX_LEN) {
    message[i++] = *buf;
    buf++;
  }
  
  // make sure the message is properly terminated
  message[i]=0;
  // update the LCD.
  LCD_setStatusMessage(message);
#endif  // HAS_LCD
}

/**
   M203 X2000 Y5000 Z200 etc...
   adjust the max feedrate of each axis
*/
void adjustMaxFeedRates() {
  int i;
  Serial.print(F("M203 "));
  for (i = 0; i < NUM_MOTORS + NUM_SERVOS; ++i) {
    max_feedrate_mm_s[i] = parseNumber(MotorNames[i], max_feedrate_mm_s[i]);
    Serial.print(MotorNames[i]);
    Serial.print(max_feedrate_mm_s[i]);
    Serial.print(' ');
  }
  Serial.println();
}

/**
   M205 X<jerk> Y<jerk> Z<jerk> U<jerk> V<jerk> W<jerk> B<us>
   adjust max jerk
*/
void parseAdvancedSettings() {
  float f;
  f = parseNumber('X', max_jerk[0]);  max_jerk[0] = max(min(f, (float)MAX_JERK), (float)0);
#if NUM_AXIES>1
  f = parseNumber('Y', max_jerk[1]);  max_jerk[1] = max(min(f, (float)MAX_JERK), (float)0);
#endif
#if NUM_AXIES>2
  f = parseNumber('Z', max_jerk[2]);  max_jerk[2] = max(min(f, (float)MAX_JERK), (float)0);
#endif
#if NUM_AXIES>3
  f = parseNumber('U', max_jerk[3]);  max_jerk[3] = max(min(f, (float)MAX_JERK), (float)0);
#endif
#if NUM_AXIES>4
  f = parseNumber('V', max_jerk[4]);  max_jerk[4] = max(min(f, (float)MAX_JERK), (float)0);
#endif
#if NUM_AXIES>5
  f = parseNumber('W', max_jerk[5]);  max_jerk[5] = max(min(f, (float)MAX_JERK), (float)0);
#endif
  f = parseNumber('B', min_segment_time_us);  min_segment_time_us = max(min(f, 1000000), (float)0);
  Serial.print("M205 X");  Serial.print(max_jerk[0]);
  Serial.print(" Y");  Serial.print(max_jerk[1]);
  Serial.print(" Z");  Serial.print(max_jerk[2]);
  Serial.print(" U");  Serial.print(max_jerk[3]);
  Serial.print(" V");  Serial.print(max_jerk[4]);
  Serial.print(" W");  Serial.print(max_jerk[5]);
  Serial.print(" B");  Serial.println(min_segment_time_us);
}

/**
   M226 P[a] S[b]
   Wait for pin a to be in state b (1 or 0).
   If there is an LCD and P or S are missing, wait for user to press click wheel on LCD.
   If there is no LCD, P must be specified.
*/
void waitForPinState() {
#ifdef HAS_LCD
  int pin = parseNumber('P', BTN_ENC);
#else
  int pin = parseNumber('P', -1);
#endif
  if (pin == -1) return; // no pin specified.

  int oldState = parseNumber('S', -1);
  if (oldState == -1) {
    // default: assume the pin is not in the requested state
    oldState = digitalRead(pin);
  } else {
    // 0 for HIGH, anything else for LOW
    oldState = (oldState == 0) ? HIGH : LOW;
  }
  Serial.print("pausing");
#ifdef HAS_SD
  sd_printing_paused = true;
#endif

  // while pin is in oldState (opposite of state for which we are waiting)
  while (digitalRead(pin) == oldState) {
#ifdef HAS_SD
    //SD_check();
#endif
#ifdef HAS_LCD
    //LCD_update();  // causes menu to change when we don't want it to change.
#endif
    //Serial.print(".");
  }

#ifdef HAS_SD
  sd_printing_paused = false;
#endif

  Serial.println(" ended.");
}


/**
   M42 P[a] S[b]
   Set digital pin a to state b (1 or 0).
   default pin is LED_BUILTIN.  default state is LOW
*/
void adjustPinState() {
  int pin = parseNumber('P', LED_BUILTIN);
  int newState = parseNumber('S', 0);
  digitalWrite(pin, newState ? HIGH : LOW);
}


/**
   M300 S[a] P[b]
   play frequency a for b milliseconds
*/
void parseBeep() {
#ifdef HAS_LCD
  int ms = parseNumber('P', 250);
  //int freq = parseNumber('S', 60);

  digitalWrite(BEEPER, HIGH);
  delay(ms);
  digitalWrite(BEEPER, LOW);
#endif
}


/**
   process commands in the serial receive buffer
*/
void processCommand() {
  if( serialBuffer[0] == '\0' || serialBuffer[0] == ';' ) return;  // blank lines
  if(!checkLineNumberAndCRCisOK()) return; // message garbled
  // remove any trailing semicolon.
  int last = strlen(serialBuffer)-1;
  if( serialBuffer[last] == ';') serialBuffer[last]=0;
  
  if( !strncmp(serialBuffer, "UID", 3) ) {
    robot_uid = atoi(strchr(serialBuffer, ' ') + 1);
    saveUID();
  }

  long cmd;

  // M codes
  cmd = parseNumber('M', -1);
  switch (cmd) {
    case   6:  toolChange(parseNumber('T', current_tool));  break;
    case  17:  motor_engage();  break;
    case  18:  motor_disengage();  break;
#ifdef HAS_SD
    case  20:  SD_listFiles();  break;
#endif
    case  42:  adjustPinState();  break;
    case 100:  help();  break;
    case 101:  parseLimits();  break;
    case 110:  line_number = parseNumber('N', line_number);  break;
    case 114:  where();  break;
    case 117:  parseMessage();  break;
    case 203:  adjustMaxFeedRates();  break;
    case 205:  parseAdvancedSettings();  break;
    case 226:  waitForPinState();  break;
    case 300:  parseBeep();  break;
    default:   break;
  }
  if (cmd != -1) return; // M command processed, stop.

  // machine style-specific codes
  cmd = parseNumber('D', -1);
  switch (cmd) {
    case  0:  jogMotors();  break;
#ifdef HAS_SD
//    case  4:  SD_StartPrintingFile(strchr(serialBuffer, ' ') + 1);  break;
#endif
    case  5:  sayFirmwareVersionNumber();  break;
    case  6:  parseSetHome();  break;
    case  7:  setCalibration();  break;
    case  8:  reportCalibration();  break;
    case  9:  saveCalibration();  break;
    case 10:  // get hardware version
      Serial.print(F("D10 V"));
      Serial.println(MACHINE_HARDWARE_VERSION);
      break;
#if MACHINE_STYLE == POLARGRAPH
    case 11:  makelangelo6Setup();  break;
    //case 12:  recordHome();  break;
#endif
#ifdef MACHINE_HAS_LIFTABLE_PEN
    case 13:  setPenAngle(parseNumber('Z', axies[2].pos));  break;
#endif
    case 14:  // get machine style
      Serial.print(F("D14 "));
      Serial.println(MACHINE_STYLE_NAME);
      break;
#if MACHINE_STYLE == STEWART
    case 15:  stewartDemo();  break;
#endif
#if MACHINE_STYLE == SIXI
    case 16:  setFeedratePerAxis();  break;
    case 17:  reportAllAngleValues();  break;
    case 18:  copySensorsToMotorPositions();  break;
    case 19:  positionErrorFlags ^= POSITION_ERROR_FLAG_CONTINUOUS;  break; // toggle
    case 20:  positionErrorFlags &= 0xffff ^ (POSITION_ERROR_FLAG_ERROR | POSITION_ERROR_FLAG_FIRSTERROR);  break; // off
    case 21:  positionErrorFlags ^= POSITION_ERROR_FLAG_ESTOP;  break; // toggle ESTOP
#endif
#if MACHINE_STYLE == POLARGRAPH
    case 22:  makelangelo33Setup();  break;
#endif
#if MACHINE_STYLE == SIXI
    case 22:  sixiResetSensorOffsets();  break;
#endif
    case 23:  reportHome();  break;
    default:  break;
  }
  if (cmd != -1) return; // D command processed, stop.

  // no M or D commands were found.  This is probably a G-command.
  // G codes
  cmd = parseNumber('G', lastGcommand);
  lastGcommand = -1;
  switch (cmd) {
    case  0:
    case  1:  parseLine();  lastGcommand = cmd;  break;
    case  2:  parseArc(1);  lastGcommand = cmd;  break; // clockwise
    case  3:  parseArc(0);  lastGcommand = cmd;  break; // counter-clockwise
    case  4:  parseDwell();  break;
    case 28:  robot_findHome();  break;
#if MACHINE_STYLE == POLARGRAPH
    //case 29:  calibrateBelts();  break;
#endif
    case 90:  absolute_mode = 1;  break; // absolute mode
    case 91:  absolute_mode = 0;  break; // relative mode
    case 92:  parseTeleport();  break;
    default:  break;
  }
}


/**
   D16 X<jerk> Y<jerk> Z<jerk> U<jerk> V<jerk> W<jerk>
   set axis n to feedrate m and jerk o.
*/
void setFeedratePerAxis() {
  float f;

  f = parseNumber('X', max_feedrate_mm_s[0]);  max_feedrate_mm_s[0] = max(f, (float)MIN_FEEDRATE);
#if NUM_AXIES > 1
  f = parseNumber('Y', max_feedrate_mm_s[1]);  max_feedrate_mm_s[1] = max(f, (float)MIN_FEEDRATE);
#endif
#if NUM_AXIES > 2
  f = parseNumber('Z', max_feedrate_mm_s[2]);  max_feedrate_mm_s[2] = max(f, (float)MIN_FEEDRATE);
#endif
#if NUM_AXIES > 3
  f = parseNumber('U', max_feedrate_mm_s[3]);  max_feedrate_mm_s[3] = max(f, (float)MIN_FEEDRATE);
#endif
#if NUM_AXIES > 4
  f = parseNumber('V', max_feedrate_mm_s[4]);  max_feedrate_mm_s[4] = max(f, (float)MIN_FEEDRATE);
#endif
#if NUM_AXIES > 5
  f = parseNumber('W', max_feedrate_mm_s[5]);  max_feedrate_mm_s[5] = max(f, (float)MIN_FEEDRATE);
#endif
  Serial.print("D16 X");  Serial.print(max_feedrate_mm_s[0]);
  Serial.print(" Y");  Serial.print(max_feedrate_mm_s[1]);
  Serial.print(" Z");  Serial.print(max_feedrate_mm_s[2]);
  Serial.print(" U");  Serial.print(max_feedrate_mm_s[3]);
  Serial.print(" V");  Serial.print(max_feedrate_mm_s[4]);
  Serial.print(" W");  Serial.println(max_feedrate_mm_s[5]);
}


#if MACHINE_STYLE == POLARGRAPH
// convert belt length to cartesian position, save that as home pos.
void calibrationToHomePosition() {
  float axies2[NUM_AXIES];
  long steps[3];
  steps[0]=calibrateLeft;
  steps[1]=calibrateRight;
  steps[2]=axies[2].pos;
  FK(steps, axies2);
  
  float homePos[NUM_AXIES];
  homePos[0] = axies2[0];
  homePos[1] = axies2[1];
  homePos[2] = axies2[2];
  setHome(homePos);
}

/**
   D11 makelangelo 6 specific setup call
*/
void makelangelo6Setup() {
  // if you accidentally upload m3 firmware to an m5 then upload it ONCE with this line uncommented.
  float limits[NUM_AXIES * 2];
  limits[0] = 707.5 / 2;
  limits[1] = -707.5 / 2;
  limits[2] = 500;
  limits[3] = -500;
  limits[4] = PEN_UP_ANGLE;
  limits[5] = PEN_DOWN_ANGLE;
  adjustLimits(limits);

  calibrateLeft = 1025;
  calibrateRight = 1025;
  saveCalibration();
  calibrationToHomePosition();
}
/**
   D11 makelangelo 5 specific setup call
*/
void makelangelo5Setup() {
  // if you accidentally upload m3 firmware to an m5 then upload it ONCE with this line uncommented.
  float limits[NUM_AXIES * 2];
  limits[0] = 325.0;
  limits[1] = -325.0;
  limits[2] = 500;
  limits[3] = -500;
  limits[4] = PEN_UP_ANGLE;
  limits[5] = PEN_DOWN_ANGLE;
  adjustLimits(limits);

  calibrateLeft = 1025;
  calibrateRight = 1025;
  saveCalibration();
  calibrationToHomePosition();
}


/**
   D13 makelangelo 3.3 specific setup call
*/
void makelangelo33Setup() {
  float limits[NUM_AXIES * 2];
  limits[0] = 1000.0;
  limits[1] = -1000.0;
  limits[2] = 800;
  limits[3] = -800;
  limits[4] = PEN_UP_ANGLE;
  limits[5] = PEN_DOWN_ANGLE;
  adjustLimits(limits);

  calibrateLeft = 2022;
  calibrateRight = 2022;
  saveCalibration();
  calibrationToHomePosition();
}
#endif


/**
   D6 [Xnnn] [Ynnn] [Znnn] [Unnn] [Vnnn] [Wnnn]
   Set home position for each axis.
*/
void parseSetHome() {
  int i;
  float homePos[NUM_AXIES];
  for (i = 0; i < NUM_AXIES; ++i) {
    homePos[i] = parseNumber(AxisNames[i], axies[i].homePos);
  }
  setHome(homePos);
}


/**
   D0 [Lnn] [Rnn] [Unn] [Vnn] [Wnn] [Tnn]
   Jog each motor nn steps.
   I don't know why the latter motor names are UVWT.
*/
void jogMotors() {
  int i, j, amount;

  motor_engage();
  findStepDelay();

  for (i = 0; i < NUM_MOTORS; ++i) {
    if (MotorNames[i] == 0) continue;
    amount = parseNumber(MotorNames[i], 0);
    if (amount != 0) {
      Serial.print(F("Moving "));
      Serial.print(MotorNames[i]);
      Serial.print(F(" ("));
      Serial.print(i);
      Serial.print(F(") "));
      Serial.print(amount);
      Serial.print(F(" steps. Dir="));
      Serial.print(motors[i].dir_pin);
      Serial.print(F(" Step="));
      Serial.print(motors[i].step_pin);
      Serial.print('\n');

      int x = amount < 0 ? STEPPER_DIR_HIGH  : STEPPER_DIR_LOW;
      digitalWrite(motors[i].dir_pin, x);

      amount = abs(amount);
      for (j = 0; j < amount; ++j) {
        digitalWrite(motors[i].step_pin, HIGH);
        digitalWrite(motors[i].step_pin, LOW);
        pause(step_delay);
      }
    }
  }
}


/**
   D7 [Lnnn] [Rnnn]
   Set calibration length of each belt
*/
void setCalibration() {
  calibrateLeft = parseNumber('L', calibrateLeft);
  calibrateRight = parseNumber('R', calibrateRight);
  reportCalibration();
}


/**
   D8
   Report calibration values for left and right belts
*/
void reportCalibration() {
  Serial.print(F("D8 L"));
  Serial.print(calibrateLeft);
  Serial.print(F(" R"));
  Serial.println(calibrateRight);
}

#if MACHINE_STYLE == SIXI
/**
   D22
   reset home position to the current angle values.
*/
void sixiResetSensorOffsets() {
  int i;
  // cancel the current home offsets
  for (i = 0; i < NUM_SENSORS; ++i) {
    axies[i].homePos = 0;
  }
  // read the sensor
  sensorUpdate();
  // apply the new offsets
  float homePos[NUM_AXIES];
  for (i = 0; i < NUM_SENSORS; ++i) {
    homePos[i] = sensorAngles[i];
  }
  setHome(homePos);
}
#endif

/**
   Compare two floats to the first decimal place.
   return true when abs(a-b)<0.1
*/
boolean equalEpsilon(float a, float b) {
  int aa = floor(a * 10);
  int bb = floor(b * 10);
  //Serial.print("aa=");        Serial.print(aa);
  //Serial.print("\tbb=");      Serial.print(bb);
  //Serial.print("\taa==bb ");  Serial.println(aa==bb?"yes":"no");

  return aa == bb;
}


void setHome(float *pos) {
  boolean changed = false;

  int i;
  for (i = 0; i < NUM_AXIES; ++i) {
    if (!equalEpsilon(axies[i].homePos, pos[i])) changed = true;
  }
  if (changed == true) {
    for (i = 0; i < NUM_AXIES; ++i) {
      axies[i].homePos = pos[i];
    }
    saveHome();
  }
}


/**
   prepares the input buffer to receive a new message and tells the serial connected device it is ready for more.
*/
void parser_ready() {
  sofar = 0; // clear input buffer
  Serial.print(F("\n> "));  // signal ready to receive input
  last_cmd_time = millis();
}



/**
   runs once on machine start
*/
void setup() {
  // start communications
  Serial.begin(BAUD);

  loadConfig();

#ifdef HAS_WIFI
  // Start WIFI
  WiFi.mode(WIFI_AP);
  Serial.println( WiFi.softAP(SSID_NAME, SSID_PASS) ? "WIFI OK" : "WIFI FAILED" );
  Serial.println( port.begin(localPort) ? "UDP OK" : "UDP FAILED" );
  // Print the IP address
  Serial.print("Use this URL to connect: http://");
  Serial.print(WiFi.softAPIP());
  Serial.print(':');
  Serial.print(localPort);
  Serial.println('/');
#endif  // HAS_WIFI

#ifdef HAS_SD
  SD_setup();
#endif
#ifdef HAS_LCD
  LCD_setup();
#endif

  motor_setup();
  motor_engage();
  findStepDelay();

  //easyPWM_init();

  // initialize the plotter position.
  float pos[NUM_AXIES];
  for (int i = 0; i < NUM_AXIES; ++i) {
    pos[i] = 0;
  }
#ifdef MACHINE_HAS_LIFTABLE_PEN
  if (NUM_AXIES >= 3) pos[2] = PEN_UP_ANGLE;
#endif
  teleport(pos);
#ifdef MACHINE_HAS_LIFTABLE_PEN
  setPenAngle(PEN_UP_ANGLE);
#endif
  setFeedRate(DEFAULT_FEEDRATE);

  robot_setup();

#if MACHINE_STYLE == SIXI
  sensorUpdate();
  sensorUpdate();
  copySensorsToMotorPositions();
#endif

  // display the help at startup.
  help();

  parser_ready();
}


/**
   See: http://www.marginallyclever.com/2011/10/controlling-your-arduino-through-the-serial-monitor/
*/
void Serial_listen() {
  // listen for serial commands
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (sofar < MAX_BUF) serialBuffer[sofar++] = c;
    if (c == '\n') {
      serialBuffer[sofar - 1] = 0;

      // echo confirmation
      //Serial.println(serialBuffer);

      // do something with the command
      processCommand();
      parser_ready();
    }
  }

#ifdef HAS_WIFI
  int packetSize = port.parsePacket();
  if (packetSize) {
    int len = port.read(serialBuffer, MAX_BUF);
    sofar = len;
    if (len > 0) serialBuffer[len - 1] = 0;
    Serial.println(serialBuffer);
    processCommand();
    port.beginPacket(port.remoteIP(), port.remotePort());
    port.write("ok\r\n");
    port.endPacket();
  }
#endif  // HAS_WIFI
}


#if MACHINE_STYLE == SIXI
/**
   D17 report the 6 axis sensor values from the Sixi robot arm.
*/
void reportAllAngleValues() {
  Serial.print(F("D17"));
  for (int i = 0; i < 6; ++i) {
    Serial.print('\t');
    Serial.print(sensorAngles[i], 2);
  }
  /*
    if(current_segment==last_segment) {
    // report estimated position
    Serial.print(F("\t-\t"));

    working_seg = get_current_segment();
    for (uint8_t i = 0; i < NUM_SENSORS; ++i) {
      //float diff = working_seg->a[i].expectedPosition - sensorAngles[i];
      //Serial.print('\t');
      //Serial.print(abs(diff),3);
      Serial.print('\t');
      Serial.print(working_seg->a[i].expectedPosition,2);
    }
    }*/

  Serial.print('\t');
  //Serial.print(((positionErrorFlags&POSITION_ERROR_FLAG_CONTINUOUS)!=0)?'+':'-');
  Serial.print(((positionErrorFlags & POSITION_ERROR_FLAG_ERROR) != 0) ? '+' : '-');
  //Serial.print(((positionErrorFlags&POSITION_ERROR_FLAG_FIRSTERROR)!=0)?'+':'-');
  //Serial.print(((positionErrorFlags&POSITION_ERROR_FLAG_ESTOP)!=0)?'+':'-');
  Serial.println();
}


/**
   D18 copy sensor values to motor step positions.
*/
void copySensorsToMotorPositions() {
  wait_for_empty_segment_buffer();
  float a[NUM_AXIES];
  int i, j;
  int numSamples = 10;

  for (j = 0; j < NUM_AXIES; ++j) a[j] = 0;

  // assert(NUM_SENSORS <= NUM_AXIES);

  for (i = 0; i < numSamples; ++i) {
    sensorUpdate();
    for (j = 0; j < NUM_SENSORS; ++j) {
      a[j] += sensorAngles[j];
    }
  }
  for (j = 0; j < NUM_SENSORS; ++j) {
    a[j] /= (float)numSamples;
  }

  teleport(a);
}
#endif


/**
   main loop
*/
void loop() {
  Serial_listen();
#ifdef HAS_SD
  SD_check();
#endif
#ifdef HAS_LCD
  LCD_update();
#endif

  // The PC will wait forever for the ready signal.
  // if Arduino hasn't received a new instruction in a while, send ready() again
  // just in case USB garbled ready and each half is waiting on the other.
  if ( !segment_buffer_full() && (millis() - last_cmd_time) > TIMEOUT_OK ) {
#ifdef HAS_TMC2130
    {
      uint32_t drv_status = driver_0.DRV_STATUS();
      uint32_t stallValue = (drv_status & SG_RESULT_bm) >> SG_RESULT_bp;
      Serial.print(stallValue, DEC);
      Serial.print('\t');
    }
    {
      uint32_t drv_status = driver_1.DRV_STATUS();
      uint32_t stallValue = (drv_status & SG_RESULT_bm) >> SG_RESULT_bp;
      Serial.println(stallValue, DEC);
    }
#endif
    parser_ready();
  }

#if MACHINE_STYLE == SIXI
  sensorUpdate();

  if ((positionErrorFlags & POSITION_ERROR_FLAG_ERROR) != 0) {
    if ((positionErrorFlags & POSITION_ERROR_FLAG_FIRSTERROR) != 0) {
      Serial.println(F("\n\n** POSITION ERROR **\n"));
      positionErrorFlags &= 0xffff ^ POSITION_ERROR_FLAG_FIRSTERROR; // turn off
    }
  } else {
    if ((positionErrorFlags & POSITION_ERROR_FLAG_FIRSTERROR) == 0) {
      positionErrorFlags |= POSITION_ERROR_FLAG_FIRSTERROR; // turn on
    }
  }

  if ((positionErrorFlags & POSITION_ERROR_FLAG_CONTINUOUS) != 0) {
    if (millis() > reportDelay) {
      reportDelay = millis() + 100;
      reportAllAngleValues();
    }
  }
#endif

#ifdef DEBUG_STEPPING
  debug_stepping();
#endif  // DEBUG_STEPPING
}
