//------ Debug ------//

#define DEBUG

#ifdef DEBUG
#define DEBUG_PRINTLN(x) debugPrintln(x) // Enable printing
#define DEBUG_PRINT(x) debugPrint(x) // Enable printing
#define SERIAL_COMMS_STEPS 5
#else
#define DEBUG_PRINT(x) // Disable printing (do nothing)
#define DEBUG_PRINTLN(x) // Disable printing (do nothing)
#define SERIAL_COMMS_STEPS 3
#endif

//------ Libraries ------//
#include "Arduino.h"
#include "EEPROM.h"
#include "string.h"
// DY player over UART
#include <SoftwareSerial.h>
#define HAS_SOFTWARE_SERIAL
#include "DYPlayerArduino.h"
//PN532 over SPI
#include <SPI.h>
#include <PN532_SPI.h>
#include <PN532.h>
#include <NfcAdapter.h>

//------ Pins -------//
// PN532
static const int pinPN532SPI = 10;  // SPI from pins D10-D13
// DFplayer
static const int pinPlayerTx = 7;  // Tx on arduino (D7) -- to Rx on player (pin 4)
static const int pinPlayerRx = 8;  // Rx on arduino (D8) -- to Tx on player (pin 3)
// Buttons
static const int pinPlayButton = 3; // Play button (D3)
static const int pinPauseButton = 4; // Pause button (D4)
static const int pinNextButton = 5; // Next button (D5)
static const int pinPreviousButton = 6; // Previous button (D6)
static const byte pinVolumePot = A0; // Volume level (A0)
static const int pinSleepButton = 2; // Sleep button (D2)

//------  DFPlayer --------//
SoftwareSerial playerSerial(pinPlayerRx, pinPlayerTx);
DY::Player player(&playerSerial);  // Create the Player object

//------ PN532 -------//
PN532_SPI interface(SPI, pinPN532SPI);  // create a PN532 SPI interface with the SPI CS terminal located at digital pin 10
NfcAdapter nfc = NfcAdapter(interface);   // create an NFC adapter object

//------- Buttons -------//
const byte numButtons = 5;
static int buttons[numButtons] = {pinPlayButton, pinPauseButton, pinNextButton, pinPreviousButton, pinSleepButton};
bool buttonStates[numButtons];

//------ Control variables ------//

// General control
unsigned long now = millis();

// Buttons
bool playPressed;
bool pausePressed;
bool forwardPressed;
bool backPressed;
bool sleepPressed;
int buttonOutcome = 0;
unsigned long forwardPressStart = 0;
bool forwardWasPressed = false;
unsigned long backPressStart = 0;
bool backWasPressed = false;
const unsigned long longPressThreshold = 400; // ms

// State machine
int state = 1; // stopped

// Book
const int maxChapters = 10; // Adjust depending on max expected chapters
int bookNumber = -1;
int bookChapters;
int bookLastTrack;
int bookChapterTracks[maxChapters];

// Serial comms
unsigned long lastSerialComms = 0;
int serialStep = 0;
bool playerPlay = 0;
bool playerStop = 0;
bool playerPause = 0;
bool playerResume = 0;
bool playerVolume = 0;
bool nfcRead = 0;

// Save track
unsigned long lastSaveTrack = 0;
static unsigned long saveTrackFrequency = 120000;

// Player info
int8_t playerStatus = -1;
int playerTrack = 0;

// Track control
int desiredTrack = -1;

// Volume
int setVolume; //variable for holding previously set volume level
byte desiredVolume = 0;  //variable for holding volume level

// Sleep
bool sleepNow = 0;
unsigned long sleepTimerSet = -1;
static unsigned long sleepTimerDuration = 10000;

//------- Decoding ------//
const char base62_chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

//*****************************************************************************************//

void setup() {

  //------ START SERIAL -------//
  Serial.begin(115200);
  delay(500);

  //------ START PLAYER -------//
  // Start player and check player has started
  player.begin();
  delay(500);
  // Set player volume
  desiredVolume = map(analogRead(pinVolumePot), 0, 1023, 1, 25);  //scale the pot value and volume level
  player.setVolume(desiredVolume);
  setVolume = desiredVolume;
  delay(500);
  player.setCycleMode(DY::PlayMode::SequenceDir);
  DEBUG_PRINTLN("Started player");
  delay(500);

  //------ START NFC --------//
  nfc.begin();
  delay(500);
  DEBUG_PRINTLN("Started nfc");

  //------ SET OTHER THINGS ------//
  pinMode(pinPlayButton, INPUT_PULLUP);
  pinMode(pinPauseButton, INPUT_PULLUP);
  pinMode(pinNextButton, INPUT_PULLUP);
  pinMode(pinPreviousButton, INPUT_PULLUP);
  pinMode(pinSleepButton, INPUT_PULLUP);
  pinMode(pinPN532SPI, OUTPUT);
  pinMode(A1, OUTPUT);
  digitalWrite(A1, HIGH);
}

//*****************************************************************************************//

void loop() {
  now = millis();
  checkButtons();
  handleButtons();
  resetFlags();
  stateMachine();
  saveTrack();
  sleepManager();
  volumeManager();
  serialComms();
}

//*****************************************************************************************//

void checkButtons(void) {
  bool tmp[numButtons];
  uint8_t i;
  for (i = 0; i < numButtons; i++) {
    tmp[i] = digitalRead(buttons[i]);
  }  // end for 1st loop

  delay(10);  //wait for debouncing the keys

  for (i = 0; i < numButtons; i++) {
    if (digitalRead(buttons[i]) == tmp[i])  //assume key is stable if still in the same position
      buttonStates[i] = 1 - tmp[i];
  }  // end for 2nd loop
  playPressed = buttonStates[0];
  pausePressed = buttonStates[1];
  forwardPressed = buttonStates[2];
  backPressed = buttonStates[3];
  sleepPressed = buttonStates[4];
  desiredVolume = map(analogRead(pinVolumePot), 0, 1023, 1, 25);  //scale the pot value and volume level
}

void handleButtons() {
  // If play is not pressed, button outcome is STOP (0)
  // Else if pause is pressed, button outcome is PAUSE (2)
  // Else if nothing else is pressed, button outcome is PLAY (1)
  // But checking if the forward or back is pressed, and on release,
  // depending on the outcome, button outcome is next track, chapter,
  // or previous track, chapter.
  
  // STOP
  if (!playPressed) {
    buttonOutcome = 0;
    forwardWasPressed = false;
    backWasPressed = false;
    return;
  }

  // PAUSE
  if (pausePressed) {
    buttonOutcome = 2;
    forwardWasPressed = false;
    backWasPressed = false;
    return;
  }

  // FORWARD BUTTON LOGIC
  if (forwardPressed && !forwardWasPressed) {
    // Button just pressed
    forwardPressStart = now;
    forwardWasPressed = true;
  } else if (!forwardPressed && forwardWasPressed) {
    // Button just released
    unsigned long duration = now - forwardPressStart;
    if (duration >= longPressThreshold) {
      buttonOutcome = 3; // next track
    } else {
      buttonOutcome = 4; // next chapter
    }
    forwardWasPressed = false;
    return;
  }

  // BACK BUTTON LOGIC
  if (backPressed && !backWasPressed) {
    backPressStart = now;
    backWasPressed = true;
  } else if (!backPressed && backWasPressed) {
    unsigned long duration = now - backPressStart;
    if (duration >= longPressThreshold) {
      buttonOutcome = 5; // previous track
    } else {
      buttonOutcome = 6; // previous chapter
    }
    backWasPressed = false;
    return;
  }

  // DEFAULT: playing
  buttonOutcome = 1;

  if (desiredVolume != setVolume) {
    playerVolume = 1;
  } else {
    playerVolume = 0;
  }
}

void stateMachine(void){
  if (buttonOutcome == 0) {
    if (state != 1 && state != 0) {
      state = 0; // stop
      DEBUG_PRINTLN("stop");
      saveTrackNow();
    }
  }
  switch (state)
  {
  case 0: // stop
    if (playerStatus == 0) {
      clearVariables();
      state = 1; //state > stopped
      DEBUG_PRINTLN("stop > stopped");
    } else {
      playerStop = 1;
    }
    break;
  case 1: // stopped
    if (playerStatus != 0) {
      state = 0; //state > stop
      DEBUG_PRINTLN("stopped > stop");
    } else if (buttonOutcome == 1) {
      state = 2; // state > readNFC
      DEBUG_PRINTLN("stopped > readNFC");
    }
    break;
  case 2: // readNFC
    if (desiredTrack == -1) {
      nfcRead = 1;
    } else {
      state = 3; // state > play
      DEBUG_PRINTLN("readNFC > play");
    }
    break;
  case 3: // play
    if (playerStatus == 1 && playerTrack==desiredTrack) {
      state = 4; // state > playing
      DEBUG_PRINTLN("play > playing");
    } else {
      playerPlay = 1;
    }
    break;
  case 4: // playing
    if (sleepNow == 1) {
      state = 8; // state > sleep
      DEBUG_PRINTLN("playing > sleep");
      saveTrackNow();
    } else if (buttonOutcome == 2) {
      state = 5; // state > pause
      DEBUG_PRINTLN("playing > pause");
      saveTrackNow();
    } else if (buttonOutcome == 3) {
      setNextTrack();
      state = 3;
      DEBUG_PRINTLN("next track");
    } else if (buttonOutcome == 4) {
      setNextChapter();
      state = 3;
      DEBUG_PRINTLN("next chapter");
    } else if (buttonOutcome == 5) {
      setPreviousTrack();
      state = 3;
      DEBUG_PRINTLN("previous track");
    } else if (buttonOutcome == 6) {
      setPreviousChapter();
      state = 3;
      DEBUG_PRINTLN("previous chapter");
    } else if (playerTrack > bookLastTrack) {
      state = 8;
      DEBUG_PRINTLN("Last chapter > sleep");
      setFirstTrack();
    }
    break;
  case 5: // pause
    if (playerStatus==1) {
      playerPause=1;
    } else if (playerStatus==2) {
      state = 6; // state > paused
      DEBUG_PRINTLN("pause > paused");
    }
    break;
  case 6: // paused
    if (playerStatus == 1) {
      state = 5; // state > pause
      DEBUG_PRINTLN("pause > paused");
    } else if (buttonOutcome == 1) {
      state = 7; // state > resume
      DEBUG_PRINTLN("paused > resume");
    }
    break;
  case 7: // resume
    if (playerStatus == 2) {
      playerResume = 1;
    }
    if (playerStatus == 1) {
      state = 4; // state > playing
      DEBUG_PRINTLN("resume > playing");
    }
    break;
  case 8: // sleep
    if (playerStatus == 0) {
      state = 9; // state > sleeping
      DEBUG_PRINTLN("sleep > sleeping");
    } else {
      playerStop = 1;
    }
    break;
  case 9: // sleeping
    if (playerStatus != 0) {
      state = 8; // state > sleep
      DEBUG_PRINTLN("sleeping > sleep");
    }
    break;
  default:
    break;
  }
}

void serialComms() {

  if (now - lastSerialComms < 300) return;

  switch (serialStep) {
    case 0:
      clearBuffer();
      playerStatus = static_cast<int>(player.checkPlayState());
      DEBUG_PRINTLN("Status: "+String(playerStatus));
      break;
      
    case 1:
      clearBuffer();
      playerTrack = player.getPlayingSound();
      DEBUG_PRINTLN("Track: "+String(playerTrack));
      break;

    case 2:
      if (playerPlay) {
        player.playSpecifiedDevicePath(DY::Device::Sd, formatFilename(bookNumber, desiredTrack));
        DEBUG_PRINT("playing track");
        DEBUG_PRINTLN(formatFilename(bookNumber, desiredTrack));
        playerPlay = 0;
      } else if (playerStop) {
        player.stop();
        playerStop = 0;
      } else if (playerPause) {
        player.pause();
        playerPause = 0;
      } else if (playerResume) {
        player.play();
        playerResume = 0;
      } else if (playerVolume) {
        player.setVolume(desiredVolume);
        setVolume = desiredVolume;
        playerVolume = 0;
      } else if (nfcRead) {
        readNfc();
        nfcRead = 0;
      }
      break;
  }
  serialStep = (serialStep + 1) % 3; // loop through 0 → 1 → 2 → 0 ...
  lastSerialComms = now;
}

void resetFlags() {
  playerPlay = 0;
  playerStop = 0;
  playerPause = 0;
  playerResume = 0;
  nfcRead = 0;
}

void clearVariables() {
  playerTrack = -1;
  desiredTrack = -1;
  bookNumber = -1;
  bookChapters = -1;
  bookLastTrack = -1;
}

void sleepManager() {
  if (sleepPressed && state == 4) {
    if (sleepTimerSet == -1) {
      sleepTimerSet = now;
    } else if (now - sleepTimerSet > sleepTimerDuration) {
      sleepNow = 1;
      sleepTimerSet = -1;
    }
  } else {
    sleepTimerSet = -1;
    sleepNow = 0;
  }
}

void saveTrack() {
  if (state == 4) {
    if (now - lastSaveTrack > saveTrackFrequency) {
      saveTrackNow();
    }
  }
}

void saveTrackNow() {
  if (playerTrack != -1) {
    EEPROM.update(bookNumber,playerTrack);
    lastSaveTrack = now;
  }
}

void setFirstTrack() {
  EEPROM.update(bookNumber,bookChapterTracks[0]);
  lastSaveTrack = now;
}

void readNfc(void) {
  clearBuffer();
  interface.wakeup();
  if (nfc.tagPresent(50)) {
    delay(100);
    clearBuffer();
    NfcTag tag = nfc.read();
    NdefMessage message = tag.getNdefMessage();
    // Get the first record
    NdefRecord record = message.getRecord(0);
    // Create byte array big enough for payload
    byte payload[record.getPayloadLength()];
    // Get payload to byte array
    record.getPayload(payload);
    // Convert byte Array to string
    String payloadString = String((char *)payload);
    // Remove first 3 and last 2 characters of string
    payloadString.remove(payloadString.length() - 2);
    payloadString.remove(0, 3);
    // Interpret string and populate global variables
    interpretNfcPayload(payloadString, bookNumber, bookChapterTracks, bookChapters, bookLastTrack);
    // Get current track from EPROM
    if (EEPROM.read(bookNumber) <= bookLastTrack && EEPROM.read(bookNumber) >= bookChapterTracks[0]) {
      desiredTrack = EEPROM.read(bookNumber);
    } else {
      desiredTrack = bookChapterTracks[0];
    }
  } else {
    DEBUG_PRINTLN("waiting for tag");
  }
}

void clearBuffer(void) {
  delay(50);
  while (Serial.available()) Serial.read(); // Clear buffer
  delay(50);
  while (playerSerial.available()) playerSerial.read();
}

// Convert base62 character to numeric value
int base62CharToValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  if (c >= 'a' && c <= 'z') return c - 'a' + 36;
  return -1; // Invalid
}

// Decode 2-character base62 string into int (0–3843)
int decodeBase62Pair(String pair) {
  if (pair.length() != 2) return -1;
  int high = base62CharToValue(pair.charAt(0));
  int low  = base62CharToValue(pair.charAt(1));
  if (high == -1 || low == -1) return -1;
  return high * 62 + low;
}

// Get the filename char from the book number and desired track.
char* formatFilename(int bookNumber, int desiredTrack) {
  static char path[25];  // Enough for "/001/0004.mp3" + null terminator
  snprintf(path, sizeof(path), "/%03d/%04d.mp3", bookNumber, desiredTrack);
  return path;
}

// Interpret the NFC payload string
void interpretNfcPayload(String input, int& bookNumber, int* bookChapterTracks, int& bookChapters, int& bookLastTrack) {
  int totalPairs = input.length() / 2;

  if (totalPairs < 2) {
    // Not enough data to have book number and last track
    bookNumber = -1;
    bookLastTrack = -1;
    bookChapters = 0;
    return;
  }

  bookNumber = decodeBase62Pair(input.substring(0, 2));
  bookLastTrack = decodeBase62Pair(input.substring(input.length() - 2));

  bookChapters = totalPairs - 2; // Exclude book and last track

  for (int i = 0; i < bookChapters && i < maxChapters; ++i) {
    String pair = input.substring((i + 1) * 2, (i + 1) * 2 + 2);
    bookChapterTracks[i] = decodeBase62Pair(pair);
  }

  DEBUG_PRINT("Book Number: ");
  DEBUG_PRINTLN(String(bookNumber));
  DEBUG_PRINTLN("Chapters:");
  for (int i = 0; i < bookChapters; ++i) {
    DEBUG_PRINT("  Chapter ");
    DEBUG_PRINT(String(i + 1));
    DEBUG_PRINT(": ");
    DEBUG_PRINTLN(String(bookChapterTracks[i]));
  }
  DEBUG_PRINT("Final Track Number: ");
  DEBUG_PRINTLN(String(bookLastTrack));
}

void setNextTrack() {
    desiredTrack = (playerTrack >= bookLastTrack) ? 1 : playerTrack + 1;
}

void setPreviousTrack() {
    desiredTrack = (playerTrack <= 1) ? bookLastTrack : playerTrack - 1;
}

void setNextChapter() {
    for (int i = 0; i < bookChapters; i++) {
        if (bookChapterTracks[i] > playerTrack) {
            desiredTrack = bookChapterTracks[i];
            return;
        }
    }
    // If no next chapter found, wrap around to the first
    desiredTrack = bookChapterTracks[0];
}

void setPreviousChapter() {
    // If playerTrack is before the first chapter (shouldn't happen, but just in case), wrap
    if (playerTrack < bookChapterTracks[0]) {
        desiredTrack = bookChapterTracks[bookChapters - 1];
        return;
    }

    for (int i = 0; i < bookChapters; i++) {
        if (bookChapterTracks[i] == playerTrack) {
            // We're at the start of a chapter — go to the previous one (or wrap)
            desiredTrack = (i == 0) ? bookChapterTracks[bookChapters - 1] : bookChapterTracks[i - 1];
            return;
        } else if (bookChapterTracks[i] > playerTrack) {
            // We're inside chapter i-1 — go to its start
            desiredTrack = bookChapterTracks[i - 1];
            return;
        }
    }

    // If we get here, we're beyond the last chapter start, so go to the last chapter
    desiredTrack = bookChapterTracks[bookChapters - 1];
}

void volumeManager() {
  if (desiredVolume - setVolume > 1) {
    playerVolume = 1;
  } else if (setVolume - desiredVolume > 1) {
    playerVolume = 1;
  }
}

void debugPrint(String input) {
  Serial.print(input);
  lastSerialComms = now;
}

void debugPrintln(String input) {
  Serial.println(input);
  lastSerialComms = now;
}