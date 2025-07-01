#include "../../../include/core/events/EventFactory.h"
#include "../../../include/core/events/BaseEvent.h"
#include "../../../include/core/Entity.h"
#include <nlohmann/json.hpp>
#include <iostream>

// Example uncommon event
class UncommonEvent : public BaseEvent {
public:
    UncommonEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity*, const Message*, EntityState*) override {
        std::cout << "UncommonEvent executed\n";
        return true;
    }
};

// Protocol-specific PBFT condition check event
class VerifyPBFTConditionsEvent : public BaseEvent {
public:
    VerifyPBFTConditionsEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        
        auto j = nlohmann::json::parse(message->getContent());
        if(j["view"].get<int>() != entity->entityInfo["view"].get<int>()) {
            std::cout << "[Node " << entity->getNodeId() << "] View mismatch: expected " << entity->getState().getViewNumber() << ", got " << j["view"].get<int>() << "\n";
            return false;
        }
        return true;
    }
};

// Registration function for uncommon events
void registerUncommonEvents(EventFactory& factory) {
    factory.registerEvent<UncommonEvent>("uncommonEvent");
    factory.registerEvent<VerifyPBFTConditionsEvent>("verifyPBFTConditions");
}

// Explicit template instantiation
template void EventFactory::registerEvent<UncommonEvent>(const std::string&);
template void EventFactory::registerEvent<VerifyPBFTConditionsEvent>(const std::string&);