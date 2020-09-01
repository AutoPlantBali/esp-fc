#ifndef _ESPFC_OUTPUT_MIXER_H_
#define _ESPFC_OUTPUT_MIXER_H_

#include "Model.h"
#include "Hardware.h"
#include "Output/Mixers.h"
#include "EscDriver.h"

namespace Espfc {

namespace Output {

class Mixer
{
  public:
    Mixer(Model& model): _model(model), _motor(NULL), _servo(NULL) {}

    int begin()
    {
      _motor = Hardware::getMotorDriver(_model);
      _servo = Hardware::getServoDriver(_model);

      _model.state.minThrottle = _model.config.output.minThrottle;
      _model.state.maxThrottle = _model.config.output.maxThrottle;
      _model.state.digitalOutput = _model.config.output.protocol >= ESC_PROTOCOL_DSHOT150;
      if(_model.state.digitalOutput)
      {
        _model.state.minThrottle = (_model.config.output.dshotIdle * 0.1f) + 1001.f;
        _model.state.maxThrottle = 2000.f;
      }
      _model.state.currentMixer = Mixers::getMixer((MixerType)_model.config.mixerType, _model.state.customMixer);
      return 1;
    }

    int update()
    {
      float outputs[OUTPUT_CHANNELS];
      const MixerConfig& mixer = _model.state.currentMixer;

      updateMixer(mixer, outputs);
      writeOutput(outputs, mixer.count);

      return 1;
    }

  private:
    void updateMixer(const MixerConfig& mixer, float * outputs)
    {
      Stats::Measure mixerMeasure(_model.state.stats, COUNTER_MIXER);

      float sources[MIXER_SOURCE_MAX];
      sources[MIXER_SOURCE_NULL]   = 0;

      sources[MIXER_SOURCE_ROLL]   = _model.state.output[AXIS_ROLL];
      sources[MIXER_SOURCE_PITCH]  = _model.state.output[AXIS_PITCH];
      sources[MIXER_SOURCE_YAW]    = _model.state.output[AXIS_YAW] * (_model.config.yawReverse ? 1.f : -1.f);
      sources[MIXER_SOURCE_THRUST] = _model.state.output[AXIS_THRUST];

      sources[MIXER_SOURCE_RC_ROLL]   = _model.state.input[AXIS_ROLL];
      sources[MIXER_SOURCE_RC_PITCH]  = _model.state.input[AXIS_PITCH];
      sources[MIXER_SOURCE_RC_YAW]    = _model.state.input[AXIS_YAW];
      sources[MIXER_SOURCE_RC_THRUST] = _model.state.input[AXIS_THRUST];

      sources[MIXER_SOURCE_RC_AUX1] = _model.state.input[AXIS_AUX_1];
      sources[MIXER_SOURCE_RC_AUX2] = _model.state.input[AXIS_AUX_2];
      sources[MIXER_SOURCE_RC_AUX3] = _model.state.input[AXIS_AUX_3];
      
      for(size_t i = 0; i < OUTPUT_CHANNELS; i++)
      {
        outputs[i] = 0.f;
      }

      // mix stabilized sources first
      const MixerEntry * entry = mixer.mixes;
      const MixerEntry * end = mixer.mixes + MIXER_RULE_MAX;
      while(entry != end)
      {
        if(entry->src == MIXER_SOURCE_NULL) break; // break on terminator
        if(entry->src <= MIXER_SOURCE_YAW && entry->dst < mixer.count && entry->rate != 0)
        {
          outputs[entry->dst] += sources[entry->src] * (entry->rate * 0.01f);
        }
        entry++;
      }

      // airmode logic
      float thrust = sources[MIXER_SOURCE_THRUST];
      if(_model.isAirModeActive())
      {
        float min = 0.f, max = 0.f;
        for(size_t i = 0; i < OUTPUT_CHANNELS; i++)
        {
          max = std::max(max, outputs[i]);
          min = std::min(min, outputs[i]);
        }
        float range = (max - min) * 0.5f;
        if(range > 1.f)
        {
          for(size_t i = 0; i < OUTPUT_CHANNELS; i++)
          {
            outputs[i] /= range;
          }
          thrust = 0.f;
        }
        else
        {
          thrust = constrain(thrust, -1.f + range, 1.f - range);
        }
      }

      // apply other channels
      entry = mixer.mixes;
      while(entry != end)
      {
        if(entry->src == MIXER_SOURCE_NULL) break; // break on terminator
        if(entry->dst < mixer.count)
        {
          if(entry->src == MIXER_SOURCE_THRUST)
          {
            outputs[entry->dst] += thrust * (entry->rate * 0.01f);
          }
          else if(entry->src > MIXER_SOURCE_THRUST && entry->src < MIXER_SOURCE_MAX)
          {
            outputs[entry->dst] += sources[entry->src] * (entry->rate * 0.01f);
          }
        }
        entry++;
      }     
    }

    void writeOutput(float * out, size_t axes)
    {
      Stats::Measure mixerMeasure(_model.state.stats, COUNTER_MIXER_WRITE);

      bool stop = _stop();
      for(size_t i = 0; i < OUTPUT_CHANNELS; i++)
      {
        const OutputChannelConfig& och = _model.config.output.channel[i];
        if(i >= axes || stop)
        {
          _model.state.outputUs[i] = och.servo && _model.state.outputDisarmed[i] == 1000 ? och.neutral : _model.state.outputDisarmed[i];
        }
        else
        {
          if(och.servo)
          {
            const int16_t tmp = lrintf(Math::map3(out[i], -1.f, 0.f, 1.f, och.reverse ? 2000 : 1000, och.neutral, och.reverse ? 1000 : 2000));
            _model.state.outputUs[i] = constrain(tmp, och.min, och.max);
          }
          else
          {
            float v = constrain(out[i], -1.f, 1.f);
            _model.state.outputUs[i] = lrintf(Math::map(v, -1.f, 1.f, _model.state.minThrottle, _model.state.maxThrottle));
          }
        }
      }
      _write();
    }

    void _write()
    {
      for(size_t i = 0; i < OUTPUT_CHANNELS; i++)
      {
        const OutputChannelConfig& och = _model.config.output.channel[i];
        if(och.servo)
        {
          if(_servo) _servo->write(i, _model.state.outputUs[i]);
        }
        else
        {
          if(_motor) _motor->write(i, _model.state.outputUs[i]);
        }
      }
      if(_motor) _motor->apply();
      if(_servo) _servo->apply();
    }

    bool _stop(void)
    {
      if(!_model.isActive(MODE_ARMED)) return true;
      if(_model.isActive(FEATURE_MOTOR_STOP) && _model.isThrottleLow()) return true;
      return false;
    }

    Model& _model;
    EscDriver * _motor;
    EscDriver * _servo;
};

}

}

#endif