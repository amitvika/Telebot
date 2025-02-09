#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_BusIO_Register.h>
#include <Adafruit_Sensor.h>
#include <esp_now.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
  // Removed Software I2C library include
#include <ESP32Servo.h>
#include <Wire.h>
#include <Wire.h>
#include <ESP32Servo.h>

// *******************************
// Global Variables and Constants
// *******************************

// Flight controller parameters
volatile float RatePitch, RateRoll, RateYaw;
float RateCalibrationPitch, RateCalibrationRoll, RateCalibrationYaw;
float AccXCalibration, AccYCalibration, AccZCalibration;

int ESCfreq = 500;      // ESC PWM frequency in Hz
float PAngleRoll = 2;   
float PAnglePitch = PAngleRoll;
float IAngleRoll = 0.5; 
float IAnglePitch = IAngleRoll;
float DAngleRoll = 0.007; 
float DAnglePitch = DAngleRoll;

float PRateRoll = 0.625;
float IRateRoll = 2.1;
float DRateRoll = 0.0088;
float PRatePitch = PRateRoll;
float IRatePitch = IRateRoll;
float DRatePitch = DRateRoll;
float PRateYaw = 4;
float IRateYaw = 3;
float DRateYaw = 0;

uint32_t LoopTimer;
float t = 0.004;    // Loop time in seconds (4ms)

Servo mot1, mot2, mot3, mot4;
const int mot1_pin = 13;
const int mot2_pin = 12;
const int mot3_pin = 14; // For some board designs (e.g., perforated boards)
const int mot4_pin = 27;

// Receiver channel variables and pins
volatile uint32_t current_time;
volatile uint32_t last_channel_1 = 0, last_channel_2 = 0, last_channel_3 = 0;
volatile uint32_t last_channel_4 = 0, last_channel_5 = 0, last_channel_6 = 0;
volatile uint32_t timer_1, timer_2, timer_3, timer_4, timer_5, timer_6;
volatile int ReceiverValue[6];
const int channel_1_pin = 34;
const int channel_2_pin = 35;
const int channel_3_pin = 32;
const int channel_4_pin = 33;
const int channel_5_pin = 25;
const int channel_6_pin = 26;

// PID variables for rate and angle control
volatile float PtermRoll, ItermRoll, DtermRoll, PIDOutputRoll;
volatile float PtermPitch, ItermPitch, DtermPitch, PIDOutputPitch;
volatile float PtermYaw, ItermYaw, DtermYaw, PIDOutputYaw;
volatile float DesiredRateRoll, DesiredRatePitch, DesiredRateYaw;
volatile float ErrorRateRoll, ErrorRatePitch, ErrorRateYaw;
volatile float InputRoll, InputThrottle, InputPitch, InputYaw;
volatile float PrevErrorRateRoll, PrevErrorRatePitch, PrevErrorRateYaw;
volatile float PrevItermRateRoll, PrevItermRatePitch, PrevItermRateYaw;
volatile float PIDReturn[] = {0, 0, 0};

// PID variables for the outer (angle) loop
volatile float DesiredAngleRoll, DesiredAnglePitch;
volatile float ErrorAngleRoll, ErrorAnglePitch;
volatile float PrevErrorAngleRoll, PrevErrorAnglePitch;
volatile float PrevItermAngleRoll, PrevItermAnglePitch;

// Filtering variables
volatile float AccX, AccY, AccZ;
volatile float AngleRoll, AnglePitch;
float complementaryAngleRoll = 0.0f;
float complementaryAnglePitch = 0.0f;

// Motor control outputs
volatile float MotorInput1, MotorInput2, MotorInput3, MotorInput4;

// Magnetometer (AK8963) readings
volatile float MagX, MagY, MagZ;

// Throttle limits
int ThrottleIdle = 1170;
int ThrottleCutOff = 1000;

// MPU9250 and magnetometer addresses
#define MPU9250_ADDRESS 0x68
#define AK8963_ADDRESS  0x0C

// *************************************
// Interrupt Service Routine for Receiver
// *************************************
void channelInterruptHandler() {
  current_time = micros();
  if (digitalRead(channel_1_pin)) {
    if (last_channel_1 == 0) { last_channel_1 = 1; timer_1 = current_time; }
  } else if (last_channel_1 == 1) {
    last_channel_1 = 0; ReceiverValue[0] = current_time - timer_1;
  }
  
  if (digitalRead(channel_2_pin)) {
    if (last_channel_2 == 0) { last_channel_2 = 1; timer_2 = current_time; }
  } else if (last_channel_2 == 1) {
    last_channel_2 = 0; ReceiverValue[1] = current_time - timer_2;
  }
  
  if (digitalRead(channel_3_pin)) {
    if (last_channel_3 == 0) { last_channel_3 = 1; timer_3 = current_time; }
  } else if (last_channel_3 == 1) {
    last_channel_3 = 0; ReceiverValue[2] = current_time - timer_3;
  }
  
  if (digitalRead(channel_4_pin)) {
    if (last_channel_4 == 0) { last_channel_4 = 1; timer_4 = current_time; }
  } else if (last_channel_4 == 1) {
    last_channel_4 = 0; ReceiverValue[3] = current_time - timer_4;
  }
  
  if (digitalRead(channel_5_pin)) {
    if (last_channel_5 == 0) { last_channel_5 = 1; timer_5 = current_time; }
  } else if (last_channel_5 == 1) {
    last_channel_5 = 0; ReceiverValue[4] = current_time - timer_5;
  }
  
  if (digitalRead(channel_6_pin)) {
    if (last_channel_6 == 0) { last_channel_6 = 1; timer_6 = current_time; }
  } else if (last_channel_6 == 1) {
    last_channel_6 = 0; ReceiverValue[5] = current_time - timer_6;
  }
}

// ******************************
// A Generic PID Equation Helper
// ******************************
void pid_equation(float Error, float P, float I, float D, float PrevError, float PrevIterm) {
  float Pterm = P * Error;
  float Iterm = PrevIterm + (I * (Error + PrevError) * (t / 2));
  if (Iterm > 400) {
    Iterm = 400;
  } else if (Iterm < -400) {
    Iterm = -400;
  }
  float Dterm = D * ((Error - PrevError) / t);
  float PIDOutput = Pterm + Iterm + Dterm;
  if (PIDOutput > 400) {
    PIDOutput = 400;
  } else if (PIDOutput < -400) {
    PIDOutput = -400;
  }
  PIDReturn[0] = PIDOutput;
  PIDReturn[1] = Error;
  PIDReturn[2] = Iterm;
}

// ****************
// Setup Function
// ****************
void setup() {
  Serial.begin(115200);
  
  // Blink LED on pin 15 as a startup indicator
  int led_time = 100;
  pinMode(15, OUTPUT);
  digitalWrite(15, LOW);
  delay(led_time);
  digitalWrite(15, HIGH);
  delay(led_time);
  digitalWrite(15, LOW);
  delay(led_time);
  digitalWrite(15, HIGH);
  delay(led_time);
  digitalWrite(15, LOW);
  delay(led_time);
  digitalWrite(15, HIGH);
  delay(led_time);
  digitalWrite(15, LOW);
  delay(led_time);
  digitalWrite(15, HIGH);
  delay(led_time);
  digitalWrite(15, LOW);
  delay(led_time);
  
  // Set up receiver pins with internal pull-ups
  pinMode(channel_1_pin, INPUT_PULLUP);
  pinMode(channel_2_pin, INPUT_PULLUP);
  pinMode(channel_3_pin, INPUT_PULLUP);
  pinMode(channel_4_pin, INPUT_PULLUP);
  pinMode(channel_5_pin, INPUT_PULLUP);
  pinMode(channel_6_pin, INPUT_PULLUP);

  // Attach interrupts for each receiver channel
  attachInterrupt(digitalPinToInterrupt(channel_1_pin), channelInterruptHandler, CHANGE);
  attachInterrupt(digitalPinToInterrupt(channel_2_pin), channelInterruptHandler, CHANGE);
  attachInterrupt(digitalPinToInterrupt(channel_3_pin), channelInterruptHandler, CHANGE);
  attachInterrupt(digitalPinToInterrupt(channel_4_pin), channelInterruptHandler, CHANGE);
  attachInterrupt(digitalPinToInterrupt(channel_5_pin), channelInterruptHandler, CHANGE);
  attachInterrupt(digitalPinToInterrupt(channel_6_pin), channelInterruptHandler, CHANGE);
  delay(100);
  
  // Initialize hardware I²C with SDA on pin 2 and SCL on pin 15
  Wire.begin(2, 15);
  Wire.setClock(400000);
  delay(250);
  
  // Initialize MPU9250 (wake up and set configurations)
  Wire.beginTransmission(MPU9250_ADDRESS);
  Wire.write(0x6B);         // Power management register
  Wire.write(0x00);         // Wake up MPU9250
  Wire.endTransmission();
  delay(100);
  
  // Enable bypass mode to access the AK8963 magnetometer directly
  Wire.beginTransmission(MPU9250_ADDRESS);
  Wire.write(0x37);         // INT_PIN_CFG register
  Wire.write(0x02);         // Enable bypass mode
  Wire.endTransmission();
  delay(100);
  
  // Initialize the AK8963 magnetometer
  Wire.beginTransmission(AK8963_ADDRESS);
  Wire.write(0x0A);         // CNTL1 register of AK8963
  Wire.write(0x16);         // 16-bit output, continuous measurement mode 2
  Wire.endTransmission();
  delay(100);
  
  // Optionally configure gyro full-scale range (e.g., 500 deg/s)
  Wire.beginTransmission(MPU9250_ADDRESS);
  Wire.write(0x1B);         // Gyro configuration register
  Wire.write(0x08);         // Set desired range (adjust as needed)
  Wire.endTransmission();
  delay(100);
  
  // Optionally configure accelerometer full-scale range (e.g., ±8g)
  Wire.beginTransmission(MPU9250_ADDRESS);
  Wire.write(0x1C);         // Accelerometer configuration register
  Wire.write(0x10);         // Set desired range (adjust as needed)
  Wire.endTransmission();
  delay(100);
  
  // Allocate PWM timers for motor control
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  delay(1000);
  
  // Attach servos to ESC pins and set the PWM period
  mot1.attach(mot1_pin, 1000, 2000);
  delay(1000);
  mot1.setPeriodHertz(ESCfreq);
  delay(100);
  mot2.attach(mot2_pin, 1000, 2000);
  delay(1000);
  mot2.setPeriodHertz(ESCfreq);
  delay(100);
  mot3.attach(mot3_pin, 1000, 2000);
  delay(1000);
  mot3.setPeriodHertz(ESCfreq);
  delay(100);
  mot4.attach(mot4_pin, 1000, 2000);
  delay(1000);
  mot4.setPeriodHertz(ESCfreq);
  delay(100);
  
  // Send initial (idle) signals to motors
  mot1.writeMicroseconds(1000);
  mot2.writeMicroseconds(1000);
  mot3.writeMicroseconds(1000);
  mot4.writeMicroseconds(1000);
  delay(500);
  digitalWrite(15, LOW);
  digitalWrite(15, HIGH);
  delay(500);
  digitalWrite(15, LOW);
  delay(500);
  
  // Set calibration offsets (tune these for your sensor)
  RateCalibrationRoll = 0.27;
  RateCalibrationPitch = -0.85;
  RateCalibrationYaw = -2.09;
  AccXCalibration = 0.03;
  AccYCalibration = 0.01;
  AccZCalibration = -0.07;
  
  LoopTimer = micros();
}

// ****************
// Main Loop
// ****************
void loop() {
  // --- Read Accelerometer and Gyro Data from MPU9250 ---
  Wire.beginTransmission(MPU9250_ADDRESS);
  Wire.write(0x3B); // Starting register for accelerometer data
  Wire.endTransmission(); 
  Wire.requestFrom(MPU9250_ADDRESS, 6);
  int16_t AccXLSB = (Wire.read() << 8) | Wire.read();
  int16_t AccYLSB = (Wire.read() << 8) | Wire.read();
  int16_t AccZLSB = (Wire.read() << 8) | Wire.read();
  
  Wire.beginTransmission(MPU9250_ADDRESS);
  Wire.write(0x43); // Starting register for gyro data
  Wire.endTransmission();
  Wire.requestFrom(MPU9250_ADDRESS, 6);
  int16_t GyroX = (Wire.read() << 8) | Wire.read();
  int16_t GyroY = (Wire.read() << 8) | Wire.read();
  int16_t GyroZ = (Wire.read() << 8) | Wire.read();
  
  RateRoll  = (float)GyroX / 65.5;
  RatePitch = (float)GyroY / 65.5;
  RateYaw   = (float)GyroZ / 65.5;
  AccX = (float)AccXLSB / 4096;
  AccY = (float)AccYLSB / 4096;
  AccZ = (float)AccZLSB / 4096;
  
  // --- Read Magnetometer Data from AK8963 ---
  Wire.beginTransmission(AK8963_ADDRESS);
  Wire.write(0x03); // Starting register for magnetometer data
  Wire.endTransmission();
  Wire.requestFrom(AK8963_ADDRESS, 7);
  if (Wire.available() == 7) {
    int16_t MagXLSB = Wire.read() | (Wire.read() << 8);
    int16_t MagYLSB = Wire.read() | (Wire.read() << 8);
    int16_t MagZLSB = Wire.read() | (Wire.read() << 8);
    uint8_t MagStatus = Wire.read(); // Status (not used here)
    MagX = (float)MagXLSB;  // Apply sensitivity adjustment as needed
    MagY = (float)MagYLSB;
    MagZ = (float)MagZLSB;
  }
  
  // --- Apply Calibration Offsets ---
  RateRoll  -= RateCalibrationRoll;
  RatePitch -= RateCalibrationPitch;
  RateYaw   -= RateCalibrationYaw;
  
  AccX -= AccXCalibration;
  AccY -= AccYCalibration;
  AccZ -= AccZCalibration;
  
  // --- Compute Angles from Accelerometer Data ---
  AngleRoll  = atan(AccY / sqrt(AccX * AccX + AccZ * AccZ)) * 57.29;
  AnglePitch = -atan(AccX / sqrt(AccY * AccY + AccZ * AccZ)) * 57.29;
  
  // --- Complementary Filter for Angle Estimation ---
  complementaryAngleRoll  = 0.991 * (complementaryAngleRoll + RateRoll * t) + 0.009 * AngleRoll;
  complementaryAnglePitch = 0.991 * (complementaryAnglePitch + RatePitch * t) + 0.009 * AnglePitch;
  complementaryAngleRoll  = (complementaryAngleRoll > 20) ? 20 : ((complementaryAngleRoll < -20) ? -20 : complementaryAngleRoll);
  complementaryAnglePitch = (complementaryAnglePitch > 20) ? 20 : ((complementaryAnglePitch < -20) ? -20 : complementaryAnglePitch);
  
  // --- Process Receiver Signals ---
  DesiredAngleRoll  = 0.1 * (ReceiverValue[0] - 1500);
  DesiredAnglePitch = 0.1 * (ReceiverValue[1] - 1500);
  InputThrottle     = ReceiverValue[2];
  DesiredRateYaw    = 0.15 * (ReceiverValue[3] - 1500);
  
  // --- Outer (Angle) Loop PID for Roll ---
  ErrorAngleRoll = DesiredAngleRoll - complementaryAngleRoll;
  PtermRoll = PAngleRoll * ErrorAngleRoll;
  ItermRoll = PrevItermAngleRoll + (IAngleRoll * (ErrorAngleRoll + PrevErrorAngleRoll) * (t / 2));
  ItermRoll = (ItermRoll > 400) ? 400 : ((ItermRoll < -400) ? -400 : ItermRoll);
  DtermRoll = DAngleRoll * ((ErrorAngleRoll - PrevErrorAngleRoll) / t);
  PIDOutputRoll = PtermRoll + ItermRoll + DtermRoll;
  PIDOutputRoll = (PIDOutputRoll > 400) ? 400 : ((PIDOutputRoll < -400) ? -400 : PIDOutputRoll);
  DesiredRateRoll = PIDOutputRoll;
  PrevErrorAngleRoll = ErrorAngleRoll;
  PrevItermAngleRoll = ItermRoll;
  
  // --- Outer (Angle) Loop PID for Pitch ---
  ErrorAnglePitch = DesiredAnglePitch - complementaryAnglePitch;
  PtermPitch = PAnglePitch * ErrorAnglePitch;
  ItermPitch = PrevItermAnglePitch + (IAnglePitch * (ErrorAnglePitch + PrevErrorAnglePitch) * (t / 2));
  ItermPitch = (ItermPitch > 400) ? 400 : ((ItermPitch < -400) ? -400 : ItermPitch);
  DtermPitch = DAnglePitch * ((ErrorAnglePitch - PrevErrorAnglePitch) / t);
  PIDOutputPitch = PtermPitch + ItermPitch + DtermPitch;
  PIDOutputPitch = (PIDOutputPitch > 400) ? 400 : ((PIDOutputPitch < -400) ? -400 : PIDOutputPitch);
  DesiredRatePitch = PIDOutputPitch;
  PrevErrorAnglePitch = ErrorAnglePitch;
  PrevItermAnglePitch = ItermPitch;
  
  // --- Inner (Rate) Loop PID for Roll ---
  ErrorRateRoll = DesiredRateRoll - RateRoll;
  PtermRoll = PRateRoll * ErrorRateRoll;
  ItermRoll = PrevItermRateRoll + (IRateRoll * (ErrorRateRoll + PrevErrorRateRoll) * (t / 2));
  ItermRoll = (ItermRoll > 400) ? 400 : ((ItermRoll < -400) ? -400 : ItermRoll);
  DtermRoll = DRateRoll * ((ErrorRateRoll - PrevErrorRateRoll) / t);
  PIDOutputRoll = PtermRoll + ItermRoll + DtermRoll;
  PIDOutputRoll = (PIDOutputRoll > 400) ? 400 : ((PIDOutputRoll < -400) ? -400 : PIDOutputRoll);
  InputRoll = PIDOutputRoll;
  PrevErrorRateRoll = ErrorRateRoll;
  PrevItermRateRoll = ItermRoll;
  
  // --- Inner (Rate) Loop PID for Pitch ---
  ErrorRatePitch = DesiredRatePitch - RatePitch;
  PtermPitch = PRatePitch * ErrorRatePitch;
  ItermPitch = PrevItermRatePitch + (IRatePitch * (ErrorRatePitch + PrevErrorRatePitch) * (t / 2));
  ItermPitch = (ItermPitch > 400) ? 400 : ((ItermPitch < -400) ? -400 : ItermPitch);
  DtermPitch = DRatePitch * ((ErrorRatePitch - PrevErrorRatePitch) / t);
  PIDOutputPitch = PtermPitch + ItermPitch + DtermPitch;
  PIDOutputPitch = (PIDOutputPitch > 400) ? 400 : ((PIDOutputPitch < -400) ? -400 : PIDOutputPitch);
  InputPitch = PIDOutputPitch;
  PrevErrorRatePitch = ErrorRatePitch;
  PrevItermRatePitch = ItermPitch;
  
  // --- Inner (Rate) Loop PID for Yaw ---
  ErrorRateYaw = DesiredRateYaw - RateYaw;
  PtermYaw = PRateYaw * ErrorRateYaw;
  ItermYaw = PrevItermRateYaw + (IRateYaw * (ErrorRateYaw + PrevErrorRateYaw) * (t / 2));
  ItermYaw = (ItermYaw > 400) ? 400 : ((ItermYaw < -400) ? -400 : ItermYaw);
  DtermYaw = DRateYaw * ((ErrorRateYaw - PrevErrorRateYaw) / t);
  PIDOutputYaw = PtermYaw + ItermYaw + DtermYaw;
  PIDOutputYaw = (PIDOutputYaw > 400) ? 400 : ((PIDOutputYaw < -400) ? -400 : PIDOutputYaw);
  InputYaw = PIDOutputYaw;
  PrevErrorRateYaw = ErrorRateYaw;
  PrevItermRateYaw = ItermYaw;
  
  // --- Limit Throttle ---
  if (InputThrottle > 1800) {
    InputThrottle = 1800;
  }
  
  // --- Motor Mixing ---
  MotorInput1 = (InputThrottle - InputRoll - InputPitch - InputYaw); // Front right (counter clockwise)
  MotorInput2 = (InputThrottle - InputRoll + InputPitch + InputYaw); // Rear right (clockwise)
  MotorInput3 = (InputThrottle + InputRoll + InputPitch - InputYaw); // Rear left (counter clockwise)
  MotorInput4 = (InputThrottle + InputRoll - InputPitch + InputYaw); // Front left (clockwise)
  
  if (MotorInput1 > 2000) { MotorInput1 = 1999; }
  if (MotorInput2 > 2000) { MotorInput2 = 1999; }
  if (MotorInput3 > 2000) { MotorInput3 = 1999; }
  if (MotorInput4 > 2000) { MotorInput4 = 1999; }
  
  if (MotorInput1 < ThrottleIdle) { MotorInput1 = ThrottleIdle; }
  if (MotorInput2 < ThrottleIdle) { MotorInput2 = ThrottleIdle; }
  if (MotorInput3 < ThrottleIdle) { MotorInput3 = ThrottleIdle; }
  if (MotorInput4 < ThrottleIdle) { MotorInput4 = ThrottleIdle; }
  
  if (ReceiverValue[2] < 1030) { // Do not arm the motors if throttle is too low
    MotorInput1 = ThrottleCutOff;
    MotorInput2 = ThrottleCutOff;
    MotorInput3 = ThrottleCutOff;
    MotorInput4 = ThrottleCutOff;
    
    PrevErrorRateRoll = 0; PrevErrorRatePitch = 0; PrevErrorRateYaw = 0;
    PrevItermRateRoll = 0; PrevItermRatePitch = 0; PrevItermRateYaw = 0;
    PrevErrorAngleRoll = 0; PrevErrorAnglePitch = 0;
    PrevItermAngleRoll = 0; PrevItermAnglePitch = 0;
  }
  
  // --- Send PWM Signals to ESCs ---
  mot1.writeMicroseconds(MotorInput1);
  mot2.writeMicroseconds(MotorInput2);
  mot3.writeMicroseconds(MotorInput3);
  mot4.writeMicroseconds(MotorInput4);
  
 
  
  Serial.print("PID Outputs: ");
  Serial.print("Roll: "); Serial.print(InputRoll);
  Serial.print("  Pitch: "); Serial.print(InputPitch);
  Serial.print("  Yaw: "); Serial.println(InputYaw);
  
  Serial.print("Motor Inputs (us): ");
  Serial.print("M1: "); Serial.print(MotorInput1);
  Serial.print("  M2: "); Serial.print(MotorInput2);
  Serial.print("  M3: "); Serial.print(MotorInput3);
  Serial.print("  M4: "); Serial.println(MotorInput4);
  
  Serial.println("---------------------------------------------------");
  
  // --- Maintain Fixed Loop Time (4ms) ---
  while (micros() - LoopTimer < (t * 1000000)) {
    // Wait until 4ms have elapsed
  }
  LoopTimer = micros();
}
