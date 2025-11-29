# train_model.py - Complete ML Training for ICU Monitoring
import pandas as pd
import numpy as np
import tensorflow as tf
from sklearn.model_selection import train_test_split
from sklearn.preprocessing import StandardScaler
from sklearn.metrics import classification_report, confusion_matrix
import joblib
import matplotlib.pyplot as plt
import os

print("ICU Health Monitoring - ML Model Training")
print("=" * 50)

# Step 1: Load your data
print("\nStep 1: Loading your CSV data...")
try:
    df = pd.read_csv('icu_patient_dataset.csv')
    print(f"Successfully loaded {len(df)} records")
except FileNotFoundError:
    print("ERROR: CSV file not found!")
    print("   Please make sure 'icu_patient_dataset.csv' is in the same folder")
    print("   Available files in current folder:")
    for file in os.listdir('.'):
        print(f"   - {file}")
    exit()

# Display dataset info
print(f"Dataset shape: {df.shape[0]} rows, {df.shape[1]} columns")
print("Columns:", df.columns.tolist())

# Check for required columns
required_columns = ['overall_health_status']
missing_columns = [col for col in required_columns if col not in df.columns]
if missing_columns:
    print(f"Missing required columns: {missing_columns}")
    exit()

print("\nStep 2: Analyzing target variable...")
status_counts = df['overall_health_status'].value_counts().sort_index()
status_names = {0: 'Normal', 1: 'Recovery', 2: 'Serious'}

print("Health Status Distribution:")
for status, count in status_counts.items():
    name = status_names.get(status, f'Unknown({status})')
    print(f"   {name} ({status}): {count} records")

# Step 3: Select features
print("\nStep 3: Selecting features...")
feature_columns = [
    'flow_inlet_ml_min', 'flow_outlet_ml_min', 'fluid_balance_ml',
    'heart_rate_bpm', 'ecg_amplitude_mv', 'hr_variability', 'qrs_duration_ms',
    'spo2_percentage', 'pulse_rate_bpm', 'body_temperature'
]

# Use only available features
available_features = [col for col in feature_columns if col in df.columns]
if len(available_features) < 5:
    print("Not enough features available!")
    print(f"   Available: {available_features}")
    exit()

print(f"Using {len(available_features)} features:")
for feature in available_features:
    print(f"   - {feature}")

# Prepare data
X = df[available_features]
y = df['overall_health_status']

print(f"Features shape: {X.shape}")
print(f"Target shape: {y.shape}")

# Step 4: Split data
print("\nStep 4: Splitting data into train/test sets...")
X_train, X_test, y_train, y_test = train_test_split(
    X, y, test_size=0.2, random_state=42, stratify=y
)

print(f"Training set: {X_train.shape[0]} records")
print(f"Testing set: {X_test.shape[0]} records")

# Step 5: Scale features
print("\nStep 5: Scaling features...")
scaler = StandardScaler()
X_train_scaled = scaler.fit_transform(X_train)
X_test_scaled = scaler.transform(X_test)

print("Features scaled successfully")

# Step 6: Build model
print("\nStep 6: Building neural network...")

def create_simple_model(input_dim):
    model = tf.keras.Sequential([
        tf.keras.layers.Dense(32, activation='relu', input_shape=(input_dim,)),
        tf.keras.layers.Dropout(0.3),
        
        tf.keras.layers.Dense(16, activation='relu'),
        tf.keras.layers.Dropout(0.3),
        
        tf.keras.layers.Dense(8, activation='relu'),
        tf.keras.layers.Dropout(0.2),
        
        tf.keras.layers.Dense(3, activation='softmax')
    ])
    
    model.compile(
        optimizer='adam',
        loss='sparse_categorical_crossentropy',
        metrics=['accuracy']
    )
    
    return model

model = create_simple_model(X_train_scaled.shape[1])
print("Model created successfully!")

# Display model architecture
print("\nModel Architecture:")
model.summary()

# Step 7: Train model
print("\nStep 7: Training model...")
print("This may take 1-2 minutes...")

# Simple training without callbacks for beginners
history = model.fit(
    X_train_scaled, y_train,
    epochs=50,
    batch_size=16,
    validation_split=0.2,
    verbose=1
)

print("Training completed!")

# Step 8: Evaluate model
print("\nStep 8: Evaluating model performance...")
test_loss, test_accuracy = model.evaluate(X_test_scaled, y_test, verbose=0)
print(f"Test Accuracy: {test_accuracy:.4f} ({test_accuracy*100:.2f}%)")
print(f"Test Loss: {test_loss:.4f}")

# Predictions
y_pred = model.predict(X_test_scaled)
y_pred_classes = np.argmax(y_pred, axis=1)

print("\nDetailed Classification Report:")
print(classification_report(y_test, y_pred_classes, 
                          target_names=['Normal', 'Recovery', 'Serious']))

# Step 9: Save everything
print("\nStep 9: Saving model and artifacts...")

# Save model
model.save('icu_health_model.h5')
print("Model saved as 'icu_health_model.h5'")

# Save scaler
joblib.dump(scaler, 'scaler.pkl')
print("Scaler saved as 'scaler.pkl'")

# Save feature list
joblib.dump(available_features, 'feature_columns.pkl')
print("Feature columns saved as 'feature_columns.pkl'")

# Save feature statistics for ESP32
feature_stats = {
    'means': scaler.mean_.tolist(),
    'stds': scaler.scale_.tolist(),
    'feature_names': available_features
}
joblib.dump(feature_stats, 'feature_stats.pkl')
print("Feature statistics saved as 'feature_stats.pkl'")

# Create training plot
print("\nCreating training history plot...")
plt.figure(figsize=(10, 4))

plt.subplot(1, 2, 1)
plt.plot(history.history['accuracy'], label='Training Accuracy')
plt.plot(history.history['val_accuracy'], label='Validation Accuracy')
plt.title('Model Accuracy')
plt.ylabel('Accuracy')
plt.xlabel('Epoch')
plt.legend()

plt.subplot(1, 2, 2)
plt.plot(history.history['loss'], label='Training Loss')
plt.plot(history.history['val_loss'], label='Validation Loss')
plt.title('Model Loss')
plt.ylabel('Loss')
plt.xlabel('Epoch')
plt.legend()

plt.tight_layout()
plt.savefig('training_results.png', dpi=300, bbox_inches='tight')
print("Training plot saved as 'training_results.png'")

# Step 10: Test with samples
print("\nStep 10: Testing with sample data...")
print("Sample predictions (first 5 records):")

for i in range(min(5, len(X_test))):
    sample_data = X_test.iloc[i:i+1]
    sample_scaled = scaler.transform(sample_data)
    prediction = model.predict(sample_scaled, verbose=0)
    predicted_class = np.argmax(prediction, axis=1)[0]
    actual_class = y_test.iloc[i]
    
    match = "Match" if predicted_class == actual_class else "Mismatch"
    print(f"   Sample {i+1}: Predicted {status_names[predicted_class]} ({predicted_class}) "
          f"- Actual {status_names[actual_class]} ({actual_class}) {match}")

# Final summary
print("\n" + "="*50)
print("TRAINING COMPLETED SUCCESSFULLY!")
print("="*50)
print("\nGenerated Files:")
print("   icu_health_model.h5 - Main model file")
print("   scaler.pkl - Feature scaler")
print("   feature_columns.pkl - Feature names")
print("   feature_stats.pkl - Statistics")
print("   training_results.png - Training history plot")

print(f"\nFinal Model Accuracy: {test_accuracy*100:.2f}%")

print("\nNext step: Run 'convert_model.py' to prepare for ESP32")
print("   Command: python convert_model.py")
