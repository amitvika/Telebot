#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <WiFi.h>
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <math.h>


/********************************************************
   Final Single ESP-NOW Slave Code
   - Receives text lines like:
       hand1X=123.4;hand1Y=45.6;hand1Z=100;hand1Pitch=30;hand1Yaw=45;hand1Roll=0;
   - Extracts x,y,z,pitch,yaw,roll for IK
   - Runs a basic IK => one joint angle -> range [0..4095]
   - Reads AS5600 sensor to get currentAngle
   - Applies direct proportional control => sets PWM on two pins
   - Some pins forced HIGH, others forced LOW
********************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include <math.h>
 #include <Adafruit_BusIO_Register.h>
  #include <Adafruit_Sensor.h>
// ---------------------- Pin Definitions ---------------------
#define PWM_PIN1       22   // One direction PWM
#define PWM_PIN2       25   // Opposite direction PWM

// Some pins to keep HIGH
int highPins[] = {21, 32, 16};
// Some pins to keep LOW
int lowPins[]  = {4};

// AS5600 I2C pins:
#define SDA_PIN_SENSOR 18
#define SCL_PIN_SENSOR 19
#define AS5600_ADDRESS 0x36

// ---------------------- PWM / Motor Control ------------------
#define PWM_FREQUENCY  1000
#define RESOLUTION     8            // 8-bit => duty 0..255
#define MAX_DUTY       255
float kp = 0.3f;                    // Simple proportional gain
int deadZone = 15;                  // Minimal PWM to overcome friction

// ---------------------- IK + Joint Selection -----------------
// We'll pick e.g. jointIndex=2 if we want to drive the 3rd joint
// from the computed angles
static const uint8_t jointIndex = 0;

// Whether to parse "hand1X" or "hand2X" from the text
bool useHand1 = true;

// desiredPosition in [0..4095]
uint16_t desiredPosition = 2000; 

// sensor tracking
uint16_t lastValidAngle  = 2000;
static const uint16_t maxAllowedJump = 5000; 
int positionErrorAllowance = 10;   // if abs(error)<this => 0 PWM

// current angle
uint16_t currentAngle = 0;

// -------------- Inverse Kinematics --------------
// Example 6-DOF DH parameters:
struct DHParameter {
  double a;
  double alpha;
  double d;
  double theta;
};
DHParameter dh_params[6] = {
  { 0.0,     M_PI/2,  140.0, 0 }, 
  { 400.0,   0,       0.0,   0 }, 
  { 250.0,   0,       0.0,   0 },
  { 0.0,     M_PI/2,  0.0,   0 },
  { 100.0,  -M_PI/2,  0.0,   0 },
  { 100.0,   M_PI/2,  25.0,  0 }
};

// Minimal rotation function
static void eulerZYXToR(double roll, double pitch, double yaw, double R[3][3]){
  double cr=cos(roll),  sr=sin(roll);
  double cp=cos(pitch), sp=sin(pitch);
  double cy=cos(yaw),   sy=sin(yaw);

  R[0][0]=cy*cp;  R[0][1]=cy*sp*sr - sy*cr;  R[0][2]=cy*sp*cr + sy*sr;
  R[1][0]=sy*cp;  R[1][1]=sy*sp*sr + cy*cr;  R[1][2]=sy*sp*cr - cy*sr;
  R[2][0]=-sp;    R[2][1]=cp*sr;            R[2][2]=cp*cr;
}

// Basic partial IK for demonstration:
bool inverseKinematics(double x, double y, double z, 
                       double pitchDeg, double yawDeg, double rollDeg, 
                       double joints[6])
{
  double pitch = pitchDeg*M_PI/180.0;
  double yaw   = yawDeg  *M_PI/180.0;
  double roll  = rollDeg *M_PI/180.0;

  // We'll do a simple approach: 
  double r= hypot(x,y);
  double L1= 400.0;
  double L2= 250.0;

  double D= (r*r + z*z - L1*L1 - L2*L2)/(2*L1*L2);
  if (fabs(D)>1.0) return false;

  double theta2= atan2(-sqrt(1-D*D), D);
  double theta1= atan2(z, r)- atan2(L2*sin(theta2), L1+ L2*cos(theta2));

  // Fill:
  joints[0]= atan2(y,x);
  joints[1]= theta1;
  joints[2]= theta2;
  joints[3]= pitch; 
  joints[4]= yaw;   
  joints[5]= roll;  

  return true;
}

// -------------- ESP-NOW Callback --------------
void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  // copy to local buffer
  char buffer[256];
  if (len>255) len=255;
  memcpy(buffer, data, len);
  buffer[len]='\0';

  // parse "hand1X=...,hand1Y=..., etc." or "hand2X=..."
  double x=0,y=0,z=0, pitch=0,yaw=0, roll=0;

  auto parseField=[&](const char* fieldName, double &storeVar){
    char* ptr= strstr(buffer, fieldName);
    if(ptr){
      ptr+= strlen(fieldName);
      if(*ptr=='=') ptr++;
      storeVar= atof(ptr);
    }
  };

  if(useHand1){
    parseField("h1X", x);
    parseField("h1Y", y);
    parseField("h1Z", z);
    parseField("h1P", pitch);
    parseField("h1Y",   yaw);
    parseField("h1R",  roll);
  } else {
    parseField("h2X", x);
    parseField("h2Y", y);
    parseField("h2Z", z);
    parseField("h2P", pitch);
    parseField("h2Y",   yaw);
    parseField("h2R",  roll);
  }

  Serial.printf("IK input => x=%.2f,y=%.2f,z=%.2f, pitch=%.2f,yaw=%.2f,roll=%.2f\n",
                x,y,z,pitch,yaw,roll);

  double joints[6];
  if(inverseKinematics(x,y,z,pitch,yaw,roll,joints)){
    double ang= joints[jointIndex];
    while(ang<0) ang+= 2.0*M_PI;
    ang= fmod(ang,2.0*M_PI);

    // Convert to [0..4095]
    uint16_t angleVal= (uint16_t)( ang*(4096.0/(2.0*M_PI)) );
    angleVal&= 0x0FFF;

    desiredPosition= angleVal;
    Serial.print("IK success => Joint");
    Serial.print(jointIndex);
    Serial.print("= ");
    Serial.print(ang*(180.0/M_PI));
    Serial.print(" deg => desiredPos=");
    Serial.println(desiredPosition);
  } else {
    Serial.println("IK failed => no update to desiredPosition");
  }
}

// -------------- Setup & Loop --------------
static uint16_t readAS5600Angle(uint16_t &lastAngle){
  Wire.beginTransmission(0x36);
  Wire.write(0x0C); 
  if(Wire.endTransmission(false)!=0){
    // no response
    return lastAngle;
  }
  if(Wire.requestFrom((uint8_t)0x36,(uint8_t)2)==2){
    uint8_t hb= Wire.read();
    uint8_t lb= Wire.read();
    uint16_t angle= ((uint16_t)hb<<8)|lb;
    angle&= 0x0FFF;
    if(abs((int)angle-(int)lastAngle)> maxAllowedJump){
      return lastAngle;
    }
    lastAngle= angle;
    return angle;
  }
  return lastAngle;
}

// We'll do a direct approach: 
// error= desiredPos- currentPos => pwm= kp* error => clamp => apply
// no ramping, just direct control
int computePWM(uint16_t currentPos, uint16_t desiredPos){
  int err= (int)desiredPos-(int)currentPos;
  if(abs(err)<= positionErrorAllowance) return 0;
  int pwm= (int)(kp*(float)err);
  pwm= constrain(pwm, -MAX_DUTY, MAX_DUTY);

  if(pwm>0)  pwm= max(pwm, deadZone);
  if(pwm<0)  pwm= min(pwm, -deadZone);

  return pwm;
}

void applyMotor(int pwmVal){
  // if pwmVal>0 => forward => ledcWrite(0, pwmVal); ledcWrite(1, 0)
  // if pwmVal<0 => reverse => ledcWrite(0,0), ledcWrite(1, -pwmVal)
  if(pwmVal>0){
    ledcWrite(0, pwmVal);
    ledcWrite(1, 0);
  } else if(pwmVal<0){
    ledcWrite(0, 0);
    ledcWrite(1, -pwmVal);
  } else {
    ledcWrite(0, 0);
    ledcWrite(1, 0);
  }
}

void setup(){
  Serial.begin(115200);
  delay(500);

  // Force certain pins HIGH / LOW
  for(unsigned i=0; i< (sizeof(highPins)/sizeof(highPins[0])); i++){
    pinMode(highPins[i], OUTPUT);
    digitalWrite(highPins[i], HIGH);
  }
  for(unsigned i=0; i< (sizeof(lowPins)/sizeof(lowPins[0])); i++){
    pinMode(lowPins[i], OUTPUT);
    digitalWrite(lowPins[i], LOW);
  }

  // Setup I2C
  Wire.begin(SDA_PIN_SENSOR, SCL_PIN_SENSOR);

  // Setup LEDC for motor
  ledcSetup(0, PWM_FREQUENCY, RESOLUTION); 
  ledcSetup(1, PWM_FREQUENCY, RESOLUTION);
  ledcAttachPin(PWM_PIN1, 0);
  ledcAttachPin(PWM_PIN2, 1);

  // Wi-Fi for ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if(esp_now_init()!= ESP_OK){
    Serial.println("Error init ESP-NOW");
    return;
  }
  // Register callback
  esp_now_register_recv_cb(onDataRecv);

  Serial.println("Final Slave with text-based IK + AS5600 closed loop, no ramping.");
}

void loop(){
  // read sensor
  uint16_t curr= readAS5600Angle(lastValidAngle);

  // compute direct PWM
  int pwmVal= computePWM(curr, desiredPosition);

  // apply
  applyMotor(pwmVal);

  // debug every 500ms
  static unsigned long lastPrint=0;
  unsigned long now= millis();
  if(now- lastPrint>500){
    lastPrint= now;
    int err= (int)desiredPosition - (int)curr;
    Serial.print("Angle=");
    Serial.print(curr);
    Serial.print(" Desired=");
    Serial.print(desiredPosition);
    Serial.print(" Err=");
    Serial.print(err);
    Serial.print(" -> PWM=");
    Serial.println(pwmVal);
  }

  delay(20);
}
