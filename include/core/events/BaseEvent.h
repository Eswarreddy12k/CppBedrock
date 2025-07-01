#pragma once
#include <nlohmann/json.hpp>

class Entity;
class Message;
class EntityState;

class BaseEvent {
public:
    virtual ~BaseEvent() = default;
    virtual bool validateMessage(const Message* message, Entity* entity) {
        return true;
    }
    virtual bool execute(Entity* entity, const Message* message, EntityState* state) = 0;
    BaseEvent(const nlohmann::json& params = {}) : params_(params) {}
protected:
    nlohmann::json params_;
};