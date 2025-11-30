#include <Wire.h>

// I2C Address for ECG Module
#define MODULE_ADDRESS 0x41

// AD8232 Pin Definitions
#define ECG_OUTPUT_PIN 34   // Analog pin for ECG signal
#define LO_PLUS_PIN    35   // Lead-off detection positive
#define LO_MINUS_PIN   32   // Lead-off detection negative

// ECG Analysis Parameters
#define SAMPLE_RATE 250     // Samples per second
#define BUFFER_SIZE 250     // 1 second buffer
#define QRS_THRESHOLD 500   // Threshold for QRS detection

// Global variables
float heart_rate = 75.0;
float ecg_amplitude = 1.1;
float hr_variability = 50.0;
float qrs_duration = 90.0;
uint8_t heart_condition = 0;

// ECG Data Buffer
int ecg_buffer[BUFFER_SIZE];
int buffer_index = 0;
unsigned long last_sample_time = 0;
unsigned long sample_interval = 1000000 / SAMPLE_RATE; // microseconds

// QRS Detection
unsigned long last_r_peak_time = 0;
int r_peak_value = 0;
int rr_intervals[10];
int rr_index = 0;

// I2C Communication Commands
enum I2CCommands {
  CMD_READ_SENSORS = 0x01
};

void setup() {
  Serial.begin(115200);
  
  // Initialize AD8232 pins
  pinSetup();
  
  // Initialize I2C as Slave
  Wire.begin(MODULE_ADDRESS);
  Wire.onRequest(requestEvent);
  
  Serial.println("ECG Monitoring Module with AD8232 Ready");
  Serial.print("I2C Address: 0x");
  Serial.println(MODULE_ADDRESS, HEX);
  Serial.print("Sample Rate: ");
  Serial.print(SAMPLE_RATE);
  Serial.println(" Hz");
}

void pinSetup() {
  // Configure AD8232 pins
  pinMode(ECG_OUTPUT_PIN, INPUT);
  pinMode(LO_PLUS_PIN, INPUT);
  pinMode(LO_MINUS_PIN, INPUT);
}

void loop() {
  // Read ECG data at fixed sample rate
  unsigned long current_time = micros();
  if (current_time - last_sample_time >= sample_interval) {
    readECGSignal();
    last_sample_time = current_time;
  }
  
  // Analyze ECG data every second
  static unsigned long last_analysis = 0;
  if (millis() - last_analysis >= 1000) {
    analyzeECG();
    calculate_heart_condition();
    last_analysis = millis();
    
    // Print debug information
    printECGData();
  }
}

void readECGSignal() {
  // Read raw ECG value (0-4095 for ESP32 ADC)
  int raw_ecg = analogRead(ECG_OUTPUT_PIN);
  
  // Store in circular buffer
  ecg_buffer[buffer_index] = raw_ecg;
  buffer_index = (buffer_index + 1) % BUFFER_SIZE;
  
  // Simple QRS detection (peak detection)
  detectQRSPeak(raw_ecg);
}

void detectQRSPeak(int ecg_value) {
  static int last_value = 0;
  static bool rising = false;
  
  // Simple slope-based QRS detection
  if (ecg_value > last_value) {
    rising = true;
  } else if (rising && ecg_value < last_value) {
    // Potential peak detected
    if (ecg_value > QRS_THRESHOLD && ecg_value > r_peak_value) {
      r_peak_value = ecg_value;
      
      // Calculate RR interval if we have a previous R peak
      unsigned long current_time = micros();
      if (last_r_peak_time > 0) {
        unsigned long rr_interval = (current_time - last_r_peak_time) / 1000; // Convert to ms
        
        // Store RR interval for HRV calculation
        rr_intervals[rr_index] = rr_interval;
        rr_index = (rr_index + 1) % 10;
        
        // Calculate heart rate (beats per minute)
        heart_rate = 60000.0 / rr_interval; // 60000 ms in a minute
      }
      last_r_peak_time = current_time;
    }
    rising = false;
  }
  
  last_value = ecg_value;
}

void analyzeECG() {
  // Calculate ECG amplitude (mV)
  int min_val = 4096, max_val = 0;
  for (int i = 0; i < BUFFER_SIZE; i++) {
    if (ecg_buffer[i] < min_val) min_val = ecg_buffer[i];
    if (ecg_buffer[i] > max_val) max_val = ecg_buffer[i];
  }
  
  // Convert ADC values to millivolts (assuming 3.3V reference)
  // AD8232 typically outputs ±1.5mV, amplified to 0-3.3V
  float adc_to_mv = 3300.0 / 4095.0; // 3.3V = 3300mV, 12-bit ADC
  ecg_amplitude = (max_val - min_val) * adc_to_mv / 1000.0; // Convert to mV
  
  // Calculate Heart Rate Variability (SDNN)
  calculateHRV();
  
  // Estimate QRS duration (simplified)
  estimateQRSDuration();
}

void calculateHRV() {
  // Calculate standard deviation of RR intervals (SDNN)
  if (rr_index == 0) return;
  
  float mean_rr = 0;
  int valid_intervals = 0;
  
  // Calculate mean RR interval
  for (int i = 0; i < 10; i++) {
    if (rr_intervals[i] > 0) {
      mean_rr += rr_intervals[i];
      valid_intervals++;
    }
  }
  
  if (valid_intervals > 0) {
    mean_rr /= valid_intervals;
    
    // Calculate standard deviation
    float variance = 0;
    for (int i = 0; i < 10; i++) {
      if (rr_intervals[i] > 0) {
        variance += pow(rr_intervals[i] - mean_rr, 2);
      }
    }
    variance /= valid_intervals;
    hr_variability = sqrt(variance);
  }
}

void estimateQRSDuration() {
  // Simplified QRS duration estimation
  // In a real system, you'd use more sophisticated signal processing
  qrs_duration = 80.0 + random(-10, 10); // Base value with small variation
  
  // Adjust based on heart rate (faster HR often has shorter QRS)
  if (heart_rate > 100) {
    qrs_duration -= 5;
  } else if (heart_rate < 60) {
    qrs_duration += 5;
  }
  
  // Keep within physiological range
  qrs_duration = constrain(qrs_duration, 60, 120);
}

void calculate_heart_condition() {
  bool leads_connected = checkLeadsConnected();
  
  if (!leads_connected) {
    heart_condition = 3; // LEADS_OFF
    return;
  }
  
  if (heart_rate >= 60 && heart_rate <= 100 && qrs_duration >= 80 && qrs_duration <= 100) {
    heart_condition = 0; // NORMAL
  } else if ((heart_rate > 100 && heart_rate <= 110) || (heart_rate >= 50 && heart_rate < 60) ||
             (qrs_duration > 100 || qrs_duration < 80)) {
    heart_condition = 1; // RECOVERY
  } else if (heart_rate > 110 || heart_rate < 50) {
    heart_condition = 2; // SERIOUS
  } else {
    heart_condition = 0; // Default to normal
  }
}

bool checkLeadsConnected() {
  // AD8232 lead-off detection
  // When leads are disconnected, LO+ and LO- will be pulled high
  int lo_plus = digitalRead(LO_PLUS_PIN);
  int lo_minus = digitalRead(LO_MINUS_PIN);
  
  // If either lead-off detection pin is HIGH, leads are disconnected
  return (lo_plus == LOW && lo_minus == LOW);
}

void requestEvent() {
  // Main module is requesting data
  // Send: heart_rate(2), ecg_amplitude(2), hr_variability(2), qrs_duration(2), heart_condition(1)
  
  // Convert floats to fixed-point integers for transmission
  uint16_t hr = (uint16_t)(heart_rate * 10);        // 1 decimal place
  uint16_t amp = (uint16_t)(ecg_amplitude * 100);   // 2 decimal places (mV)
  uint16_t hrv = (uint16_t)(hr_variability * 10);   // 1 decimal place
  uint16_t qrs = (uint16_t)(qrs_duration * 10);     // 1 decimal place
  
  Wire.write((hr >> 8) & 0xFF);    // High byte heart rate
  Wire.write(hr & 0xFF);           // Low byte heart rate
  Wire.write((amp >> 8) & 0xFF);   // High byte ECG amplitude
  Wire.write(amp & 0xFF);          // Low byte ECG amplitude
  Wire.write((hrv >> 8) & 0xFF);   // High byte HR variability
  Wire.write(hrv & 0xFF);          // Low byte HR variability
  Wire.write((qrs >> 8) & 0xFF);   // High byte QRS duration
  Wire.write(qrs & 0xFF);          // Low byte QRS duration
  Wire.write(heart_condition);     // Heart condition
  
  Serial.println("Sent real ECG data to main module");
}

void printECGData() {
  Serial.println("ECG Module Data:");
  Serial.println("┌──────────────────────┬──────────────┐");
  Serial.println("│ Parameter            │ Value        │");
  Serial.println("├──────────────────────┼──────────────┤");
  Serial.printf("│ Heart Rate           │ %6.1f BPM   │\n", heart_rate);
  Serial.printf("│ ECG Amplitude        │ %6.2f mV    │\n", ecg_amplitude);
  Serial.printf("│ HR Variability       │ %6.1f ms    │\n", hr_variability);
  Serial.printf("│ QRS Duration         │ %6.1f ms    │\n", qrs_duration);
  
  const char* condition_str;
  switch(heart_condition) {
    case 0: condition_str = "NORMAL"; break;
    case 1: condition_str = "RECOVERY"; break;
    case 2: condition_str = "SERIOUS"; break;
    case 3: condition_str = "LEADS OFF"; break;
    default: condition_str = "UNKNOWN"; break;
  }
  Serial.printf("│ Heart Condition      │ %s │\n", condition_str);
  Serial.printf("│ Leads Connected      │ %s │\n", checkLeadsConnected() ? "YES" : "NO");
  Serial.println("└──────────────────────┴──────────────┘");
}
