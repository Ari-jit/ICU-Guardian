#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "model_data.h"
#include "feature_constants.h"

// TensorFlow Lite for Microcontrollers
#include <tensorflow/lite/micro/all_ops_resolver.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/schema/schema_generated.h>

// WiFi Credentials
const char* ssid = "Arijit's S23";
const char* password = "Acg272006";

// Web server on port 80
WebServer server(80);

// I2C Module Addresses
#define FLUID_MODULE_ADDR    0x40
#define ECG_MODULE_ADDR      0x41
#define OXIMETER_MODULE_ADDR 0x42

// Actuator pins
#define PUMP_RELAY_PIN     4
#define OXYGEN_VALVE_PIN   5
#define BUZZER_PIN         6
#define STATUS_LED_PIN     7

// I2C Communication Commands
enum I2CCommands {
  CMD_READ_SENSORS = 0x01,
  CMD_PUMP_CONTROL = 0x02,
  CMD_OXYGEN_CONTROL = 0x03,
  CMD_READ_CONDITION = 0x04
};

// Global variables
float current_sensor_data[10];
bool oxygen_supply_available = true;
String health_status = "Initializing";
String fluid_condition = "Initializing";
String heart_condition = "Initializing";
bool fluid_inlet_control = false;
unsigned long last_prediction_time = 0;
const unsigned long PREDICTION_INTERVAL = 10000;

// Health status labels
const char* HEALTH_LABELS[] = {"NORMAL", "RECOVERY", "SERIOUS"};

// TensorFlow Lite model
namespace {
  const tflite::Model* model = nullptr;
  tflite::MicroInterpreter* interpreter = nullptr;
  TfLiteTensor* input = nullptr;
  TfLiteTensor* output = nullptr;
  const int tensor_arena_size = 20 * 1024;
  uint8_t tensor_arena[tensor_arena_size];
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nICU Health Monitoring System Starting...");
  
  // Initialize I2C as Master
  Wire.begin();
  Serial.println("I2C Master Initialized");
  
  // Test module communication
  test_module_communication();
  
  // Initialize actuator pins
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  pinMode(OXYGEN_VALVE_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);
  
  digitalWrite(PUMP_RELAY_PIN, LOW);
  digitalWrite(OXYGEN_VALVE_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(STATUS_LED_PIN, HIGH);
  
  Serial.println("Actuator Pins Initialized");
  
  // Load TensorFlow Lite model
  load_model();
  
  // Connect to WiFi
  setup_wifi();
  
  // Setup web server routes
  setup_webserver();
  
  // Blink LED to indicate ready
  for(int i = 0; i < 3; i++) {
    digitalWrite(STATUS_LED_PIN, LOW);
    delay(200);
    digitalWrite(STATUS_LED_PIN, HIGH);
    delay(200);
  }
  
  Serial.println("ICU Monitoring System Ready!");
  Serial.print("Web Dashboard: http://");
  Serial.println(WiFi.localIP());
}

void loop() {
  server.handleClient();
  
  if (millis() - last_prediction_time >= PREDICTION_INTERVAL) {
    read_all_modules();
    int prediction = predict_health();
    control_all_actuators(prediction);
    last_prediction_time = millis();
    print_system_status();
  }
  
  // Blink status LED
  static unsigned long last_blink = 0;
  if (millis() - last_blink >= 1000) {
    digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN));
    last_blink = millis();
  }
}

void test_module_communication() {
  Serial.println("Testing module communication...");
  
  bool fluid_ok = test_i2c_device(FLUID_MODULE_ADDR);
  bool ecg_ok = test_i2c_device(ECG_MODULE_ADDR);
  bool oximeter_ok = test_i2c_device(OXIMETER_MODULE_ADDR);
  
  if (fluid_ok && ecg_ok && oximeter_ok) {
    Serial.println("All modules responding correctly");
  } else {
    Serial.println("Some modules not responding!");
    if (!fluid_ok) Serial.println("  - Fluid Module (0x40) not found");
    if (!ecg_ok) Serial.println("  - ECG Module (0x41) not found");
    if (!oximeter_ok) Serial.println("  - Oximeter Module (0x42) not found");
  }
}

bool test_i2c_device(uint8_t address) {
  Wire.beginTransmission(address);
  return (Wire.endTransmission() == 0);
}

void read_all_modules() {
  read_fluid_module();
  read_ecg_module();
  read_oximeter_module();
  Serial.println("Data collected from all modules");
}

void read_fluid_module() {
  Wire.beginTransmission(FLUID_MODULE_ADDR);
  Wire.write(CMD_READ_SENSORS);
  Wire.endTransmission();
  
  delay(10);
  
  Wire.requestFrom(FLUID_MODULE_ADDR, 6);
  if (Wire.available() == 6) {
    current_sensor_data[0] = (Wire.read() << 8) | Wire.read(); // Flow Inlet
    current_sensor_data[1] = (Wire.read() << 8) | Wire.read(); // Flow Outlet
    
    uint8_t condition = Wire.read();
    uint8_t pump_status = Wire.read();
    
    switch(condition) {
      case 0: fluid_condition = "NORMAL"; break;
      case 1: fluid_condition = "RECOVERY"; break;
      case 2: fluid_condition = "SERIOUS"; break;
      default: fluid_condition = "ERROR"; break;
    }
    
    current_sensor_data[2] = current_sensor_data[0] - current_sensor_data[1]; // Fluid Balance
    fluid_inlet_control = (pump_status == 1);
  }
}

void read_ecg_module() {
  Wire.beginTransmission(ECG_MODULE_ADDR);
  Wire.write(CMD_READ_SENSORS);
  Wire.endTransmission();
  
  delay(10);
  
  Wire.requestFrom(ECG_MODULE_ADDR, 9);
  if (Wire.available() == 9) {
    current_sensor_data[3] = (Wire.read() << 8) | Wire.read(); // Heart Rate
    current_sensor_data[4] = (Wire.read() << 8) | Wire.read(); // ECG Amplitude
    current_sensor_data[5] = (Wire.read() << 8) | Wire.read(); // HR Variability
    current_sensor_data[6] = (Wire.read() << 8) | Wire.read(); // QRS Duration
    
    uint8_t condition = Wire.read();
    switch(condition) {
      case 0: heart_condition = "NORMAL"; break;
      case 1: heart_condition = "RECOVERY"; break;
      case 2: heart_condition = "SERIOUS"; break;
      default: heart_condition = "ERROR"; break;
    }
  }
}

void read_oximeter_module() {
  Wire.beginTransmission(OXIMETER_MODULE_ADDR);
  Wire.write(CMD_READ_SENSORS);
  Wire.endTransmission();
  
  delay(10);
  
  Wire.requestFrom(OXIMETER_MODULE_ADDR, 7);
  if (Wire.available() == 7) {
    current_sensor_data[7] = (Wire.read() << 8) | Wire.read(); // SpO2
    current_sensor_data[8] = (Wire.read() << 8) | Wire.read(); // Pulse Rate
    current_sensor_data[9] = (Wire.read() << 8) | Wire.read(); // Temperature
    
    uint8_t oxygen_status = Wire.read();
    oxygen_supply_available = (oxygen_status == 1);
  }
}

void control_all_actuators(int health_status_code) {
  bool pump_state = (health_status_code == 1);
  control_fluid_pump(pump_state);
  
  bool oxygen_state = (health_status_code >= 1) && oxygen_supply_available;
  control_oxygen_valve(oxygen_state);
  
  // Buzzer control
  digitalWrite(BUZZER_PIN, (health_status_code == 2) ? HIGH : LOW);
}

void control_fluid_pump(bool state) {
  Wire.beginTransmission(FLUID_MODULE_ADDR);
  Wire.write(CMD_PUMP_CONTROL);
  Wire.write(state ? 1 : 0);
  Wire.endTransmission();
  
  digitalWrite(PUMP_RELAY_PIN, state ? HIGH : LOW);
  fluid_inlet_control = state;
  Serial.println(state ? "Pump ON" : "Pump OFF");
}

void control_oxygen_valve(bool state) {
  Wire.beginTransmission(OXIMETER_MODULE_ADDR);
  Wire.write(CMD_OXYGEN_CONTROL);
  Wire.write(state ? 1 : 0);
  Wire.endTransmission();
  
  digitalWrite(OXYGEN_VALVE_PIN, state ? HIGH : LOW);
  Serial.println(state ? "Oxygen ON" : "Oxygen OFF");
}

void load_model() {
  Serial.println("Loading TensorFlow Lite model...");
  model = tflite::GetModel(model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("Model schema mismatch!");
    while(1);
  }
  
  static tflite::AllOpsResolver resolver;
  static tflite::MicroInterpreter static_interpreter(model, resolver, tensor_arena, tensor_arena_size);
  interpreter = &static_interpreter;
  
  if (interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("Failed to allocate tensors!");
    while(1);
  }
  
  input = interpreter->input(0);
  output = interpreter->output(0);
  Serial.println("TensorFlow Lite model loaded successfully!");
}

int predict_health() {
  // Scale features for ML model
  for (int i = 0; i < num_features; i++) {
    input->data.f[i] = (current_sensor_data[i] - feature_means[i]) / feature_stds[i];
  }
  
  TfLiteStatus invoke_status = interpreter->Invoke();
  if (invoke_status != kTfLiteOk) {
    Serial.println("Inference failed!");
    return -1;
  }
  
  int prediction = 0;
  float max_prob = output->data.f[0];
  for (int i = 1; i < 3; i++) {
    if (output->data.f[i] > max_prob) {
      max_prob = output->data.f[i];
      prediction = i;
    }
  }
  
  health_status = HEALTH_LABELS[prediction];
  Serial.print("ML Prediction: ");
  Serial.print(health_status);
  Serial.print(" (Confidence: ");
  Serial.print(max_prob * 100, 1);
  Serial.println("%)");
  
  return prediction;
}

void setup_wifi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed - Offline mode");
  }
}

void print_system_status() {
  Serial.println("\nSYSTEM STATUS SUMMARY:");
  Serial.println("┌──────────────────────┬──────────┬────────────┐");
  Serial.println("│ Module               │ Status   │ Value      │");
  Serial.println("├──────────────────────┼──────────┼────────────┤");
  Serial.printf("│ Overall Health       │ %-8s │            │\n", health_status.c_str());
  Serial.printf("│ Fluid Condition      │ %-8s │ Balance: %.1f│\n", fluid_condition.c_str(), current_sensor_data[2]);
  Serial.printf("│ Heart Condition      │ %-8s │ Rate: %.1f   │\n", heart_condition.c_str(), current_sensor_data[3]);
  Serial.printf("│ Oxygen Supply        │ %-8s │            │\n", oxygen_supply_available ? "Available" : "Unavailable");
  Serial.println("└──────────────────────┴──────────┴────────────┘");
}

void setup_webserver() {
  server.on("/", HTTP_GET, []() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ICU Health Monitor</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { 
            font-family: 'Segoe UI', Arial, sans-serif; 
            color: #333; min-height: 100vh; padding: 20px;
            transition: all 0.5s ease; position: relative; overflow-x: hidden;
        }
        body.normal { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); }
        body.recovery { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); }
        body.serious { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); animation: pulse-glow 2s infinite; }
        @keyframes pulse-glow { 0%, 100% { opacity: 1; } 50% { opacity: 0.8; } }
        
        .container { max-width: 1200px; margin: 0 auto; background: rgba(255,255,255,0.95); 
                     border-radius: 20px; padding: 30px; box-shadow: 0 15px 40px rgba(0,0,0,0.3); }
        .header { text-align: center; margin-bottom: 20px; padding-bottom: 15px; border-bottom: 2px solid #e9ecef; }
        .header h1 { font-size: 2.5em; color: #2c3e50; margin-bottom: 8px; text-shadow: 2px 2px 4px rgba(0,0,0,0.1); }
        .header p { color: #7f8c8d; font-size: 1.1em; }
        
        .main-status { text-align: center; margin: 20px 0; padding: 20px; border-radius: 12px; font-size: 1.6em; 
                      font-weight: bold; text-transform: uppercase; letter-spacing: 1px; box-shadow: 0 6px 20px rgba(0,0,0,0.15);
                      transition: all 0.3s ease; background: rgba(255,255,255,0.9); border: 3px solid #dee2e6; }
        .main-status.normal { background: linear-gradient(135deg, #28a745, #20c997); color: white; border: 3px solid #1e7e34; box-shadow: 0 6px 20px rgba(40,167,69,0.4); }
        .main-status.recovery { background: linear-gradient(135deg, #ffc107, #fd7e14); color: #856404; border: 3px solid #e0a800; box-shadow: 0 6px 20px rgba(255,193,7,0.4); }
        .main-status.serious { background: linear-gradient(135deg, #dc3545, #c82333); color: white; border: 3px solid #bd2130; box-shadow: 0 6px 20px rgba(220,53,69,0.4); animation: pulse 2s infinite; }
        @keyframes pulse { 0% { transform: scale(1); } 50% { transform: scale(1.01); } 100% { transform: scale(1); } }
        
        .condition-row { display: grid; grid-template-columns: 1fr 1fr; gap: 20px; margin-bottom: 25px; }
        .condition-card { padding: 20px; border-radius: 15px; text-align: center; font-size: 1.1em; font-weight: bold;
                         box-shadow: 0 6px 20px rgba(0,0,0,0.15); transition: all 0.3s ease; background: rgba(255,255,255,0.9);
                         border: 2px solid #e9ecef; position: relative; overflow: hidden; }
        .condition-card::before { content: ''; position: absolute; top: 0; left: 0; right: 0; height: 6px; background: #6c757d; transition: all 0.3s ease; }
        .condition-card:hover { transform: translateY(-5px); box-shadow: 0 10px 25px rgba(0,0,0,0.2); }
        .condition-card h2 { font-size: 1.1em; margin-bottom: 15px; color: #2c3e50; font-weight: 700; text-transform: uppercase; letter-spacing: 0.5px; }
        .condition-card .status-value { font-size: 1.6em; padding: 15px; border-radius: 10px; color: white; font-weight: 800; text-shadow: 1px 1px 2px rgba(0,0,0,0.3);
                                       box-shadow: 0 3px 10px rgba(0,0,0,0.2); transition: all 0.3s ease; background: linear-gradient(135deg, #6c757d, #495057);
                                       border: 2px solid rgba(255,255,255,0.5); }
        
        .fluid-card.normal::before { background: linear-gradient(90deg, #28a745, #20c997); }
        .fluid-card.normal .status-value { background: linear-gradient(135deg, #28a745, #20c997); box-shadow: 0 3px 12px rgba(40,167,69,0.4); border-color: rgba(40,167,69,0.3); }
        .fluid-card.recovery::before { background: linear-gradient(90deg, #ffc107, #fd7e14); }
        .fluid-card.recovery .status-value { background: linear-gradient(135deg, #ffc107, #fd7e14); color: #856404; text-shadow: 1px 1px 2px rgba(255,255,255,0.5); box-shadow: 0 3px 12px rgba(255,193,7,0.4); border-color: rgba(255,193,7,0.3); }
        .fluid-card.serious::before { background: linear-gradient(90deg, #dc3545, #c82333); animation: pulse-top 2s infinite; }
        .fluid-card.serious .status-value { background: linear-gradient(135deg, #dc3545, #c82333); box-shadow: 0 3px 12px rgba(220,53,69,0.4); border-color: rgba(220,53,69,0.3); animation: pulse-value 1.5s infinite; }
        
        .heart-card.normal::before, .heart-card.normal .status-value { background: linear-gradient(135deg, #28a745, #20c997); box-shadow: 0 3px 12px rgba(40,167,69,0.4); border-color: rgba(40,167,69,0.3); }
        .heart-card.recovery::before, .heart-card.recovery .status-value { background: linear-gradient(135deg, #ffc107, #fd7e14); color: #856404; text-shadow: 1px 1px 2px rgba(255,255,255,0.5); box-shadow: 0 3px 12px rgba(255,193,7,0.4); border-color: rgba(255,193,7,0.3); }
        .heart-card.serious::before, .heart-card.serious .status-value { background: linear-gradient(135deg, #dc3545, #c82333); box-shadow: 0 3px 12px rgba(220,53,69,0.4); border-color: rgba(220,53,69,0.3); animation: pulse-value 1.5s infinite; }
        
        @keyframes pulse-top { 0% { opacity: 1; } 50% { opacity: 0.7; } 100% { opacity: 1; } }
        @keyframes pulse-value { 0% { transform: scale(1); } 50% { transform: scale(1.03); } 100% { transform: scale(1); } }
        
        .control-panel { background: linear-gradient(135deg, #e9ecef, #dee2e6); padding: 25px; border-radius: 12px; text-align: center; margin: 25px 0; box-shadow: 0 5px 15px rgba(0,0,0,0.1); border: 2px solid #ced4da; }
        .control-panel h2 { margin-bottom: 20px; color: #2c3e50; font-size: 1.6em; text-transform: uppercase; letter-spacing: 1px; }
        .control-buttons { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 15px; max-width: 600px; margin: 0 auto; }
        .control-item { display: flex; flex-direction: column; align-items: center; gap: 8px; padding: 15px; background: rgba(255,255,255,0.8); border-radius: 10px; box-shadow: 0 3px 10px rgba(0,0,0,0.1); }
        .control-item h3 { color: #2c3e50; font-size: 1.1em; font-weight: 600; }
        .btn { color: white; border: none; padding: 15px 25px; border-radius: 25px; cursor: pointer; font-size: 1.1em; font-weight: bold; 
               transition: all 0.3s ease; min-width: 160px; box-shadow: 0 3px 10px rgba(0,0,0,0.2); text-transform: uppercase; letter-spacing: 1px; }
        .btn:hover { transform: translateY(-2px); box-shadow: 0 6px 20px rgba(0,0,0,0.3); }
        .btn:active { transform: translateY(-1px); }
        .btn-on { background: linear-gradient(135deg, #28a745, #20c997); }
        .btn-on:hover { background: linear-gradient(135deg, #218838, #1ea085); }
        .btn-off { background: linear-gradient(135deg, #dc3545, #c82333); }
        .btn-off:hover { background: linear-gradient(135deg, #c82333, #a71e2a); }
        
        .sensor-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 15px; margin: 25px 0; }
        .sensor-card { background: linear-gradient(135deg, #f8f9fa, #e9ecef); padding: 20px; border-radius: 10px; border-left: 5px solid #007bff; 
                      text-align: center; transition: all 0.3s ease; box-shadow: 0 3px 10px rgba(0,0,0,0.1); min-height: 130px; 
                      display: flex; flex-direction: column; justify-content: center; }
        .sensor-card:hover { transform: translateY(-3px); box-shadow: 0 6px 20px rgba(0,0,0,0.15); }
        .sensor-value { font-size: 1.8em; font-weight: bold; color: #007bff; margin: 12px 0; }
        
        .last-update { text-align: center; padding: 15px; color: #6c757d; font-style: italic; background: rgba(248,249,250,0.8); 
                      border-radius: 8px; margin-top: 25px; font-size: 1em; border: 1px solid #dee2e6; }
        
        @media (max-width: 768px) { .condition-row { grid-template-columns: 1fr; } .sensor-grid { grid-template-columns: repeat(2, 1fr); } }
        @media (max-width: 480px) { .sensor-grid { grid-template-columns: 1fr; } .control-buttons { grid-template-columns: 1fr; } }
    </style>
</head>
<body id="bodyElement">
    <div class="container">
        <div class="header">
            <h1>ICU HEALTH MONITOR</h1>
            <p>Real-time Patient Health Monitoring System</p>
        </div>
        
        <div class="main-status" id="mainStatus">Overall Health: <span id="mainStatusText">Loading...</span></div>
        
        <div class="condition-row">
            <div class="condition-card fluid-card" id="fluidCondition">
                <h2>Fluid Condition</h2>
                <div class="status-value" id="fluidText">Loading...</div>
            </div>
            <div class="condition-card heart-card" id="heartCondition">
                <h2>Heart Condition</h2>
                <div class="status-value" id="heartText">Loading...</div>
            </div>
        </div>
        
        <div class="control-panel">
            <h2>Control Panel</h2>
            <div class="control-buttons">
                <div class="control-item">
                    <h3>Oxygen Supply</h3>
                    <button class="btn btn-off" id="oxygenButton" onclick="toggleOxygen()">Oxygen Off</button>
                    <span id="oxygenStatus" style="font-weight: bold; font-size: 1em;">Status: Checking...</span>
                </div>
                <div class="control-item">
                    <h3>Fluid Inlet</h3>
                    <button class="btn btn-off" id="fluidButton" onclick="toggleFluid()">Fluid Inlet Off</button>
                    <span id="fluidStatus" style="font-weight: bold; font-size: 1em;">Status: Checking...</span>
                </div>
            </div>
        </div>
        
        <h2 style="text-align: center; margin: 25px 0; color: #2c3e50; font-size: 1.8em;">Vital Signs Monitor</h2>
        <div class="sensor-grid" id="sensorGrid"></div>
        
        <div class="last-update">
            <div>Last updated: <span id="timestamp">-</span></div>
            <div>System IP: <span id="ipAddress">-</span> | WiFi: <span id="wifiStatus">Connected</span></div>
        </div>
    </div>

    <script>
        function updateDashboard() {
            fetch('/data').then(response => response.json()).then(data => {
                updateMainStatus(data.health_status);
                updateConditionCard('fluidCondition', 'fluidText', data.fluid_status || 'NORMAL', 'fluid');
                updateConditionCard('heartCondition', 'heartText', data.heart_status || 'NORMAL', 'heart');
                updateOxygenButton(data.oxygen_available);
                updateFluidButton(data.fluid_inlet_control);
                updateSensorGrid(data.sensors);
                document.getElementById('timestamp').textContent = new Date().toLocaleTimeString();
                document.getElementById('ipAddress').textContent = data.ip_address || 'Unknown';
            }).catch(error => {
                console.error('Error updating dashboard:', error);
                document.getElementById('mainStatusText').textContent = 'Connection Error';
            });
        }
        
        function updateMainStatus(status) {
            const mainStatus = document.getElementById('mainStatus');
            const mainStatusText = document.getElementById('mainStatusText');
            const bodyElement = document.getElementById('bodyElement');
            mainStatusText.textContent = status;
            mainStatus.classList.remove('normal', 'recovery', 'serious');
            bodyElement.classList.remove('normal', 'recovery', 'serious');
            if (status === 'NORMAL') { mainStatus.classList.add('normal'); bodyElement.classList.add('normal'); }
            else if (status === 'RECOVERY') { mainStatus.classList.add('recovery'); bodyElement.classList.add('recovery'); }
            else if (status === 'SERIOUS') { mainStatus.classList.add('serious'); bodyElement.classList.add('serious'); }
        }
        
        function updateConditionCard(cardId, textId, status, type) {
            const card = document.getElementById(cardId);
            const text = document.getElementById(textId);
            text.textContent = status;
            card.classList.remove('normal', 'recovery', 'serious');
            if (status === 'NORMAL') card.classList.add('normal');
            else if (status === 'RECOVERY') card.classList.add('recovery');
            else if (status === 'SERIOUS') card.classList.add('serious');
        }
        
        function updateOxygenButton(isAvailable) {
            const button = document.getElementById('oxygenButton');
            const status = document.getElementById('oxygenStatus');
            if (isAvailable) {
                button.textContent = 'Oxygen On'; button.classList.remove('btn-off'); button.classList.add('btn-on');
                status.textContent = 'Status: Active';
            } else {
                button.textContent = 'Oxygen Off'; button.classList.remove('btn-on'); button.classList.add('btn-off');
                status.textContent = 'Status: Inactive';
            }
        }
        
        function updateFluidButton(isActive) {
            const button = document.getElementById('fluidButton');
            const status = document.getElementById('fluidStatus');
            if (isActive) {
                button.textContent = 'Fluid Inlet On'; button.classList.remove('btn-off'); button.classList.add('btn-on');
                status.textContent = 'Status: Active';
            } else {
                button.textContent = 'Fluid Inlet Off'; button.classList.remove('btn-on'); button.classList.add('btn-off');
                status.textContent = 'Status: Inactive';
            }
        }
        
        function updateSensorGrid(sensors) {
            const sensorGrid = document.getElementById('sensorGrid');
            sensorGrid.innerHTML = '';
            const sensorOrder = ['Flow Inlet', 'Flow Outlet', 'Fluid Balance', 'Heart Rate', 'SpO2', 'Temperature', 'ECG Amplitude', 'HR Variability', 'QRS Duration'];
            sensorOrder.forEach(sensorName => {
                if (sensors.hasOwnProperty(sensorName)) {
                    const card = document.createElement('div');
                    card.className = 'sensor-card';
                    card.innerHTML = `<h3>${sensorName}</h3><div class="sensor-value">${sensors[sensorName]}</div><small>${getSensorUnit(sensorName)}</small>`;
                    sensorGrid.appendChild(card);
                }
            });
        }
        
        function getSensorUnit(sensor) {
            const units = {'Flow Inlet': 'ml/min', 'Flow Outlet': 'ml/min', 'Fluid Balance': 'ml', 'Heart Rate': 'BPM', 
                          'ECG Amplitude': 'mV', 'HR Variability': 'ms', 'QRS Duration': 'ms', 'SpO2': '%', 'Temperature': '°C'};
            return units[sensor] || '';
        }
        
        function toggleOxygen() { fetch('/oxygen', {method: 'POST'}).then(response => response.json()).then(data => updateOxygenButton(data.available)); }
        function toggleFluid() { fetch('/fluid', {method: 'POST'}).then(response => response.json()).then(data => updateFluidButton(data.fluid_inlet_control)); }
        
        setInterval(updateDashboard, 5000);
        updateDashboard();
    </script>
</body>
</html>
    )rawliteral";
    server.send(200, "text/html", html);
  });

  server.on("/data", HTTP_GET, []() {
    StaticJsonDocument<1024> doc;
    doc["health_status"] = health_status;
    doc["fluid_status"] = fluid_condition;
    doc["heart_status"] = heart_condition;
    doc["timestamp"] = millis();
    doc["oxygen_available"] = oxygen_supply_available;
    doc["fluid_inlet_control"] = fluid_inlet_control;
    doc["ip_address"] = WiFi.localIP().toString();
    
    JsonObject sensors = doc.createNestedObject("sensors");
    sensors["Flow Inlet"] = current_sensor_data[0];
    sensors["Flow Outlet"] = current_sensor_data[1];
    sensors["Fluid Balance"] = current_sensor_data[2];
    sensors["Heart Rate"] = current_sensor_data[3];
    sensors["ECG Amplitude"] = current_sensor_data[4];
    sensors["HR Variability"] = current_sensor_data[5];
    sensors["QRS Duration"] = current_sensor_data[6];
    sensors["SpO2"] = current_sensor_data[7];
    sensors["Temperature"] = current_sensor_data[9];
    

    if (output != nullptr) {
      float max_prob = 0;
      for (int i = 0; i < 3; i++) {
        if (output->data.f[i] > max_prob) max_prob = output->data.f[i];
      }
      doc["confidence"] = max_prob;
    }
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });

  server.on("/oxygen", HTTP_POST, []() {
    oxygen_supply_available = !oxygen_supply_available;
    digitalWrite(OXYGEN_VALVE_PIN, oxygen_supply_available ? HIGH : LOW);
    StaticJsonDocument<200> doc;
    doc["available"] = oxygen_supply_available;
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
    Serial.println(oxygen_supply_available ? "Oxygen ENABLED" : "Oxygen DISABLED");
  });

  server.on("/fluid", HTTP_POST, []() {
    fluid_inlet_control = !fluid_inlet_control;
    digitalWrite(PUMP_RELAY_PIN, fluid_inlet_control ? HIGH : LOW);
    StaticJsonDocument<200> doc;
    doc["fluid_inlet_control"] = fluid_inlet_control;
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
    Serial.println(fluid_inlet_control ? "Fluid Inlet ENABLED" : "Fluid Inlet DISABLED");
  });

  server.begin();
  Serial.println("Web server started on port 80");
}
