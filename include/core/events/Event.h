#pragma once

#include "EventHandler.h"

class Event {
public:
    template <typename T>
    void accept(const EventHandler<T> *handler, T *context);
};