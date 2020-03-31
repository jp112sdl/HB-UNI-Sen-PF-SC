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
#include <sensors/As5600.h>

#define LED1_PIN                  6
#define LED2_PIN                  4
#define CONFIG_BUTTON_PIN         8

#define BATTERY_EXT               A3
#define BATTERY_EXT_EN            A2
#define BATTERY_MEASURE_INTERVAL  60UL*60*6  //every 6
#define CYCLETIME                 60UL*60*20 //every 20h


#define PEERS_PER_CHANNEL 10

using namespace as;

// define all device properties
const struct DeviceInfo PROGMEM devinfo = {
    {0xF3,0x4D,0x01},       // Device ID
    "JPPFSC0001",           // Device Serial
    {0xF3,0x4D},            // Device Model
    0x10,                   // Firmware Version
    as::DeviceType::ThreeStateSensor, // Device Type
    {0x01,0x00}             // Info Bytes
};

typedef AvrSPI<10,11,12,13> SPIType;
typedef Radio<SPIType,2> RadioType;
typedef DualStatusLed<LED2_PIN,LED1_PIN> LedType;
typedef AskSin<LedType,BatterySensorUni<BATTERY_EXT,BATTERY_EXT_EN>,RadioType> BaseHal;
class Hal : public BaseHal {
public:
  void init (const HMID& id) {
    BaseHal::init(id);
    battery.init(seconds2ticks(BATTERY_MEASURE_INTERVAL),sysclock);
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
    //msgForPosA(1); // CLOSED
    //msgForPosB(2); // OPEN OUTGOING
    //msgForPosC(3); // OPEN INCOMING
    aesActive(false);
    eventDelaytime(0);
    ledOntime(100);
    transmitTryMax(6);
    angleMeasureInterval(1000);
    angleDefault(90);    // = 180 degrees, will be multiplied by 2
    angleHysteresis(10); // = 20 degrees,  will be multiplied by 2
  }
};

class As5600PinPosition : public Position {
private:
  As5600<AS5600PowerMode::LPM3> as5600;
  uint16_t _ms;
  uint16_t _angle_default;
  uint16_t _angle_hyst;
  bool asfail;
  uint8_t lastPos;
public:
  As5600PinPosition () : _ms(1000), _angle_default(180), _angle_hyst(20), asfail(false), lastPos(0) {}

  void init ()                      { as5600.init(); }

  void setInterval(uint16_t ms)     { _ms = ms; }

  void setAngleDefault(uint16_t a)  { _angle_default = a; }

  void setAngleHysteris(uint16_t a) { _angle_hyst = a; }

  uint8_t getAsState()              { return as5600.status(); }

  uint32_t interval ()              { return millis2ticks(_ms); }

  void measure (__attribute__((unused)) bool async=false) {
    as5600.measure();
    uint16_t angle = as5600.angle();

    //DPRINT(F("AGC: "));DDEC(as5600.getAGC());DPRINT(F(", angle: "));DDEC(angle);DPRINT(F(", interval:"));DDEC(_ms);DPRINT(F(", angle def:"));DDEC(_angle_default);DPRINT(F(", angle hyst:"));DDECLN(_angle_hyst);

    if (angle != 0xFFFF) {
      _position = State::PosA;
      if (angle > (_angle_default + _angle_hyst)) _position = State::PosB;
      if (angle < (_angle_default - _angle_hyst)) _position = State::PosC;

      if (_position != lastPos) {
        DPRINT("Position changed Angle = ");DDECLN(angle);
        lastPos = _position;
      }

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
    uint8_t prev_state;
  public:
    As5600SensorCheckAlarm (As5600Channel& c) : Alarm (1), ch(c), prev_state(0) {}
    virtual ~As5600SensorCheckAlarm () {}

    void trigger (AlarmClock& clock)  {
      set(seconds2ticks(5));
      clock.add(*this);
      uint8_t curr_state = ch.possens.getAsState();
      ch.setAs5600State(curr_state); //set the flags() correctly
      if (prev_state != curr_state) { //if the AS5600 state has changed, send info message
        ch.changed(true);
        prev_state = curr_state;
      }
    }
  } sensorcheck;

private:
  uint8_t _asstate;
public:
  typedef StateGenericChannel<As5600PinPosition,HALTYPE,List0Type,List1Type,List4Type,PEERCOUNT> BaseChannel;

  As5600Channel () : BaseChannel(), sensorcheck(*this), _asstate(0) {};
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
    DPRINTLN(F("configChanged List1"));
    DPRINT(F("angleMeasureInterval() = "));DDECLN(this->getList1().angleMeasureInterval());
    DPRINT(F("angleDefault()         = "));DDECLN(this->getList1().angleDefault());
    DPRINT(F("angleHysteresis()      = "));DDECLN(this->getList1().angleHysteresis());
    DPRINT(F("msgForPosA()           = "));DDECLN(this->getList1().msgForPosA());
    DPRINT(F("msgForPosB()           = "));DDECLN(this->getList1().msgForPosB());
    DPRINT(F("msgForPosC()           = "));DDECLN(this->getList1().msgForPosC());
  }

  void setAs5600State(uint8_t s) {
    _asstate = s;
  }

  uint8_t flags () const {

    uint8_t flags = 0x00;

    switch (_asstate) {
      case 0x08:
        flags = 0x01 << 1;
      break;
      case 0x10:
        flags = 0x02 << 1;
      break;
      case 0x28:
        flags = 0x03 << 1;
      break;
      case 0x30:
        flags = 0x04 << 1;
      break;
      case 0x38:
        flags = 0x05 << 1;
      break;
    }

    flags |= this->device().battery().low() ? 0x80 : 0x00;
    return flags;
  }

};

class OperatingVoltageChannel : public Channel<Hal, List1, EmptyList, List4, PEERS_PER_CHANNEL, CFList0>, public Alarm {
  class OperatingVoltageEventMsg : public Message {
    public:
      void init(uint8_t msgcnt, uint8_t voltage) { Message::init(0x0a, msgcnt, 0x53, BCAST | RPTEN , voltage & 0xff, 0x00); }
  } msg;

  public:
    OperatingVoltageChannel () : Channel(), Alarm(10) {}
    virtual ~OperatingVoltageChannel () {}

    virtual void trigger (__attribute__ ((unused)) AlarmClock& clock) {
      tick = seconds2ticks(BATTERY_MEASURE_INTERVAL);
      msg.init(device().nextcount(), device().battery().current());
      device().broadcastEvent(msg);
      sysclock.add(*this);
    }

    void configChanged() { }

    void setup(Device<Hal, CFList0>* dev, uint8_t number, uint16_t addr) {
      Channel::setup(dev, number, addr);
      sysclock.add(*this);
    }

    uint8_t status () const { return 0; }

    uint8_t flags  () const { return 0; }
};


typedef As5600Channel<Hal,CFList0,CFList1,DefList4,PEERS_PER_CHANNEL> AS5600Channel;

class CFType : public ChannelDevice<Hal, VirtBaseChannel<Hal, CFList0>, 2, CFList0> {
  class CycleInfoAlarm : public Alarm {
    CFType& dev;
   public:
     CycleInfoAlarm (CFType& d) : Alarm (seconds2ticks(CYCLETIME)), dev(d) {}
     virtual ~CycleInfoAlarm () {}

     void trigger (AlarmClock& clock)  {
       set(seconds2ticks(CYCLETIME));
       clock.add(*this);
       dev.channel(1).changed(true);
     }
   } cycle;

  public:
    VirtChannel<Hal, AS5600Channel          , CFList0>  ch1;
    VirtChannel<Hal, OperatingVoltageChannel, CFList0>  ch2;
  public:
    typedef ChannelDevice<Hal, VirtBaseChannel<Hal, CFList0>, 2, CFList0> DeviceType;

    CFType (const DeviceInfo& info, uint16_t addr) : DeviceType(info, addr), cycle(*this) {
      DeviceType::registerChannel(ch1, 1);
      DeviceType::registerChannel(ch2, 2);
    }
    virtual ~CFType () {}

    AS5600Channel&           channel1 () { return ch1; }

    OperatingVoltageChannel& channel2 () { return ch2; }

    virtual void configChanged () {
      DeviceType::configChanged();

      // set battery low/critical values
      uint8_t lb = max(10, this->getList0().lowBatLimit());
      DPRINT("LOWBAT ");DDECLN(lb);
      battery().low(lb);

      if( this->getList0().cycleInfoMsg() == true ) {
        DPRINTLN(F("Activate Cycle Msg"));
        sysclock.cancel(cycle);
        cycle.set(seconds2ticks(CYCLETIME));
        sysclock.add(cycle);
      }
      else {
        DPRINTLN(F("Deactivate Cycle Msg"));
        sysclock.cancel(cycle);
      }

    }
};

CFType sdev(devinfo,0x20);
ConfigButton<CFType> cfgBtn(sdev);

void setup () {
  DINIT(57600,ASKSIN_PLUS_PLUS_IDENTIFIER);
  sdev.init(hal);
  buttonISR(cfgBtn,CONFIG_BUTTON_PIN);
  sdev.channel1().init();
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
