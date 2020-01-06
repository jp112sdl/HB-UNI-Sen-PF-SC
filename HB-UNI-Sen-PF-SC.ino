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
#define LED1_PIN 6
#define LED2_PIN 4
// Arduino pin for the config button
// B0 == PIN 8 on Pro Mini
#define CONFIG_BUTTON_PIN 8

#define BATTERY_EXT     A3
#define BATTERY_EXT_EN  A2

// number of available peers per channel
#define PEERS_PER_CHANNEL 10

// all library classes are placed in the namespace 'as'
using namespace as;

// define all device properties
const struct DeviceInfo PROGMEM devinfo = {
    {0xF3,0x4D,0x01},       // Device ID
    "JPPFSC0001",           // Device Serial
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

DEFREGISTER(Reg1,CREG_AES_ACTIVE,CREG_MSGFORPOS,CREG_EVENTDELAYTIME,CREG_LEDONTIME,CREG_TRANSMITTRYMAX, 0x94, 0x95, 0x96, 0x97)
class CFList1 : public RegList1<Reg1> {
public:
  CFList1 (uint16_t addr) : RegList1<Reg1>(addr) {}

  bool angleMeasureInterval (uint16_t value) const {
    return this->writeRegister(0x94, (value >> 8) & 0xff) && this->writeRegister(0x95, value & 0xff);
  }
  uint16_t angleMeasureInterval () const {
    return (this->readRegister(0x94, 0) << 8) + this->readRegister(0x95, 0);
  }

  bool angleDefault (uint8_t value) const {
    return this->writeRegister(0x96, value & 0xff);
  }
  uint8_t angleDefault () const {
    return this->readRegister(0x96, 0);
  }

  bool angleHysteresis (uint8_t value) const {
    return this->writeRegister(0x97, value & 0xff);
  }
  uint8_t angleHysteresis () const {
    return this->readRegister(0x97, 0);
  }


  void defaults () {
    clear();
    msgForPosA(1); // CLOSED
    msgForPosB(2); // OPEN OUTGOING
    msgForPosC(3); // OPEN INCOMING
    aesActive(false);
    eventDelaytime(0);
    ledOntime(100);
    transmitTryMax(6);
    angleMeasureInterval(1000);
    angleDefault(90); // = 180 degrees, will be multiplied by 2
    angleHysteresis(10); // = 20 degrees, will be multiplied by 2
  }
};

class As5600PinPosition : public Position {
private:
  As5600<AS5600PowerMode::LPM3> as5600;
  uint16_t _ms;
  uint16_t _angle_default;
  uint16_t _angle_hyst;
  bool asfail;
public:
  As5600PinPosition () : _ms(1000), _angle_default(180), _angle_hyst(20), asfail(false) {}
  void init () {
    as5600.init();
  }

  void setInterval(uint16_t ms) { _ms = ms; }

  void setAngleDefault(uint16_t a) { _angle_default = a; }

  void setAngleHysteris(uint16_t a) {_angle_hyst = a; }

  bool getAsFail() { return as5600.isOK() == false; }

  uint32_t interval () { return millis2ticks(_ms); }

  void measure (__attribute__((unused)) bool async=false) {
    //DPRINT(F("interval:"));DDEC(_ms);DPRINT(F(", angle def:"));DDEC(_angle_default);DPRINT(F(", angle hyst:"));DDECLN(_angle_hyst);
    as5600.measure();
    uint16_t angle = as5600.angle();
    if (angle != 0xFFFF) {
      _position = State::PosA;

      DPRINT("angle: ");DDECLN(angle);

      if (angle < (_angle_default - _angle_hyst))
        _position = State::PosC;
      if (angle >= (_angle_default - _angle_hyst) && angle < (_angle_default + _angle_hyst))
        _position = State::PosA;
      if (angle >= (_angle_default + _angle_hyst) && angle < 360)
        _position = State::PosB;

    } else {
      DPRINT(F("ERROR. Angle out of range: "));DDECLN(angle);
    }
  }
};

template <class HALTYPE,class List0Type,class List1Type,class List4Type,int PEERCOUNT>
class As5600Channel : public StateGenericChannel<As5600PinPosition,HALTYPE,List0Type,List1Type,List4Type,PEERCOUNT> {

  //Alarm to check if the AS5600 works properly
  class As5600SensorCheckAlarm : public Alarm {
    As5600Channel& ch;
  private:
    bool prev_state;
  public:
    As5600SensorCheckAlarm (As5600Channel& c) : Alarm (5), ch(c), prev_state(false) {}
    virtual ~As5600SensorCheckAlarm () {}

    void trigger (AlarmClock& clock)  {
      set(seconds2ticks(5));
      clock.add(*this);
      bool curr_state =  ch.possens.getAsFail();
      ch.setAs5600Failure(curr_state); //set the flags() correctly
      if (prev_state != curr_state) { //if the AS5600 state has changed, send info message
        ch.changed(true);
        prev_state = curr_state;
      }
    }
  } sensorcheck;

private:
  bool _asfail;
public:
  typedef StateGenericChannel<As5600PinPosition,HALTYPE,List0Type,List1Type,List4Type,PEERCOUNT> BaseChannel;

  As5600Channel () : BaseChannel(), sensorcheck(*this), _asfail(false) {};
  ~As5600Channel () {}

  void init () {
   BaseChannel::init();
   BaseChannel::possens.init();
   sysclock.add(sensorcheck);
  }

  void configChanged() {
    BaseChannel::possens.setInterval(max(this->getList1().angleMeasureInterval(), 250));
    BaseChannel::possens.setAngleDefault(this->getList1().angleDefault() * 2);
    BaseChannel::possens.setAngleHysteris(max(this->getList1().angleHysteresis() * 2, 10));
  }

  void setAs5600Failure(bool f) {
    _asfail = f;
  }

  uint8_t flags () const {
    uint8_t flags = _asfail ? 0x05 << 1 : 0x00;
    flags |= this->device().battery().low() ? 0x80 : 0x00;
    return flags;
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
  sdev.channel(1).changed(true);
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
