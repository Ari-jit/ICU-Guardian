#include <Wire.h>              // For I2C SLAVE (main communication)
#include <OneWire.h>
#include <DallasTemperature.h>
#include "MAX30105.h"

// I2C Address for this module (SLAVE)
#define MODULE_ADDRESS 0x42

// Sensor Pin Definitions
#define DS18B20_PIN 4         // OneWire data pin for DS18B20
#define OXYGEN_VALVE_PIN 5    // Oxygen valve control

// Define second I2C bus for sensors (Wire1)
TwoWire I2CSensor = TwoWire(1);  // I2C port 1

// MAX30102 Sensor (DEFAULT constructor)
MAX30105 particleSensor;

// DS18B20 Temperature Sensor
OneWire oneWire(DS18B20_PIN);
DallasTemperature tempSensor(&oneWire);

// Global variables
float spo2 = 96.0;
float pulse_rate = 75.0;
float temperature = 37.0;
bool oxygen_valve_state = false;
bool oxygen_available = true;

// MAX30102 Data
const byte RATE_SIZE = 4;
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute;
int beatAvg;

// Beat detection variables
long beatThreshold = 100000;
long beatMin = 30000;

// I2C Communication Commands
enum I2CCommands {
  CMD_READ_SENSORS = 0x01,
  CMD_OXYGEN_CONTROL = 0x03
};

void setup() {
  Serial.begin(115200);
  Serial.println("Oximeter & Temperature Module Initializing...");
  
  // Initialize pins
  pinMode(OXYGEN_VALVE_PIN, OUTPUT);
  digitalWrite(OXYGEN_VALVE_PIN, LOW);
  
  // Initialize I2C SLAVE FIRST (Wire0: default pins GPIO21/22)
  Wire.begin(MODULE_ADDRESS);
  Wire.onRequest(requestEvent);
  Wire.onReceive(receiveEvent);
  
  // Initialize SENSOR I2C SECOND (Wire1: explicitly GPIO21/22 at 400kHz)
  I2CSensor.begin(21, 22, 400000);  // SDA=21, SCL=22, 400kHz
  
  // Initialize MAX30102 (uses Wire1 via begin() call)
  if (!initializeMAX30102()) {
    Serial.println("MAX30102 not found on Wire1. Check wiring/power.");
    while (1);
  }
  
  // Initialize DS18B20
  initializeDS18B20();
  
  Serial.println("Oximeter & Temperature Module Ready");
  Serial.print("I2C Slave Address: 0x");
  Serial.println(MODULE_ADDRESS, HEX);
  Serial.println("Sensors: MAX30102 (Wire1) + DS18B20");
}

bool initializeMAX30102() {
  // Initialize MAX30102 on Wire1 using begin() method
  if (!particleSensor.begin(I2CSensor, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 begin() failed");
    return false;
  }
  
  // Conservative settings for stability
  byte ledBrightness = 0x1F;  // 7.6mA (reduced from 0x7F)
  byte sampleAverage = 4;
  byte ledMode = 2;           // Red + IR
  int sampleRate = 400;
  int pulseWidth = 411;
  int adcRange = 4096;
  
  particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);
  
  Serial.println("MAX30102 initialized on Wire1 (GPIO21/22)");
  return true;
}

void initializeDS18B20() {
  tempSensor.begin();
  int deviceCount = tempSensor.getDeviceCount();
  Serial.print("DS18B20 initialized. Found ");
  Serial.print(deviceCount);
  Serial.println(" devices.");
  
  if (deviceCount == 0) {
    Serial.println("No DS18B20 sensors found!");
  }
}

void loop() {
  // Read MAX30102 sensor data
  readMAX30102();
  
  // Read DS18B20 temperature every 2 seconds
  static unsigned long lastTempRead = 0;
  if (millis() - lastTempRead >= 2000) {
    readDS18B20();
    lastTempRead = millis();
    printSensorData();
  }
  
  delay(10);
}

bool checkForBeatCustom(long irValue) {
  static long lastIRValue = 0;
  static unsigned long lastBeatTime = 0;
  static bool lastBeatState = false;
  
  long slope = irValue - lastIRValue;
  lastIRValue = irValue;
  
  if (slope > 2000 && !lastBeatState && irValue > beatMin) {
    lastBeatState = true;
    return true;
  }
  
  if (slope < 0) {
    lastBeatState = false;
  }
  
  return false;
}

void readMAX30102() {
  long irValue = particleSensor.getIR();
  
  if (irValue > 50000) {
    // Finger detected
    if (checkForBeatCustom(irValue)) {
      long delta = millis() - lastBeat;
      lastBeat = millis();
      
      beatsPerMinute = 60 / (delta / 1000.0);
      
      if (beatsPerMinute < 255 && beatsPerMinute > 20) {
        rates[rateSpot++] = (byte)beatsPerMinute;
        rateSpot %= RATE_SIZE;
        
        beatAvg = 0;
        for (byte x = 0; x < RATE_SIZE; x++)
          beatAvg += rates[x];
        beatAvg /= RATE_SIZE;
        
        pulse_rate = beatAvg;
      }
    }
    
    // Simplified SpO2
    float currentSpO2 = readSpO2();
    if (currentSpO2 > 0) {
      spo2 = currentSpO2;
    }
  } else {
    // No finger
    pulse_rate = 0;
    spo2 = 0;
    for (byte x = 0; x < RATE_SIZE; x++)
      rates[x] = 0;
    rateSpot = 0;
    beatAvg = 0;
  }
}

float readSpO2() {
  long redValue = particleSensor.getRed();
  long irValue = particleSensor.getIR();
  
  if (irValue == 0 || redValue == 0) return 0;
  
  float ratio = (float)redValue / (float)irValue;
  float calculatedSpO2 = 110.0 - (ratio * 25.0);
  calculatedSpO2 = constrain(calculatedSpO2, 70.0, 100.0);
  
  return calculatedSpO2;
}

void readDS18B20() {
  tempSensor.requestTemperatures();
  float tempC = tempSensor.getTempCByIndex(0);
  
  if (tempC != DEVICE_DISCONNECTED_C) {
    temperature = tempC;
  } else {
    Serial.println("Error reading DS18B20 temperature");
  }
}

void requestEvent() {
  uint16_t spo2_val = (uint16_t)(spo2 * 10);
  uint16_t pulse = (uint16_t)(pulse_rate * 10);
  uint16_t temp = (uint16_t)(temperature * 10);
  
  Wire.write((spo2_val >> 8) & 0xFF);
  Wire.write(spo2_val & 0xFF);
  Wire.write((pulse >> 8) & 0xFF);
  Wire.write(pulse & 0xFF);
  Wire.write((temp >> 8) & 0xFF);
  Wire.write(temp & 0xFF);
  Wire.write(oxygen_available ? 1 : 0);
  
  Serial.println("Sent oximeter & temperature data to main module");
}

void receiveEvent(int bytesReceived) {
  if (bytesReceived >= 2) {
    uint8_t command = Wire.read();
    uint8_t data = Wire.read();
    
    switch(command) {
      case CMD_OXYGEN_CONTROL:
        oxygen_valve_state = (data == 1) && oxygen_available;
        digitalWrite(OXYGEN_VALVE_PIN, oxygen_valve_state ? HIGH : LOW);
        Serial.println(oxygen_valve_state ? "Oxygen valve OPENED" : "Oxygen valve CLOSED");
        break;
    }
  }
}

void printSensorData() {
  Serial.println("\nOximeter & Temperature Module - Sensor Readings:");
  Serial.println("┌──────────────────────┬──────────────┬──────────────┐");
  Serial.println("│ Sensor               │ Value        │ Status       │");
  Serial.println("├──────────────────────┼──────────────┼──────────────┤");
  
  long irValue = particleSensor.getIR();
  String fingerStatus = (irValue > 50000) ? "Finger" : "No Finger";
  Serial.printf("│ MAX30102 Detection   │ %12ld │ %-12s │\n", irValue, fingerStatus.c_str());
  Serial.printf("│ Heart Rate (BPM)     │ %12.1f │              │\n", pulse_rate);
  Serial.printf("│ SpO2 (%)             │ %12.1f │              │\n", spo2);
  Serial.printf("│ Temperature (°C)     │ %12.1f │              │\n", temperature);
  Serial.printf("│ Temperature (°F)     │ %12.1f │              │\n", (temperature * 9.0/5.0) + 32.0);
  
  String oxygenValveStatus = oxygen_valve_state ? "OPEN" : "CLOSED";
  String oxygenAvailableStatus = oxygen_available ? "AVAILABLE" : "UNAVAILABLE";
  Serial.printf("│ Oxygen Valve         │ %-12s │ %-12s │\n", "", oxygenValveStatus.c_str());
  Serial.printf("│ Oxygen Supply        │ %-12s │ %-12s │\n", "", oxygenAvailableStatus.c_str());
  
  Serial.println("└──────────────────────┴──────────────┴──────────────┘");
  
  if (irValue > 50000) {
    Serial.println("MAX30102 Raw Values:");
    Serial.printf("   IR: %ld, Red: %ld\n", particleSensor.getIR(), particleSensor.getRed());
  }
}
