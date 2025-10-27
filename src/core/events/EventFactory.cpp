#include "../../../include/core/events/EventFactory.h"
#include "../../../include/core/events/BaseEvent.h"
#include "../../../include/core/Entity.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <arpa/inet.h>
#include <unistd.h>
#include "../../../include/core/events/ProtoMessage.h"

void registerUncommonEvents(EventFactory& factory);

int computeQuorumEventFactory(const std::string& quorumStr, int f) {
    if (quorumStr == "2f") return 2 * f;
    if (quorumStr == "2f+1") return (2 * f) + 1;
    try { return std::stoi(quorumStr); } catch (...) { return 1; }
}

// Example derived event class for incrementing sequence
class IncrementSequenceEvent : public BaseEvent {
public:
    IncrementSequenceEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity*, const Message*, EntityState* state) override {
        state->incrementSequenceNumber();
        std::cout << "Sequence incremented" << std::endl;
        return true;
    }
};

class AddLogEvent : public BaseEvent {
public:
    AddLogEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity*, const Message*, EntityState*) override {
        std::cout << "Log entry added" << std::endl;
        return true;
    }
};

class UpdateLogEvent : public BaseEvent {
public:
    UpdateLogEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity*, const Message*, EntityState*) override {
        std::cout << "Log entry updated" << std::endl;
        return true;
    }
};

class SendToClientEvent : public BaseEvent {
public:
    SendToClientEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity*, const Message*, EntityState*) override {
        std::cout << "Sent to client" << std::endl;
        return true;
    }
};


// Protocol Events
class StoreMessageEvent : public BaseEvent {
public:
    StoreMessageEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        if (auto p = dynamic_cast<const ProtoMessage*>(message)) {
            const std::string phase = p->phase();
            const int seq = p->sequence();
            const int senderId = p->sender_id();

            if (seq > entity->entityInfo["sequence"].get<int>()) {
                entity->entityInfo["sequence"] = seq;
            }

            nlohmann::json toStore;
            toStore["type"] = phase;
            toStore["sequence"] = seq;
            toStore["message_sender_id"] = senderId;
            toStore["view"] = p->view();
            toStore["operation"] = p->operation();
            if (phase == "PrePrepare") {
                toStore["timestamp"] = p->timestamp();
                toStore["client_listen_port"] = p->client_listen_port();
                if (p->has_tx()) {
                    nlohmann::json tx;
                    tx["from"] = p->tx_from();
                    tx["to"] = p->tx_to();
                    tx["amount"] = p->tx_amount();
                    toStore["transaction"] = tx;
                }
            }

            const std::string key = phase + "_" + std::to_string(seq) + "_" + std::to_string(senderId);
            const std::string aggKey = phase + "_" + std::to_string(seq);

            // Fast quorum aggregation
            entity->keyToSenderIds[aggKey].insert(senderId);

            // Avoid storing post-completion commits
            if (phase == "Commit" && entity->processedOperations.count(seq)) {
                return true;
            }

            entity->dataset.update(key, toStore);
            return true;
        }
        // ...existing JSON path...
        return true;
    }
};

class ManageTimerEvent : public BaseEvent {
public:
    ManageTimerEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        if (auto p = dynamic_cast<const ProtoMessage*>(message)) {
            if (p->phase() == "PrePrepare") {
                if (entity->timeKeeper) entity->timeKeeper->start();
            } else {
                if (entity->timeKeeper) entity->timeKeeper->reset();
            }
            return true;
        }
        auto j = nlohmann::json::parse(message->getContent());
        std::string currentPhase = j["type"];
        if (currentPhase == "PrePrepare") {
            // std::cout << "[Node " << entity->getNodeId() << "] Starting timer for PrePrepare" << std::endl;
            if (entity->timeKeeper) entity->timeKeeper->start();
        } else {
            if (entity->timeKeeper) entity->timeKeeper->reset();
        }
        return true;
    }
};

class StartTimerEvent : public BaseEvent {
public:
    StartTimerEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        //std::cout << "[Node " << entity->getNodeId() << "] Starting timer" << std::endl;
        if (entity->timeKeeper){
            entity->timeKeeper->start();
            // std::cout << "[Node " << entity->getNodeId() << "] Timer started" << std::endl;
        } 
        
        return true;
    }
};

class ResetTimerEvent : public BaseEvent {
public:
    ResetTimerEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        //std::cout << "[Node " << entity->getNodeId() << "] Resetting timer" << std::endl;
        if (entity->timeKeeper) entity->timeKeeper->reset();
        return true;
    }
};

class StopTimerEvent : public BaseEvent {
public:
    StopTimerEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        //std::cout << "[Node " << entity->getNodeId() << "] Resetting timer" << std::endl;
        if (entity->timeKeeper) {
            entity->timeKeeper->stop();
            entity->timeKeeper.reset();
        }
        return true;
    }
};

class CheckQuorumEvent : public BaseEvent {
public:
    CheckQuorumEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        int seq = -1;
        std::string currentPhase;
        if (auto p = dynamic_cast<const ProtoMessage*>(message)) {
            seq = p->sequence();
            currentPhase = p->phase();
        } else {
            auto j = nlohmann::json::parse(message->getContent());
            seq = j.value("sequence", -1);
            currentPhase = state ? state->getState() : j.value("type", "");
        }
        if (seq < 0 || currentPhase.empty()) return false;

        YAML::Node phaseConfig = entity->getPhaseConfigInsensitive(currentPhase);
        if (!phaseConfig || !phaseConfig["quorum"]) return false;
        int quorum = computeQuorumEventFactory(phaseConfig["quorum"].as<std::string>(), entity->getF());

        const std::string aggKey = currentPhase + "_" + std::to_string(seq);
        size_t votes = 0;
        auto it = entity->keyToSenderIds.find(aggKey);
        if (it != entity->keyToSenderIds.end()) votes = it->second.size();

        return static_cast<int>(votes) >= quorum;
    }
};

class CheckQuorumEventForSBFT : public BaseEvent {
public:
    CheckQuorumEventForSBFT(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        if (!validateMessage(message, entity)) return false;
        auto j = nlohmann::json::parse(message->getContent());
        int seq = j["sequence"].get<int>();
        std::string currentPhase = j["type"];
        YAML::Node phaseConfig = entity->getPhaseConfig(currentPhase);
        if (!phaseConfig || !phaseConfig["quorum"]) return false;
        std::string quorumStr = phaseConfig["quorum"].as<std::string>();
        int quorum = computeQuorumEventFactory(quorumStr, entity->getF());
        bool quorumMet = true;

        // Use DataSet for quorum check (messages JSON file)
        std::string fileName = "messages_" + std::to_string(entity->getNodeId()) + ".json";
        // entity->dataset.loadFromFile(fileName);
        auto records = entity->dataset.getRecords();
        std::string key2 = currentPhase + "_" + std::to_string(seq);
        //std::cout << key2 << std::endl;
        int uniqueSendersSize = entity->keyToSenderIds[key2].size();

        std::unordered_set<int> uniqueSenders;
        
        quorumMet = uniqueSendersSize >= quorum;
        // std::cout << "[Node " << entity->getNodeId() << "] Quorum check for phase " << currentPhase 
        //           << " and sequence " << seq << ": " << uniqueSendersSize 
        //           << " unique senders found, required: " << quorum 
        //           << (quorumMet ? " - Quorum met" : " - Quorum NOT met") << std::endl;
        if (!quorumMet) return false;

        // Optionally, you can keep the timer logic for prepare phase if needed
        if (currentPhase == "prepare") {
            if (!entity->preparePhaseTimerRunning[seq].exchange(true) && uniqueSendersSize <= quorum) {
                std::thread([entity, seq, phaseConfig, currentPhase, uniqueSendersSize, state, j, records]() {
                    std::unordered_set<int> uniqueSenders;
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    entity->preparePhaseTimerRunning[seq] = false;
                    // std::cout << "[Node " << entity->getNodeId() << "] Prepare phase timer expired for seq " << uniqueSendersSize << "\n";    
                    YAML::Node nextPhaseConfig = entity->getPhaseConfig(currentPhase);
                    std::string nextState;
                    //print unique senders
                    
                    std::string key = currentPhase + "_" + std::to_string(seq);
                    // std::cout << "[Node " << entity->getNodeId() << "] Unique senders in prepare phase: " << entity->keyToSenderIds[key].size() << std::endl;
                    if (entity->keyToSenderIds[key].size() == 7) {
                        // Go to commit phase
                        nextState = nextPhaseConfig["next_state"].as<std::string>();
                        entity->sequenceStates[seq].setState(nextState);
                        // std::cout << "[Node " << entity->getNodeId() << "] Prepare phase complete for seq " << seq << ", transitioning to " << nextState << std::endl;
                        
                    } else {
                        nextState = currentPhase;
                        // std::cout << "[Node " << entity->getNodeId() << "] Prepare phase NOT complete for seq " << seq << ", staying in " << nextState << std::endl;
                    }
                    // if (nextPhaseConfig && nextPhaseConfig["actions"] && nextPhaseConfig["actions"].IsSequence()) {
                    //     for (const auto& actionNode : nextPhaseConfig["actions"]) {
                    //         std::string actionName = actionNode.as<std::string>();
                    //         std::cout << "[Node " << entity->getNodeId() << "] going directly Executing action: " << actionName << " for seq " << seq << "\n";
                    //         auto it = entity->actions.find(actionName);
                    //         if (it != entity->actions.end()) {
                    //             nlohmann::json outMsg;
                    //             // outMsg["type"] = nextState;
                    //             // outMsg["view"] = entity->sequenceStates[seq].getViewNumber();
                    //             // outMsg["sequence"] = seq;
                    //             // outMsg["message_sender_id"] = entity->getNodeId();
                    //             // outMsg["qc"] = state->getLockedQC();
                    //             // outMsg["transaction"] = j.value("transaction", nlohmann::json{});
                    //             // outMsg["client_listen_port"] = j.value("client_listen_port", -1);
                    //             // outMsg["clientid"] = j.value("clientid", "");
                    //             // outMsg["timestamp"] = j.value("timestamp", "");
                    //             // if (entity->prepareOperations.count(seq))
                    //             //     outMsg["operation"] = entity->prepareOperations[seq];
                    //             // else if (entity->prePrepareOperations.count(seq))
                    //             //     outMsg["operation"] = entity->prePrepareOperations[seq];
                    //             // else
                    //             //     outMsg["operation"] = "";
                    //             // outMsg["sender"] = entity->getNodeId();
                                
                    //             // Message protocolMsg(outMsg.dump());
                    //             // it->second->execute(entity, &protocolMsg, &entity->sequenceStates[seq]);
                    //             Message protocolMsg(outMsg.dump());
                    //             entity->sendToAll(protocolMsg);
                    //         }
                    //     }
                    // }
                    nlohmann::json combinedMessages = nlohmann::json::array();
                    for (const auto& [key, record] : records) {
                        if (record.contains("sequence") && record["sequence"] == seq &&
                            record.contains("message_sender_id")) {
                            int senderId = -1;
                            if (record["message_sender_id"].is_number_integer()) {
                                senderId = record["message_sender_id"].get<int>();
                            } else if (record["message_sender_id"].is_string()) {
                                try { senderId = std::stoi(record["message_sender_id"].get<std::string>()); } catch (...) {}
                            }
                            if (senderId != -1 && uniqueSenders.insert(senderId).second) {
                                nlohmann::json filtered;
                                filtered["view"] = record.value("view", -1);
                                filtered["sequence"] = record.value("sequence", -1);
                                filtered["digest"] = record.value("digest", "");
                                filtered["message_sender_id"] = record.value("message_sender_id", -1);
                                filtered["signature"] = "";
                                filtered["client_listen_port"] = record.value("client_listen_port", -1);
                                filtered["clientid"] = record.value("clientid", "");
                                filtered["timestamp"] = record.value("timestamp", "");
                                filtered["transaction"] = record.value("transaction", nlohmann::json{});
                                combinedMessages.push_back(filtered);

                            }
                        }
                    }
                    // std::cout << "[Node " << entity->getNodeId() << "] urike bro for this state ra unga " << nextState  << std::endl;
                    
                    if (nextPhaseConfig && nextPhaseConfig["actions"] && nextPhaseConfig["actions"].IsSequence()) {
                        for (const auto& actionNode : nextPhaseConfig["actions"]) {
                            std::string actionName = actionNode.as<std::string>();
                            // std::cout << "[Node " << entity->getNodeId() << "] Executing action: " << actionName << " for seq " << seq << "\n";
                            auto it = entity->actions.find(actionName);
                            if (it != entity->actions.end()) {
                                nlohmann::json outMsg;
                                outMsg["type"] = nextState;
                                outMsg["view"] = entity->getState().getViewNumber();
                                outMsg["sequence"] = seq;
                                outMsg["operation"] = j["operation"];
                                outMsg["message_sender_id"] = entity->getNodeId();
                                outMsg["combinedMessages"] = combinedMessages;
                                outMsg["qc"] = state->getLockedQC(); // Include QC if available
                                outMsg["transaction"] = j["transaction"];
                                outMsg["client_listen_port"] = j["client_listen_port"];
                                outMsg["clientid"] = j["clientid"];
                                outMsg["timestamp"] = j["timestamp"];

                                Message protocolMsg(outMsg.dump());
                                it->second->execute(entity, &protocolMsg, &entity->sequenceStates[seq]);
                            }
                        }
                    }

                }).detach();
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
    CheckQCEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
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
        if (std::stoi(curview)<= (entity->entityInfo["view"]) ){
            //std::cout << "[Node " << entity->getNodeId() << "] QC is missing required fields for seq " << j["sequence"] << "\n";
            return true;
        }
        return false;
    }
};

class BroadcastEvent : public BaseEvent {
public:
    BroadcastEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        if (auto p = dynamic_cast<const ProtoMessage*>(message)) {
            const std::string phase = p->phase();
            YAML::Node phaseCfg = entity->getPhaseConfigInsensitive(phase);
            if (!phaseCfg || !phaseCfg["next_state"]) return true;
            std::string nextPhase = phaseCfg["next_state"].as<std::string>();

            // Build next-phase envelope
            bedrock::ProtocolEnvelope env;
            if (phase == "PrePrepare") {
                auto* m = env.mutable_prepare();
                m->set_view(p->view());
                m->set_sequence(p->sequence());
                m->set_operation(p->operation());
                m->set_message_sender_id(entity->getNodeId());
            } else if (phase == "Prepare" || phase == "prepare") {
                auto* m = env.mutable_commit();
                m->set_view(p->view());
                m->set_sequence(p->sequence());
                m->set_operation(p->operation());
                m->set_message_sender_id(entity->getNodeId());
            } else {
                return true; // not a basic phase
            }
            entity->sendProtocolToAll(env);
            return true;
        }

        // Fallback JSON path
        auto j = nlohmann::json::parse(message->getContent());
        // test to trigger view change by making node 1 as faulty by skipping broadcast
        // Uncomment the following lines to simulate a faulty leader
        
        if(entity->isByzantine && j["type"] == "prepare") {
            return false;
        }
        else{
            // std::cout << "[Node " << entity->getNodeId() << "] Broadcasting prepare: Not Byzantine\n";
        }
        int seq = j["sequence"].get<int>();

        YAML::Node phaseConfig = entity->getPhaseConfig(j["type"]);
        if (phaseConfig["next_state"]) {
            std::string nextPhase = phaseConfig["next_state"].as<std::string>();
            j["type"] = nextPhase;
            j["qc"] = state->getLockedQC(); // Include QC if available
            j["message_sender_id"] = entity->getNodeId(); // Add sender ID to the message
            class ProtocolMessage : public Message {
            public:
                ProtocolMessage(const std::string& content) : Message(content) {}
                bool execute(Entity*, const Message*, EntityState*) override { return true; }
            };
            ProtocolMessage protocolMsg(j.dump());
            entity->sendToAll(protocolMsg);
            // std::cout << "[Node " << entity->getNodeId() << "] Broadcasting message for " << nextPhase << " for seq " << seq << "\n";
        }
        return true;
    }
};

class CompleteEvent : public BaseEvent {
public:
    CompleteEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        if (auto p = dynamic_cast<const ProtoMessage*>(message)) {
            const int seq = p->sequence();

            // Idempotency: skip if this seq already completed locally
            if (entity->processedOperations.count(seq)) return true;

            // Find a stored preprepare with tx/timestamp/client port
            auto records = entity->dataset.getRecords();
            nlohmann::json preprepare;
            for (const auto& [key, rec] : records) {
                if (rec.contains("type") && rec["type"] == "PrePrepare" &&
                    rec.contains("sequence") && rec["sequence"] == seq) {
                    preprepare = rec;
                    break;
                }
            }

            if (!preprepare.is_null()) {
                std::string txnId = preprepare.value("timestamp", "");
                if (!txnId.empty() && entity->executedTransactions.count(txnId) == 0) {
                    if (preprepare.contains("transaction")) {
                        auto tx = preprepare["transaction"];
                        std::string from = tx.value("from", "");
                        std::string to = tx.value("to", "");
                        int amount = tx.value("amount", 0);
                        if (!from.empty() && !to.empty() && amount > 0) {
                            entity->updateBalances(from, to, amount);
                            entity->executedTransactions.insert(txnId);
                            std::cout << "[Node " << entity->getNodeId() << "] Transaction executed: "
                                      << from << " -> " << to << " : " << amount
                                      << " Sequence: " << seq << std::endl << std::endl;
                        }
                    }
                }
                // Mark processed once (prints only on first mark)
                entity->markOperationProcessed(seq);

                // Reply to client once
                const int clientPort = preprepare.value("client_listen_port", -1);
                if (clientPort > 0) {
                    nlohmann::json response;
                    response["type"] = "Response";
                    response["view"] = entity->getState().getViewNumber();
                    response["timestamp"] = txnId;
                    response["message_sender_id"] = entity->getNodeId();
                    response["result"] = "success";
                    Message reply(response.dump());
                    entity->sendTo(clientPort - 5000, reply);
                }
            }
            return true;
        }

        // Fallback JSON path
        auto j = nlohmann::json::parse(message->getContent());
        int seq = j["sequence"].get<int>();

        // Idempotency: skip if this seq already completed locally
        if (entity->processedOperations.count(seq)) return true;

        std::string txnId = j.value("timestamp", "");
        if (!txnId.empty() && entity->executedTransactions.count(txnId) == 0) {
            if (j.contains("transaction")) {
                auto tx = j["transaction"];
                std::string from = tx.value("from", "");
                std::string to = tx.value("to", "");
                int amount = tx.value("amount", 0);
                if (!from.empty() && !to.empty() && amount > 0) {
                    entity->updateBalances(from, to, amount);
                    entity->executedTransactions.insert(txnId);
                    std::cout << "[Node " << entity->getNodeId() << "] Transaction executed: "
                              << from << " -> " << to << " : " << amount
                              << " Sequence: " << seq << std::endl << std::endl;
                }
            }
        }

        entity->markOperationProcessed(seq);

        if (j.contains("client_listen_port")) {
            int clientPort = j.value("client_listen_port", -1);
            nlohmann::json response;
            response["type"] = "Response";
            response["view"] = j.value("view", -1);
            response["timestamp"] = j.value("timestamp", "");
            response["message_sender_id"] = entity->getNodeId();
            response["result"] = "success";
            response["clientid"] = j.value("clientid", "");
            Message BalancesReply(response.dump());
            if (clientPort != -1) entity->sendTo(clientPort - 5000, BalancesReply);
        }
        return true;
    }
};

class UpdateLockedQCEvent : public BaseEvent {
public:
    UpdateLockedQCEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        auto j = nlohmann::json::parse(message->getContent());
        if (!j.contains("qc")) {
            // std::cout << "[Node " << entity->getNodeId() << "] No QC found in message for seq " << j["sequence"] << "\n";
            return false;
        }
        // Store or update the lockedQC in the state or entity
        state->setLockedQC(j["qc"]);
        return true;
    }
};

class BroadcastIfLeaderEvent : public BaseEvent {
public:
    BroadcastIfLeaderEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        
        
        if ((entity->entityInfo["view"].get<int>() + 1) % (entity->peerPorts.size()) == entity->getNodeId()) {
            
            auto j = nlohmann::json::parse(message->getContent());
            
            if(entity->isByzantine && j["type"] == "prepare") {
                // std::cout << "[Node " << entity->getNodeId() << "] Skipping broadcast for prepare phase as faulty leader\n";
                return false; // Skip if not leader
            }

            int seq = j["sequence"].get<int>();
            std::string phase = j["type"];

            // --- Load messages from file ---
            std::string fileName = "messages_" + std::to_string(entity->getNodeId()) + ".json";
            //entity->dataset.loadFromFile(fileName);
            auto records = entity->dataset.getRecords();

            // --- Find unique senders for this phase and sequence ---
            nlohmann::json combinedMessages = nlohmann::json::array();
            std::unordered_set<int> uniqueSenders;
            for (const auto& [key, record] : records) {
                if (record.contains("type") && record["type"] == phase &&
                    record.contains("sequence") && record["sequence"] == seq &&
                    record.contains("message_sender_id")) {
                    int senderId = -1;
                    if (record["message_sender_id"].is_number_integer()) {
                        senderId = record["message_sender_id"].get<int>();
                    } else if (record["message_sender_id"].is_string()) {
                        try { senderId = std::stoi(record["message_sender_id"].get<std::string>()); } catch (...) {}
                    }
                    if (senderId != -1 && uniqueSenders.insert(senderId).second) {
                        nlohmann::json filtered;
                        filtered["view"] = record.value("view", -1);
                        filtered["sequence"] = record.value("sequence", -1);
                        filtered["digest"] = record.value("digest", "");
                        filtered["message_sender_id"] = record.value("message_sender_id", -1);
                        filtered["signature"] = "";
                        filtered["client_listen_port"] = record.value("client_listen_port", -1);
                        filtered["clientid"] = record.value("clientid", "");
                        filtered["timestamp"] = record.value("timestamp", "");
                        filtered["transaction"] = record.value("transaction", nlohmann::json{});
                        combinedMessages.push_back(filtered);

                    }
                }
            }
            if (phase == "prepare" && entity->protocolConfig["protocol"].as<std::string>() == "SBFT") {
                std::unordered_set<int> uniqueSenders;
                for (const auto& [key, record] : records) {
                    if (record.contains("type") && record["type"] == phase) {
                        uniqueSenders.insert(record["message_sender_id"].get<int>());
                    }
                }
                
                if(entity->preparePhaseTimerRunning[seq] && uniqueSenders.size() <= 7) {
                    // std::cout << "[Node " << entity->getNodeId() << "] Prepare phase timer is still running for seq " << uniqueSenders.size() << ", skipping broadcast.\n";
                    return false;
                }
            }

            // std::cout << "[Node " << entity->getNodeId() << "] Combined messages for " << phase << " for seq " << seq << ": " << combinedMessages.dump() << "\n";
            // std::cout << "[Node " << entity->getNodeId() << "] Broadcasting combined messages for " << phase << " for seq " << seq  << " phase: " << phase << "\n";
            // std::cout << "[Node " << entity->getNodeId() << "] Broadcasting message if leader" << j["type"] << "\n";
            // Prepare the broadcast message
            YAML::Node phaseConfig = entity->getPhaseConfig(j["type"]);
            if (phaseConfig["next_state"]) {
                nlohmann::json outMsg;
                outMsg["type"] = j["type"];
                outMsg["view"] = entity->getState().getViewNumber();
                outMsg["sequence"] = seq;
                outMsg["operation"] = j["operation"];
                outMsg["message_sender_id"] = entity->getNodeId();
                outMsg["combinedMessages"] = combinedMessages;
                outMsg["qc"] = state->getLockedQC(); // Include QC if available
                outMsg["transaction"] = j["transaction"];
                outMsg["client_listen_port"] = j["client_listen_port"];
                outMsg["clientid"] = j["clientid"];
                outMsg["timestamp"] = j["timestamp"];
                Message protocolMsg(outMsg.dump());
                
                entity->sendToAll(protocolMsg);
            }
        } else {
            // std::cout << "[Node " << entity->getNodeId() << "] Not the leader, skipping broadcast.\n";
        }
        // std::cout << "[Node " << entity->getNodeId() << "] BroadcastIfLeaderEvent executed successfully.\n";
        return true;
    }
};

class UnicastIfParticipantEvent : public BaseEvent {
public:
    UnicastIfParticipantEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        bool condition = (entity->entityInfo["view"].get<int>() + 1) % (entity->peerPorts.size()) != entity->getNodeId();
        condition = true;
        if (condition) {
            auto j = nlohmann::json::parse(message->getContent());
            int seq = j["sequence"].get<int>();
            YAML::Node phaseConfig = entity->getPhaseConfig(j["type"]);
            if (phaseConfig["next_state"]) {
                std::string nextPhase = phaseConfig["next_state"].as<std::string>();
                j["type"] = nextPhase;
                j["message_sender_id"] = entity->getNodeId();
                j["qc"] = state->getLockedQC(); // Include QC if available
                Message protocolMsg(j.dump());
                //current view is 
                //std::cout << entity->getState().getViewNumber() << " and next phase is " << nextPhase << "\n";
                int leaderId = (entity->entityInfo["view"].get<int>()+1) % (entity->peerPorts.size());
                // std::cout << "[Node " << entity->getNodeId() << "] Unicasting to leader: " << leaderId << " phase:" << nextPhase << "\n";
                entity->sendTo(leaderId, protocolMsg);
            }
        }
        return true;
    }
};

// ViewChange Event
class HandleViewChangeEvent : public BaseEvent {
public:
    HandleViewChangeEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState*) override {
        auto j = nlohmann::json::parse(message->getContent());
        // print receive message
        // std::cout << "[Node " << entity->getNodeId() << "] Received ViewChange message: " << j.dump() << "\n";
        int newView = j["new_view"].get<int>();
        int senderId = j["message_sender_id"].get<int>();
        entity->viewChangeMessages[newView].push_back(j);

        int quorum = computeQuorumEventFactory("2f+1", entity->getF());
        if ((int)entity->viewChangeMessages[newView].size() >= quorum && entity->inViewChange) {
            // std::cout << "[Node " << entity->getNodeId() << "] View change quorum reached for view " << newView << "\n";
            

            int leaderId = (newView+1) % (entity->peerPorts.size());
            if (entity->getNodeId() == leaderId && !entity->isByzantine) {
                // std::cout << "[Node " << entity->getNodeId() << "] I am the new leader for view " << newView << "\n";
                nlohmann::json newViewMsg;
                newViewMsg["type"] = "NewView";
                newViewMsg["new_view"] = newView;
                newViewMsg["message_sender_id"] = entity->getNodeId();
                Message msg(newViewMsg.dump());
                entity->sendToAll(msg);
                

                
                
                // --- Aggregate prepare messages from all view change messages ---
                std::map<int, nlohmann::json> bestPreparePerSeq; // seq -> prepare message

                for (const auto& viewChangeMsg : entity->viewChangeMessages[newView]) {
                    if (viewChangeMsg.contains("prepare_messages") && viewChangeMsg["prepare_messages"].is_array()) {
                        for (const auto& prepareMsg : viewChangeMsg["prepare_messages"]) {
                            if (prepareMsg.contains("sequence")) {
                                int seq = prepareMsg["sequence"].get<int>();
                                // Always assign: last message wins (or add logic to pick best)
                                bestPreparePerSeq[seq] = prepareMsg;
                            }
                        }
                    }
                }
                // std::cout << "[Node " << entity->getNodeId() << "] Best prepare messages aggregated for view change:\n";
                // for (const auto& [seq, prepareMsg] : bestPreparePerSeq) {
                //     std::cout << "[Node " << entity->getNodeId() << "] Seq " << seq << ": " << prepareMsg.dump() << "\n";
                // }
                for (const auto& item : bestPreparePerSeq) {
                    int seq = item.first;
                    const auto& prepareMsg = item.second;
                    // Extract fields from prepareMsg 
                    std::string stringforDigest;
                    if (prepareMsg.contains("transaction") && prepareMsg.contains("timestamp")) {
                        stringforDigest = prepareMsg["transaction"].dump() + prepareMsg["timestamp"].get<std::string>();
                    } else {
                        stringforDigest = "";
                    }
                    std::string digest = stringforDigest.empty() ? "" : computeSHA256(stringforDigest);
                    std::string signature = stringforDigest.empty() ? "" : entity->cryptoProvider->sign(stringforDigest);

                    nlohmann::json preprepareMsg;
                    preprepareMsg["type"] = "PrePrepare";
                    preprepareMsg["toturnoffflag"] = "true"; // Indicate this is a re-proposal
                    preprepareMsg["view"] = entity->entityInfo["view"];
                    preprepareMsg["sequence"] = seq;
                    preprepareMsg["digest"] = digest;
                    preprepareMsg["signature"] = signature;

                    std::string clientid;
                    if (prepareMsg.contains("clientid") && prepareMsg["clientid"].is_string()) {
                        clientid = prepareMsg["clientid"].get<std::string>();
                    } else if (prepareMsg.contains("message_sender_id")) {
                        if (prepareMsg["message_sender_id"].is_string()) {
                            clientid = prepareMsg["message_sender_id"].get<std::string>();
                        } else if (prepareMsg["message_sender_id"].is_number_integer()) {
                            clientid = std::to_string(prepareMsg["message_sender_id"].get<int>());
                        }
                    } else {
                        clientid = "";
                    }
                    preprepareMsg["clientid"] = clientid;

                    preprepareMsg["transaction"] = prepareMsg.value("transaction", nlohmann::json{});
                    preprepareMsg["timestamp"] = prepareMsg.value("timestamp", "");
                    preprepareMsg["operation"] = prepareMsg.value("operation", "");
                    preprepareMsg["message_sender_id"] = entity->getNodeId();

                    Message protocolMsg(preprepareMsg.dump());
                    entity->sendToAll(protocolMsg);
                    // std::cout << "[Node " << entity->getNodeId() << "] Re-proposed PrePrepare for seq " << seq << "\n";
                    
                    
                }
                // std::cout << "[Node " << entity->getNodeId() << "] View change complete \n";
            }
            else{
                // entity->getNodeId() == leaderId && !entity->isByzantine print which condition failed
                
                // std::cout << "[Node " << entity->getNodeId() << "] I am not the leader for view " << newView << leaderId << entity->isByzantine << ", waiting for new view message.\n";
            }
            
        }
        
        return true;
    }
};

// NewView Event
class HandleNewViewEvent : public BaseEvent {
public:
    HandleNewViewEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState*) override {
        auto j = nlohmann::json::parse(message->getContent());
        int newView = j["new_view"].get<int>();
        std::cout << "[Node " << entity->getNodeId() << "] Received NewView for view " << newView << "\n";
        entity->entityInfo["view"] = newView;
        //entity->saveEntityInfo();  
        entity->inViewChange = false;

        // {
            
        //     std::lock_guard<std::mutex> lock(entity->timerMtx);
            
        //     if (entity->timeKeeper) {
                
        //         entity->timeKeeper->stop();
                
        //         entity->timeKeeper.reset();
                
        //     }
        // }
        // std::cout << "[Node " << entity->getNodeId() << "] Stopping timekeeper and resetting view change messages for view " << newView << "\n";
        entity->viewChangeMessages.erase(newView);
        //std::cout << "[Node " << entity->getNodeId() << "] 1.Stopping timekeeper and resetting view change messages for view " << newView << "\n";
        {
            //std::cout << "[Node " << entity->getNodeId() << "] 2.Stopping timekeeper and resetting view change messages for view " << newView << "\n";
            //std::lock_guard<std::mutex> lock(entity->timerMtx);
            if (entity->timeKeeper) {
                entity->timeKeeper->stop();
                entity->timeKeeper.reset();
            }
        }
        //std::cout << "[Node " << entity->getNodeId() << "] Updated to new view: " << newView << "\n";
        return true;
    }
};

class HandleViewChangeHotstuffEvent : public BaseEvent {
public:
    HandleViewChangeHotstuffEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        auto j = nlohmann::json::parse(message->getContent());
        int newView = j["new_view"].get<int>();
        int sender = j["message_sender_id"].get<int>();

        // Store the sender for quorum counting
        entity->viewChangeMessages[newView].push_back(j);

        // Store all required data in an array for this view
        ViewChangeData data;
        data.sender = sender;
        data.last_sequence = j.value("last_sequence", -1);
        data.last_operation = j.value("last_operation", "");
        data.locked_qc = j.value("locked_qc", nlohmann::json{});
        entity->viewChangeDataArray[newView].push_back(data);
        // Update local view if needed
        if (newView > entity->entityInfo["view"].get<int>()) {
            entity->entityInfo["view"] = newView;
            // entity->saveEntityInfo();  
            //std::cout << "[Node " << entity->getNodeId() << "] Updated to new view: " << newView << std::endl;
        }

        // If this node is the new leader, and has enough view change messages, propose a new block
        int n = entity->peerPorts.size();
        int quorum = computeQuorumEventFactory("2f+1", entity->getF());
        int leaderId = (newView + 1) % n;
        if (entity->getNodeId() == leaderId && entity->viewChangeMessages[newView].size() >= quorum) {
            //std::cout << "[Node " << entity->getNodeId() << "] I am the new leader for view " << newView << ", proposing new block." << std::endl;

            // Find the highest QC and associated data
            int highestViewNum = -1;
            nlohmann::json highestQC;
            int parentSeq = -1;
            std::string lastOp;
            
            nlohmann::json newViewMsg;
            newViewMsg["type"] = "NewView";
            newViewMsg["new_view"] = newView;
            newViewMsg["message_sender_id"] = entity->getNodeId();
            Message msg(newViewMsg.dump());
            entity->sendToAll(msg);
            
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
            

            if (parentSeq == -1 || lastOp.empty()) {
                //std::cout << "[Node " << entity->getNodeId() << "] Skipping proposal: parentSeq == -1 or lastOp is empty\n";
                entity->inViewChange = false;
                {
                    std::lock_guard<std::mutex> lock(entity->timerMtx);
                    if (entity->timeKeeper) {
                        entity->timeKeeper->stop();
                        entity->timeKeeper.reset();
                    }
                }
                entity->viewChangeMessages.erase(newView);
                entity->viewChangeDataArray.erase(newView);
                return true;
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

            //std::cout << "[Node " << entity->getNodeId() << "] New proposal: " << proposal.dump() << std::endl;
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

class HandleClientRequestAsLeaderEvent : public BaseEvent {
public:
    HandleClientRequestAsLeaderEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        std::cout << "[Node " << entity->getNodeId() << "] Handling client request as leader\n";
        int n = entity->peerPorts.size();
        int currentView = entity->entityInfo["view"].get<int>();
        int leaderId = (currentView + 1) % n;
        nlohmann::json j = nlohmann::json::parse(message->getContent());
        if (leaderId != entity->getNodeId()) {
            std::cout << "[Node " << entity->getNodeId() << "] Not the leader, trying to reach leader " << leaderId << "\n";
            int leaderPort = 5000 + leaderId; // Assuming leader ports are 5000 + node ID
            try {
                TcpConnection leaderConn(leaderPort, false);
                leaderConn.closeConnection();
                if (!entity->timeKeeper) {
                    entity->timeKeeper = std::make_unique<TimeKeeper>(1500, [entity] {
                        entity->onTimeout();
                    });
                }
                entity->timeKeeper->start();
                //std::cout << "[Node " << entity->getNodeId() << "] Successfully reached leader " << leaderId << "\n";
            } catch (...) {
                //std::cout << "[Node " << entity->getNodeId() << "] Could not connect to leader " << leaderId << ", triggering view change\n";
                entity->onTimeout(); // or your view change trigger logic
            }
            return false; // Skip if not leader
        }
        //std::cout << "[Node " << entity->getNodeId() << "] Handling Request as leader\n";
        
        std::string operation = j["operation"].get<std::string>();
        std::string currentPhase = j["type"];
        // print entity->entityInfo json
        // std::cout << "[Node " << entity->getNodeId() << "] Entity Info: " << entity->entityInfo.dump(4) << "\n";

        if (!operation.empty() && !entity->hasProcessedOperation(std::stoi(operation.substr(9)))) {

            int seq = entity->allocateNextSequence(); // NEW

            // Create sequence state if absent
            if (!entity->sequenceStates.count(seq)) {
                entity->sequenceStates.emplace(
                    seq,
                    EntityState(entity->getState().getRole(),
                                currentPhase,
                                entity->entityInfo["view"],
                                seq));
            }

            std::string stringforDigest = j["transaction"].dump() + j["timestamp"].get<std::string>();
            std::string digest = computeSHA256(stringforDigest);
            
            // Create PrePrepare message
            nlohmann::json preprepareMsg;
            preprepareMsg["type"] = entity->getPhaseConfig(currentPhase)["next_state"].as<std::string>();
            preprepareMsg["view"] = entity->entityInfo["view"];
            preprepareMsg["sequence"] = seq;
            preprepareMsg["digest"] = digest;
            preprepareMsg["signature"] = entity->cryptoProvider->sign(stringforDigest);
            preprepareMsg["clientid"] = j.value("message_sender_id", "");
            preprepareMsg["transaction"] = j.value("transaction", nlohmann::json{});
            preprepareMsg["timestamp"] = j.value("timestamp", "");
            preprepareMsg["operation"] = operation;
            preprepareMsg["message_sender_id"] = entity->getNodeId();
            preprepareMsg["client_listen_port"] = j["client_listen_port"].get<int>();

            // Create typed PrePrepare and broadcast via protobuf
            bedrock::ProtocolEnvelope env;
            auto* m = env.mutable_pre_prepare();
            m->set_view(entity->entityInfo["view"].get<int>());
            m->set_sequence(seq);
            m->set_timestamp(j.value("timestamp",""));
            m->set_operation(operation);
            auto* tx = m->mutable_transaction();
            tx->set_from(j["transaction"].value("from",""));
            tx->set_to(j["transaction"].value("to",""));
            tx->set_amount(j["transaction"].value("amount",0));
            m->set_client_listen_port(j["client_listen_port"].get<int>());
            m->set_signature(entity->cryptoProvider->sign(j["transaction"].dump() + j["timestamp"].get<std::string>()));
            m->set_message_sender_id(entity->getNodeId());
            m->set_client_id(j.value("message_sender_id", std::string("client")));

            // Persist locally via StoreMessageEvent (typed path will store from ProtoMessage)
            ProtoMessage pmsg(env);
            auto storeEv = EventFactory::getInstance().createEvent("storeMessage");
            if (storeEv) storeEv->execute(entity, &pmsg, state);

            // Broadcast typed
            // std::cout << "[Node " << entity->getNodeId() << "] Broadcasting PrePrepare for seq " << seq << " operation " << operation << "\n";
            entity->sendProtocolToAll(env);
            std::cout << "[Node " << entity->getNodeId() << "] Leader broadcasted PrePrepare for seq " << seq << " operation " << operation << "\n\n";
            return true;
        }
        else{
            // std::cout << "[Node " << entity->getNodeId() << "] Operation already processed or empty, skipping broadcast.\n";
            //print processed operations
            //std::cout << "[Node " << entity->getNodeId() << "] Processed operations: ";
            // for (const auto& op : entity->processedOperations) {
            //     std::cout << op << " ";
            // }
            // std::cout << "\n";
        }
        return true;
    }
};

class VerifySignatureEvent : public BaseEvent {
public:
    VerifySignatureEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        if (dynamic_cast<const ProtoMessage*>(message)) {
            return true; // TODO: add real proto signature check
        }
        auto j = nlohmann::json::parse(message->getContent());
        // ...existing verification logic...
        return true;
    }
};

class QueryBalancesEvent : public BaseEvent {
public:
    QueryBalancesEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        auto j = nlohmann::json::parse(message->getContent());
        
        if (j.contains("client_listen_port")) {
            int clientPort = j["client_listen_port"].get<int>();

            // // Debug: print current committed and speculative balances
            // std::cout << "[Node " << entity->getNodeId() << "] Current balances:";
            // if (entity->balances.empty()) {
            //     std::cout << " <empty>";
            // } else {
            //     for (const auto& [k, v] : entity->balances) {
            //         std::cout << " " << k << "=" << v;
            //     }
            // }
            // std::cout << std::endl;

            // std::cout << "[Node " << entity->getNodeId() << "] Current sbalances:";
            // if (entity->speculativeBalances.empty()) {
            //     std::cout << " <empty>";
            // } else {
            //     for (const auto& [k, v] : entity->speculativeBalances) {
            //         std::cout << " " << k << "=" << v;
            //     }
            // }
            // std::cout << std::endl;

            nlohmann::json response;
            response["type"] = "BalancesReply";
            response["view"] = entity->entityInfo["view"];
            // If balances is empty, send speculativeBalances instead
            if (entity->balances.empty()) {
                response["balances"] = entity->speculativeBalances;
            } else {
                response["balances"] = entity->balances;
            }
            response["message_sender_id"] = entity->getNodeId();
            Message BalancesReply(response.dump());
            entity->sendTo(clientPort-5000, BalancesReply);
        }
        return true;
    }
};

class StorePiggybackEvent : public BaseEvent {
public:
    StorePiggybackEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        auto j = nlohmann::json::parse(message->getContent());
        int seq = j["sequence"].get<int>();

        YAML::Node phaseConfig = entity->getPhaseConfig(j["type"]);
        if (phaseConfig["next_state"]) {
            std::string nextPhase = phaseConfig["next_state"].as<std::string>();
            j["type"] = nextPhase;
            j["qc"] = state->getLockedQC(); // Include QC if available
            j["message_sender_id"] = entity->getNodeId(); // Add sender ID to the message

            // Store in piggyback array
            {
                std::lock_guard<std::mutex> lock(entity->piggybackMtx);
                entity->piggyback.push_back(j);
            }

            // Optionally print for debug
            // std::cout << "[Node " << entity->getNodeId() << "] Stored message in piggyback: " << j.dump() << std::endl;
        }
        return true;
    }
};

#include <thread>
#include <chrono>

class PeriodicPiggybackBroadcastEvent : public BaseEvent {
public:
    PeriodicPiggybackBroadcastEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}

    bool execute(Entity* entity, const Message*, EntityState* state) override {
        // Only start one broadcast thread per entity
        if (entity->piggybackBroadcastStarted) return true;
        entity->piggybackBroadcastStarted = true;

        std::thread([entity]() {
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                nlohmann::json piggybackCopy;
                {
                    std::lock_guard<std::mutex> lock(entity->piggybackMtx);
                    if (entity->piggyback.is_array() && !entity->piggyback.empty()) {
                        piggybackCopy = entity->piggyback;
                        entity->piggyback.clear();
                    }
                }
                if (!piggybackCopy.is_null() && piggybackCopy.is_array() && !piggybackCopy.empty()) {
                    nlohmann::json outMsg;
                    outMsg["type"] = "PiggybackBroadcast";
                    outMsg["view"] = entity->entityInfo["view"];
                    outMsg["message_sender_id"] = entity->getNodeId();
                    outMsg["piggyback"] = piggybackCopy;
                    Message protocolMsg(outMsg.dump());
                    entity->sendToAll(protocolMsg);
                    // std::cout << "[Node " << entity->getNodeId() << "] Periodically broadcasted piggyback: " << piggybackCopy.dump(2) << std::endl;
                }
            }
        }).detach();
        return true;
    }
};

// Singleton instance of the EventFactory
EventFactory& EventFactory::getInstance() {
    static EventFactory instance;
    return instance;
}



// Create event based on name
std::unique_ptr<BaseEvent> EventFactory::createEvent(const std::string& name, const nlohmann::json& params) {
    auto it = factoryMap.find(name);
    if (it != factoryMap.end()) {
        return it->second(params);
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
    this->registerEvent<StopTimerEvent>("resetTimer");
    
    // Register handle events
    
    this->registerEvent<HandleViewChangeEvent>("handleViewChange");
    this->registerEvent<HandleNewViewEvent>("handleNewView");
    this->registerEvent<BroadcastIfLeaderEvent>("broadcastifLeader");
    this->registerEvent<UnicastIfParticipantEvent>("unicastifParticipant");
    this->registerEvent<UpdateLockedQCEvent>("UpdateLockedQC");
    this->registerEvent<HandleViewChangeHotstuffEvent>("handleViewChangeHotstuff");
    this->registerEvent<HandleClientRequestAsLeaderEvent>("handleClientRequestAsLeader");
    this->registerEvent<VerifySignatureEvent>("verifySignature");
    this->registerEvent<QueryBalancesEvent>("queryBalances");
    this->registerEvent<StorePiggybackEvent>("storePiggyback");
    this->registerEvent<PeriodicPiggybackBroadcastEvent>("periodicPiggybackBroadcast");
    

    registerUncommonEvents(*this);
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
template void EventFactory::registerEvent<StopTimerEvent>(const std::string&);
template void EventFactory::registerEvent<HandleViewChangeHotstuffEvent>(const std::string&);
template void EventFactory::registerEvent<HandleClientRequestAsLeaderEvent>(const std::string&);
template void EventFactory::registerEvent<VerifySignatureEvent>(const std::string&);
template void EventFactory::registerEvent<QueryBalancesEvent>(const std::string&);
template void EventFactory::registerEvent<StorePiggybackEvent>(const std::string&);
template void EventFactory::registerEvent<PeriodicPiggybackBroadcastEvent>(const std::string&);


#include "../../../include/core/Entity.h" // or the header where computeQuorumEventFactory is defined

