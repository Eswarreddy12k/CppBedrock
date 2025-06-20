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
        
        //std::cout << "[Node " << entity->getNodeId() << "] Storing message: " << j.dump() << std::endl;
        // If combinedMessages is present, store each message in the array
        if (j.contains("combinedMessages") && j["combinedMessages"].is_array()) {
            for (const auto& msg : j["combinedMessages"]) {
                int seq = j["sequence"].get<int>(); // Use the outer sequence for all, or msg["sequence"] if each has its own
                std::string operation = j["operation"].get<std::string>();
                int senderId = msg.get<int>(); // If your set stores senderId, otherwise adjust as needed
                std::string currentPhase = j["type"];
                std::string protocolName = "Unknown - will be set later"; // Placeholder for protocol name
                ProtocolMessageRecord record{seq, senderId, operation, currentPhase, protocolName};
                entity->allMessagesBySeq[seq].push_back(record);
                YAML::Node phaseConfig = entity->getPhaseConfig(currentPhase);
                if (currentPhase == "commit" && phaseConfig["next_state"].as<std::string>()=="PBFTRequest") {
                    std::unordered_set<int> uniqueSenders;
                    for (const auto& record : entity->allMessagesBySeq[seq]) {
                        if (record.phase == currentPhase) {
                            uniqueSenders.insert(record.senderId);
                        }
                    }
                    // --- Trigger CompleteEvent if quorum is now met ---
                    if(uniqueSenders.size() >= 6) {
                        auto completeEvent = EventFactory::getInstance().createEvent("complete");
                        if (completeEvent) {
                            nlohmann::json outMsg;
                            outMsg["type"] = "commit";
                            outMsg["view"] = entity->getState().getViewNumber();
                            outMsg["sequence"] = seq;
                            outMsg["operation"] = entity->commitOperations[seq];
                            outMsg["sender"] = entity->getNodeId();
                            outMsg["qc"] = state->getLockedQC(); // Include QC if available
                            Message protocolMsg(outMsg.dump());
                            completeEvent->execute(entity, &protocolMsg, state);
                        }
                    }
                    // --- End CompleteEvent trigger ---
                    
                }
            }
            return true;
        }

        // Fallback: single message storage (original logic)
        int seq = j["sequence"].get<int>();
        std::string operation = j["operation"].get<std::string>();
        int senderId = j["sender"].get<int>();
        std::string currentPhase = j["type"];
        std::string protocolName = "Unknown - will be set later"; // Placeholder for protocol name
        // Check for duplicate sender in this phase and sequence
        bool isDuplicate = false;
        for (const auto& rec : entity->allMessagesBySeq[seq]) {
            if (rec.phase == "commit" && rec.senderId == senderId) {
                isDuplicate = true;
                break;
            }
        }
        if (isDuplicate) {
            return false; // Duplicate, stop further actions
        }
        ProtocolMessageRecord record{seq, senderId, operation, currentPhase, protocolName};
        entity->allMessagesBySeq[seq].push_back(record);
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

class StartTimerEvent : public BaseEvent {
public:
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        //std::cout << "[Node " << entity->getNodeId() << "] Starting timer" << std::endl;
        if (entity->timeKeeper) entity->timeKeeper->start();
        return true;
    }
};

class ResetTimerEvent : public BaseEvent {
public:
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        //std::cout << "[Node " << entity->getNodeId() << "] Resetting timer" << std::endl;
        if (entity->timeKeeper) entity->timeKeeper->reset();
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
        // if (currentPhase == "prepare") {
        //     quorumMet = entity->prepareMessages[seq].size() >= quorum;
        //     if (!quorumMet){
        //         //std::cout << "[Node " << entity->getNodeId() << "] Prepare quorum not met for seq " << seq << std::endl;
        //         return false;
        //     }
            
        // }
        // else if (currentPhase == "commit") {
        //     quorumMet = entity->commitMessages[seq].size() >= quorum;
        //     if (!quorumMet){
        //         //std::cout << "[Node " << entity->getNodeId() << "] commit quorum not met for seq " << seq << std::endl;
        //         return false;
        //     }
            
        // }
        std::unordered_set<int> uniqueSenders;
        for (const auto& record : entity->allMessagesBySeq[seq]) {
            if (record.phase == currentPhase) {
                uniqueSenders.insert(record.senderId);
            }
        }
        quorumMet = uniqueSenders.size() >= quorum;
        if (!quorumMet) {
            return false;
        }
        if (quorumMet && phaseConfig["next_state"]) {
            std::string nextState = phaseConfig["next_state"].as<std::string>();
            //state->setState(nextState);
            //std::cout << "[Node " << entity->getNodeId() << "] Transitioning to " << nextState << " for seq " << seq << std::endl;
        }
        return quorumMet;
    }
};

class CheckQuorumEventForSBFT : public BaseEvent {
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

        if (currentPhase == "prepare") {
            std::unordered_set<int> uniqueSenders;
            for (const auto& record : entity->allMessagesBySeq[seq]) {
                if (record.phase == "prepare") {
                    uniqueSenders.insert(record.senderId);
                }
            }
            quorumMet = uniqueSenders.size() >= quorum;
            if (!quorumMet) return false;

            // Start timer only once per sequence
            if (!entity->preparePhaseTimerRunning[seq].exchange(true) && uniqueSenders.size() <= quorum) {
                std::thread([entity, seq, phaseConfig, currentPhase, uniqueSenders]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    entity->preparePhaseTimerRunning[seq] = false;
                    std::cout << "[Node " << entity->getNodeId() << "] Prepare phase timer expired for seq " << uniqueSenders.size() << "\n";    
                    YAML::Node nextPhaseConfig = entity->getPhaseConfig(currentPhase);
                    std::string nextState;
                    if (uniqueSenders.size() == 6) {
                        // Go to commit phase
                        nextState = nextPhaseConfig["next_state"].as<std::string>();
                        entity->sequenceStates[seq].setState(nextState);
                        std::cout << "[Node " << entity->getNodeId() << "] Prepare phase complete for seq " << seq << ", transitioning to " << nextState << std::endl;

                        // Copy all prepare senders to commitMessages
                        for (const auto& senderId : entity->prepareMessages[seq]) {
                            entity->commitMessages[seq].insert(senderId);
                        }
                        // Copy operation if present
                        if (entity->prepareOperations.count(seq)) {
                            entity->commitOperations[seq] = entity->prepareOperations[seq];
                        }
                    } else {
                        // Stay in prepare phase and (optionally) restart timer
                        nextState = currentPhase;
                        std::cout << "[Node " << entity->getNodeId() << "] Prepare phase NOT complete for seq " << seq << ", staying in " << nextState << std::endl;
                        // Optionally, restart timer here if you want to keep waiting for more messages
                        // (You can recursively start another timer if needed)
                    }
                    if (nextPhaseConfig && nextPhaseConfig["actions"] && nextPhaseConfig["actions"].IsSequence()) {
                        for (const auto& actionNode : nextPhaseConfig["actions"]) {
                            std::string actionName = actionNode.as<std::string>();
                            std::cout << "[Node " << entity->getNodeId() << "] Executing action: " << actionName << " for seq " << seq << "\n";
                            auto it = entity->actions.find(actionName);
                            if (it != entity->actions.end()) {
                                nlohmann::json outMsg;
                                outMsg["type"] = nextState;
                                outMsg["view"] = entity->sequenceStates[seq].getViewNumber();
                                outMsg["sequence"] = seq;
                                if (entity->prepareOperations.count(seq))
                                    outMsg["operation"] = entity->prepareOperations[seq];
                                else if (entity->prePrepareOperations.count(seq))
                                    outMsg["operation"] = entity->prePrepareOperations[seq];
                                else
                                    outMsg["operation"] = "";
                                outMsg["sender"] = entity->getNodeId();
                                
                                Message protocolMsg(outMsg.dump());
                                it->second->execute(entity, &protocolMsg, &entity->sequenceStates[seq]);
                            }
                        }
                    }
                }).detach();
            }
        }
        else if (currentPhase == "commit") {
            
            std::unordered_set<int> uniqueSenders;
            for (const auto& record : entity->allMessagesBySeq[seq]) {
                if (record.phase == "commit") {
                    uniqueSenders.insert(record.senderId);
                }
            }
            quorumMet = uniqueSenders.size() >= quorum;
            if (!quorumMet){
                return false;
            }
        }
        if (quorumMet && phaseConfig["next_state"]) {
            std::string nextState = phaseConfig["next_state"].as<std::string>();
        }
        return quorumMet;
    }
};

class CheckQCEvent : public BaseEvent {
public:
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        auto j = nlohmann::json::parse(message->getContent());
        // Check if QC is present in the message, if not it is initial message
        if(!j.contains("qc") || j["qc"].is_null() || j["qc"].empty() || j["qc"]=="") {
            //std::cout << "[Node " << entity->getNodeId() << "] No QC found in message for seq " << j["sequence"] << "\n";
            return true;
        }
        auto qc = j["qc"];
        // Example: Check QC fields (type, viewNumber, node, sig)
        //std::cout << "current view is ::: " << entity->getState().getViewNumber() << "  " << qc << "\n";
        std::string curview = qc.get<std::string>();
        if (std::stoi(curview)<= (entity->getState().getViewNumber()) ){
            //std::cout << "[Node " << entity->getNodeId() << "] QC is missing required fields for seq " << j["sequence"] << "\n";
            return true;
        }
        return false;
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
            outMsg["view"] = entity->getState().getViewNumber();
            outMsg["sequence"] = seq;
            outMsg["operation"] = j["operation"];
            outMsg["sender"] = entity->getNodeId();
            outMsg["qc"] = state->getLockedQC(); // Include QC if available
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
        return true;
    }
};

class UpdateLockedQCEvent : public BaseEvent {
public:
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        auto j = nlohmann::json::parse(message->getContent());
        if (!j.contains("qc")) {
            std::cout << "[Node " << entity->getNodeId() << "] No QC found in message for seq " << j["sequence"] << "\n";
            return false;
        }
        // Store or update the lockedQC in the state or entity
        state->setLockedQC(j["qc"]);
        return true;
    }
};

class BroadcastIfLeaderEvent : public BaseEvent {
public:
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        // test to trigger view change by making node 1 as faulty by skipping broadcast
        // Uncomment the following lines to simulate a faulty leader
        // if(entity->getNodeId() == 1){
        //     return false; // Skip if not leader
        // }
        if ((entity->getState().getViewNumber()+1) % (entity->peerPorts.size()) == entity->getNodeId()) {
            auto j = nlohmann::json::parse(message->getContent());
            int seq = j["sequence"].get<int>();
            // if already processed, skip
            if (entity->processedOperations.find(seq) != entity->processedOperations.end()) {
                return false;
            }
            std::string phase = j["type"];
            //std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (phase == "prepare") {
                std::unordered_set<int> uniqueSenders;
                for (const auto& record : entity->allMessagesBySeq[seq]) {
                    if (record.phase == "prepare") {
                        uniqueSenders.insert(record.senderId);
                    }
                }
                if(entity->preparePhaseTimerRunning[seq] && uniqueSenders.size() != 7) {
                    std::cout << "[Node " << entity->getNodeId() << "] Prepare phase timer is still running for seq " << uniqueSenders.size() << ", skipping broadcast.\n";
                    return false;
                }
            }
            // Combine all messages for this phase and sequence
            // Combine all messages for this phase and sequence
            nlohmann::json combinedMessages = nlohmann::json::array();
            std::unordered_set<int> uniqueSenders;
            for (const auto& record : entity->allMessagesBySeq[seq]) {
                if (record.phase == phase && uniqueSenders.insert(record.senderId).second) {
                    combinedMessages.push_back(record.senderId);
                }
            }
            std::cout << "[Node " << entity->getNodeId() << "] Combined messages for " << phase << " for seq " << seq << ": " << combinedMessages.dump() << "\n";
            std::cout << "[Node " << entity->getNodeId() << "] Broadcasting combined messages for " << phase << " for seq " << seq << "\n";
            // Prepare the broadcast message
            YAML::Node phaseConfig = entity->getPhaseConfig(j["type"]);
            if (phaseConfig["next_state"]) {
                nlohmann::json outMsg;
                outMsg["type"] = j["type"];
                outMsg["view"] = entity->getState().getViewNumber();
                outMsg["sequence"] = seq;
                outMsg["operation"] = j["operation"];
                outMsg["sender"] = entity->getNodeId();
                outMsg["combinedMessages"] = combinedMessages;
                outMsg["qc"] = state->getLockedQC(); // Include QC if available
                Message protocolMsg(outMsg.dump());

                entity->sendToAll(protocolMsg);
            }
        }
        return true;
    }
};

class UnicastIfParticipantEvent : public BaseEvent {
public:
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        if ((entity->getState().getViewNumber()+1) % (entity->peerPorts.size()) != entity->getNodeId()) {
            auto j = nlohmann::json::parse(message->getContent());
            int seq = j["sequence"].get<int>();
            YAML::Node phaseConfig = entity->getPhaseConfig(j["type"]);
            if (phaseConfig["next_state"]) {
                std::string nextPhase = phaseConfig["next_state"].as<std::string>();
                nlohmann::json outMsg;
                outMsg["type"] = nextPhase;
                outMsg["view"] = entity->getState().getViewNumber();
                outMsg["sequence"] = seq;
                outMsg["operation"] = j["operation"];
                outMsg["sender"] = entity->getNodeId();
                outMsg["qc"] = state->getLockedQC(); // Include QC if available
                Message protocolMsg(outMsg.dump());
                //current view is 
                //std::cout << entity->getState().getViewNumber() << " and next phase is " << nextPhase << "\n";
                int leaderId = (entity->getState().getViewNumber()+1) % (entity->peerPorts.size());
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

class HandleViewChangeHotstuffEvent : public BaseEvent {
public:
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        auto j = nlohmann::json::parse(message->getContent());
        int newView = j["new_view"].get<int>();
        int sender = j["sender"].get<int>();

        // Store the sender for quorum counting
        entity->viewChangeMessages[newView].insert(sender);

        // Store all required data in an array for this view
        ViewChangeData data;
        data.sender = sender;
        data.last_sequence = j.value("last_sequence", -1);
        data.last_operation = j.value("last_operation", "");
        data.locked_qc = j.value("locked_qc", nlohmann::json{});
        entity->viewChangeDataArray[newView].push_back(data);
        // Update local view if needed
        if (newView > entity->getState().getViewNumber()) {
            state->setViewNumber(newView);
            std::cout << "[Node " << entity->getNodeId() << "] Updated to new view: " << newView << std::endl;
        }

        // If this node is the new leader, and has enough view change messages, propose a new block
        int n = entity->peerPorts.size();
        int quorum = computeQuorumEventFactory("2f+1", entity->getF());
        int leaderId = (newView + 1) % n;
        if (entity->getNodeId() == leaderId && entity->viewChangeMessages[newView].size() >= quorum) {
            std::cout << "[Node " << entity->getNodeId() << "] I am the new leader for view " << newView << ", proposing new block." << std::endl;

            // Find the highest QC and associated data
            int highestViewNum = -1;
            nlohmann::json highestQC;
            int parentSeq = -1;
            std::string lastOp;
            

            for (const auto& d : entity->viewChangeDataArray[newView]) {
                //std::cout << "[Node " << entity->getNodeId() << "] Processing view change data from sender " << d.sender << "\n";
                if (!d.locked_qc.empty()) {
                    int qcView = std::stoi(d.locked_qc.get<std::string>());
                    //std::cout << "[Node " << entity->getNodeId() << "] Checking QC from sender " << d.sender << " with view " << qcView << "\n";
                    if (qcView > highestViewNum) {
                        highestViewNum = qcView;
                        highestQC = d.locked_qc;
                        parentSeq = d.last_sequence;
                        lastOp = d.last_operation;
                    }
                }
            }

            // Construct new proposal
            nlohmann::json proposal;
            proposal["type"] = "Prepare";
            proposal["toturnoffflag"] = "true";
            proposal["view"] = newView;
            proposal["sender"] = entity->getNodeId();
            proposal["sequence"] = parentSeq;
            proposal["operation"] = lastOp;
            proposal["qc"] = highestQC;

            std::cout << "[Node " << entity->getNodeId() << "] New proposal: " << proposal.dump() << std::endl;
            entity->inViewChange = false;
            std::lock_guard<std::mutex> lock(entity->timerMtx);
            if (entity->timeKeeper) {
                entity->timeKeeper->stop();
                entity->timeKeeper.reset();
            }
            entity->viewChangeMessages.erase(newView);
            entity->viewChangeDataArray.erase(newView);
            Message proposalMsg(proposal.dump());
            entity->sendToAll(proposalMsg);
        }
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
    this->registerEvent<CheckQuorumEventForSBFT>("checkQuorumForSBFT");
    this->registerEvent<CheckQCEvent>("checkQC");
    this->registerEvent<BroadcastEvent>("broadcast");
    this->registerEvent<ManageTimerEvent>("manageTimer");
    this->registerEvent<CompleteEvent>("complete");
    this->registerEvent<StartTimerEvent>("startTimer");
    this->registerEvent<ResetTimerEvent>("resetTimer");
    
    // Register handle events
    
    this->registerEvent<HandleViewChangeEvent>("handleViewChange");
    this->registerEvent<HandleNewViewEvent>("handleNewView");
    this->registerEvent<BroadcastIfLeaderEvent>("broadcastifLeader");
    this->registerEvent<UnicastIfParticipantEvent>("unicastifParticipant");
    this->registerEvent<UpdateLockedQCEvent>("UpdateLockedQC");
    this->registerEvent<HandleViewChangeHotstuffEvent>("handleViewChangeHotstuff");
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
template void EventFactory::registerEvent<CheckQuorumEventForSBFT>(const std::string&);
template void EventFactory::registerEvent<CheckQCEvent>(const std::string&);
template void EventFactory::registerEvent<BroadcastEvent>(const std::string&);
template void EventFactory::registerEvent<CompleteEvent>(const std::string&);
template void EventFactory::registerEvent<BroadcastIfLeaderEvent>(const std::string&);
template void EventFactory::registerEvent<UnicastIfParticipantEvent>(const std::string&);
template void EventFactory::registerEvent<UpdateLockedQCEvent>(const std::string&);
template void EventFactory::registerEvent<StartTimerEvent>(const std::string&);
template void EventFactory::registerEvent<ResetTimerEvent>(const std::string&);
template void EventFactory::registerEvent<HandleViewChangeHotstuffEvent>(const std::string&);

#include "../../../include/core/Entity.h" // or the header where computeQuorumEventFactory is defined

