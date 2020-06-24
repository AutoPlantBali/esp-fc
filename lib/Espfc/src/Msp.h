#ifndef _ESPFC_MSP_H_
#define _ESPFC_MSP_H_

#include <Arduino.h>
#include "Model.h"
#include "Hardware.h"

extern "C" {
#include "msp/msp_protocol.h"
#include "platform.h"
}

// TODO: update to 1.37 https://github.com/betaflight/betaflight/compare/v3.2.0...v3.3.0
static const char * flightControllerIdentifier = BETAFLIGHT_IDENTIFIER;
static const char * boardIdentifier = "ESPF";

namespace Espfc {

class Msp
{
  public:
    Msp(Model& model): _model(model) {}

    bool process(char c, MspMessage& msg, MspResponse& res, Stream& s)
    {
      switch(msg.state)
      {
        case MSP_STATE_IDLE:               // sync char 1 '$'
          if(c == '$')
          {
            msg.state = MSP_STATE_HEADER_START;
          }
          break;

        case MSP_STATE_HEADER_START:       // sync char 2 'M'
            if(c == 'M') msg.state = MSP_STATE_HEADER_M;
            else msg.state = MSP_STATE_IDLE;
            break;

        case MSP_STATE_HEADER_M:               // direction (should be >)
          switch(c)
          {
            case '>':
              msg.dir = MSP_TYPE_REPLY;
              msg.state = MSP_STATE_HEADER_ARROW;
              break;
            case '<':
              msg.dir = MSP_TYPE_CMD;
              msg.state = MSP_STATE_HEADER_ARROW;
              break;
            default:
              msg.state = MSP_STATE_IDLE;
          }
          break;
        case MSP_STATE_HEADER_ARROW:
          if (c > MSP_BUF_SIZE) msg.state = MSP_STATE_IDLE;
          else
          {
            msg.expected = c;
            msg.received = 0;
            msg.read = 0;
            msg.checksum = 0;
            msg.checksum ^= c;
            msg.state = MSP_STATE_HEADER_SIZE;
          }
          break;

        case MSP_STATE_HEADER_SIZE:
          msg.cmd = c;
          msg.checksum ^= c;
          msg.state = MSP_STATE_HEADER_CMD;
          break;

        case MSP_STATE_HEADER_CMD:
          if(msg.received < msg.expected)
          {
            msg.checksum ^= c;
            msg.buffer[msg.received++] = c;
          }
          else if(msg.received >= msg.expected)
          {
            msg.state = msg.checksum == c ? MSP_STATE_RECEIVED : MSP_STATE_IDLE;
          }

        default:
          //msg.state = MSP_STATE_IDLE;
          break;
      }

      if(msg.state == MSP_STATE_RECEIVED)
      {
        debugMessage(msg);
        switch(msg.dir)
        {
          case MSP_TYPE_CMD:
            processCommand(msg, res);
            sendResponse(res, s);
            msg = MspMessage();
            res = MspResponse();
            break;
          case MSP_TYPE_REPLY:
            //processCommand(msg, s);
            break;
        }
        return true;
      }

      return msg.state != MSP_STATE_IDLE;
    }

    void processCommand(MspMessage& m, MspResponse& r)
    {
      r.cmd = m.cmd;
      r.result = 1;
      switch(m.cmd)
      {
        case MSP_API_VERSION:
          r.writeU8(MSP_PROTOCOL_VERSION);
          r.writeU8(API_VERSION_MAJOR);
          r.writeU8(API_VERSION_MINOR);
          break;

        case MSP_FC_VARIANT:
          r.writeData(flightControllerIdentifier, FLIGHT_CONTROLLER_IDENTIFIER_LENGTH);
          break;

        case MSP_FC_VERSION:
          r.writeU8(FC_VERSION_MAJOR);
          r.writeU8(FC_VERSION_MINOR);
          r.writeU8(FC_VERSION_PATCH_LEVEL);
          break;

        case MSP_BOARD_INFO:
          r.writeData(boardIdentifier, BOARD_IDENTIFIER_LENGTH);
          r.writeU16(0); // No other build targets currently have hardware revision detection.
          r.writeU8(0);  // 0 == FC
          break;

        case MSP_BUILD_INFO:
          r.writeData(buildDate, BUILD_DATE_LENGTH);
          r.writeData(buildTime, BUILD_TIME_LENGTH);
          r.writeData(shortGitRevision, GIT_SHORT_REVISION_LENGTH);
          break;

        case MSP_UID:
          {
#if defined(ESP8266)
            r.writeU32(ESP.getChipId());
            r.writeU32(ESP.getFlashChipId());
            r.writeU32(ESP.getFlashChipSize());
#else
            int64_t mac = ESP.getEfuseMac();
            r.writeU32((uint32_t)mac);
            r.writeU32((uint32_t)(mac >> 32));
            r.writeU32(0);
#endif
          }
          break;

        case MSP_STATUS_EX:
        case MSP_STATUS:
          r.writeU16(lrintf(_model.state.loopTimer.getDeltaReal() * 1000000.f));
          r.writeU16(_model.state.i2cErrorCount); // i2c error count
          //         acc,     baro,    mag,     gps,     sonar,   gyro
          r.writeU16(_model.accelActive() | _model.baroActive() << 1 | _model.magActive() << 2 | 0 << 3 | 0 << 4 | _model.gyroActive() << 5);
          r.writeU32(_model.state.modeMask); // flight mode flags
          r.writeU8(0); // pid profile
          r.writeU16(lrintf(_model.state.stats.getTotalLoad()));
          if (m.cmd == MSP_STATUS_EX) {
            r.writeU8(1); // max profile count
            r.writeU8(0); // current rate profile index
          } else {  // MSP_STATUS
            //r.writeU16(_model.state.gyroTimer.interval); // gyro cycle time
            r.writeU16(0);
          }

          // flight mode flags (above 32 bits)
          r.writeU8(0); // count

          // Write arming disable flags
          r.writeU8(ARMING_DISABLED_FLAGS_COUNT);  // 1 byte, flag count
          r.writeU32(_model.state.armingDisabledFlags);  // 4 bytes, flags
          r.writeU8(0); // rebbot required
          break;

        case MSP_NAME:
          r.writeString(_model.config.modelName);
          break;

        case MSP_SET_NAME:
          memset(&_model.config.modelName, 0, MODEL_NAME_LEN + 1);
          for(size_t i = 0; i < std::min((size_t)m.received, MODEL_NAME_LEN); i++)
          {
            _model.config.modelName[i] = m.readU8();
          }
          break;

        case MSP_BOXNAMES:
          r.writeString(F("ARM;ANGLE;AIRMODE;BUZZER;FAILSAFE;"));
          break;

        case MSP_BOXIDS:
          r.writeU8(MODE_ARMED);
          r.writeU8(MODE_ANGLE);
          r.writeU8(MODE_AIRMODE);
          r.writeU8(MODE_BUZZER);
          r.writeU8(MODE_FAILSAFE);
          break;

        case MSP_MODE_RANGES:
          for(size_t i = 0; i < ACTUATOR_CONDITIONS; i++)
          {
            r.writeU8(_model.config.conditions[i].id);
            r.writeU8(_model.config.conditions[i].ch - AXIS_AUX_1);
            r.writeU8((_model.config.conditions[i].min - 900) / 25);
            r.writeU8((_model.config.conditions[i].max - 900) / 25);
          }
          break;

        case MSP_MODE_RANGES_EXTRA:
          r.writeU8(ACTUATOR_CONDITIONS);
          for(size_t i = 0; i < ACTUATOR_CONDITIONS; i++)
          {
            r.writeU8(_model.config.conditions[i].id);
            r.writeU8(_model.config.conditions[i].logicMode);
            r.writeU8(_model.config.conditions[i].linkId);
          }

          break;

        case MSP_SET_MODE_RANGE:
          {
            size_t i = m.readU8();
            if(i < ACTUATOR_CONDITIONS)
            {
              _model.config.conditions[i].id = m.readU8();
              _model.config.conditions[i].ch = m.readU8() + AXIS_AUX_1;
              _model.config.conditions[i].min = m.readU8() * 25 + 900;
              _model.config.conditions[i].max = m.readU8() * 25 + 900;
              if(m.remain() >= 2) {
                _model.config.conditions[i].logicMode = m.readU8(); // mode logic
                _model.config.conditions[i].linkId = m.readU8(); // link to
              }
            }
            else
            {
              r.result = -1;
            }
          }
          break;

        case MSP_ANALOG:
          r.writeU8(_model.state.battery.voltage);  // voltage
          r.writeU16(0); // mah drawn
          r.writeU16(0); // rssi
          r.writeU16(0); // amperage
          r.writeU16(_model.state.battery.voltage * 10);  // voltage: TODO to volts
          break;

        case MSP_FEATURE_CONFIG:
          r.writeU32(_model.config.featureMask);
          break;

        case MSP_SET_FEATURE_CONFIG:
          _model.config.featureMask = m.readU32();
          _model.reload();
          break;

        case MSP_BATTERY_CONFIG:
          r.writeU8(34);  // vbatmincellvoltage
          r.writeU8(42);  // vbatmaxcellvoltage
          r.writeU8(_model.config.vbatCellWarning);  // vbatwarningcellvoltage // TODO to volts
          r.writeU16(0); // batteryCapacity
          r.writeU8(1);  // voltageMeterSource
          r.writeU8(0);  // currentMeterSource
          r.writeU16(340); // vbatmincellvoltage
          r.writeU16(420); // vbatmaxcellvoltage
          r.writeU16(_model.config.vbatCellWarning * 10); // vbatwarningcellvoltage // TODO to volts
          break;

        case MSP_SET_BATTERY_CONFIG:
          m.readU8();  // vbatmincellvoltage
          m.readU8();  // vbatmaxcellvoltage
          _model.config.vbatCellWarning = m.readU8();  // vbatwarningcellvoltage // TODO to volts
          m.readU16(); // batteryCapacity
          m.readU8();  // voltageMeterSource
          m.readU8();  // currentMeterSource
          if(m.remain() >= 6)
          {
            m.readU16();
            m.readU16();
            _model.config.vbatCellWarning = (m.readU16() + 5) / 10;
          }
          break;

        case MSP_BATTERY_STATE:
          // battery characteristics
          r.writeU8(_model.state.battery.cells); // cell count, 0 indicates battery not detected.
          r.writeU16(0); // capacity in mAh

          // battery state
          r.writeU8(_model.state.battery.voltage); // in 0.1V steps
          r.writeU16(0); // milliamp hours drawn from battery
          r.writeU16(0); // send current in 0.01 A steps, range is -320A to 320A

          // battery alerts
          r.writeU8(0);
          r.writeU16(_model.state.battery.voltage * 10); // FIXME: in volts
          break;

        case MSP_VOLTAGE_METERS:
          for(int i = 0; i < 1; i++)
          {
            r.writeU8(i + 10);  // meter id (10-19 vbat adc)
            r.writeU8(_model.state.battery.voltage);  // meter value
          }
          break;

        case MSP_CURRENT_METERS:
          break;

        case MSP_VOLTAGE_METER_CONFIG:
          r.writeU8(1); // num voltage sensors
          for(int i = 0; i < 1; i++)
          {
            r.writeU8(5); // frame size (5)
            r.writeU8(i + 10); // id (10-19 vbat adc)
            r.writeU8(0); // type resistor divider
            r.writeU8(_model.config.vbatScale); // scale
            r.writeU8(_model.config.vbatResDiv);  // resdivval
            r.writeU8(_model.config.vbatResMult);  // resdivmultiplier
          }
          break;

        case MSP_SET_VOLTAGE_METER_CONFIG:
          {
            int id = m.readU8();
            if(id == 10 + 0) // id (10-19 vbat adc, allow only 10)
            {
              _model.config.vbatScale = m.readU8();
              _model.config.vbatResDiv = m.readU8();
              _model.config.vbatResMult = m.readU8();
            }
          }
          break;

        case MSP_DATAFLASH_SUMMARY:
          r.writeU8(0); // FlashFS is neither ready nor supported
          r.writeU32(0);
          r.writeU32(0);
          r.writeU32(0);
          break;

        case MSP_ACC_TRIM:
          r.writeU16(0); // pitch
          r.writeU16(0); // roll
          break;

        case MSP_MIXER_CONFIG:
          r.writeU8(_model.config.mixerType); // mixerMode, QUAD_X
          r.writeU8(_model.config.yawReverse); // yaw_motors_reversed
          break;

        case MSP_SET_MIXER_CONFIG:
          _model.config.mixerType = m.readU8(); // mixerMode, QUAD_X
          _model.config.yawReverse = m.readU8(); // yaw_motors_reversed
          break;

        case MSP_SENSOR_CONFIG:
          r.writeU8(_model.config.accelDev); // 3 acc mpu6050
          r.writeU8(_model.config.baroDev);  // 2 baro bmp085
          r.writeU8(_model.config.magDev);   // 3 mag hmc5883l
          break;

        case MSP_SET_SENSOR_CONFIG:
          _model.config.accelDev = m.readU8(); // 3 acc mpu6050
          _model.config.baroDev = m.readU8();  // 2 baro bmp085
          _model.config.magDev = m.readU8();   // 3 mag hmc5883l
          _model.reload();
          break;

        case MSP_SENSOR_ALIGNMENT:
          r.writeU8(_model.config.gyroAlign); // gyro align
          r.writeU8(_model.config.gyroAlign); // acc align
          r.writeU8(_model.config.magAlign);  // mag align
          //1.41+
          r.writeU8(1); // gyro detection mask GYRO_1_MASK
          r.writeU8(_model.config.gyroAlign);
          r.writeU8(0); // default align
          break;

        case MSP_SET_SENSOR_ALIGNMENT:
          {
            uint8_t gyroAlign = m.readU8(); // gyro align
            m.readU8(); // discard deprecated acc align
            _model.config.magAlign = m.readU8(); // mag align
            if(m.remain() >= 3)
            {
              m.readU8(); // gyro to use
              _model.config.gyroAlign = m.readU8(); // gyro 1 align
              m.readU8(); // gyro 2 align
            }
            else
            {
              _model.config.gyroAlign = gyroAlign;
            }
            _model.config.accelAlign = _model.config.gyroAlign;
          }
          break;

        case MSP_CF_SERIAL_CONFIG:
          for(int i = SERIAL_UART_0; i < SERIAL_UART_COUNT; i++)
          {
            if(_model.config.serial[i].id >= 30 && !_model.isActive(FEATURE_SOFTSERIAL)) break;
            r.writeU8(_model.config.serial[i].id); // identifier
            r.writeU16(_model.config.serial[i].functionMask); // functionMask
            r.writeU8(_model.config.serial[i].baudIndex); // msp_baudrateIndex
            r.writeU8(0); // gps_baudrateIndex
            r.writeU8(0); // telemetry_baudrateIndex
            r.writeU8(_model.config.serial[i].blackboxBaudIndex); // blackbox_baudrateIndex
          }
          break;

        case MSP_SET_CF_SERIAL_CONFIG:
          {
            const int packetSize = 1 + 2 + 4;
            while(m.remain() >= packetSize)
            {
              int id = m.readU8();
#if defined(ESP32)
              if(id != SERIAL_UART_0 && id != SERIAL_UART_1 && id != SERIAL_UART_2)
#elif defined(ESP8266)
              if(id != SERIAL_UART_0 && id != SERIAL_UART_1 && id != 30)
#endif
              {
                m.advance(packetSize - 1);
                continue;
              }
              size_t k = id;
#if defined(ESP8266)
               if(id == 30) k = SERIAL_SOFT_0;
#endif
              _model.config.serial[k].id = id;
              _model.config.serial[k].functionMask = m.readU16();
              _model.config.serial[k].baudIndex = m.readU8();
              m.readU8();
              m.readU8();
              _model.config.serial[k].blackboxBaudIndex = m.readU8();
            }
          }
          _model.reload();
          break;

        case MSP_BLACKBOX_CONFIG:
          r.writeU8(1); // Blackbox supported
          r.writeU8(_model.config.blackboxDev); // device serial or none
          r.writeU8(1); // blackboxGetRateNum()); // unused
          r.writeU8(1); // blackboxGetRateDenom());
          r.writeU16(_model.config.blackboxPdenom); // p_denom
          break;

        case MSP_SET_BLACKBOX_CONFIG:
          // Don't allow config to be updated while Blackbox is logging
          if (true) {
            _model.config.blackboxDev = m.readU8();
            const int rateNum = m.readU8(); // was rate_num
            const int rateDenom = m.readU8(); // was rate_denom
            if (m.remain() >= 2) {
                _model.config.blackboxPdenom = m.readU16(); // p_denom specified, so use it directly
            } else {
                // p_denom not specified in MSP, so calculate it from old rateNum and rateDenom
                //p_denom = blackboxCalculatePDenom(rateNum, rateDenom);
                (void)(rateNum + rateDenom);
            }
          }
          break;

        case MSP_ATTITUDE:
          r.writeU16(lrintf(degrees(_model.state.angle.x) * 10.f)); // roll  [decidegrees]
          r.writeU16(lrintf(degrees(_model.state.angle.y) * 10.f)); // pitch [decidegrees]
          r.writeU16(lrintf(degrees(-_model.state.angle.z)));       // yaw   [degrees]
          break;

        case MSP_ALTITUDE:
          r.writeU32(lrintf(_model.state.baroAltitude * 100.f));    // alt [cm]
          r.writeU16(0); // vario
          break;

        case MSP_BEEPER_CONFIG:
          r.writeU32(~_model.config.buzzer.beeperMask); // beeper mask
          r.writeU8(0);  // dshot beacon tone
          r.writeU32(0); // dshot beacon off flags
          break;

        case MSP_SET_BEEPER_CONFIG:
          _model.config.buzzer.beeperMask = ~m.readU32(); // beeper mask
          break;

        case MSP_BOARD_ALIGNMENT_CONFIG:
          r.writeU16(0); // roll
          r.writeU16(0); // pitch
          r.writeU16(0); // yaw
          break;

        case MSP_RX_MAP:
          for(size_t i = 0; i < INPUT_CHANNELS; i++)
          {
            r.writeU8(_model.config.input.channel[i].map);
          }
          break;

        case MSP_RSSI_CONFIG:
          r.writeU8(0);
          break;

        case MSP_MOTOR_CONFIG:
          r.writeU16(_model.config.output.minThrottle); // minthrottle
          r.writeU16(_model.config.output.maxThrottle); // maxthrottle
          r.writeU16(_model.config.output.minCommand);  // mincommand
          r.writeU8(_model.state.currentMixer.count);   // motor count
          // 1.42+
          r.writeU8(14); // motor pole count
          r.writeU8(0); // dshot telemtery
          r.writeU8(0); // esc sensor
          break;

        case MSP_SET_MOTOR_CONFIG:
          _model.config.output.minThrottle = m.readU16(); // minthrottle
          _model.config.output.maxThrottle = m.readU16(); // maxthrottle
          _model.config.output.minCommand = m.readU16();  // mincommand
          if(m.remain() >= 2)
          {
            m.readU8();
            m.readU8();
          }
          _model.reload();
          break;

        case MSP_MOTOR_3D_CONFIG:
          r.writeU16(1406); // deadband3d_low;
          r.writeU16(1514); // deadband3d_high;
          r.writeU16(1460); // neutral3d;
          break;

        case MSP_ARMING_CONFIG:
          r.writeU8(5); // auto_disarm delay
          r.writeU8(0);  // disarm kill switch
          r.writeU8(180); // small angle
          break;

        case MSP_RC_DEADBAND:
          r.writeU8(_model.config.input.deadband);
          r.writeU8(0); // yaw deadband
          r.writeU8(0); // alt hold deadband
          r.writeU16(0); // deadband 3d throttle
          break;

        case MSP_SET_RC_DEADBAND:
          _model.config.input.deadband = m.readU8();
          m.readU8(); // yaw deadband
          m.readU8(); // alt hod deadband
          m.readU16(); // deadband 3d throttle
          break;

        case MSP_RX_CONFIG:
          r.writeU8(_model.config.input.serialRxProvider); // serialrx_provider
          r.writeU16(_model.config.input.maxCheck); //maxcheck
          r.writeU16(_model.config.input.midRc); //midrc
          r.writeU16(_model.config.input.minCheck); //mincheck
          r.writeU8(0); // spectrum bind
          r.writeU16(_model.config.input.minRc); //min_us
          r.writeU16(_model.config.input.maxRc); //max_us
          r.writeU8(_model.config.input.interpolationMode); // rc interpolation
          r.writeU8(_model.config.input.interpolationInterval); // rc interpolation interval
          r.writeU16(1500); // airmode activate threshold
          r.writeU8(0); // rx spi prot
          r.writeU32(0); // rx spi id
          r.writeU8(0); // rx spi chan count
          r.writeU8(0); // fpv camera angle
          r.writeU8(2); // rc iterpolation channels: RPYT
          r.writeU8(0); // rc_smoothing_type
          r.writeU8(0); // rc_smoothing_input_cutoff
          r.writeU8(0); // rc_smoothing_derivative_cutoff
          r.writeU8(0); // rc_smoothing_input_type
          r.writeU8(0); // rc_smoothing_derivative_type
          r.writeU8(0); // usb type
          // 1.42+
          r.writeU8(0); // rc_smoothing_auto_factor

          break;

        case MSP_SET_RX_CONFIG:
          _model.config.input.serialRxProvider = m.readU8(); // serialrx_provider
          _model.config.input.maxCheck = m.readU16(); //maxcheck
          _model.config.input.midRc = m.readU16(); //midrc
          _model.config.input.minCheck = m.readU16(); //mincheck
          m.readU8(); // spectrum bind
          _model.config.input.minRc = m.readU16(); //min_us
          _model.config.input.maxRc = m.readU16(); //max_us
          if (m.remain() >= 4) {
            _model.config.input.interpolationMode = m.readU8(); // rc interpolation
             _model.config.input.interpolationInterval = m.readU8(); // rc interpolation interval
            m.readU16(); // airmode activate threshold
          }
          if (m.remain() >= 6) {
            m.readU8(); // rx spi prot
            m.readU32(); // rx spi id
            m.readU8(); // rx spi chan count
          }
          if (m.remain() >= 1) {
            m.readU8(); // fpv camera angle
          }
          // 1.40+
          if (m.remain() >= 6) {
            m.readU8(); // rc iterpolation channels
            m.readU8(); // rc_smoothing_type
            m.readU8(); // rc_smoothing_input_cutoff
            m.readU8(); // rc_smoothing_derivative_cutoff
            m.readU8(); // rc_smoothing_input_type
            m.readU8(); // rc_smoothing_derivative_type
          }
          if (m.remain() >= 1) {
            m.readU8(); // usb type
          }
          // 1.42+
          if (m.remain() >= 1) {
            m.readU8(); // rc_smoothing_auto_factor
          }

          _model.reload();
          break;

        case MSP_FAILSAFE_CONFIG:
          r.writeU8(0); // failsafe_delay
          r.writeU8(0); // failsafe_off_delay
          r.writeU16(1000); //failsafe_throttle
          r.writeU8(0); // failsafe_kill_switch
          r.writeU16(0); // failsafe_throttle_low_delay
          r.writeU8(1); //failsafe_procedure; default drop
          break;

        case MSP_SET_FAILSAFE_CONFIG:
          m.readU8(); //failsafe_delay
          m.readU8(); //failsafe_off_delay
          m.readU16(); //failsafe_throttle
          m.readU8(); //failsafe_kill_switch
          m.readU16(); //failsafe_throttle_low_delay
          m.readU8(); //failsafe_procedure
          break;

        case MSP_RXFAIL_CONFIG:
          for (size_t i = 0; i < INPUT_CHANNELS; i++)
          {
            r.writeU8(_model.config.input.channel[i].fsMode);
            r.writeU16(_model.config.input.channel[i].fsValue);
          }
          break;

        case MSP_SET_RXFAIL_CONFIG:
          {
            size_t i = m.readU8();
            if(i < INPUT_CHANNELS)
            {
              _model.config.input.channel[i].fsMode = m.readU8(); // mode
              _model.config.input.channel[i].fsValue = m.readU16(); // pulse
            }
            else
            {
              r.result = -1;
            }
          }
          break;

        case MSP_RC:
          for(size_t i = 0; i < INPUT_CHANNELS; i++)
          {
            r.writeU16(lrintf(_model.state.inputUs[i]));
          }
          break;

        case MSP_RC_TUNING:
          r.writeU8(_model.config.input.rate[AXIS_ROLL]);
          r.writeU8(_model.config.input.expo[AXIS_ROLL]);
          for(size_t i = 0; i < 3; i++)
          {
            r.writeU8(_model.config.input.superRate[i]);
          }
          r.writeU8(_model.config.tpaScale); // dyn thr pid
          r.writeU8(50); // thrMid8
          r.writeU8(0);  // thr expo
          r.writeU16(_model.config.tpaBreakpoint); // tpa breakpoint
          r.writeU8(_model.config.input.expo[AXIS_YAW]); // yaw expo
          r.writeU8(_model.config.input.rate[AXIS_YAW]); // yaw rate
          r.writeU8(_model.config.input.rate[AXIS_PITCH]); // pitch rate
          r.writeU8(_model.config.input.expo[AXIS_PITCH]); // pitch expo
          // 1.41+
          r.writeU8(0); // throtle limit type (off)
          r.writeU8(100); // throtle limit percent (100%)
          //1.42+
          r.writeU16(1998); // rate limit roll
          r.writeU16(1998); // rate limit pitch
          r.writeU16(1998); // rate limit yaw
          // 1.43+
          //r.writeU8(0); // rates type // TODO: requires support

          break;

        case MSP_SET_RC_TUNING:
          if(m.remain() >= 10)
          {
            const uint8_t rate = m.readU8();
            if(_model.config.input.rate[AXIS_PITCH] == _model.config.input.rate[AXIS_ROLL])
            {
              _model.config.input.rate[AXIS_PITCH] = rate;
            }
            _model.config.input.rate[AXIS_ROLL] = rate;

            const uint8_t expo = m.readU8();
            if(_model.config.input.expo[AXIS_PITCH] == _model.config.input.expo[AXIS_ROLL])
            {
              _model.config.input.expo[AXIS_PITCH] = expo;
            }
            _model.config.input.expo[AXIS_ROLL] = expo;

            for(size_t i = 0; i < 3; i++)
            {
              _model.config.input.superRate[i] = m.readU8();
            }
            _model.config.tpaScale = Math::clamp(m.readU8(), (uint8_t)0, (uint8_t)90); // dyn thr pid
            m.readU8(); // thrMid8
            m.readU8();  // thr expo
            _model.config.tpaBreakpoint = Math::clamp(m.readU16(), (uint16_t)1000, (uint16_t)2000); // tpa breakpoint
            if(m.remain() >= 1)
            {
              _model.config.input.expo[AXIS_YAW] = m.readU8(); // yaw expo
            }
            if(m.remain() >= 1)
            {
              _model.config.input.rate[AXIS_YAW]  = m.readU8(); // yaw rate
            }
            if(m.remain() >= 1)
            {
              _model.config.input.rate[AXIS_PITCH] = m.readU8(); // pitch rate
            }
            if(m.remain() >= 1)
            {
              _model.config.input.expo[AXIS_PITCH]  = m.readU8(); // pitch expo
            }
          }
          else
          {
            r.result = -1;
            // error
          }
          break;

        case MSP_ADVANCED_CONFIG:
          r.writeU8(_model.config.gyroSync);
          r.writeU8(_model.config.loopSync);
          r.writeU8(_model.config.output.async);
          r.writeU8(_model.config.output.protocol);
          r.writeU16(_model.config.output.rate);
          r.writeU16(_model.config.output.dshotIdle);
          r.writeU8(0);    // 32k gyro
          r.writeU8(0);    // PWM inversion
          r.writeU8(0);    // gyro to use: {1:0, 2:1. 2:both}
          r.writeU8(0);    // gyro high fsr (flase)
          r.writeU8(48);   // gyro cal threshold
          r.writeU16(125); // gyro cal duration (1.25s)
          r.writeU16(0);   // gyro offset yaw
          r.writeU8(0);    // check overflow
          r.writeU8(_model.config.debugMode);
          r.writeU8(DEBUG_COUNT);
          break;

        case MSP_SET_ADVANCED_CONFIG:
          _model.config.gyroSync = m.readU8();
          _model.config.loopSync = m.readU8();
          _model.config.output.async = m.readU8();
          _model.config.output.protocol = m.readU8();
          _model.config.output.rate = m.readU16();
          if(m.remain() >= 2) {
            _model.config.output.dshotIdle = m.readU16(); // dshot idle
          }
          if(m.remain()) {
            m.readU8();  // 32k gyro
          }
          if(m.remain()) {
            m.readU8();  // PWM inversion
          }
          if(m.remain() >= 8) {
            m.readU8();  // gyro to use
            m.readU8();  // gyro high fsr
            m.readU8();  // gyro cal threshold
            m.readU16(); // gyro cal duration
            m.readU16(); // gyro offset yaw
            m.readU8();  // check overflow
          }
          if(m.remain()) {
            _model.config.debugMode = m.readU8();
          }
          _model.reload();
          break;

        case MSP_GPS_CONFIG:
          r.writeU8(0); // provider
          r.writeU8(0); // sbasMode
          r.writeU8(0); // autoConfig
          r.writeU8(0); // autoBaud
          // 1.43+
          //m.writeU8(0); // gps_set_home_point_once
          //m.writeU8(0); // gps_ublox_use_galileo
          break;

        case MSP_COMPASS_CONFIG:
          r.writeU16(0); // mag_declination * 10
          break;

        case MSP_FILTER_CONFIG:
          r.writeU8(_model.config.gyroFilter.freq);           // gyro lpf
          r.writeU16(_model.config.dtermFilter.freq);         // dterm lpf
          r.writeU16(_model.config.yawFilter.freq);           // yaw lpf
          r.writeU16(_model.config.gyroNotch1Filter.freq);    // gyro notch 1 hz
          r.writeU16(_model.config.gyroNotch1Filter.cutoff);  // gyro notch 1 cutoff
          r.writeU16(_model.config.dtermNotchFilter.freq);    // dterm notch hz
          r.writeU16(_model.config.dtermNotchFilter.cutoff);  // dterm notch cutoff
          r.writeU16(_model.config.gyroNotch2Filter.freq);    // gyro notch 2 hz
          r.writeU16(_model.config.gyroNotch2Filter.cutoff);  // gyro notch 2 cutoff
          r.writeU8(_model.config.dtermFilter.type);          // dterm type
          r.writeU8(_model.config.gyroDlpf == GYRO_DLPF_256 ? 0 : (_model.config.gyroDlpf == GYRO_DLPF_EX ? 1 : 2)); // dlfp type
          r.writeU8(0);                                       // dlfp 32khz type
          r.writeU16(_model.config.gyroFilter.freq);          // lowpass1 freq
          r.writeU16(_model.config.gyroFilter2.freq);         // lowpass2 freq
          r.writeU8(_model.config.gyroFilter.type);           // lowpass1 type
          r.writeU8(_model.config.gyroFilter2.type);          // lowpass2 type
          r.writeU16(_model.config.dtermFilter2.freq);        // dterm lopwass2 freq
          // 1.41+
          r.writeU8(_model.config.dtermFilter2.type);         // dterm lopwass2 type
          r.writeU16(_model.config.gyroDynLpfFilter.cutoff);  // dyn lpf gyro min
          r.writeU16(_model.config.gyroDynLpfFilter.freq);    // dyn lpf gyro max
          r.writeU16(_model.config.dtermDynLpfFilter.cutoff); // dyn lpf dterm min
          r.writeU16(_model.config.dtermDynLpfFilter.freq);   // dyn lpf dterm max
          // gyro analyse
          r.writeU8(0);  // deprecated dyn notch range
          r.writeU8(0);  // dyn_notch_width_percent
          r.writeU16(0); // dyn_notch_q
          r.writeU16(0); // dyn_notch_min_hz
          // rpm filter
          r.writeU8(0);  // gyro_rpm_notch_harmonics
          r.writeU8(0);  // gyro_rpm_notch_min
          // 1.43+
          //r.writeU16(0); // dyn_notch_max_hz

          break;

        case MSP_SET_FILTER_CONFIG:
          _model.config.gyroFilter.freq = m.readU8();
          _model.config.dtermFilter.freq = m.readU16();
          _model.config.yawFilter.freq = m.readU16();
          if (m.remain() >= 8) {
              _model.config.gyroNotch1Filter.freq = m.readU16();
              _model.config.gyroNotch1Filter.cutoff = m.readU16();
              _model.config.dtermNotchFilter.freq = m.readU16();
              _model.config.dtermNotchFilter.cutoff = m.readU16();
          }
          if (m.remain() >= 4) {
              _model.config.gyroNotch2Filter.freq = m.readU16();
              _model.config.gyroNotch2Filter.cutoff = m.readU16();
          }
          if (m.remain() >= 1) {
              _model.config.dtermFilter.type = (FilterType)m.readU8();
          }
          if (m.remain() >= 10) {
            m.readU8(); // dlfp type
            m.readU8(); // 32k dlfp type
            _model.config.gyroFilter.freq = m.readU16();
            _model.config.gyroFilter2.freq = m.readU16();
            _model.config.gyroFilter.type = m.readU8();
            _model.config.gyroFilter2.type = m.readU8();
            _model.config.dtermFilter2.freq = m.readU16();
          }
          // 1.41+
          if (m.remain() >= 9) {
            _model.config.dtermFilter2.type = m.readU8();
            _model.config.gyroDynLpfFilter.cutoff = m.readU16();  // dyn gyro lpf min
            _model.config.gyroDynLpfFilter.freq = m.readU16();    // dyn gyro lpf max
            _model.config.dtermDynLpfFilter.cutoff = m.readU16(); // dyn dterm lpf min
            _model.config.dtermDynLpfFilter.freq = m.readU16();   // dyn dterm lpf min
          }
          if (m.remain() >= 8) {
            m.readU8();  // deprecated dyn_notch_range
            m.readU8();  // dyn_notch_width_percent
            m.readU16(); // dyn_notch_q
            m.readU16(); // dyn_notch_min_hz
            m.readU8();  // gyro_rpm_notch_harmonics
            m.readU8();  // gyro_rpm_notch_min
          }
          // 1.43+
          if (m.remain() >= 1) {
            m.readU16(); // dyn_notch_max_hz
          }
          _model.reload();
          break;

        case MSP_PID_CONTROLLER:
          r.writeU8(1); // betaflight controller id
          break;

        case MSP_PIDNAMES:
          r.writeString(F("ROLL;PITCH;YAW;ALT;Pos;PosR;NavR;LEVEL;MAG;VEL;"));
          break;

        case MSP_PID:
          for(size_t i = 0; i < PID_ITEM_COUNT; i++)
          {
            r.writeU8(_model.config.pid[i].P);
            r.writeU8(_model.config.pid[i].I);
            r.writeU8(_model.config.pid[i].D);
          }
          break;

        case MSP_SET_PID:
          for (int i = 0; i < PID_ITEM_COUNT; i++)
          {
            _model.config.pid[i].P = m.readU8();
            _model.config.pid[i].I = m.readU8();
            _model.config.pid[i].D = m.readU8();
          }
          _model.reload();
          break;

        case MSP_PID_ADVANCED: /// !!!FINISHED HERE!!!
          r.writeU16(0);
          r.writeU16(0);
          r.writeU16(0); // was pidProfile.yaw_p_limit
          r.writeU8(0); // reserved
          r.writeU8(0); // vbatPidCompensation;
          r.writeU8(0); // feedForwardTransition;
          r.writeU8((uint8_t)std::min(_model.config.dtermSetpointWeight, (int16_t)255)); // was low byte of dtermSetpointWeight
          r.writeU8(0); // reserved
          r.writeU8(0); // reserved
          r.writeU8(0); // reserved
          r.writeU16(0); // rateAccelLimit;
          r.writeU16(0); // yawRateAccelLimit;
          r.writeU8(_model.config.angleLimit); // levelAngleLimit;
          r.writeU8(0); // was pidProfile.levelSensitivity
          r.writeU16(0); // itermThrottleThreshold;
          r.writeU16(0); // itermAcceleratorGain;
          r.writeU16(_model.config.dtermSetpointWeight);
          r.writeU8(0); // iterm rotation
          r.writeU8(0); // smart feed forward
          r.writeU8(0); // iterm relax
          r.writeU8(0); // iterm ralx type
          r.writeU8(0); // abs control gain
          r.writeU8(0); // throttle boost
          r.writeU8(0); // acro trainer max angle
          r.writeU16(_model.config.pid[PID_ROLL].F); //pid roll f
          r.writeU16(_model.config.pid[PID_PITCH].F); //pid pitch f
          r.writeU16(_model.config.pid[PID_YAW].F); //pid yaw f
          r.writeU8(0); // antigravity mode
          // 1.41+
          r.writeU8(0); // d min roll
          r.writeU8(0); // d min pitch
          r.writeU8(0); // d min yaw
          r.writeU8(0); // d min gain
          r.writeU8(0); // d min advance

          r.writeU8(0); // use_integrated_yaw
          r.writeU8(0); // integrated_yaw_relax
          // 1.42+
          r.writeU8(0); // iterm_relax_cutoff
          // 1.43+
          //r.writeU8(0); // motor_output_limit
          //r.writeU8(0); // auto_profile_cell_count
          //r.writeU8(0); // idle_min_rpm

          break;

        case MSP_SET_PID_ADVANCED:
          m.readU16();
          m.readU16();
          m.readU16(); // was pidProfile.yaw_p_limit
          m.readU8(); // reserved
          m.readU8();
          m.readU8();
          _model.config.dtermSetpointWeight = m.readU8();
          m.readU8(); // reserved
          m.readU8(); // reserved
          m.readU8(); // reserved
          m.readU16();
          m.readU16();
          if (m.remain() >= 2) {
              _model.config.angleLimit = m.readU8();
              m.readU8(); // was pidProfile.levelSensitivity
          }
          if (m.remain() >= 4) {
              m.readU16();
              m.readU16();
          }
          if (m.remain() >= 2) {
            _model.config.dtermSetpointWeight = m.readU16();
          }
          if (m.remain() >= 14) {
            m.readU8(); //iterm rotation
            m.readU8(); //smart feed forward
            m.readU8(); //iterm relax
            m.readU8(); //iterm ralx type
            m.readU8(); //abs control gain
            m.readU8(); //throttle boost
            m.readU8(); //acro trainer max angle
            _model.config.pid[PID_ROLL].F = m.readU16(); // pid roll f
            _model.config.pid[PID_PITCH].F = m.readU16(); // pid pitch f
            _model.config.pid[PID_YAW].F = m.readU16(); // pid yaw f
            m.readU8(); //antigravity mode
          }
          // 1.41+
          if (m.remain() >= 7) {
            m.readU8(); // d min roll
            m.readU8(); // d min pitch
            m.readU8(); // g min yaw
            m.readU8(); // d min gain
            m.readU8(); // d min advance
            m.readU8(); // use_integrated_yaw
            m.readU8(); // integrated_yaw_relax
          }
          // 1.42+
          if (m.remain() >= 1) {
            m.readU8(); // iterm_relax_cutoff
          }
          // 1.43+
          //if (m.remain() >= 3) {
          //  m.readU8(); // motor_output_limit
          //  m.readU8(); // auto_profile_cell_count
          //  m.readU8(); // idle_min_rpm
          //}
          _model.reload();
          break;

        case MSP_RAW_IMU:
          for (int i = 0; i < 3; i++)
          {
            r.writeU16(lrintf(_model.state.accel[i] * ACCEL_G_INV * 512.f));
          }
          for (int i = 0; i < 3; i++)
          {
            r.writeU16(lrintf(Math::toDeg(_model.state.gyro[i])));
          }
          for (int i = 0; i < 3; i++)
          {
            r.writeU16(lrintf(_model.state.mag[i]));
          }
          break;

        case MSP_MOTOR:
          for (size_t i = 0; i < OUTPUT_CHANNELS; i++)
          {
            if (i >= OUTPUT_CHANNELS || _model.config.pin[i + PIN_OUTPUT_0] == -1)
            {
              r.writeU16(0);
              continue;
            }
            r.writeU16(_model.state.outputUs[i]);
          }
          break;

        case MSP_SET_MOTOR:
          for(size_t i = 0; i < OUTPUT_CHANNELS; i++)
          {
            _model.state.outputDisarmed[i] = m.readU16();
          }
          break;

        case MSP_SERVO:
          for(size_t i = 0; i < OUTPUT_CHANNELS; i++)
          {
            if (i >= OUTPUT_CHANNELS || _model.config.pin[i + PIN_OUTPUT_0] == -1)
            {
              r.writeU16(1500);
              continue;
            }
            r.writeU16(_model.state.outputUs[i]);
          }
          break;

        case MSP_SERVO_CONFIGURATIONS:
          for(size_t i = 0; i < 8; i++)
          {
            if(i < OUTPUT_CHANNELS)
            {
              r.writeU16(_model.config.output.channel[i].min);
              r.writeU16(_model.config.output.channel[i].max);
              r.writeU16(_model.config.output.channel[i].neutral);
            }
            else
            {
              r.writeU16(1000);
              r.writeU16(2000);
              r.writeU16(1500);
            }
            r.writeU8(100);
            r.writeU8(-1);
            r.writeU32(0);
          }
          break;

        case MSP_SET_SERVO_CONFIGURATION:
          {
            uint8_t i = m.readU8();
            if(i < OUTPUT_CHANNELS)
            {
              _model.config.output.channel[i].min = m.readU16();
              _model.config.output.channel[i].max = m.readU16();
              _model.config.output.channel[i].neutral = m.readU16();
              m.readU8();
              m.readU8();
              m.readU32();
            }
            else
            {
              r.result = -1;
            }
          }
          break;

        case MSP_ACC_CALIBRATION:
          if(!_model.isActive(MODE_ARMED)) _model.calibrateGyro();
          break;

        case MSP_MAG_CALIBRATION:
          if(!_model.isActive(MODE_ARMED)) _model.calibrateMag();
          break;

        case MSP_VTX_CONFIG:
          r.writeU8(0xff); // vtx type unknown
          r.writeU8(0);    // band
          r.writeU8(0);    // channel
          r.writeU8(0);    // power
          r.writeU8(0);    // status
          r.writeU16(0);   // freq
          r.writeU8(0);    // ready
          r.writeU8(0);    // low power disarm
          // 1.42
          r.writeU16(0);   // pit mode freq
          r.writeU8(0);    // vtx table available (no)
          r.writeU8(0);    // vtx table bands
          r.writeU8(0);    // vtx table channels
          r.writeU8(0);    // vtx power levels
          break;

        case MSP_DEBUG:
          for (int i = 0; i < 4; i++) {
            r.writeU16(_model.state.debug[i]);
          }
          break;

        case MSP_EEPROM_WRITE:
          _model.save();
          break;

        case MSP_RESET_CONF:
          if(!_model.isActive(MODE_ARMED))
          {
            _model.reset();
          }
          break;

        case MSP_REBOOT:
          _reboot = true;
          break;

        default:
          r.result = 0;
          break;
      }
    }

    void sendResponse(MspResponse& r, Stream& s)
    {
      debugResponse(r);

      uint8_t hdr[5] = { '$', 'M', '>' };
      if(r.result == -1)
      {
        hdr[2] = '!'; // error ??
      }
      hdr[3] = r.len;
      hdr[4] = r.cmd;
      uint8_t checksum = crc(0, &hdr[3], 2);
      s.write(hdr, 5);
      if(r.len > 0)
      {
        s.write(r.data, r.len);
        checksum = crc(checksum, r.data, r.len);
      }
      s.write(checksum);
      postCommand();
    }

    void postCommand()
    {
      if(_reboot)
      {
        Hardware::restart(_model);
      }
    }

    uint8_t crc(uint8_t checksum, const uint8_t *data, int len)
    {
      while (len-- > 0)
      {
        checksum ^= *data++;
      }
      return checksum;
    }

    bool debugSkip(uint8_t cmd)
    {
      //return true;
      //return false;
      if(cmd == MSP_STATUS) return true;
      if(cmd == MSP_STATUS_EX) return true;
      if(cmd == MSP_BOXNAMES) return true;
      if(cmd == MSP_ANALOG) return true;
      if(cmd == MSP_ATTITUDE) return true;
      if(cmd == MSP_ALTITUDE) return true;
      if(cmd == MSP_RC) return true;
      if(cmd == MSP_RAW_IMU) return true;
      if(cmd == MSP_MOTOR) return true;
      if(cmd == MSP_SERVO) return true;
      if(cmd == MSP_BATTERY_STATE) return true;
      if(cmd == MSP_VOLTAGE_METERS) return true;
      if(cmd == MSP_CURRENT_METERS) return true;
      return false;
    }

    void debugMessage(const MspMessage& m)
    {
      if(debugSkip(m.cmd)) return;
      Stream * s = _model.getSerialStream(SERIAL_FUNCTION_TELEMETRY_HOTT);
      if(!s) return;

      s->print(m.dir == MSP_TYPE_REPLY ? '>' : '<'); s->print(' ');
      s->print(m.cmd); s->print(' ');
      s->print(m.expected); s->print(' ');
      for(size_t i = 0; i < m.expected; i++)
      {
        s->print(m.buffer[i]); s->print(' ');
      }
      s->println();
    }

    void debugResponse(const MspResponse& r)
    {
      if(debugSkip(r.cmd)) return;
      Stream * s = _model.getSerialStream(SERIAL_FUNCTION_TELEMETRY_HOTT);
      if(!s) return;

      s->print(r.result == 1 ? '>' : (r.result == -1 ? '!' : '@')); s->print(' ');
      s->print(r.cmd); s->print(' ');
      s->print(r.len); s->print(' ');
      for(size_t i = 0; i < r.len; i++)
      {
        s->print(r.data[i]); s->print(' ');
      }
      s->println();
    }

  private:
    Model& _model;
    bool _reboot;
};

}

#endif
