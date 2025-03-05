#pragma once

#include "Event.h"
#include "Message.h"

template <typename T>
class EventHandler {
public:
    void handleEvent(const Event *event, T *context);

    virtual void handleMessage(const Message *message, T *context) = 0;
};