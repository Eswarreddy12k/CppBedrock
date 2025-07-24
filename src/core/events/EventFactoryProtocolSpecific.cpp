#include "../../../include/core/events/EventFactory.h"
#include "../../../include/core/events/BaseEvent.h"
#include "../../../include/core/Entity.h"
#include <nlohmann/json.hpp>
#include <iostream>

int computeQuorumEventFactoryProtocolSpecific(const std::string& quorumStr, int f) {
    if (quorumStr == "2f") return 2 * f;
    if (quorumStr == "2f+1") return (2 * f) + 1;
    try { return std::stoi(quorumStr); } catch (...) { return 1; }
}

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
            std::cout << "[Node " << entity->getNodeId() << "] View mismatch: expected " << entity->entityInfo["view"]<< ", got " << j["view"].get<int>() << "\n";
            return false;
        }
        entity->entityInfo["sequence"] = j["sequence"].get<int>();
        return true;
    }
};

// Event to store NewView message for HotStuff2
class StoreNewViewMessageEvent : public BaseEvent {
public:
    StoreNewViewMessageEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        auto j = nlohmann::json::parse(message->getContent());
        int view = j.value("new_view", -1);
        int sender = j.value("message_sender_id", -1);
        if (view != -1 && sender != -1) {
            entity->newViewHotstuffSenders[view].insert(sender);
            std::cout << "[Node " << entity->getNodeId() << "] Stored NewView sender " << sender << " for view " << view << std::endl;
        } else {
            std::cout << "[Node " << entity->getNodeId() << "] Invalid NewView message: " << j.dump() << std::endl;
        }
        return true;
    }
};

// Event to check if NewView quorum is reached for HotStuff2
class CheckNewViewQuorumEvent : public BaseEvent {
public:
    CheckNewViewQuorumEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        auto j = nlohmann::json::parse(message->getContent());
        int view = entity->entityInfo["view"].get<int>();
        int quorum = computeQuorumEventFactoryProtocolSpecific("2f+1", entity->getF()); // or use your config/quorum logic

        if (view != -1) {
            size_t count = entity->newViewHotstuffSenders[view].size();
            std::cout << "[Node " << entity->getNodeId() << "] NewView quorum check for view " << view
                      << ": " << count << " / " << quorum << std::endl;
            if (count >= static_cast<size_t>(quorum)) {
                std::cout << "[Node " << entity->getNodeId() << "] NewView quorum reached for view " << view << std::endl;
                // You can trigger next protocol step here if needed
                return true;
            }
        }
        return false;
    }
};

// Event to send NewView message to the next leader
class SendNewViewToNextLeaderEvent : public BaseEvent {
public:
    SendNewViewToNextLeaderEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message*, EntityState*) override {
        int currentView = entity->entityInfo["view"].get<int>();
        currentView += 1;
        entity->entityInfo["view"] = currentView;
        int nextLeader = (currentView + 1) % entity->peerPorts.size();

        nlohmann::json newViewMsg;
        newViewMsg["type"] = "NewViewforHotstuff";
        newViewMsg["new_view"] = currentView;
        newViewMsg["message_sender_id"] = entity->getNodeId();
        Message msg(newViewMsg.dump());
        entity->sendTo(nextLeader, msg);
        std::cout << "[Node " << entity->getNodeId() << "] Sent NewView message to node " << nextLeader << "\n";
        return true;
    }
};

// Registration function for uncommon events
void registerUncommonEvents(EventFactory& factory) {
    factory.registerEvent<UncommonEvent>("uncommonEvent");
    factory.registerEvent<VerifyPBFTConditionsEvent>("verifyPBFTConditions");
    factory.registerEvent<StoreNewViewMessageEvent>("storeNewViewMessage");
    factory.registerEvent<CheckNewViewQuorumEvent>("checkNewViewQuorum");
    factory.registerEvent<SendNewViewToNextLeaderEvent>("sendNewViewToNextLeader"); // <-- Add this line
}

// Explicit template instantiation
template void EventFactory::registerEvent<UncommonEvent>(const std::string&);
template void EventFactory::registerEvent<VerifyPBFTConditionsEvent>(const std::string&);
template void EventFactory::registerEvent<StoreNewViewMessageEvent>(const std::string&);
template void EventFactory::registerEvent<CheckNewViewQuorumEvent>(const std::string&);
template void EventFactory::registerEvent<SendNewViewToNextLeaderEvent>(const std::string&);