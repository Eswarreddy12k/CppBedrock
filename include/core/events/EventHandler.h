#pragma once

#include "Event.h"

template <typename T>
class EventHandler {
public:
    virtual ~EventHandler() = default;
    virtual void handleEvent(const Event* event, T* context) = 0;
};