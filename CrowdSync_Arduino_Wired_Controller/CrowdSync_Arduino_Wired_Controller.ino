/* CrowdSync Arduino wristband controller.
 * Target: classic 16 MHz ATmega328P Arduino Uno R3 / Nano, powered at 5 V.
 * D9 -> wristband MCU pin 4 / test pad; GND -> wristband ground (MCU pin 8).
 * Wristband supplies its own ~3.3 V pull-up. NO Arduino power pin connection.
 * D9 only uses INPUT (HI-Z, pull-up OFF) or OUTPUT with a LOW latch.
 * It never drives HIGH and never enables INPUT_PULLUP.
 *
 * Optional D8 <- wristband MCU pin 2 (RF enable) for QUIET ON mode.
 * This waits for RF OFF before injecting and aborts if RF switches ON.
 * Leave QUIET OFF unless D8 is connected. Default is OFF.
 * Receiver noise can corrupt frames when RF is powered; see README.md.
 * Wired recovery from effect 15 was not demonstrated; use RF or power-cycle.
 *
 * Serial 115200, newline:
 *   GROUP EFFECT RED GREEN BLUE   decimal; starts repeated complete frames
 *   STOP                         stops after current frame, releases D9
 *   QUIET ON / QUIET OFF          optional RF-enable monitoring
 *   STATUS                       prints mode and sent/aborted frame counts
 *   PERIOD ms                    minimum frame-start interval; 0=continuous
 *   ONCE GROUP EFFECT R G B      sends one packet
 *   RAW <22 hex bytes>           sends bytes without repairing the checksum
 *   TIMING short long sync gap   delays in microseconds
 *   PREFIX ON / PREFIX OFF      synchronization guard; default ON
 *   TAP ms                       local-button pulse
 * GROUP 1..10 changes one cached pair; 0 changes all ten.
 * EFFECT and RGB 0..15. Example: 6 0 15 0 0 = group-6 steady red.
 * Other groups initially contain black commands, not an ignore value.
 * Includes repeated packets, one-shot/raw packets, timing controls and local-button tests.
 */
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__AVR_ATmega328P__) || F_CPU != 16000000UL
#error "Use a classic 16 MHz ATmega328P Uno R3 / Nano (5 V). Other boards need an electrical review."
#endif

const uint8_t DATA_PIN=9, RF_ENABLE_PIN=8;
uint16_t SHORT_US=120, LONG_US=320, SYNC_US=1600, GAP_US=2400;
uint32_t framePeriodUs=0, nextFrameDue=0;
uint16_t finiteFrames=0;
bool resyncGuard=true;
const uint16_t RESYNC_GUARD_US=600;
const uint16_t QUIET_SETTLE_US=500;
uint8_t packet[22]={0};
bool streaming=false, monitorQuiet=false, quietTiming=false;
uint32_t quietSince=0, framesSent=0, framesAborted=0;
char line[112];
uint8_t used=0;
bool overflowed=false;

static void releaseLine() {
  pinMode(DATA_PIN,INPUT);        // No output driver; the TARGET supplies HIGH.
  digitalWrite(DATA_PIN,LOW);     // Explicitly keep Arduino pull-up disabled.
}

static void driveLow() {
  digitalWrite(DATA_PIN,LOW);     // Establish LOW latch BEFORE enabling output.
  pinMode(DATA_PIN,OUTPUT);
}

static bool quietReady() {
  if (!monitorQuiet)return true;
  if (digitalRead(RF_ENABLE_PIN)!=LOW) { quietTiming=false;return false; }
  uint32_t now=micros();
  if (!quietTiming) { quietTiming=true;quietSince=now;return false; }
  return (uint32_t)(now-quietSince)>=QUIET_SETTLE_US;
}

static bool pulse(bool asserted,uint16_t duration) {
  // Inverted envelope: carrier ON maps to LOW; carrier OFF maps to HI-Z.
  // The wristband compares edge intervals, so this inversion preserves bits.
  if (asserted)driveLow();else releaseLine();
  uint32_t start=micros();
  while ((uint32_t)(micros()-start)<duration) {
    if (monitorQuiet && digitalRead(RF_ENABLE_PIN)!=LOW) {
      releaseLine();quietTiming=false;return false;
    }
  }
  return true;
}

static bool sendFrame() {
  if (resyncGuard && (!pulse(true,SHORT_US) || !pulse(false,RESYNC_GUARD_US)))return false;
  if (!pulse(true,SHORT_US) || !pulse(false,SYNC_US))return false;
  for (uint8_t i=0;i<22;++i) {
    for (int8_t bit=7;bit>=0;--bit) {
      bool one=packet[i]&(1U<<bit);
      if (!pulse(true,one?LONG_US:SHORT_US) ||
          !pulse(false,one?SHORT_US:LONG_US))return false;
    }
  }
  // This falling edge finishes the final bit's second interval on pin 4.
  if (!pulse(true,SHORT_US) || !pulse(false,GAP_US))return false;
  releaseLine();
  return true;
}

static void status() {
  Serial.print(F("Streaming="));Serial.print(streaming?F("ON"):F("OFF"));
  Serial.print(F(" quiet="));Serial.print(monitorQuiet?F("ON"):F("OFF"));
  Serial.print(F(" sent="));Serial.print(framesSent);
  Serial.print(F(" aborted="));Serial.println(framesAborted);
}

// Returns true only after accepting a new numeric packet assignment.
static bool baseCommand(char *s) {
  int v[5];uint8_t n=0;
  char *token=strtok(s," \t");
  while (token && n<5) {
    char *end=nullptr;long x=strtol(token,&end,10);
    if (*end || x<0 || x>15) { Serial.println(F("Use: GROUP EFFECT R G B, decimal."));return false; }
    v[n++]=(int)x;token=strtok(nullptr," \t");
  }
  if (n!=5 || token || v[0]>10) {
    Serial.println(F("GROUP 0=all or 1..10; EFFECT/RGB 0..15."));return false;
  }
  uint8_t first=v[0]?v[0]-1:0,last=v[0]?v[0]-1:9;
  for (uint8_t k=first;k<=last;++k) {
    packet[2*k]=(v[1]<<4)|v[2];packet[2*k+1]=(v[3]<<4)|v[4];
  }
  packet[20]=0;for (uint8_t i=0;i<20;++i)packet[20]^=packet[i];packet[21]=0;
  streaming=true;
  Serial.print(F("Repeating: "));
  for (uint8_t i=0;i<22;++i) {
    if(packet[i]<16)Serial.print('0');Serial.print(packet[i],HEX);Serial.print(' ');
  }
  Serial.println();
  return true;
}

// Additional packet, timing and local-button controls.
static bool readNumbers(char *s,long *v,uint8_t count,int base=10) {
  char *token=strtok(s," \t");uint8_t n=0;
  while(token&&n<count){char *end=nullptr;v[n++]=strtol(token,&end,base);if(*end)return false;token=strtok(nullptr," \t");}
  return n==count&&!token;
}
static void command(char *s) {
  if(!strcmp(s,"PREFIX ON")||!strcmp(s,"PREFIX OFF")){resyncGuard=!strcmp(s,"PREFIX ON");Serial.println(resyncGuard?F("Guard prefix ON"):F("Guard prefix OFF"));return;}
  if(!strcmp(s,"STOP")){
    finiteFrames=0;streaming=false;releaseLine();Serial.println(F("Stopped; D9 released (HI-Z)."));return;
  }
  if(!strcmp(s,"STATUS")){status();return;}
  if(!strcmp(s,"QUIET ON")||!strcmp(s,"QUIET OFF")){
    monitorQuiet=!strcmp(s,"QUIET ON");quietTiming=false;releaseLine();status();return;
  }
  if(!strncmp(s,"PERIOD ",7)){
    long v[1];if(!readNumbers(s+7,v,1)||v[0]<0||v[0]>60000){Serial.println(F("PERIOD 0..60000 ms"));return;}
    framePeriodUs=(uint32_t)v[0]*1000UL;nextFrameDue=micros();Serial.print(F("Period ms="));Serial.println(v[0]);return;
  }
  if(!strncmp(s,"TIMING ",7)){
    long v[4];if(!readNumbers(s+7,v,4)||v[0]<20||v[0]>1000||v[1]<20||v[1]>1000||v[2]<100||v[2]>5000||v[3]<100||v[3]>10000){Serial.println(F("TIMING short long sync gap (us), bounds rejected"));return;}
    SHORT_US=v[0];LONG_US=v[1];SYNC_US=v[2];GAP_US=v[3];Serial.println(F("Timing set."));return;
  }
  if(!strncmp(s,"ONCE ",5)){
    if(baseCommand(s+5)){finiteFrames=1;nextFrameDue=micros();}
    return;
  }
  if(!strncmp(s,"RAW ",4)){
    long v[22];if(!readNumbers(s+4,v,22,16)){Serial.println(F("RAW requires 22 hex bytes"));return;}
    for(uint8_t i=0;i<22;i++)if(v[i]<0||v[i]>255){Serial.println(F("RAW byte outside range"));return;}
    for(uint8_t i=0;i<22;i++)packet[i]=v[i];
    finiteFrames=1;streaming=true;nextFrameDue=micros();Serial.println(F("Raw one-frame queued."));return;
  }
  if(!strncmp(s,"TAP ",4)){
    long v[1];if(!readNumbers(s+4,v,1)||v[0]<1||v[0]>2500){Serial.println(F("TAP 1..2500 ms"));return;}
    streaming=false;finiteFrames=0;releaseLine();delay(4);driveLow();delay(v[0]);releaseLine();Serial.println(F("Tap complete; released."));return;
  }
  if(baseCommand(s)){finiteFrames=0;nextFrameDue=micros();}
}

void setup() {
  releaseLine();
  pinMode(RF_ENABLE_PIN,INPUT);digitalWrite(RF_ENABLE_PIN,LOW);
  Serial.begin(115200);
  Serial.println(F("CrowdSync controller: GROUP EFFECT R G B; STOP / QUIET / STATUS / PERIOD / ONCE / RAW / TIMING / PREFIX / TAP."));
  Serial.println(F("D9 is LOW/HI-Z only. QUIET ON requires D8 connected to RF enable."));
}

void loop() {
  while (Serial.available()) {
    char ch=Serial.read();
    if (ch=='\r')continue;
    if (ch=='\n') {
      if (overflowed)Serial.println(F("Line too long."));
      else if (used) { line[used]=0;command(line); }
      used=0;overflowed=false;
    } else if (used<sizeof(line)-1)line[used++]=ch;
    else overflowed=true;
  }
  if (streaming && (int32_t)(micros()-nextFrameDue)>=0 && quietReady()) {
    uint32_t began=micros();
    if (sendFrame()){
      ++framesSent;
      if(finiteFrames&&!--finiteFrames){streaming=false;releaseLine();}
    }else ++framesAborted;
    nextFrameDue=began+framePeriodUs;
  }
}
