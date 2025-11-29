# convert_model.py - Convert model for ESP32
import tensorflow as tf
import numpy as np
import joblib
import pandas as pd
import os

print("Converting Model for ESP32")
print("=" * 40)

def convert_to_tflite():
    print("\nStep 1: Loading trained model...")
    
    if not os.path.exists('icu_health_model.h5'):
        print("Error: No trained model found!")
        print("   Run 'python train_model.py' first")
        return None
    
    model = tf.keras.models.load_model('icu_health_model.h5')
    print("Model loaded successfully")
    
    print("\nStep 2: Converting to TensorFlow Lite...")
    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    
    # ESP32 compatibility
    converter.target_spec.supported_ops = [
        tf.lite.OpsSet.TFLITE_BUILTINS,
        tf.lite.OpsSet.SELECT_TF_OPS
    ]
    
    tflite_model = converter.convert()
    
    # Save TFLite model
    with open('icu_health_model.tflite', 'wb') as f:
        f.write(tflite_model)
    
    print(f"TensorFlow Lite model saved! Size: {len(tflite_model)} bytes")
    return tflite_model

def convert_to_c_array(tflite_model):
    print("\nStep 3: Converting to C array for ESP32...")
    
    # Convert to hex array
    hex_array = []
    for byte in tflite_model:
        hex_array.append(f"0x{byte:02x}")
    
    # Create C header file
    c_code = "// ICU Health Monitoring Model - Auto-generated\n"
    c_code += "// TensorFlow Lite model for ESP32\n\n"
    c_code += "#ifndef MODEL_DATA_H\n#define MODEL_DATA_H\n\n"
    c_code += "const unsigned char model_data[] = {\n"
    
    # Format with 12 bytes per line
    for i in range(0, len(hex_array), 12):
        c_code += "    " + ", ".join(hex_array[i:i+12]) + ",\n"
    
    c_code = c_code.rstrip(",\n") + "\n};\n\n"
    c_code += f"const int model_data_len = {len(tflite_model)};\n\n"
    c_code += "#endif // MODEL_DATA_H\n"
    
    with open('model_data.h', 'w') as f:
        f.write(c_code)
    
    print(f"C header file created: {len(hex_array)} bytes")
    print("   File: model_data.h")

def generate_arduino_constants():
    print("\nStep 4: Generating Arduino constants...")
    
    if not os.path.exists('feature_stats.pkl'):
        print("Feature statistics not found!")
        return
    
    feature_stats = joblib.load('feature_stats.pkl')
    
    arduino_code = "// Feature scaling constants for ESP32\n"
    arduino_code += "// Auto-generated from Python training\n\n"
    arduino_code += "#ifndef FEATURE_CONSTANTS_H\n#define FEATURE_CONSTANTS_H\n\n"
    
    # Feature means
    arduino_code += "const float feature_means[] = {\n    "
    arduino_code += ", ".join([f"{mean:.6f}f" for mean in feature_stats['means']])
    arduino_code += "\n};\n\n"
    
    # Feature standard deviations
    arduino_code += "const float feature_stds[] = {\n    "
    arduino_code += ", ".join([f"{std:.6f}f" for std in feature_stats['stds']])
    arduino_code += "\n};\n\n"
    
    # Feature count
    arduino_code += f"const int num_features = {len(feature_stats['feature_names'])};\n\n"
    arduino_code += "#endif // FEATURE_CONSTANTS_H\n"
    
    with open('feature_constants.h', 'w') as f:
        f.write(arduino_code)
    
    print("Arduino constants saved: feature_constants.h")

def test_conversion():
    print("\nStep 5: Testing converted model...")
    
    # Load test data
    df = pd.read_csv('icu_patient_dataset.csv')
    feature_columns = joblib.load('feature_columns.pkl')
    scaler = joblib.load('scaler.pkl')
    
    # Test with 2 samples
    X_test = df[feature_columns].iloc[:2]
    X_test_scaled = scaler.transform(X_test)
    
    # Load TFLite model
    interpreter = tf.lite.Interpreter(model_path='icu_health_model.tflite')
    interpreter.allocate_tensors()
    
    input_details = interpreter.get_input_details()
    output_details = interpreter.get_output_details()
    
    print("Testing TFLite model:")
    for i in range(len(X_test_scaled)):
        input_data = X_test_scaled[i:i+1].astype(np.float32)
        interpreter.set_tensor(input_details[0]['index'], input_data)
        interpreter.invoke()
        output_data = interpreter.get_tensor(output_details[0]['index'])
        prediction = np.argmax(output_data, axis=1)[0]
        actual = df['overall_health_status'].iloc[i]
        
        status_names = {0: 'Normal', 1: 'Recovery', 2: 'Serious'}
        match = "Match" if prediction == actual else "Mismatch"
        print(f"   Sample {i+1}: {status_names[prediction]} vs {status_names[actual]} {match}")

if __name__ == "__main__":
    print("Starting model conversion process...")
    
    # Convert to TFLite
    tflite_model = convert_to_tflite()
    if tflite_model is None:
        exit()
    
    # Convert to C array
    convert_to_c_array(tflite_model)
    
    # Generate Arduino constants
    generate_arduino_constants()
    
    # Test conversion
    test_conversion()
    
    print("\n" + "="*40)
    print("CONVERSION COMPLETED!")
    print("="*40)
    print("\nFiles for ESP32:")
    print("   model_data.h - Model as C array")
    print("   feature_constants.h - Scaling constants")
    print("   icu_health_model.tflite - TensorFlow Lite model")
    
    print("\nNext: Copy these files to your ESP32 Arduino project")
