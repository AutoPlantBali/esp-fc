#pragma once

// https://www.raspberrypi.com/documentation/pico-sdk/high_level.html#queue
// https://techtutorialsx.com/2017/08/20/esp32-arduino-freertos-queues/
// https://www.freertos.org/a00116.html

#include "Utils/MemoryHelper.h"

namespace Espfc {

enum EventType
{
  EVENT_IDLE,
  EVENT_GYRO_READ,
  EVENT_ACCEL_READ,
  EVENT_DISARM,
};

class Event
{
  public:
    Event(): type(EVENT_IDLE) {}
    Event(EventType t): type(t) {}
    Event(const Event& e) = default;
  public:
    EventType type;
};

}

#if defined(ARCH_RP2040)
  #include <pico/util/queue.h>
  typedef queue_t TargetQueueHandle;
#elif defined(ESPFC_FREE_RTOS_QUEUE)
  #include <freertos/queue.h>
  typedef QueueHandle_t TargetQueueHandle;
#elif defined(ESPFC_ATOMIC_QUEUE)
  #include "QueueAtomic.h"
  typedef Espfc::QueueAtomic<Espfc::Event, 63> TargetQueueHandle;
#elif defined(UNIT_TEST) || !defined(ESPFC_MULTI_CORE)
  typedef int TargetQueueHandle;
#else
  #error "Not yet implelented multicore queue"
#endif

namespace Espfc {

namespace Target {

class Queue
{
  public:
    void begin();
    void send(const Event& e) FAST_CODE_ATTR;
    Event receive() FAST_CODE_ATTR;
    bool isEmpty() const FAST_CODE_ATTR;
    bool isFull() const FAST_CODE_ATTR;

  private:
    TargetQueueHandle _q;
};

}

}
