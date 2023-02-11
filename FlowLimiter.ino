/*
Copyright 2022 Bouchier Engineering LLC

MIT License

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
*/

#include "secrets.h"
#include <WiFiClientSecure.h>
#include <MQTTClient.h>
#include <ArduinoJson.h>
#include "WiFi.h"
#include <M5StickCPlus.h>
#include <EEPROM.h>
#include <esp_timer.h>
#include <string>

// GPIOs
#define FLOW_PULSE_PIN 36
#define INHIBIT_PIN 26

// EEPROM variable offsets
#define DEV_NUM_ADDR 0
#define SIM_FLOW_ADDR 1
#define FLOW_LIMIT_INDEX_ADDR 2
#define EEPROM_SIZE 10  // define the size of EEPROM(Byte).

// The MQTT topics that this device should publish/subscribe
#define AWS_IOT_PUBLISH_TOPIC   "flow-limiter/flow"
#define AWS_IOT_SUBSCRIBE_TOPIC "esp32/sub"

// RTC time variables
RTC_TimeTypeDef RTC_TimeStruct;
RTC_DateTypeDef RTC_DateStruct;


// Display variables
int lineCnt = 0;
int nextLcdUpdateTime = 0;
int displayMode = 0;  // 0: flow, 1: time
int timeMode = 0; // 0: display, 1: set year, 2 set month...

// button variables
bool buttonA = false;
bool buttonB = false;

// water-subsystem variables
const float pulsesPerLiter = 60 * 6.6;

uint8_t flowSensorOutput;  // current state of flow sensor output
uint8_t lastFlowSensorOutput;
int64_t flowCount = 0;  // how many total pulses have been seen
int64_t lastDisplayflowCount = 0;
float litersSinceStart = 0;  // how many liters since start of today
int litersSinceStart_int;
float lpm = 0;  // liters/minute for display
float flowLimit;
float flowLimitTable[] = {2000.0, 1000.0, 500.0, 200.0, 100.0, 20.0};
int flowLimitTableSize = sizeof(flowLimitTable) / sizeof(float);
unsigned char flowLimitTableIndex;

float lastReportedTotal = 0;
float reportIncrement = 0;
float reportRate = 0.0;
float displayRate = 0.0;

// Shutoff valve variables
bool valveClosed = false;  // true = closed - flow shut off
bool shutoffLogPrinted = false;

// water-flow simulator variables
const int flowOffDurationSec = 30;  // how long water should flow for
const int flowOnDurationSec = 30;   // how long water should not flow for
const int flowSim30LpmHalfPeriod = 2382; // 2.832 msec for half a cycle at 30L/min
const int flowSim600LphHalfPeriod = flowSim30LpmHalfPeriod * 3;  // adjust to 600 Lph
const int flowSimMinHalfPeriod = flowSim600LphHalfPeriod * 100;  // always a little flow: 1%
const float flowSimMinFreq = 1000000.0 / flowSimMinHalfPeriod;  // 1.40 Hz rate of sensor transitions is min flow for sim
int flowSimHalfPeriod = flowSimMinHalfPeriod;  // set initial flow to min
bool simulateFlow;  // Overall flow sim enable, initialized from EEPROM
bool generateFlowPulses = false;  // true: generate flow pulses

// Time-related variables
int64_t secondsSinceStart = 0;
int64_t nextSecondTime;
int64_t nextFlowSensorTransition;
bool simFlowSensorOutput = false;
int nextFlowToggleTime;
int lastHour = 0;
const int reportingPeriodSec = 300;  // report each 5 minutes
int nextPeriodTime = reportingPeriodSec;
float rateDivisor = reportingPeriodSec / 60;  // convert flow increment to lpm

int deviceId;
WiFiClientSecure net = WiFiClientSecure();
MQTTClient client = MQTTClient(256);

void connectAWS()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Wi-Fi:");
  M5.Lcd.print("Wi-Fi:");

  while (WiFi.status() != WL_CONNECTED){
    delay(500);
    Serial.print(".");
    M5.Lcd.print(".");
  }
  Serial.println("OK");
  M5.Lcd.println("OK");
  lineCnt++;

  // Configure WiFiClientSecure to use the AWS IoT device credentials
  net.setCACert(AWS_CERT_CA);
  net.setCertificate(AWS_CERT_CRT);
  net.setPrivateKey(AWS_CERT_PRIVATE);

  // set keepalive to 180 sec, timeout to 1000 sec
  client.setOptions(180, true, 1000);
  // Connect to the MQTT broker on the AWS endpoint we defined earlier
  client.begin(AWS_IOT_ENDPOINT, 8883, net);

  // Create a message handler
  client.onMessage(messageHandler);

  Serial.print("AWS IoT: ");
  M5.Lcd.print("AWS IoT: ");

  while (!client.connect(THINGNAME)) {
    Serial.print(".");
    M5.Lcd.print(".");
    delay(100);
  }

  if(!client.connected()){
    Serial.println("Fail: AWS IoT Timeout!");
    return;
  }
  else{
    Serial.println("OK");
    M5.Lcd.println("OK");
    lineCnt++;
  }

  // Subscribe to a topic
  client.subscribe(AWS_IOT_SUBSCRIBE_TOPIC);

  delay(2000);
}

void publishMessage()
{
  // generate timestamp
  char datetime[30];
  sprintf(datetime, "%04d-%02d-%02d %02d:%02d:%02d",
    RTC_DateStruct.Year, RTC_DateStruct.Month, RTC_DateStruct.Date,
    RTC_TimeStruct.Hours, RTC_TimeStruct.Minutes, RTC_TimeStruct.Seconds);
  String datetimeString = String(datetime);

  StaticJsonDocument<200> doc;
  doc["deviceId"] = deviceId;
  doc["time"] = datetimeString;
  doc["cum"] = litersSinceStart_int;
  doc["rate"] = reportIncrement / rateDivisor;
  doc["shutoff"] = valveClosed;
  char jsonBuffer[512];
  serializeJson(doc, jsonBuffer); // print to client

  client.publish(AWS_IOT_PUBLISH_TOPIC, jsonBuffer);
  Serial.print("published to AWS at ");
  Serial.println(datetimeString);
}

void messageHandler(String &topic, String &payload) {
  Serial.println("incoming: " + topic + " - " + payload);

  StaticJsonDocument<200> doc;
  deserializeJson(doc, payload);
  const char* message = doc["message"];
  Serial.printf("ERROR: Received message %s", payload);
}

void resetDisplayMode()
{
  timeMode = 0;
  displayMode = 0;
}

void displayFlowLine1()
{
  M5.Lcd.setCursor(0, 0, 2);
  if (0 == (RTC_TimeStruct.Seconds % 4))
  {
    M5.Lcd.print(" Button B: Reset");
  }
  else
  {
    M5.Lcd.print(" Litres today:");
  }
}

void displayFlowData()
{
  M5.Lcd.setCursor(0, 40, 4);
  M5.Lcd.printf("% .0f", litersSinceStart);
  M5.Lcd.setCursor(0, 100, 2);
  if (valveClosed)
  {
    M5.Lcd.printf(" Flow: *INHIBITED*");
  }
  else
  {
    int64_t displayFlowCountInc = flowCount - lastDisplayflowCount;
    lastDisplayflowCount = flowCount;
    float litersSinceDisplay = displayFlowCountInc / pulsesPerLiter;
    lpm = litersSinceDisplay * 60;
    M5.Lcd.printf("Flow: on %.1f lpm", lpm);  // if flow on, display lpm
  }
}

void displayFlow()
{
  displayFlowLine1();
  displayFlowData();

  if (buttonB)
  {
    flowCount = 0;  // reset liters in the day
    litersSinceStart = 0;
    lastReportedTotal = 0;
    setShutoff(false);  // re-enable flow
  }
}

void displayFlowLimit()
{
  M5.Lcd.setCursor(0, 0, 1);
  M5.Lcd.println("Flow Limit\nButton B: change");
  M5.Lcd.setCursor(0, 40, 4);
  int flowLimit_int = static_cast<int>(flowLimit);
  M5.Lcd.printf("%d", flowLimit_int);

  if (buttonB)
  {
    flowLimitTableIndex++;
    if (flowLimitTableIndex == flowLimitTableSize)
      flowLimitTableIndex = 0;
    flowLimit = flowLimitTable[flowLimitTableIndex];

    // write new flow-limit index to EEPROM
    EEPROM.write(FLOW_LIMIT_INDEX_ADDR, flowLimitTableIndex);
    EEPROM.commit();
  }
}

void displayDateTime()
{
  M5.Lcd.setCursor(0, 0, 1);
  M5.Lcd.println("Date and Time");
  M5.Lcd.setCursor(0, 40, 1);
  M5.Lcd.printf("Date: %04d-%02d-%02d\n", RTC_DateStruct.Year,
  RTC_DateStruct.Month, RTC_DateStruct.Date);
  M5.Lcd.setCursor(0, 60);
  M5.Lcd.printf("Time: %02d:%02d:%02d\n", RTC_TimeStruct.Hours,
    RTC_TimeStruct.Minutes, RTC_TimeStruct.Seconds);

  // Display prompt for date/time field to set
  M5.Lcd.setCursor(0, 80);
  switch (timeMode)
  {
    case 0:
      M5.Lcd.println("Button A: next\nButton B: Adjust\ntime/date");
      if (buttonB)
        timeMode++;
      break;
    case 1:
      M5.Lcd.println("Button A: next\nButton B: set year");
      if (buttonB)
      {
        RTC_DateStruct.Year++;
        if (RTC_DateStruct.Year > 2028)
        {
          RTC_DateStruct.Year = 2023;
        }
        M5.Rtc.SetData(&RTC_DateStruct);
      }
      break;
    case 2:
      M5.Lcd.println("Button A: next\nButton B: set month");
      if (buttonB)
      {
        RTC_DateStruct.Month++;
        if (RTC_DateStruct.Month > 12)
        {
          RTC_DateStruct.Month = 1;
        }
        M5.Rtc.SetData(&RTC_DateStruct);
      }
      break;
    case 3:
      M5.Lcd.println("Button A: next\nButton B: set day-of-month");
      if (buttonB)
      {
        RTC_DateStruct.Date++;
        if (RTC_DateStruct.Date > 31)
        {
          RTC_DateStruct.Date = 1;
        }
        M5.Rtc.SetData(&RTC_DateStruct);
      }
      break;
    case 4:
      M5.Lcd.println("Button A: next\nButton B: set hour");
      if (buttonB)
      {
        RTC_TimeStruct.Hours++;
        if (RTC_TimeStruct.Hours > 23)
        {
          RTC_TimeStruct.Hours = 0;
        }
        M5.Rtc.SetTime(&RTC_TimeStruct);
      }
      break;
    case 5:
      M5.Lcd.println("Button A: next\nButton B: set minute");
      if (buttonB)
      {
        RTC_TimeStruct.Minutes++;
        if (RTC_TimeStruct.Minutes > 59)
        {
          RTC_TimeStruct.Minutes = 0;
        }
        M5.Rtc.SetTime(&RTC_TimeStruct);
      }
      break;
    case 6:
      M5.Lcd.println("Button A: next\nButton B: clear seconds");
      if (buttonB)
      {
        RTC_TimeStruct.Seconds = 0;
        M5.Rtc.SetTime(&RTC_TimeStruct);
      }
      break;
    default:
      resetDisplayMode();
  }
}

void displayValveOverride()
{
  M5.Lcd.setCursor(0, 0, 1);
  M5.Lcd.printf(" Valve State: %s\n", valveClosed?"closed":"open");
  M5.Lcd.println(" Button B: Toggle");

  displayFlowData();

  if (buttonB)
  {
    setShutoff(!valveClosed);
    Serial.printf("Set valveClosed to %s\n", valveClosed?"true":"false");
  }
}

void displaySimMode()
{
  M5.Lcd.setCursor(0, 0, 1);
  M5.Lcd.printf("Flow simulation: %s\n Button B: toggle", simulateFlow?"on":"off");

  if (buttonB)
  {
    simulateFlow = !simulateFlow;
    EEPROM.write(SIM_FLOW_ADDR, (byte)simulateFlow);
    EEPROM.commit();
  }
}

void setShutoff(bool valveState)
{
  if (valveState != valveClosed)
  {
    shutoffLogPrinted = false; // print log on valve state changes
  }
  valveClosed = valveState;     // reenable flow if it was disabled
  digitalWrite(INHIBIT_PIN, valveClosed);    // set valve
}

void midnight()
{
  Serial.println("Midnight");
  delay(1000);
  ESP.restart();  // reboot device to re-establish AWS comms if it had dropped
}

void hoursUpdate()
{
  int currentHour = RTC_TimeStruct.Hours;
  if (currentHour != lastHour)
  {
    Serial.printf("New hour: %d\n", currentHour);

    if (0 == currentHour)
      midnight();
    
    if (currentHour > 7 && currentHour < 18)
    {
      // simulated water flow takes place between 8am & 6pm, random up to 10 lpm
      int flowRandFactor = rand() % 100;  // randomize flow in the range 1/100 to 1 * base rate
      float flowSimRandFreq = flowSimMinFreq + flowSimMinFreq * flowRandFactor;  // 1.40Hz - 140Hz
      flowSimHalfPeriod = static_cast<int>(1000000.0 / flowSimRandFreq);
      Serial.printf("flowRandFactor: %d flowSimMinFreq: %f flowSimRandFreq: %f flowSimHalfPeriod %d\n", 
        flowRandFactor, flowSimMinFreq,  flowSimRandFreq, flowSimHalfPeriod);
    }
    else
    {
      // Outside flow hours, simulated water flow is 0.3 lpm
      flowSimHalfPeriod = flowSimMinHalfPeriod;
    }

    lastHour = currentHour;
  }
}

void secondsUpdate()
{
  // Check if it's time to increment the seconds-counter
  int64_t now_us = esp_timer_get_time();
  if (now_us > nextSecondTime)
  {
    secondsSinceStart++;
    nextSecondTime += 1000000;
    // Serial.printf("%lld toggleTime: %d state %d\n", secondsSinceStart, nextFlowToggleTime, generateFlowPulses);

    // Get the time-of-day from the real-time clock.
    M5.Rtc.GetTime(&RTC_TimeStruct);
    M5.Rtc.GetData(&RTC_DateStruct);

    // update measured flow and rate each second
    litersSinceStart = flowCount / pulsesPerLiter;  // assumes 1/60 lit = 6.6 pulses
    litersSinceStart_int = static_cast<int>(litersSinceStart);

    // update the LCD display once/sec - blank it and hand off to display functions
    M5.Lcd.fillScreen(BLACK);

    if (displayMode == 0)
      displayFlow();
    else if (displayMode == 1)
      displayFlowLimit();
    else if (displayMode == 2)
      displayDateTime();
    else if (displayMode == 3)
      displayValveOverride();
    else if (displayMode == 4)
      displaySimMode();
    else
      displayMode = 0;  // should never get here
    // clear buttonB after display functions have used it
    buttonB = false;
    
    // turn water off if limit exceeded
    if (litersSinceStart > flowLimit)
    {
      setShutoff(true);
      if (!shutoffLogPrinted)
      {
        Serial.printf("Set valveClosed to %s\n", valveClosed?"true":"false");
        shutoffLogPrinted = true;
      }
    }

    // Check if we need to send a report to the cloud
    if (secondsSinceStart > nextPeriodTime)
    {
      nextPeriodTime = secondsSinceStart + reportingPeriodSec;
      reportIncrement = litersSinceStart - lastReportedTotal;
      if (reportIncrement < 0)
        reportIncrement = 0;
      lastReportedTotal = litersSinceStart;
      publishMessage();
      Serial.printf("liters since start: %f Increment: %f\n", litersSinceStart, reportIncrement);
    }

    // check if we are in a new hour and do hour-aligned work
    hoursUpdate();
  }
}

void flowSim()
{
  int64_t now_us = esp_timer_get_time();

  // Check if it's time to toggle water flow state
  if (secondsSinceStart > nextFlowToggleTime)
  {
    if (false == valveClosed && generateFlowPulses == false)
    {
      generateFlowPulses = true;
      nextFlowSensorTransition = now_us + flowSimHalfPeriod;
      nextFlowToggleTime = secondsSinceStart + flowOnDurationSec;
      // Serial.println("Sim flow started");
    }
    else
    {
      generateFlowPulses = false;
      nextFlowToggleTime = secondsSinceStart + flowOffDurationSec;
      // Serial.println("Sim flow stopped");
    }
  }

  // toggle the flow rate sensor output
  if (false == valveClosed && true == generateFlowPulses)
  {
    if (now_us > nextFlowSensorTransition)
    {
      simFlowSensorOutput = !simFlowSensorOutput;
      nextFlowSensorTransition = now_us + flowSimHalfPeriod;
    }
  }
}

void setup() {
  M5.begin();
  M5.Lcd.setRotation(1);
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(0, 0, 2);
  M5.Lcd.setTextColor(TFT_WHITE,TFT_BLACK);
  M5.Lcd.setTextSize(2);

  Serial.begin(115200);

  // for Flow Limiter, G36 is flow pulse input, G26 is valve output
  pinMode(FLOW_PULSE_PIN, INPUT_PULLUP);
  pinMode(INHIBIT_PIN, OUTPUT);
  gpio_pulldown_dis(GPIO_NUM_25);
  gpio_pullup_dis(GPIO_NUM_25);
  digitalWrite(INHIBIT_PIN, false);

  // Initialize EEPROM
  while (!EEPROM.begin(EEPROM_SIZE)) {  // Request storage of SIZE size(success return)
    Serial.println("\nFailed to initialise EEPROM!");
    M5.Lcd.println("EEPROM Fail");
    delay(1000000);
  }

  // Initialize variables from EEPROM
  deviceId = EEPROM.read(DEV_NUM_ADDR);
  Serial.printf("Device ID: %d\n", deviceId);

  simulateFlow = EEPROM.read(SIM_FLOW_ADDR);
  Serial.printf("Simulate Flow: %s\n", simulateFlow?"true":"false");

  flowLimitTableIndex = EEPROM.read(FLOW_LIMIT_INDEX_ADDR);
  if (flowLimitTableIndex < flowLimitTableSize)
    flowLimit = flowLimitTable[flowLimitTableIndex];
  Serial.printf("flow limit: %f from index %d\n", flowLimit, flowLimitTableIndex);

  connectAWS();

  int64_t now = esp_timer_get_time();
  nextSecondTime = now + 1000000;  // time to increment the seconds counter

  // Get the time-of-day from the real-time clock.
  M5.Rtc.GetTime(&RTC_TimeStruct);
  M5.Rtc.GetData(&RTC_DateStruct);
  lastHour = RTC_TimeStruct.Hours;

  if (simulateFlow)
  {
    if (simFlowSensorOutput)
      flowSensorOutput = HIGH;
    else
      flowSensorOutput = LOW;
    nextFlowToggleTime = secondsSinceStart + flowOffDurationSec; // start with water off
    srand(100);
  }
  else
  {
    flowSensorOutput = digitalRead(FLOW_PULSE_PIN);
  }
  lastFlowSensorOutput = flowSensorOutput;

  setShutoff(false);  // re-enable flow if it was inhibited

  delay(2000);  // pause to allow reading startup msgs
  M5.Lcd.fillScreen(BLACK);
  resetDisplayMode();
}

void loop() {
  // Read buttons
  M5.update();
  if (M5.BtnA.wasReleased())
  {
    buttonA = true;
  }
  if (M5.BtnB.wasReleased())
  {
    buttonB = true;
  }

  // Big button cycles through display mode & time set fields
  if (buttonA)
  {
    if (displayMode == 0)
      displayMode = 1;
    else if (displayMode == 1)
      displayMode = 2;
    else if (displayMode == 2 && timeMode == 0)
      displayMode = 3;
    else if (displayMode == 2 && timeMode != 0)
      timeMode++;
    else if (displayMode == 3)
      displayMode = 4;
    else if (displayMode == 4)
      displayMode = 0;

    buttonA = false;
  }

  // process everything that should be done each second
  secondsUpdate();

  // get flow sensor output
  if (simulateFlow)
  {
    // call the flow simulator
    flowSim();
    if (simFlowSensorOutput)
      flowSensorOutput = HIGH;
    else
      flowSensorOutput = LOW;
  }
  else
  {
    flowSensorOutput = digitalRead(FLOW_PULSE_PIN);
  }

  // Count rising edges from flow sensor
  if (HIGH == flowSensorOutput && LOW == lastFlowSensorOutput)
  {
    flowCount++;
    // Serial.print("+");
  }
  lastFlowSensorOutput = flowSensorOutput;

  client.loop();  // check for any received msgs
}
