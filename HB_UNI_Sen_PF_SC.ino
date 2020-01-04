//- -----------------------------------------------------------------------------------------------------------------------
// AskSin++
// 2016-10-31 papa Creative Commons - http://creativecommons.org/licenses/by-nc-sa/3.0/de/
// 2020-01-04 jp112sdl Creative Commons - http://creativecommons.org/licenses/by-nc-sa/3.0/de/
//- -----------------------------------------------------------------------------------------------------------------------

#define EI_NOTEXTERNAL
#include <EnableInterrupt.h>
#include <AskSinPP.h>
#include <LowPower.h>

#include <Register.h>
#include <ContactState.h>
#include "As5600.h"

// we use a Pro Mini
// Arduino pin for the LED
// D4 == PIN 4 on Pro Mini
#define LED1_PIN 4
#define LED2_PIN 5
// Arduino pin for the config button
// B0 == PIN 8 on Pro Mini
#define CONFIG_BUTTON_PIN 8

#define BATTERY_EXT     A0
#define BATTERY_EXT_EN   7

// number of available peers per channel
#define PEERS_PER_CHANNEL 10

#define MEASURE_INTERVAL 500 //interval in ms to read the angle
#define POS_B_ANGLE      180 //degrees in normal position
#define ANGLE_HYST        20 // +/- deg

// all library classes are placed in the namespace 'as'
using namespace as;

// define all device properties
const struct DeviceInfo PROGMEM devinfo = {
    {0xF3,0x4D,0x00},       // Device ID
    "JPPFSC0000",           // Device Serial
    {0xF3,0x4D},            // Device Model
    0x22,                   // Firmware Version
    as::DeviceType::ThreeStateSensor, // Device Type
    {0x01,0x00}             // Info Bytes
};

/**
 * Configure the used hardware
 */
typedef AvrSPI<10,11,12,13> SPIType;
typedef Radio<SPIType,2> RadioType;
typedef DualStatusLed<LED2_PIN,LED1_PIN> LedType;
typedef AskSin<LedType,BatterySensorUni<BATTERY_EXT,BATTERY_EXT_EN>,RadioType> BaseHal;
class Hal : public BaseHal {
public:
  void init (const HMID& id) {
    BaseHal::init(id);
    // measure battery every 1h
    battery.init(seconds2ticks(60UL*60),sysclock);
  }
} hal;

DEFREGISTER(Reg0,DREG_INTKEY,DREG_CYCLICINFOMSG,MASTERID_REGS,DREG_TRANSMITTRYMAX, DREG_LOWBATLIMIT)
class CFList0 : public RegList0<Reg0> {
public:
  CFList0(uint16_t addr) : RegList0<Reg0>(addr) {}
  void defaults () {
    clear();
    cycleInfoMsg(true);
    transmitDevTryMax(6);
    lowBatLimit(50);
  }
};

DEFREGISTER(Reg1,CREG_AES_ACTIVE,CREG_MSGFORPOS,CREG_EVENTDELAYTIME,CREG_LEDONTIME,CREG_TRANSMITTRYMAX)
class CFList1 : public RegList1<Reg1> {
public:
  CFList1 (uint16_t addr) : RegList1<Reg1>(addr) {}
  void defaults () {
    clear();
    msgForPosA(1); // CLOSED
    msgForPosB(2); // OPEN INCOMING
    msgForPosC(3); // OPEN OUTGOING
    // aesActive(false);
    // eventDelaytime(0);
    ledOntime(100);
    transmitTryMax(6);
  }
};

class As5600PinPosition : public Position {
private:
  As5600<AS5600PowerMode::LPM3> as5600;
public:
  As5600PinPosition () {}
  void init () {
    as5600.init();
  }

  uint32_t interval () { return millis2ticks(MEASURE_INTERVAL); }

  void measure (__attribute__((unused)) bool async=false) {
    as5600.measure();
    uint16_t angle = as5600.angle();

    DPRINT("angle: ");DDECLN(angle);

    switch (angle) {
     case 0 ... POS_B_ANGLE - ANGLE_HYST - 1:
      _position = State::PosB;
     break;
     case POS_B_ANGLE - ANGLE_HYST ... POS_B_ANGLE + ANGLE_HYST:
      _position = State::PosA;
     break;
     case POS_B_ANGLE + ANGLE_HYST + 1 ... 359:
      _position = State::PosC;
     break;

     default:
      _position = State::PosA;
    }
  }
};

template <class HALTYPE,class List0Type,class List1Type,class List4Type,int PEERCOUNT>
class As5600Channel : public StateGenericChannel<As5600PinPosition,HALTYPE,List0Type,List1Type,List4Type,PEERCOUNT> {

public:
  typedef StateGenericChannel<As5600PinPosition,HALTYPE,List0Type,List1Type,List4Type,PEERCOUNT> BaseChannel;

  As5600Channel () : BaseChannel() {};
  ~As5600Channel () {}

  void init () {
   BaseChannel::init();
   BaseChannel::possens.init();
  }

};

typedef As5600Channel<Hal,CFList0,CFList1,DefList4,PEERS_PER_CHANNEL> ChannelType;

class CFType : public ThreeStateDevice<Hal,ChannelType,1,CFList0> {
public:
  typedef ThreeStateDevice<Hal,ChannelType,1,CFList0> TSDevice;
  CFType(const DeviceInfo& info,uint16_t addr) : TSDevice(info,addr) {}
  virtual ~CFType () {}

  virtual void configChanged () {
    TSDevice::configChanged();
    // set battery low/critical values
    uint8_t lb = this->getList0().lowBatLimit();
    DPRINT("LOWBAT ");DDECLN(lb);
    battery().low(lb);
  }
};

CFType sdev(devinfo,0x20);
ConfigButton<CFType> cfgBtn(sdev);

void setup () {
  DINIT(57600,ASKSIN_PLUS_PLUS_IDENTIFIER);
  sdev.init(hal);
  buttonISR(cfgBtn,CONFIG_BUTTON_PIN);
  sdev.channel(1).init();
  sdev.initDone();
}

void loop() {
  bool worked = hal.runready();
  bool poll = sdev.pollRadio();
  if( worked == false && poll == false ) {
    if( hal.battery.critical() ) {
      hal.sleepForever();
    }
    hal.sleep<>();
  }
}
