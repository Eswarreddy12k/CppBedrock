#pragma once

#include "events/Event.h"
#include "events/EventHandler.h"
#include "state/EntityState.h"
#include "pipeline/Pipeline.h"

#include <vector>

class Entity {
public:
    void start();
    void stop();
    void handleEvent(Event event);

private:
    EntityState _entityState;
    std::vector<EventHandler<EntityState>> _handlers;
    Pipeline _pipeline;
};