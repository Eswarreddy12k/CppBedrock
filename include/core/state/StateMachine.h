#pragma once

#include "../events/EventHandler.h"
#include "../events/Message.h"
#include "EntityState.h"

class StateMachine : public EventHandler<EntityState> {
public:
    void handleMessage(const Message *message, EntityState *context) override;
};