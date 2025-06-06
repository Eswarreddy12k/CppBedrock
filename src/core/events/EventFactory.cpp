#include "../../../include/core/events/EventFactory.h"
#include "../../../include/core/Entity.h"
#include <nlohmann/json.hpp>
#include <iostream>

int computeQuorumEventFactory(const std::string& quorumStr, int f) {
    if (quorumStr == "2f") return 2 * f;
    if (quorumStr == "2f+1") return 2 * f + 1;
    try { return std::stoi(quorumStr); } catch (...) { return 1; }
}

// Example derived event class for incrementing sequence
class IncrementSequenceEvent : public Event {
public:
    bool execute(Entity*, const Message*, EntityState* state) override {
        state->incrementSequenceNumber();
        std::cout << "Sequence incremented" << std::endl;
        return true;
    }
};

class AddLogEvent : public Event {
public:
    bool execute(Entity*, const Message*, EntityState*) override {
        std::cout << "Log entry added" << std::endl;
        return true;
    }
};

class UpdateLogEvent : public Event {
public:
    bool execute(Entity*, const Message*, EntityState*) override {
        std::cout << "Log entry updated" << std::endl;
        return true;
    }
};

class SendToClientEvent : public Event {
public:
    bool execute(Entity*, const Message*, EntityState*) override {
        std::cout << "Sent to client" << std::endl;
        return true;
    }
};

// Base Event Classes
class BaseEvent : public Event {
protected:
    bool validateMessage(const Message* message, Entity* entity) {
        try {
            auto j = nlohmann::json::parse(message->getContent());
            return j.contains("sequence") && j.contains("sender");
        } catch(...) {
            return false;
        }
    }
};

// Protocol Events
class StoreMessageEvent : public BaseEvent {
public:
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        if (!validateMessage(message, entity)) return false;
        auto j = nlohmann::json::parse(message->getContent());

        // If combinedMessages is present, store each message in the array
        if (j.contains("combinedMessages") && j["combinedMessages"].is_array()) {
            for (const auto& msg : j["combinedMessages"]) {
                int seq = j["sequence"].get<int>(); // Use the outer sequence for all, or msg["sequence"] if each has its own
                std::string operation = j["operation"].get<std::string>();
                int senderId = msg.get<int>(); // If your set stores senderId, otherwise adjust as needed
                std::string currentPhase = j["type"];

                if (currentPhase == "PrePrepare") {
                    if (!entity->prePrepareMessages[seq].count(senderId)) {
                        entity->prePrepareMessages[seq].insert(senderId);
                        if (!operation.empty()) entity->prePrepareOperations[seq] = operation;
                    }
                } else if (currentPhase == "prepare") {
                    if (!entity->prepareMessages[seq].count(senderId)) {
                        entity->prepareMessages[seq].insert(senderId);
                        if (!operation.empty()) entity->prepareOperations[seq] = operation;
                    }
                } else if (currentPhase == "commit") {
                    if (!entity->commitMessages[seq].count(senderId)) {
                        entity->commitMessages[seq].insert(senderId);
                        if (!operation.empty()) entity->commitOperations[seq] = operation;
                    }
                }
            }
            return true;
        }

        // Fallback: single message storage (original logic)
        int seq = j["sequence"].get<int>();
        std::string operation = j["operation"].get<std::string>();
        int senderId = j["sender"].get<int>();
        std::string currentPhase = j["type"];

        if (currentPhase == "PrePrepare") {
            if (!entity->prePrepareMessages[seq].count(senderId)) {
                entity->prePrepareMessages[seq].insert(senderId);
                if (!operation.empty()) entity->prePrepareOperations[seq] = operation;
            } else {
                return false; // Duplicate, stop further actions
            }
        } 
        else if (currentPhase == "prepare") {
            if (!entity->prepareMessages[seq].count(senderId)) {
                entity->prepareMessages[seq].insert(senderId);
                if (!operation.empty()) entity->prepareOperations[seq] = operation;
            } else {
                return false; // Duplicate, stop further actions
            }
        }
        else if (currentPhase == "commit") {
            if(entity->commitMessages[seq].size() >= computeQuorumEventFactory("2f+1", entity->getF())) {
                return false; // Quorum already met, stop further actions
            }
            if (!entity->commitMessages[seq].count(senderId)) {
                entity->commitMessages[seq].insert(senderId);
                if (!operation.empty()) entity->commitOperations[seq] = operation;
            } else {
                return false; // Duplicate, stop further actions
            }
        }
        return true;
    }
};

class ManageTimerEvent : public BaseEvent {
public:
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        auto j = nlohmann::json::parse(message->getContent());
        std::string currentPhase = j["type"];
        if (currentPhase == "PrePrepare") {
            std::cout << "[Node " << entity->getNodeId() << "] Starting timer for PrePrepare" << std::endl;
            if (entity->timeKeeper) entity->timeKeeper->start();
        } else {
            if (entity->timeKeeper) entity->timeKeeper->reset();
        }
        return true;
    }
};

class CheckQuorumEvent : public BaseEvent {
public:
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        if (!validateMessage(message, entity)) return false;
        auto j = nlohmann::json::parse(message->getContent());
        int seq = j["sequence"].get<int>();
        std::string currentPhase = state->getState();
        YAML::Node phaseConfig = entity->getPhaseConfig(currentPhase);
        if (!phaseConfig || !phaseConfig["quorum"]) return false;
        std::string quorumStr = phaseConfig["quorum"].as<std::string>();
        int quorum = computeQuorumEventFactory(quorumStr, entity->getF());
        bool quorumMet = true;
        //std::cout << "[Node " << entity->getNodeId() << "] Checking quorum for " << currentPhase << " for seq " << seq << std::endl;
        if (currentPhase == "prepare") {
            quorumMet = entity->prepareMessages[seq].size() >= quorum;
            if (!quorumMet){
                //std::cout << "[Node " << entity->getNodeId() << "] Prepare quorum not met for seq " << seq << std::endl;
                return false;
            }
            
        }
        else if (currentPhase == "commit") {
            quorumMet = entity->commitMessages[seq].size() >= quorum;
            if (!quorumMet){
                //std::cout << "[Node " << entity->getNodeId() << "] commit quorum not met for seq " << seq << std::endl;
                return false;
            }
            
        }
        if (quorumMet && phaseConfig["next_state"]) {
            std::string nextState = phaseConfig["next_state"].as<std::string>();
            //state->setState(nextState);
            //std::cout << "[Node " << entity->getNodeId() << "] Transitioning to " << nextState << " for seq " << seq << std::endl;
        }
        return quorumMet;
    }
};

class BroadcastEvent : public BaseEvent {
public:
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        auto j = nlohmann::json::parse(message->getContent());
        int seq = j["sequence"].get<int>();

        YAML::Node phaseConfig = entity->getPhaseConfig(j["type"]);
        if (phaseConfig["next_state"]) {
            std::string nextPhase = phaseConfig["next_state"].as<std::string>();
            nlohmann::json outMsg;
            outMsg["type"] = nextPhase;
            outMsg["view"] = state->getViewNumber();
            outMsg["sequence"] = seq;
            outMsg["operation"] = j["operation"];
            outMsg["sender"] = entity->getNodeId();

            class ProtocolMessage : public Message {
            public:
                ProtocolMessage(const std::string& content) : Message(content) {}
                bool execute(Entity*, const Message*, EntityState*) override { return true; }
            };
            ProtocolMessage protocolMsg(outMsg.dump());
            entity->sendToAll(protocolMsg);
        }
        return true;
    }
};

class CompleteEvent : public BaseEvent {
public:
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        auto j = nlohmann::json::parse(message->getContent());
        std::string operation = j["operation"].get<std::string>();
        int seq = j["sequence"].get<int>();
        std::cout << "[Node " << entity->getNodeId() << "] Completed operation: " << operation << " for seq " << seq << "\n";
        entity->markOperationProcessed(seq);
        //entity->removeSequenceState(seq);
        return true;
    }
};


class BroadcastIfLeaderEvent : public BaseEvent {
public:
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        if (entity->getState().getRole() == "Leader") {
            auto j = nlohmann::json::parse(message->getContent());
            int seq = j["sequence"].get<int>();
            // if already processed, skip
            if (entity->processedOperations.find(seq) != entity->processedOperations.end()) {
                //std::cout << "[Node " << entity->getNodeId() << "] Already processed seq " << seq << ", skipping broadcast.\n";
                return false;
            }
            std::string phase = j["type"];
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Combine all messages for this phase and sequence
            nlohmann::json combinedMessages = nlohmann::json::array();
            if (phase == "PrePrepare") {
                for (const auto& msg : entity->prePrepareMessages[seq]) {
                    combinedMessages.push_back(msg);
                }
            } else if (phase == "prepare") {
                for (const auto& msg : entity->prepareMessages[seq]) {
                    combinedMessages.push_back(msg);
                }
            } else if (phase == "commit") {
                for (const auto& msg : entity->commitMessages[seq]) {
                    combinedMessages.push_back(msg);
                }
            }
            std::cout << "[Node " << entity->getNodeId() << "] Combined messages for " << phase << " for seq " << seq << ": " << combinedMessages.dump() << "\n";
            std::cout << "[Node " << entity->getNodeId() << "] Broadcasting combined messages for " << phase << " for seq " << seq << "\n";
            // Prepare the broadcast message
            YAML::Node phaseConfig = entity->getPhaseConfig(j["type"]);
            if (phaseConfig["next_state"]) {
                //std::string nextPhase = phaseConfig["next_state"].as<std::string>();
                //std::cout << "[Node " << entity->getNodeId() << "] Broadcasting to next phase: " << nextPhase << "\n";
                nlohmann::json outMsg;
                outMsg["type"] = j["type"];
                outMsg["view"] = state->getViewNumber();
                outMsg["sequence"] = seq;
                outMsg["operation"] = j["operation"];
                outMsg["sender"] = entity->getNodeId();
                outMsg["combinedMessages"] = combinedMessages;

                Message protocolMsg(outMsg.dump());

                // Optionally, call the existing BroadcastEvent logic
                // Or just send directly:
                entity->sendToAll(protocolMsg);
            }
        }
        return true;
    }
};

class UnicastIfParticipantEvent : public BaseEvent {
public:
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        if (entity->getState().getRole() != "Leader") {
            // Your unicast logic here (send to leader or specific node)
            auto j = nlohmann::json::parse(message->getContent());
            int seq = j["sequence"].get<int>();
            YAML::Node phaseConfig = entity->getPhaseConfig(j["type"]);
            if (phaseConfig["next_state"]) {
                std::string nextPhase = phaseConfig["next_state"].as<std::string>();
                nlohmann::json outMsg;
                outMsg["type"] = nextPhase;
                outMsg["view"] = state->getViewNumber();
                outMsg["sequence"] = seq;
                outMsg["operation"] = j["operation"];
                outMsg["sender"] = entity->getNodeId();
                Message protocolMsg(outMsg.dump());
                // Assuming you have a method to get leader ID
                int leaderId = (state->getViewNumber()+1) % (entity->peerPorts.size());
                std::cout << "[Node " << entity->getNodeId() << "] Unicasting to leader: " << leaderId << " phase:" << nextPhase << "\n";
                entity->sendTo(leaderId, protocolMsg);
            }
        }
        return true;
    }
};

// ViewChange Event
class HandleViewChangeEvent : public BaseEvent {
public:
    bool execute(Entity* entity, const Message* message, EntityState*) override {
        auto j = nlohmann::json::parse(message->getContent());
        int newView = j["new_view"].get<int>();
        int senderId = j["sender"].get<int>();
        entity->viewChangeMessages[newView].insert(senderId);

        int quorum = computeQuorumEventFactory("2f+1", entity->getF());
        if ((int)entity->viewChangeMessages[newView].size() >= quorum) {
            std::cout << "[Node " << entity->getNodeId() << "] View change quorum reached for view " << newView << "\n";
            entity->getState().setViewNumber(newView);
            entity->inViewChange = false;

            int leaderId = newView % (entity->peerPorts.size() + 1);
            if (entity->getNodeId() == leaderId) {
                nlohmann::json newViewMsg;
                newViewMsg["type"] = "NewView";
                newViewMsg["new_view"] = newView;
                newViewMsg["sender"] = entity->getNodeId();
                Message msg(newViewMsg.dump());
                entity->sendToAll(msg);

                std::lock_guard<std::mutex> lock(entity->timerMtx);
                if (entity->timeKeeper) {
                    entity->timeKeeper->stop();
                    entity->timeKeeper.reset();
                }

                // Re-propose all uncommitted sequences
                for (const auto& [seq, state] : entity->sequenceStates) {
                    if (entity->commitMessages[seq].size() < computeQuorumEventFactory("2f+1", entity->getF())) {
                        std::string operation;
                        if (entity->prePrepareOperations.count(seq))
                            operation = entity->prePrepareOperations[seq];
                        else if (entity->prepareOperations.count(seq))
                            operation = entity->prepareOperations[seq];
                        else if (entity->commitOperations.count(seq))
                            operation = entity->commitOperations[seq];
                        else
                            continue;

                        nlohmann::json preprepareMsg;
                        preprepareMsg["type"] = "PrePrepare";
                        preprepareMsg["view"] = entity->getState().getViewNumber();
                        preprepareMsg["sequence"] = seq;
                        preprepareMsg["operation"] = operation;
                        preprepareMsg["sender"] = entity->getNodeId();

                        Message protocolMsg(preprepareMsg.dump());
                        entity->sendToAll(protocolMsg);
                        std::cout << "[Node " << entity->getNodeId() << "] Re-proposed PrePrepare for seq " << seq << " in view " << entity->getState().getViewNumber() << "\n";
                    }
                }
            }
        }
        return true;
    }
};

// NewView Event
class HandleNewViewEvent : public BaseEvent {
public:
    bool execute(Entity* entity, const Message* message, EntityState*) override {
        auto j = nlohmann::json::parse(message->getContent());
        int newView = j["new_view"].get<int>();
        std::cout << "[Node " << entity->getNodeId() << "] Received NewView for view " << newView << "\n";
        entity->getState().setViewNumber(newView);
        entity->inViewChange = false;

        std::lock_guard<std::mutex> lock(entity->timerMtx);
        if (entity->timeKeeper) {
            entity->timeKeeper->stop();
            entity->timeKeeper.reset();
        }
        entity->viewChangeMessages.erase(newView);
        return true;
    }
};

// Singleton instance of the EventFactory
EventFactory& EventFactory::getInstance() {
    static EventFactory instance;
    return instance;
}

// Register event dynamically (this is defined in the .cpp file)
template<typename T>
void EventFactory::registerEvent(const std::string& name) {
    factoryMap[name] = []() { return std::make_unique<T>(); };
}

// Create event based on name
std::unique_ptr<Event> EventFactory::createEvent(const std::string& name) {
    auto it = factoryMap.find(name);
    if (it != factoryMap.end()) {
        return it->second();
    }
    return nullptr;
}

// Register all events
void EventFactory::initialize() {
    // Register base protocol events
    this->registerEvent<StoreMessageEvent>("storeMessage");
    this->registerEvent<CheckQuorumEvent>("checkQuorum");
    this->registerEvent<BroadcastEvent>("broadcast");
    this->registerEvent<ManageTimerEvent>("manageTimer");
    this->registerEvent<CompleteEvent>("complete");
    
    // Register handle events
    
    this->registerEvent<HandleViewChangeEvent>("handleViewChange");
    this->registerEvent<HandleNewViewEvent>("handleNewView");
    this->registerEvent<BroadcastIfLeaderEvent>("broadcastifLeader");
    this->registerEvent<UnicastIfParticipantEvent>("unicastifParticipant");
}

// Explicit template instantiations to avoid linker errors (for all events you register)
template void EventFactory::registerEvent<IncrementSequenceEvent>(const std::string&);
template void EventFactory::registerEvent<AddLogEvent>(const std::string&);
template void EventFactory::registerEvent<UpdateLogEvent>(const std::string&);
template void EventFactory::registerEvent<SendToClientEvent>(const std::string&);

template void EventFactory::registerEvent<HandleViewChangeEvent>(const std::string&);
template void EventFactory::registerEvent<HandleNewViewEvent>(const std::string&);
template void EventFactory::registerEvent<StoreMessageEvent>(const std::string&);
template void EventFactory::registerEvent<ManageTimerEvent>(const std::string&);
template void EventFactory::registerEvent<CheckQuorumEvent>(const std::string&);
template void EventFactory::registerEvent<BroadcastEvent>(const std::string&);
template void EventFactory::registerEvent<CompleteEvent>(const std::string&);
template void EventFactory::registerEvent<BroadcastIfLeaderEvent>(const std::string&);
template void EventFactory::registerEvent<UnicastIfParticipantEvent>(const std::string&);

#include "../../../include/core/Entity.h" // or the header where computeQuorumEventFactory is defined

