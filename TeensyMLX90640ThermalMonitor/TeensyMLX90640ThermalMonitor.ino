#include <TimeLib.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MLX90640.h>

// --- Pin & Display Definitions ---
const int LED_PIN = 13;             
const int SD_CS_PIN = BUILTIN_SDCARD; // Teensy 3.5 built-in SD slot

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

Adafruit_MLX90640 mlx;
float mlx90640Frame[768];

// --- UI View States ---
enum DisplayView {
  VIEW_THERMAL_IMAGE,
  VIEW_METADATA
};
DisplayView currentView = VIEW_THERMAL_IMAGE;
unsigned long lastViewSwitchTime = 0;
const unsigned long VIEW_SWITCH_INTERVAL = 4000; // Switch every 4 seconds

// --- Data Log Structure & RAM Buffer ---
struct LogEntry {
  time_t timestamp;
  float maxTemp;
  float minTemp;
  float avgTemp;
  float anomalyScore;
  bool isEvent;
};

const int BUFFER_SIZE = 20;             
LogEntry logBuffer[BUFFER_SIZE];
int bufferCount = 0;

unsigned long lastFlushTime = 0;
const unsigned long FLUSH_INTERVAL = 600000; // 10 minutes max batch interval

// --- Sampling Intervals & Burst Mode ---
const unsigned long BASELINE_INTERVAL = 250;   // ~4 Hz matching camera refresh rate for Python stream
const unsigned long BURST_INTERVAL = 250;      
const unsigned long BURST_DURATION = 30000;    // Minimum burst window

unsigned long currentSampleInterval = BASELINE_INTERVAL;
unsigned long burstStartTime = 0;
bool inBurstMode = false;

// ==========================================
// --- ADAPTIVE ML SYSTEM ENGINE ---
// ==========================================
enum MLState { 
  STATE_WARMUP,      // Sensor stabilization
  STATE_LEARNING,    // Initial baseline seed collection
  STATE_MONITORING   // Active adaptive EWMA monitoring & inference
};
MLState mlState = STATE_WARMUP;

unsigned long stateStartTime = 0;
const unsigned long WARMUP_DURATION = 5000;        // 5 seconds warm-up
const unsigned long LEARNING_DURATION = 60000UL;   // 1 minute initial baseline learning window

// Welford's streaming accumulators for thermal frame metrics
unsigned long sampleCount = 0;
float meanMaxT = 0, M2MaxT = 0;
float meanAvgT = 0, M2AvgT = 0;

// Adaptive EWMA Running State Parameters
const float EWMA_ALPHA = 0.02f;  
float adaptMeanMaxT = 0, adaptVarMaxT = 1.0;
float adaptMeanAvgT = 0, adaptVarAvgT = 1.0;

// Forward Declarations
void updateWelfordStats(float x, float &mean, float &M2);
void initializeAdaptiveModel();
void updateDisplay();
time_t getTeensy3Time();
void flushBufferToSD();

void setup() {
  Serial.begin(460800); // High baud rate for fast frame streaming to Python GUI
  delay(2000); // Allow native USB to initialise
  
  Serial.println(F("=================================================="));
  Serial.println(F(" Teensy 3.5 MLX90640 Thermal Suite & ML Monitor    "));
  Serial.println(F("=================================================="));

  pinMode(LED_PIN, OUTPUT);

  // --- OLED Initialization & Robust I2C Scanner ---
  Wire.begin();
  Wire.setClock(100000); // Stable initial clock for bus scanning
  
  Serial.print(F("[OLED] Scanning I2C bus for display... "));
  uint8_t oledAddr = 0;

  Wire.beginTransmission(0x3C);
  if (Wire.endTransmission() == 0) {
    oledAddr = 0x3C;
    Serial.println(F("Found at 0x3C!"));
  } else {
    Wire.beginTransmission(0x3D);
    if (Wire.endTransmission() == 0) {
      oledAddr = 0x3D;
      Serial.println(F("Found at 0x3D!"));
    }
  }

  if (oledAddr != 0) {
    display.begin(SSD1306_SWITCHCAPVCC, oledAddr);
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("Initializing System..."));
    display.display();
  }

  // --- Initialize Hardware RTC Sync ---
  setSyncProvider(getTeensy3Time);
  Serial.print(F("[RTC] Syncing time... "));
  if (timeStatus() != timeSet) {
    Serial.println(F("WARNING: RTC time not set! Check battery."));
  } else {
    Serial.println(F("Synced successfully!"));
  }

  // --- Initialize SD Card ---
  Serial.print(F("[SD CARD] Initializing built-in card... "));
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println(F("Failed! Check if card is inserted."));
  } else {
    Serial.println(F("Initialized successfully!"));
    if (!SD.exists("thermal_ml_log.csv")) {
      File dataFile = SD.open("thermal_ml_log.csv", FILE_WRITE);
      if (dataFile) {
        dataFile.println("DateTime,MaxTemp,MinTemp,AvgTemp,AnomalyScore,Event");
        dataFile.close();
        Serial.println(F("[SD CARD] Created new log file: thermal_ml_log.csv"));
      }
    }
  }

  // --- Initialize MLX90640 Thermal Sensor ---
  Serial.print(F("[MLX90640] Initializing... "));
  if (!mlx.begin()) {
    Serial.println(F("Sensor not found, check wiring! Check that SDA is pin 18, SCL is pin 19, and power is 3.3V."));
    while (1) { delay(1000); }
  } else {
    Serial.println(F("Initialized successfully!"));
    mlx.setMode(MLX90640_CHESS);
    mlx.setResolution(MLX90640_ADC_18BIT);
    mlx.setRefreshRate(MLX90640_4_HZ); 
  }

  lastFlushTime = millis();
  stateStartTime = millis();
  lastViewSwitchTime = millis();
  mlState = STATE_WARMUP;
  Serial.println(F("----------------------------------------\n"));
}

void loop() {
  unsigned long currentMillis = millis();

  // 1. State Machine Transitions
  if (mlState == STATE_WARMUP && (currentMillis - stateStartTime >= WARMUP_DURATION)) {
    mlState = STATE_LEARNING;
    stateStartTime = currentMillis;
    sampleCount = 0;
    Serial.println(F("[ML SYSTEM] Sensor ready. Starting initial baseline learning phase..."));
  }
  else if (mlState == STATE_LEARNING && (currentMillis - stateStartTime >= LEARNING_DURATION)) {
    initializeAdaptiveModel();
    mlState = STATE_MONITORING;
    Serial.println(F("[ML SYSTEM] Baseline locked. Continuous adaptive thermal monitoring active."));
  }

  // 2. Handle Burst Mode Expiration Timer
  if (inBurstMode && (currentMillis - burstStartTime >= BURST_DURATION)) {
    inBurstMode = false;
    flushBufferToSD();
  }

  // 3. View Switcher Timer
  if (currentMillis - lastViewSwitchTime >= VIEW_SWITCH_INTERVAL) {
    lastViewSwitchTime = currentMillis;
    currentView = (currentView == VIEW_THERMAL_IMAGE) ? VIEW_METADATA : VIEW_THERMAL_IMAGE;
  }

  // 4. Sensor Sampling & Processing Engine (Matched to 4Hz camera refresh)
  static unsigned long lastSampleTime = 0;
  if (currentMillis - lastSampleTime >= currentSampleInterval) {
    lastSampleTime = currentMillis;

    int status = mlx.getFrame(mlx90640Frame);
    if (status < 0) {
      return; // Skip bad frame reads
    }

    // Calculate frame statistics
    float maxTemp = -100.0f;
    float minTemp = 1000.0f;
    float sumTemp = 0.0f;

    for (int i = 0; i < 768; i++) {
      float t = mlx90640Frame[i];
      if (t > maxTemp) maxTemp = t;
      if (t < minTemp) minTemp = t;
      sumTemp += t;
    }
    float avgTemp = sumTemp / 768.0f;
    time_t currentTime = now();

    float anomalyScore = 0.0f;
    bool eventDetected = false;

    if (mlState == STATE_WARMUP) {
      // Still stream frames even during warmup so GUI stays responsive
    }
    else if (mlState == STATE_LEARNING) {
      sampleCount++;
      updateWelfordStats(maxTemp, meanMaxT, M2MaxT);
      updateWelfordStats(avgTemp, meanAvgT, M2AvgT);
    }
    else {
      // --- ACTIVE ADAPTIVE INFERENCE ---
      float stdMax = max(sqrt(adaptVarMaxT), 0.2f);
      float stdAvg = max(sqrt(adaptVarAvgT), 0.1f);

      float zMax = (maxTemp - adaptMeanMaxT) / stdMax;
      float zAvg = (avgTemp - adaptMeanAvgT) / stdAvg;

      anomalyScore = sqrt(sq(zMax) + sq(zAvg));
      bool rawAnomaly = (anomalyScore > 3.5f);

      // --- CONSECUTIVE OUTLIER DEBOUNCING ---
      static int consecutiveAnomalyCount = 0;
      if (rawAnomaly) {
        consecutiveAnomalyCount++;
      } else {
        consecutiveAnomalyCount = 0; 
      }
      eventDetected = (consecutiveAnomalyCount >= 2);

      // --- ADAPTATION UPDATE ---
      float activeAlpha = eventDetected ? 0.001f : EWMA_ALPHA; 

      adaptMeanMaxT = (activeAlpha * maxTemp) + ((1.0f - activeAlpha) * adaptMeanMaxT);
      adaptMeanAvgT = (activeAlpha * avgTemp) + ((1.0f - activeAlpha) * adaptMeanAvgT);
      adaptVarMaxT  = (activeAlpha * sq(maxTemp - adaptMeanMaxT)) + ((1.0f - activeAlpha) * adaptVarMaxT);
      adaptVarAvgT  = (activeAlpha * sq(avgTemp - adaptMeanAvgT)) + ((1.0f - activeAlpha) * adaptVarAvgT);
    }

    bool freshEvent = false;
    if (eventDetected && !inBurstMode) {
      inBurstMode = true;
      burstStartTime = currentMillis;
      freshEvent = true;
    }

    // Store in RAM buffer for SD logging
    logBuffer[bufferCount] = {currentTime, maxTemp, minTemp, avgTemp, anomalyScore, eventDetected};
    bufferCount++;

    // Stream full frame to Python GUI in the exact format it expects
    Serial.print(F("FRAME:"));
    for (int i = 0; i < 768; i++) {
      Serial.print(mlx90640Frame[i], 2);
      if (i < 767) {
        Serial.print(',');
      }
    }
    Serial.println();

    if (bufferCount >= BUFFER_SIZE || freshEvent || (currentMillis - lastFlushTime >= FLUSH_INTERVAL)) {
      flushBufferToSD();
    }
  }

  // Refresh OLED Display continuously
  updateDisplay();
}

void updateWelfordStats(float x, float &mean, float &M2) {
  float delta = x - mean;
  mean += delta / sampleCount;
  float delta2 = x - mean;
  M2 += delta * delta2;
}

void initializeAdaptiveModel() {
  if (sampleCount < 2) return;
  adaptMeanMaxT = meanMaxT; adaptVarMaxT = max(M2MaxT / (sampleCount - 1), 0.1f);
  adaptMeanAvgT = meanAvgT; adaptVarAvgT = max(M2AvgT / (sampleCount - 1), 0.05f);
}

void updateDisplay() {
  display.clearDisplay();

  if (mlState == STATE_WARMUP) {
    display.setCursor(0, 0);
    display.println(F("[SYSTEM WARMUP]"));
    display.setCursor(0, 20);
    display.println(F("Stabilizing sensor..."));
    display.display();
    return;
  }

  if (mlState == STATE_LEARNING) {
    display.setCursor(0, 0);
    display.println(F("[LEARNING BASELINE]"));
    unsigned long pct = ((millis() - stateStartTime) * 100) / LEARNING_DURATION;
    display.setCursor(0, 24);
    display.print(F("Progress: ")); display.print(pct); display.print(F("%"));
    display.display();
    return;
  }

  // Render based on active view toggle
  if (currentView == VIEW_THERMAL_IMAGE) {
    // --- DRAW DOWNSAMPLED THERMAL PREVIEW (16x12 grid) ---
    float fMin = 1000.0f, fMax = -1000.0f;
    for (int i = 0; i < 768; i++) {
      if (mlx90640Frame[i] < fMin) fMin = mlx90640Frame[i];
      if (mlx90640Frame[i] > fMax) fMax = mlx90640Frame[i];
    }
    float span = max(fMax - fMin, 1.0f);

    // Map 32x24 down to 16x12 blocks (each block averages 2x2 pixels)
    for (int y = 0; y < 12; y++) {
      for (int x = 0; x < 16; x++) {
        int idx1 = (y * 2) * 32 + (x * 2);
        int idx2 = (y * 2) * 32 + (x * 2 + 1);
        int idx3 = ((y * 2) + 1) * 32 + (x * 2);
        int idx4 = ((y * 2) + 1) * 32 + (x * 2 + 1);
        float blockVal = (mlx90640Frame[idx1] + mlx90640Frame[idx2] + mlx90640Frame[idx3] + mlx90640Frame[idx4]) / 4.0f;

        float norm = (blockVal - fMin) / span;
        if (norm > 0.5f) {
          display.fillRect(x * 4, y * 4, 3, 3, SSD1306_WHITE);
        }
      }
    }

    // Side Metadata Box
    display.setCursor(70, 0);
    display.println(F("THERMAL"));
    display.setCursor(70, 14);
    display.print(F("Max:")); display.print(fMax, 0); display.print("C");
    display.setCursor(70, 28);
    display.print(F("Min:")); display.print(fMin, 0); display.print("C");
    display.setCursor(70, 48);
    display.print(inBurstMode ? F("!BURST!") : F("Normal"));

  } else {
    // --- METADATA & ML VIEW ---
    display.setCursor(0, 0);
    display.print(F("--- ML METRICS ---"));
    
    display.setCursor(0, 16);
    display.print(F("Baseline Max: ")); display.print(adaptMeanMaxT, 1); display.print("C");

    display.setCursor(0, 30);
    if (bufferCount > 0) {
      display.print(F("Last Score: ")); display.print(logBuffer[bufferCount - 1].anomalyScore, 2);
    } else {
      display.print(F("Score: 0.00"));
    }

    display.setCursor(0, 48);
    display.print(F("SD Buffer: ")); display.print(bufferCount);
    display.print(inBurstMode ? F(" [BURST]") : F(""));
  }

  display.display();
}

void printDateTime(Print &out, time_t t) {
  out.print(year(t)); out.print(F("-"));
  if (month(t) < 10) out.print('0'); out.print(month(t)); out.print(F("-"));
  if (day(t) < 10) out.print('0'); out.print(day(t)); out.print(F(" "));
  if (hour(t) < 10) out.print('0'); out.print(hour(t)); out.print(F(":"));
  if (minute(t) < 10) out.print('0'); out.print(minute(t)); out.print(F(":"));
  if (second(t) < 10) out.print('0'); out.print(second(t));
}

void flushBufferToSD() {
  if (bufferCount == 0) return;

  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH); delay(30);
    digitalWrite(LED_PIN, LOW); delay(30);
  }

  File dataFile = SD.open("thermal_ml_log.csv", FILE_WRITE);
  if (dataFile) {
    for (int i = 0; i < bufferCount; i++) {
      printDateTime(dataFile, logBuffer[i].timestamp);
      dataFile.print(F(","));
      dataFile.print(logBuffer[i].maxTemp); dataFile.print(",");
      dataFile.print(logBuffer[i].minTemp); dataFile.print(",");
      dataFile.print(logBuffer[i].avgTemp); dataFile.print(",");
      dataFile.print(logBuffer[i].anomalyScore); dataFile.print(",");
      dataFile.println(logBuffer[i].isEvent ? "1" : "0");
    }
    dataFile.close();
    Serial.println(F("[SD CARD] Flushed thermal buffer block to disk."));
  } else {
    Serial.println(F("[SD CARD ERROR] Failed to open file for writing!"));
  }

  bufferCount = 0;
  lastFlushTime = millis();
}

time_t getTeensy3Time() {
  return Teensy3Clock.get();
}