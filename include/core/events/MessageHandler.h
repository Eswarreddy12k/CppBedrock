#pragma once

class Entity;
class Message;
class EntityState;

class MessageHandler {
public:
    virtual ~MessageHandler() = default;
    virtual void handle(Entity* entity, const Message* message, EntityState* context) = 0;
};