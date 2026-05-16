/**
 * @file distillation_controller_final.cpp
 * @brief Production-ready control code for an Arduino-based distillation column.
 * @version 48.0 (Modern Web UI)
 */

// LIBRARIES
#include <SPI.h>
#include <WiFiNINA.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_BMP280.h>
#include <PubSubClient.h>
#if defined(__AVR__)
#include <avr/wdt.h>
#elif defined(ARDUINO_ARCH_SAMD)
#include <Adafruit_SleepyDog.h>
#endif
#include "secrets.h"
#include "index.h"

// --- HARDWARE PIN DEFINITIONS ---
#define ONE_WIRE_BUS 2
#define MOTOR_PWM_PIN 3
#define MOTOR_DIR_PIN 4
#define MULTI_BUTTON_PIN 7 // Single button (short/long press)
#define LED_GREEN_PIN 9    // Foreshots indicator
#define LED_RED_PIN 10     // Hearts indicator

// --- WIFI CONFIGURATION ---
const char WIFI_SSID[] = SECRET_SSID;
const char WIFI_PASS[] = SECRET_PASS;

// --- MQTT CONFIGURATION ---
const char* MQTT_SERVER = "mqtt3.thingspeak.com";
const int MQTT_PORT = 1883;
const char* MQTT_CLIENT_ID = SECRET_MQTT_CLIENT_ID;
const char* MQTT_USER = SECRET_MQTT_USER;
const char* MQTT_PASS = SECRET_MQTT_PASS;
const long THINGSPEAK_CHANNEL_ID = SECRET_THINGSPEAK_CHANNEL_ID;

// --- PROCESS CONTROL CONSTANTS ---
const int TEMPERATURE_PRECISION = 12;
const float MIN_OPERATING_TEMP_C = 75.0;
const float MAX_KEG_TEMP_C = 96.0;
const float TEMP_HYSTERESIS_C = 0.4;
const int COLLECTION_RATE_HYSTERESIS = 2;
const int MAX_COLLECTION_RATE = 40;
const int MIN_PWM_COLLECTION_RATE = 20;
const int PWM_MIN_OUTPUT = 165;
const int PWM_MAX_OUTPUT = 255;
const int INITIAL_RATE_FORESHOTS = 10;
const int INITIAL_RATE_HEARTS = 40;
const int INITIAL_RATE_TAILS = 10;
const float FORESHOTS_CUTOFF_VOLUME_ML = 200.0;
const float VOLUME_CALIBRATION_FACTOR = 1.35f;

// --- ETHANOL BOILING POINT CALIBRATION ---
// Linear interpolation of ethanol boiling point vs atmospheric pressure.
// Data from a catalog of ethanol BP at varied pressures (BP ~78.37C @ 1013.25 hPa).
const float ETHANOL_BP_LOW_PRESS_HPA  = 962.21f;
const float ETHANOL_BP_HIGH_PRESS_HPA = 1062.29f;
const float ETHANOL_BP_LOW_TEMP_C     = 77.0f;
const float ETHANOL_BP_HIGH_TEMP_C    = 79.5f;

// --- TIMING INTERVALS (in milliseconds) ---
const unsigned long SENSOR_READ_INTERVAL = 1000UL;
const unsigned long TEMP_CONVERSION_MS = 750UL;       // DS18B20 conversion time @ 12-bit
const unsigned long VOLUME_CALC_INTERVAL = 5000UL;
const unsigned long PWM_PULSE_INTERVAL = 60000UL;
const unsigned long STABILIZATION_CHECK_INTERVAL = 150000UL;
const unsigned long WIFI_CHECK_INTERVAL = 15000UL;
const unsigned long MQTT_POST_INTERVAL = 20000UL;     // 20s to fit ThingSpeak free-tier rate limit (15s)
const unsigned long MQTT_FAIL_BACKOFF = 30000UL;      // wait 30s after failed connect before retry
const unsigned long LONG_PRESS_TIME = 1000UL;         // 1 second threshold for long press

// --- GLOBAL OBJECTS ---
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensors(&oneWire);
Adafruit_BMP280 bmp;
Adafruit_Sensor *pressureSensor = bmp.getPressureSensor();
bool bmpOk = false;
WiFiServer server(80);
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);

// --- SENSOR ADDRESSES ---
// Replace with the unique 8-byte HEX addresses of YOUR DS18B20 probes.
// Use the OneWire/DallasTemperature "Tester" example sketch to discover them.
DeviceAddress kegSensorAddress    = {0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
DeviceAddress columnSensorAddress = {0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// --- STATE MACHINE DEFINITION ---
enum ProcessState { IDLE, COLLECTING_FORESHOTS, COLLECTING_HEARTS, COLLECTING_TAILS, FINISHED };
ProcessState currentState = IDLE;

// --- GLOBAL VARIABLES ---
float kegTemperature = 0.0f, columnTemperature = 0.0f, pressure = 0.0f;
float calculatedBoilingPoint = 0.0f, boilingPointAdjustment = 0.0f;
const char* processStateString = "Idle";
int collectionRate = 0;
int targetCollectionRateForeshots = INITIAL_RATE_FORESHOTS;
int targetCollectionRateHearts = INITIAL_RATE_HEARTS;
int targetCollectionRateTails = INITIAL_RATE_TAILS;
bool isSystemStable = false;
bool manualPause = false;
bool recoveryRequired = false;
bool foreshotsCompleted = false;  // sticky flag - after foreshots cutoff, persists until next mode pick
bool restartPending = false;      // deferred MCU reset (after HTTP response is sent)
float foreshotsVolume = 0.0f;
float heartsVolume    = 0.0f;
float tailsVolume     = 0.0f;

// Timing variables
unsigned long lastSensorReadTime = 0, lastVolumeCalcTime = 0, lastPwmPulseTime = 0;
unsigned long lastStabilizationTime = 0, lastWifiCheckTime = 0, lastMqttPublishTime = 0;
unsigned long lastTempRequestTime = 0;
unsigned long lastMqttFailedTime = 0;
unsigned long lastWifiBeginTime = 0;
const unsigned long WIFI_BEGIN_INTERVAL = 30000UL;  // min interval between WiFi.begin() calls on disconnect
bool tempConversionPending = false;
bool wifiConnected = false;
bool serverStarted = false;  // tracks whether server.begin() was called (needed for reconnect)

// Button & LED variables
unsigned long btnPressTime = 0;
bool isBtnPressed = false;
unsigned long lastLedTime = 0;
bool ledState = false;
int lastPrintState = -1; // prevents repeated console spam

// --- FUNCTION PROTOTYPES ---
void initializeSensors();
void initializeWiFi();
void testMotor();
void requestSensorConversion();
void completeSensorRead();
void logSensorsCompact();
void updateBoilingPoint();
void performCriticalSafetyCheck();
void handleHttpClient();
void buildHtmlResponse(char* out, size_t out_size);
void handleProcessLogic();
void checkAndAdjustStabilization();
void updateMotorPwm();
void updateCollectedVolume();
void checkWifiConnection();
void softwareReset();
bool connectToMqtt();
void publishMqttData();
double fmap(double val, double in_min, double in_max, double out_min, double out_max);
void handleButton();
void handleLedAndSerial();

// --- SETUP ---
void setup() {
    pinMode(MOTOR_PWM_PIN, OUTPUT);
    pinMode(MOTOR_DIR_PIN, OUTPUT);
    digitalWrite(MOTOR_DIR_PIN, LOW);
    analogWrite(MOTOR_PWM_PIN, 0);

    // Button + LED pins
    pinMode(MULTI_BUTTON_PIN, INPUT_PULLUP); // button shorts to GND when pressed
    pinMode(LED_GREEN_PIN, OUTPUT);
    pinMode(LED_RED_PIN, OUTPUT);

    Serial.begin(115200);
    delay(200);  // brief moment for USB CDC init, but does NOT block forever
    Serial.println("\n--- Distillation Control System Initializing (v48.0) ---");

    initializeSensors();
    initializeWiFi();

    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setSocketTimeout(2);   // 2s instead of default 15s - avoids main loop stalls
    mqttClient.setKeepAlive(60);      // 60s keepalive (default 15s; our publish interval is comfortably below)

    testMotor();

#if defined(ARDUINO_ARCH_SAMD)
    Watchdog.enable(4000);  // 4s watchdog - main loop never blocks longer than this
    Serial.println("SAMD watchdog enabled (4s).");
#endif

    Serial.println("\nInitialization Complete. System is running.");
    Serial.println("=============================================");
}

// --- MAIN LOOP ---
void loop() {
    unsigned long currentTime = millis();

#if defined(ARDUINO_ARCH_SAMD)
    Watchdog.reset();
#endif

    handleHttpClient();
    handleButton();
    handleLedAndSerial();

    // === ASYNC SENSOR READ ===
    // Phase 1: schedule a new conversion every SENSOR_READ_INTERVAL (if none in flight)
    if (!tempConversionPending && currentTime - lastSensorReadTime >= SENSOR_READ_INTERVAL) {
        lastSensorReadTime = currentTime;
        requestSensorConversion();
    }
    // Phase 2: if conversion has been running for >=750ms - read values and compute derived data
    completeSensorRead();

    handleProcessLogic();
    updateMotorPwm();

    if (currentTime - lastVolumeCalcTime >= VOLUME_CALC_INTERVAL) {
        lastVolumeCalcTime = currentTime;
        updateCollectedVolume();
    }

    if (currentTime - lastStabilizationTime >= STABILIZATION_CHECK_INTERVAL) {
        lastStabilizationTime = currentTime;
        if (currentState != IDLE && currentState != FINISHED) {
            checkAndAdjustStabilization();
        }
    }

    if (currentTime - lastWifiCheckTime >= WIFI_CHECK_INTERVAL) {
        lastWifiCheckTime = currentTime;
        checkWifiConnection();
    }

    // === MQTT ===
    // mqttClient.loop() MUST be called regularly for keepalive, regardless of publishing.
    if (wifiConnected) {
        mqttClient.loop();
    }

    if (wifiConnected && currentTime - lastMqttPublishTime >= MQTT_POST_INTERVAL) {
        lastMqttPublishTime = currentTime;
        if (!mqttClient.connected()) {
            // Backoff after failed connect - don't hammer broker, don't stall main loop
            if (currentTime - lastMqttFailedTime >= MQTT_FAIL_BACKOFF) {
                if (!connectToMqtt()) {
                    lastMqttFailedTime = currentTime;
                }
            }
        }
        if (mqttClient.connected()) {
            publishMqttData();
        }
    }
}

// --- CORE FUNCTIONS ---

// Physical button supports ONLY Foreshots (short press) and Hearts (long press).
// TAILS mode is available ONLY via the web UI.
void handleButton() {
    // INPUT_PULLUP: LOW means pressed
    bool currentBtnReading = (digitalRead(MULTI_BUTTON_PIN) == LOW);

    if (currentBtnReading && !isBtnPressed) {
        // Button just got pressed
        btnPressTime = millis();
        isBtnPressed = true;
    }
    else if (!currentBtnReading && isBtnPressed) {
        // Button released - measure duration
        isBtnPressed = false;
        unsigned long pressDuration = millis() - btnPressTime;

        if (pressDuration >= LONG_PRESS_TIME) {
            // LONG PRESS -> Hearts (ignore if already in Hearts)
            if (currentState != COLLECTING_HEARTS) {
                currentState = COLLECTING_HEARTS;
                lastStabilizationTime = 0;
                isSystemStable = false;
                manualPause = false;
                recoveryRequired = false;
                foreshotsCompleted = false;
            }
        }
        else if (pressDuration > 50) {
            // SHORT PRESS (debounced > 50ms) -> Foreshots (ignore if already in Foreshots)
            if (currentState != COLLECTING_FORESHOTS) {
                currentState = COLLECTING_FORESHOTS;
                lastStabilizationTime = 0;
                isSystemStable = false;
                manualPause = false;
                recoveryRequired = false;
                foreshotsCompleted = false;
            }
        }
    }
}

void handleLedAndSerial() {
    unsigned long currentMillis = millis();

    if (currentState == COLLECTING_FORESHOTS && !isSystemStable) {
        // Foreshots picked, waiting for stabilization - green blinks 500ms
        if (lastPrintState != 0) {
            Serial.println("[STATE] Foreshots - waiting for column stabilization");
            lastPrintState = 0;
        }
        if (currentMillis - lastLedTime >= 500) {
            lastLedTime = currentMillis;
            ledState = !ledState;
            digitalWrite(LED_GREEN_PIN, ledState);
            digitalWrite(LED_RED_PIN, LOW);
        }
    }
    else if (currentState == COLLECTING_HEARTS && !isSystemStable) {
        // Hearts picked, waiting for stabilization - red blinks 500ms
        if (lastPrintState != 1) {
            Serial.println("[STATE] Hearts - waiting for column stabilization");
            lastPrintState = 1;
        }
        if (currentMillis - lastLedTime >= 500) {
            lastLedTime = currentMillis;
            ledState = !ledState;
            digitalWrite(LED_RED_PIN, ledState);
            digitalWrite(LED_GREEN_PIN, LOW);
        }
    }
    else if (currentState == IDLE && isSystemStable && columnTemperature >= MIN_OPERATING_TEMP_C) {
        // Column stable, waiting for mode selection - both blink fast alternating
        if (lastPrintState != 2) {
            Serial.println("[STATE] Column stable - waiting for mode selection");
            lastPrintState = 2;
        }
        if (currentMillis - lastLedTime >= 100) {
            lastLedTime = currentMillis;
            ledState = !ledState;
            digitalWrite(LED_GREEN_PIN, ledState);
            digitalWrite(LED_RED_PIN, !ledState);
        }
    }
    else {
        lastPrintState = -1;

        if (currentState == COLLECTING_FORESHOTS && isSystemStable) {
            digitalWrite(LED_GREEN_PIN, HIGH);
            digitalWrite(LED_RED_PIN, LOW);
        }
        else if (currentState == COLLECTING_HEARTS && isSystemStable) {
            digitalWrite(LED_GREEN_PIN, LOW);
            digitalWrite(LED_RED_PIN, HIGH);
        }
        else {
            digitalWrite(LED_GREEN_PIN, LOW);
            digitalWrite(LED_RED_PIN, LOW);
        }
    }
}

void performCriticalSafetyCheck() {
    if (columnTemperature >= (calculatedBoilingPoint + TEMP_HYSTERESIS_C) || columnTemperature < MIN_OPERATING_TEMP_C) {
        if (isSystemStable) {
            Serial.println("!!! CRITICAL STOP - column temp out of range. Pump halted. !!!");
            isSystemStable = false;
            recoveryRequired = true;
        }
    }
}

void checkAndAdjustStabilization() {
    if (!isSystemStable) {
        // RECOVERY HYSTERESIS
        bool hasCooledSufficiently = (columnTemperature <= (calculatedBoilingPoint - (TEMP_HYSTERESIS_C / 2.0)));

        if (hasCooledSufficiently && columnTemperature >= MIN_OPERATING_TEMP_C) {
            if (recoveryRequired) {
                Serial.println("STABILIZATION: Recovery after fault. Soft restart.");
                switch(currentState) {
                    case COLLECTING_FORESHOTS: targetCollectionRateForeshots = max(0, targetCollectionRateForeshots - COLLECTION_RATE_HYSTERESIS); break;
                    case COLLECTING_HEARTS:    targetCollectionRateHearts    = max(0, targetCollectionRateHearts    - COLLECTION_RATE_HYSTERESIS); break;
                    case COLLECTING_TAILS:     targetCollectionRateTails     = max(0, targetCollectionRateTails     - COLLECTION_RATE_HYSTERESIS); break;
                    default: break;
                }
                recoveryRequired = false;
            }
            else {
                Serial.println("STABILIZATION: Temperature within range. Starting at full rate.");
            }
            isSystemStable = true;
        }
    }
}

void handleProcessLogic() {
    switch (currentState) {
        case IDLE:
            // "Foreshots Done" is sticky until operator picks a new mode
            processStateString = foreshotsCompleted ? "Foreshots Done" : "Idle";
            collectionRate = 0;
            break;
        case COLLECTING_FORESHOTS:
            processStateString = "Foreshots";
            collectionRate = isSystemStable ? targetCollectionRateForeshots : 0;
            if (foreshotsVolume >= FORESHOTS_CUTOFF_VOLUME_ML) {
                currentState = IDLE;
                foreshotsCompleted = true;  // IDLE case will display "Foreshots Done"
            }
            break;
        case COLLECTING_HEARTS: processStateString = "Hearts"; collectionRate = isSystemStable ? targetCollectionRateHearts : 0; break;
        case COLLECTING_TAILS:  processStateString = "Tails";  collectionRate = isSystemStable ? targetCollectionRateTails  : 0; break;
        case FINISHED:          processStateString = "Finished"; collectionRate = 0; break;
    }

    if (manualPause) {
        collectionRate = 0;
    }

    if (kegTemperature >= MAX_KEG_TEMP_C && currentState != FINISHED) {
        currentState = FINISHED;
        processStateString = "Finished";
        Serial.println("!!! KEG TEMPERATURE EXCEEDED MAX - process FINISHED. !!!");
        // Motor will be turned off by updateMotorPwm() (collectionRate = 0 in FINISHED).
        // Main loop stays alive - UI/HTTP remain responsive, restart available via web.
    }
}

void updateMotorPwm() {
    if (collectionRate <= 0) { digitalWrite(MOTOR_DIR_PIN, LOW); analogWrite(MOTOR_PWM_PIN, 0); return; }
    if (collectionRate >= MIN_PWM_COLLECTION_RATE) {
        int pwmValue = map(collectionRate, MIN_PWM_COLLECTION_RATE, MAX_COLLECTION_RATE, PWM_MIN_OUTPUT, PWM_MAX_OUTPUT);
        analogWrite(MOTOR_PWM_PIN, constrain(pwmValue, 0, 255));
        digitalWrite(MOTOR_DIR_PIN, HIGH);
    }
    else {
        // Sub-MIN_PWM rate: pump pulses at minimum PWM with duty cycle modulation
        analogWrite(MOTOR_PWM_PIN, PWM_MIN_OUTPUT);
        long onTime = map(collectionRate, 0, MIN_PWM_COLLECTION_RATE, 0, PWM_PULSE_INTERVAL);
        unsigned long currentTime = millis();
        if (currentTime - lastPwmPulseTime > PWM_PULSE_INTERVAL) { lastPwmPulseTime = currentTime; }
        if (currentTime - lastPwmPulseTime < onTime) { digitalWrite(MOTOR_DIR_PIN, HIGH); }
        else { digitalWrite(MOTOR_DIR_PIN, LOW); }
    }
}

void updateCollectedVolume() {
    if (digitalRead(MOTOR_DIR_PIN) == HIGH) {
        float actualPhysicalRate = 0;
        if (collectionRate > 0 && collectionRate < MIN_PWM_COLLECTION_RATE) {
            actualPhysicalRate = MIN_PWM_COLLECTION_RATE;
        }
        else {
            actualPhysicalRate = collectionRate;
        }
        float volumeToAdd = actualPhysicalRate * (float)(VOLUME_CALC_INTERVAL / 60000.0f);
        volumeToAdd *= VOLUME_CALIBRATION_FACTOR;
        switch (currentState) {
            case COLLECTING_FORESHOTS: foreshotsVolume += volumeToAdd; break;
            case COLLECTING_HEARTS:    heartsVolume    += volumeToAdd; break;
            case COLLECTING_TAILS:     tailsVolume     += volumeToAdd; break;
            default: break;
        }
    }
}

// --- MQTT FUNCTIONS ---

bool connectToMqtt() {
    Serial.print("[MQTT] connect... ");
    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
        Serial.println("OK");
        return true;
    } else {
        Serial.print("FAIL rc=");
        Serial.print(mqttClient.state());
        Serial.print(" - backoff ");
        Serial.print(MQTT_FAIL_BACKOFF / 1000);
        Serial.println("s");
        return false;
    }
}

void publishMqttData() {
    int stateNumeric = 0;
    switch (currentState) {
        case COLLECTING_FORESHOTS: stateNumeric = 1; break;
        case COLLECTING_HEARTS:    stateNumeric = 2; break;
        case COLLECTING_TAILS:     stateNumeric = 3; break;
        default: break;
    }

    char keg[10], col[10], pres[10];
    char foreV[10], heartV[10], tailV[10];
    dtostrf(kegTemperature,    0, 2, keg);
    dtostrf(columnTemperature, 0, 2, col);
    dtostrf(pressure,          0, 2, pres);
    dtostrf(foreshotsVolume,   0, 1, foreV);
    dtostrf(heartsVolume,      0, 1, heartV);
    dtostrf(tailsVolume,       0, 1, tailV);

    char payload[220];
    snprintf(payload, sizeof payload,
        "field1=%s&field2=%s&field3=%s&field4=%d&field5=%d"
        "&field6=%s&field7=%s&field8=%s&status=MQTTPUBLISH",
        keg, col, pres, collectionRate, stateNumeric,
        foreV, heartV, tailV);

    char topic[48];
    snprintf(topic, sizeof topic, "channels/%ld/publish", THINGSPEAK_CHANNEL_ID);

    if (mqttClient.publish(topic, payload)) {
        Serial.println("[MQTT] PUB OK");
    } else {
        Serial.println("[MQTT] PUB FAIL");
    }
}

// --- UTILITY AND INITIALIZATION FUNCTIONS ---

void handleHttpClient() {
    WiFiClient client = server.available();
    if (!client) return;

    char readString[101];
    int readPos = 0;
    readString[0] = '\0';
    bool currentLineIsBlank = true;
    unsigned long clientTimeout = millis() + 500;  // shorter timeout - less main-loop blocking on slow clients

    while (client.connected() && millis() < clientTimeout) {
        if (client.available()) {
            char c = client.read();

            // Char limit to avoid filling Arduino RAM with long URLs
            if (readPos < 100) {
                readString[readPos++] = c;
                readString[readPos] = '\0';
            }

            if (c == '\n' && currentLineIsBlank) {
                // End of HTTP header. Inspect what client sent.
                const char* qmark = strchr(readString, '?');
                bool isApiCall = (qmark != NULL && qmark != readString);

                if (!isApiCall) {
                    // Client requested main page - serve UI from index.h
                    client.println("HTTP/1.1 200 OK");
                    client.println("Content-type: text/html; charset=utf-8");
                    client.println("Connection: close");
                    client.println();
                    client.print(index_html);
                }
                else {
                    // AJAX request - data refresh or button click
                    int* currentTargetRate = nullptr;
                    if (currentState == COLLECTING_FORESHOTS) currentTargetRate = &targetCollectionRateForeshots;
                    if (currentState == COLLECTING_HEARTS)    currentTargetRate = &targetCollectionRateHearts;
                    if (currentState == COLLECTING_TAILS)     currentTargetRate = &targetCollectionRateTails;

                    if (strstr(readString, "?plus") != NULL) {
                        if (isSystemStable && currentTargetRate != nullptr && *currentTargetRate < MAX_COLLECTION_RATE) { *currentTargetRate += 2; }
                    }
                    else if (strstr(readString, "?minus") != NULL) {
                        if (isSystemStable && currentTargetRate != nullptr && *currentTargetRate > 0) { *currentTargetRate -= 2; }
                    }
                    // Mode switch: ignore if already in this mode (defense in depth - UI also blocks this)
                    else if (strstr(readString, "?foreshots") != NULL) {
                        if (currentState != COLLECTING_FORESHOTS) {
                            currentState = COLLECTING_FORESHOTS; lastStabilizationTime = 0;
                            isSystemStable = false; manualPause = false; recoveryRequired = false; foreshotsCompleted = false;
                        }
                    }
                    else if (strstr(readString, "?hearts") != NULL) {
                        if (currentState != COLLECTING_HEARTS) {
                            currentState = COLLECTING_HEARTS; lastStabilizationTime = 0;
                            isSystemStable = false; manualPause = false; recoveryRequired = false; foreshotsCompleted = false;
                        }
                    }
                    else if (strstr(readString, "?tails") != NULL) {
                        if (currentState != COLLECTING_TAILS) {
                            currentState = COLLECTING_TAILS; lastStabilizationTime = 0;
                            isSystemStable = false; manualPause = false; recoveryRequired = false; foreshotsCompleted = false;
                        }
                    }
                    else if (strstr(readString, "?stop")    != NULL) { manualPause = true; }
                    else if (strstr(readString, "?start")   != NULL) { manualPause = false; }
                    else if (strstr(readString, "?histp")   != NULL) { boilingPointAdjustment += 0.2f; }
                    else if (strstr(readString, "?histm")   != NULL) { boilingPointAdjustment -= 0.2f; }
                    // Restart: deferred until after HTTP response is sent (so UI can confirm)
                    else if (strstr(readString, "?restart") != NULL) { restartPending = true; }

                    // Plain-text response consumed by JavaScript poll
                    char respBuf[160];
                    buildHtmlResponse(respBuf, sizeof respBuf);

                    client.println("HTTP/1.1 200 OK");
                    client.println("Content-Type: text/plain; charset=utf-8");
                    client.println("Connection: close");
                    client.println();
                    client.print(respBuf);
                }
                break;
            }
            if (c == '\n') { currentLineIsBlank = true; } else if (c != '\r') { currentLineIsBlank = false; }
        }
    }
    client.flush();
    client.stop();

    // Deferred restart - after response delivered and socket closed
    if (restartPending) {
        delay(50);  // grace period for last bytes to flush
        softwareReset();
    }
}

void buildHtmlResponse(char* out, size_t out_size) {
    char keg[10], col[10], pres[10], boil[10], adj[10];
    char foreV[10], heartV[10], tailV[10];
    dtostrf(kegTemperature,         0, 2, keg);
    dtostrf(columnTemperature,      0, 2, col);
    dtostrf(pressure,               0, 2, pres);
    dtostrf(calculatedBoilingPoint, 0, 2, boil);
    dtostrf(boilingPointAdjustment, 0, 2, adj);
    dtostrf(foreshotsVolume,        0, 1, foreV);
    dtostrf(heartsVolume,           0, 1, heartV);
    dtostrf(tailsVolume,            0, 1, tailV);

    snprintf(out, out_size,
        ";%s;%s;%s;%s;%d;%s ml;%s ml;%s ml;%s;%s;%d;%d;",
        processStateString, keg, col, pres, collectionRate,
        foreV, heartV, tailV, boil, adj,
        isSystemStable ? 1 : 0, manualPause ? 1 : 0);
}

// Phase 1 of sensor cycle: schedule a DS18B20 conversion. Non-blocking after setWaitForConversion(false).
void requestSensorConversion() {
    tempSensors.requestTemperatures();
    lastTempRequestTime = millis();
    tempConversionPending = true;
}

// Phase 2: when conversion has elapsed (>=750ms), read values and recompute derived data.
// Called every main-loop iteration but only does work when ready.
void completeSensorRead() {
    if (!tempConversionPending) return;
    if (millis() - lastTempRequestTime < TEMP_CONVERSION_MS) return;

    kegTemperature    = tempSensors.getTempC(kegSensorAddress);
    columnTemperature = tempSensors.getTempC(columnSensorAddress);
    sensors_event_t pe;
    if (bmpOk) { pressureSensor->getEvent(&pe); pressure = pe.pressure; }
    tempConversionPending = false;

    updateBoilingPoint();
    performCriticalSafetyCheck();
    logSensorsCompact();
}

// One line per tick - easy to parse and doesn't spam the console.
// Format: [t=12345s] keg=85.20 col=78.30 P=970.20 BP=77.20 st=Hearts stab=Y pause=N rate=40
void logSensorsCompact() {
    Serial.print("[t=");
    Serial.print(millis() / 1000);
    Serial.print("s] keg=");   Serial.print(kegTemperature, 2);
    Serial.print(" col=");     Serial.print(columnTemperature, 2);
    Serial.print(" P=");       Serial.print(pressure, 2);
    Serial.print(" BP=");      Serial.print(calculatedBoilingPoint, 2);
    Serial.print(" st=");      Serial.print(processStateString);
    Serial.print(" stab=");    Serial.print(isSystemStable ? "Y" : "N");
    Serial.print(" pause=");   Serial.print(manualPause ? "Y" : "N");
    Serial.print(" rate=");    Serial.println(collectionRate);
}

void updateBoilingPoint() {
    calculatedBoilingPoint = fmap(pressure,
                                  ETHANOL_BP_LOW_PRESS_HPA, ETHANOL_BP_HIGH_PRESS_HPA,
                                  ETHANOL_BP_LOW_TEMP_C,    ETHANOL_BP_HIGH_TEMP_C)
                             + boilingPointAdjustment;
}

void initializeSensors() {
    Serial.println("Initializing sensors...");
    tempSensors.begin();
    tempSensors.setResolution(kegSensorAddress, TEMPERATURE_PRECISION);
    tempSensors.setResolution(columnSensorAddress, TEMPERATURE_PRECISION);
    tempSensors.setWaitForConversion(false);  // async mode - requestTemperatures() does not block
    Serial.print("Found "); Serial.print(tempSensors.getDeviceCount()); Serial.println(" temperature sensors.");
    bmpOk = bmp.begin(0x76);
    if (!bmpOk) { Serial.println("ERROR: BMP280 sensor not found!"); }
    else {
        bmp.setSampling(Adafruit_BMP280::MODE_NORMAL, Adafruit_BMP280::SAMPLING_X2,
                        Adafruit_BMP280::SAMPLING_X16, Adafruit_BMP280::FILTER_X16,
                        Adafruit_BMP280::STANDBY_MS_500);
        Serial.println("BMP280 sensor initialized.");
    }
}

void initializeWiFi() {
    Serial.print("Connecting to WiFi: "); Serial.println(WIFI_SSID);

    // No WiFi module - DO NOT block the system, continue in offline mode.
    // checkWifiConnection() will retry periodically; if module returns, we'll pick it up.
    if (WiFi.status() == WL_NO_MODULE) {
        Serial.println("ERROR: no WiFi module. Running offline (manual button only).");
        wifiConnected = false;
        return;
    }

    int attempts = 0;
    const int MAX_ATTEMPTS = 4;  // 4 x 5s = 20s instead of 50s
    while (WiFi.status() != WL_CONNECTED && attempts < MAX_ATTEMPTS) {
        attempts++; Serial.print("Attempt "); Serial.println(attempts);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        delay(5000);
    }
    if (WiFi.status() == WL_CONNECTED) {
        server.begin();
        serverStarted = true;
        wifiConnected = true;
        Serial.println("\nConnected to WiFi!");
        Serial.print("IP address: "); Serial.println(WiFi.localIP());
    } else {
        Serial.println("\nFailed to connect to WiFi. Running offline (manual button only).");
        Serial.println("Background reconnect attempted every 15s.");
        wifiConnected = false;
    }
}

void checkWifiConnection() {
    unsigned long now = millis();
    if (WiFi.status() != WL_CONNECTED) {
        if (wifiConnected) {
            Serial.println("[WiFi] Lost connection. Attempting reconnect...");
            wifiConnected = false;
        }
        // Throttle: don't call WiFi.begin() more often than every 30s, to not interrupt an in-flight attempt.
        if (now - lastWifiBeginTime >= WIFI_BEGIN_INTERVAL) {
            WiFi.begin(WIFI_SSID, WIFI_PASS);
            lastWifiBeginTime = now;
        }
    } else {
        if (!wifiConnected) {
            Serial.print("[WiFi] Connected. IP: ");
            Serial.println(WiFi.localIP());
            // After disconnect the listening socket is dead - re-call server.begin().
            server.begin();
            serverStarted = true;
            wifiConnected = true;
        }
    }
}

void testMotor() {
    Serial.println("Motor test (2 seconds)...");
    analogWrite(MOTOR_PWM_PIN, 255);
    digitalWrite(MOTOR_DIR_PIN, HIGH);
    delay(2000);
    digitalWrite(MOTOR_DIR_PIN, LOW);
    analogWrite(MOTOR_PWM_PIN, 0);
    Serial.println("Motor test complete.");
}

double fmap(double val, double in_min, double in_max, double out_min, double out_max) {
    return (val - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

void softwareReset() {
    Serial.println("--- System Restart ---");
    Serial.flush();
    delay(100);
#if defined(ARDUINO_ARCH_SAMD)
    // MKR WiFi 1010 / Nano 33 IoT - full CPU + peripheral reset (Cortex-M0+)
    NVIC_SystemReset();
#elif defined(__AVR__)
    // Uno WiFi Rev2 / Nano with NINA - reset via watchdog timer
    wdt_enable(WDTO_15MS);
    while (1) { ; }
#else
    // Fallback: jump to address 0 (does NOT reset peripherals - last resort)
    void (*resetFunc)(void) = 0;
    resetFunc();
#endif
}
