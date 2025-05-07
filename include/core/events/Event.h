#pragma once
#include "../state/EntityState.h"

class Entity;
class Message;
class EntityState;

class Event {
public:
    virtual ~Event() = default;
    // For protocol-agnostic events
    virtual bool execute(Entity* entity, const Message* message, EntityState* state) { return false; }
};
