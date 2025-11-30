#include <Wire.h>              
#include <YFS201.h>            // Flow sensor library

// I2C Address for Fluid module
#define MODULE_ADDRESS 0x40

// Sensor Pin Definitions
#define FLOW_INLET_PIN  4      // Flow sensor inlet
#define FLOW_OUTLET_PIN 5      // Flow sensor outlet
#define PUMP_RELAY_PIN  16     // Pump control

// Flow sensor objects
YFS201 flowInlet;
YFS201 flowOutlet;

// Global variables
float flow_inlet_ml_min = 0.0;
float flow_outlet_ml_min = 0.0;
float fluid_balance_ml = 0.0;
bool pump_state = false;
uint8_t fluid_condition = 0;  // 0=NORMAL, 1=RECOVERY, 2=SERIOUS

// Fluid balance tracking
unsigned long last_flow_update = 0;
float total_inlet_ml = 0.0;
float total_outlet_ml = 0.0;

// Pulse counting for flow sensors
volatile int inlet_pulse_count = 0;
volatile int outlet_pulse_count = 0;

// Flow sensor calibration (pulses per liter)
#define INLET_PULSE_PER_LITER 450.0
#define OUTLET_PULSE_PER_LITER 450.0

// I2C Communication Commands
enum I2CCommands {
  CMD_READ_SENSORS = 0x01,
  CMD_PUMP_CONTROL = 0x02,
  CMD_READ_CONDITION = 0x04
};

void setup() {
  Serial.begin(115200);
  Serial.println("Fluid Monitoring Module Initializing...");
  
  // Initialize pins
  pinMode(FLOW_INLET_PIN, INPUT_PULLUP);
  pinMode(FLOW_OUTLET_PIN, INPUT_PULLUP);
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  digitalWrite(PUMP_RELAY_PIN, LOW);
  
  // Attach interrupts for flow sensors
  attachInterrupt(digitalPinToInterrupt(FLOW_INLET_PIN), inletPulseCounter, FALLING);
  attachInterrupt(digitalPinToInterrupt(FLOW_OUTLET_PIN), outletPulseCounter, FALLING);
  
  // Initialize flow sensors
  flowInlet.begin(WATER);
  flowOutlet.begin(WATER);
  
  // Initialize I2C SLAVE
  Wire.begin(MODULE_ADDRESS);
  Wire.onRequest(requestEvent);
  Wire.onReceive(receiveEvent);
  
  Serial.println("Fluid Monitoring Module Ready");
  Serial.print("I2C Slave Address: 0x");
  Serial.println(MODULE_ADDRESS, HEX);
  Serial.println("Sensors: YF-S201 Inlet + Outlet");
}

void loop() {
  // Update flow rates every 1000ms
  static unsigned long last_update = 0;
  if (millis() - last_update >= 1000) {
    calculateFlowRates();
    updateFluidCondition();
    printSensorData();
    last_update = millis();
  }
  
  delay(50);
}

// Interrupt service routines for pulse counting
void inletPulseCounter() {
  inlet_pulse_count++;
}

void outletPulseCounter() {
  outlet_pulse_count++;
}

void calculateFlowRates() {
  // Calculate flow rates in ml/min
  flow_inlet_ml_min = ((1000.0 / (millis() - last_flow_update)) * ((inlet_pulse_count / INLET_PULSE_PER_LITER) * 1000));
  flow_outlet_ml_min = ((1000.0 / (millis() - last_flow_update)) * ((outlet_pulse_count / OUTLET_PULSE_PER_LITER) * 1000));
  
  // Accumulate total volume
  total_inlet_ml += flow_inlet_ml_min * (millis() - last_flow_update) / 60000.0;
  total_outlet_ml += flow_outlet_ml_min * (millis() - last_flow_update) / 60000.0;
  
  // Calculate fluid balance (positive = net gain, negative = net loss)
  fluid_balance_ml = total_inlet_ml - total_outlet_ml;
  
  // Reset pulse counters
  inlet_pulse_count = 0;
  outlet_pulse_count = 0;
  last_flow_update = millis();
  
  // Constrain values
  flow_inlet_ml_min = constrain(flow_inlet_ml_min, 0.0, 500.0);
  flow_outlet_ml_min = constrain(flow_outlet_ml_min, 0.0, 500.0);
}

void updateFluidCondition() {
  // Simple rule-based condition assessment
  if (abs(fluid_balance_ml) < 50.0 && 
      flow_inlet_ml_min > 10.0 && flow_outlet_ml_min > 5.0 &&
      abs(flow_inlet_ml_min - flow_outlet_ml_min) < 20.0) {
    fluid_condition = 0;  // NORMAL
  }
  else if (abs(fluid_balance_ml) < 200.0 || 
           (flow_inlet_ml_min > 5.0 && flow_outlet_ml_min > 2.0)) {
    fluid_condition = 1;  // RECOVERY
  }
  else {
    fluid_condition = 2;  // SERIOUS
  }
}

void requestEvent() {
  // Send 6 bytes: flow_inlet(2), flow_outlet(2), condition(1), pump_status(1)
  uint16_t inlet_flow = (uint16_t)(flow_inlet_ml_min * 10);
  uint16_t outlet_flow = (uint16_t)(flow_outlet_ml_min * 10);
  
  Wire.write((inlet_flow >> 8) & 0xFF);
  Wire.write(inlet_flow & 0xFF);
  Wire.write((outlet_flow >> 8) & 0xFF);
  Wire.write(outlet_flow & 0xFF);
  Wire.write(fluid_condition);
  Wire.write(pump_state ? 1 : 0);
  
  Serial.print("Sent fluid data: Inlet=");
  Serial.print(flow_inlet_ml_min);
  Serial.print(", Outlet=");
  Serial.print(flow_outlet_ml_min);
  Serial.print(", Condition=");
  Serial.print(fluid_condition);
  Serial.println();
}

void receiveEvent(int bytesReceived) {
  if (bytesReceived >= 2) {
    uint8_t command = Wire.read();
    uint8_t data = Wire.read();
    
    switch(command) {
      case CMD_PUMP_CONTROL:
        pump_state = (data == 1);
        digitalWrite(PUMP_RELAY_PIN, pump_state ? HIGH : LOW);
        Serial.println(pump_state ? "Pump ENABLED" : "Pump DISABLED");
        break;
        
      case CMD_READ_CONDITION:
        // Respond with current condition (handled by requestEvent)
        break;
    }
  }
}

void printSensorData() {
  Serial.println("\nFluid Monitoring Module - Sensor Readings:");
  Serial.println("┌──────────────────────┬──────────────┬──────────────┐");
  Serial.println("│ Sensor               │ Value        │ Status       │");
  Serial.println("├──────────────────────┼──────────────┼──────────────┤");
  
  Serial.printf("│ Flow Inlet (ml/min)  │ %12.1f │              │\n", flow_inlet_ml_min);
  Serial.printf("│ Flow Outlet (ml/min) │ %12.1f │              │\n", flow_outlet_ml_min);
  Serial.printf("│ Fluid Balance (ml)   │ %12.1f │              │\n", fluid_balance_ml);
  Serial.printf("│ Total Inlet (ml)     │ %12.1f │              │\n", total_inlet_ml);
  Serial.printf("│ Total Outlet (ml)    │ %12.1f │              │\n", total_outlet_ml);
  
  String condition_str;
  switch(fluid_condition) {
    case 0: condition_str = "NORMAL"; break;
    case 1: condition_str = "RECOVERY"; break;
    case 2: condition_str = "SERIOUS"; break;
    default: condition_str = "ERROR"; break;
  }
  
  String pump_status = pump_state ? "ON" : "OFF";
  Serial.printf("│ Fluid Condition      │ %-12s │ Pump: %-7s │\n", condition_str.c_str(), pump_status.c_str());
  
  Serial.println("└──────────────────────┴──────────────┴──────────────┘");
}
