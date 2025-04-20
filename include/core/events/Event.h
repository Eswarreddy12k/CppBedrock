#pragma once
#include "../state/EntityState.h"

class Event {
public:
    virtual ~Event() = default;
    virtual void execute(EntityState& state) = 0;
};
