#include "../../include/core/Entity.h"
#include "../../include/coordination/connections/TcpConnection.h"
#include "../../include/core/events/EventFactory.h"
#include "../../include/core/events/MessageHandler.h"
#include "../../include/core/TimeKeeper.h"
#include <iostream>
#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>
#include <functional>
#include <unordered_map>
#include <memory>
#include <cctype>
#include <set>

using json = nlohmann::json;

// ===================== Utility =====================
int computeQuorum(const std::string& quorumStr, int f) {
    if (quorumStr == "2f") return 2 * f;
    if (quorumStr == "2f+1") return 2 * f + 1;
    try { return std::stoi(quorumStr); } catch (...) { return 1; }
}


// ===================== Entity Methods =====================
Entity::Entity(const std::string& role, int id, const std::vector<int>& peers)
    : _entityState(role, "PBFTRequest", 0, 0), nodeId(id), peerPorts(peers), connection(5000+id,true), processingThread(), f(2), prePrepareBroadcasted() {
    EventFactory::getInstance().initialize();
    loadProtocolConfig("/Users/eswar/Downloads/CppBedrock/config/config.linearpbft.yaml");
    timeKeeper = std::make_unique<TimeKeeper>(1200, [this] {
        std::lock_guard<std::mutex> lock(timerMtx);
        this->onTimeout();
    });
    
}

void Entity::onTimeout() {
    if (!timeKeeper) return;
    std::cout << "[Node " << getNodeId() << "] Timeout occurred! Initiating view change.\n";
    int newView = getState().getViewNumber() + 1;
    getState().setViewNumber(newView);

    inViewChange = true; // <--- Set flag

    nlohmann::json viewChangeMsg;
    viewChangeMsg["type"] = "ViewChange";
    viewChangeMsg["new_view"] = newView;
    viewChangeMsg["sender"] = getNodeId();
    Message msg(viewChangeMsg.dump());
    sendToAll(msg);
}

void Entity::printDataStore() {
    std::cout << "========== Data Store ==========\n";
    std::cout << "[Entity] Commit Message Store:\n";
    for (const auto& [seq, nodes] : commitMessages) {
        std::cout << "  Sequence " << seq << ": { ";
        for (int id : nodes) std::cout << id << " ";
        std::cout << "}";
        if (commitOperations.count(seq)) {
            std::cout << " | operation: " << commitOperations.at(seq);
        }
        std::cout << "\n";
    }
    std::cout << "================================\n";
}
void Entity::start() {
    std::cout << "[Entity] Starting entity with role: " << _entityState.getRole() << "\n";
    connection.startListening();
    processingThread = std::thread(&Entity::processMessages, this);
}
void Entity::stop() {
    std::cout << "[Entity] Stopping entity: " << _entityState.getRole() << "\n";
    connection.stopListening();
    if (processingThread.joinable()) processingThread.join();
}
void Entity::processMessages() {
    while (true) {
        std::string receivedData = connection.receive();
        Message msg(receivedData);
        handleEvent(&msg, &_entityState);
    }
}
void Entity::loadProtocolConfig(const std::string& configFile) {
    try {
        protocolConfig = YAML::LoadFile(configFile);
        const YAML::Node& phases = protocolConfig["phases"];
        if (!phases.IsMap()) {
            std::cerr << "Error: 'phases' should be a map in YAML file" << std::endl;
            return;
        }
        for (const auto& phase_pair : phases) {
            std::string phaseName = phase_pair.first.as<std::string>();
            const YAML::Node& phaseConfig = phase_pair.second;
            if (phaseConfig["actions"] && phaseConfig["actions"].IsSequence()) {
                for (const auto& actionNode : phaseConfig["actions"]) {
                    std::string actionName = actionNode.as<std::string>();
                    std::unique_ptr<Event> event = EventFactory::getInstance().createEvent(actionName);
                    if (event) actions[actionName] = std::move(event);
                    else std::cerr << "Unknown event: " << actionName << std::endl;
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to load configuration: " << e.what() << std::endl;
    }
}
void Entity::handleEvent(const Event* event, EntityState* context) {
    if (const Message* message = dynamic_cast<const Message*>(event)) {
        try {
            json j = json::parse(message->getContent());
            std::string messageType = j["type"].get<std::string>();

            int seq = j.contains("sequence") ? j["sequence"].get<int>() : -1;
            int sender = j.contains("sender") ? j["sender"].get<int>() : -1;
            //std::cout << "  Current State: " << context->getState() << std::endl;

            if (inViewChange && messageType != "ViewChange" && messageType != "NewView") {
                std::cout << "  [IGNORED] Node in view change\n";
                return;
            }

            std::string phase = messageType;
            

            YAML::Node phaseConfig = getPhaseConfig(phase);
            if (phaseConfig && phaseConfig["actions"] && phaseConfig["actions"].IsSequence()) {
                //std::cout << "  Phase Configuration found for: " << phase << std::endl;
                //std::cout << "  Executing actions:" << std::endl;
                bool actionsSucceeded = true;
                // Execute all actions
                bool quorumMet = false;
                for (const auto& actionNode : phaseConfig["actions"]) {
                    std::string actionName = actionNode.as<std::string>();
                    auto it = actions.find(actionName);
                    if (it != actions.end()) {
                        // Now pass 'context' to all event actions
                        bool shouldContinue = it->second->execute(this, message, &sequenceStates[seq]);
                        
                        if (!shouldContinue) {
                            actionsSucceeded = false; // <-- Add this line
                            break; // Stop further actions if any action returns false
                        }
                    }
                }

                // Transition state only if all actions succeeded
                if (actionsSucceeded && phaseConfig["next_state"] && context) {
                    std::string nextState = phaseConfig["next_state"].as<std::string>();
                    sequenceStates[seq].setState(nextState);
                    //std::cout << "\n[Node " << getNodeId() << "] " << "Transitioning to state: " << nextState << std::endl;
                }
                
            } else {
                std::cout << " No phase configuration found for: " << phase << std::endl;
            }

            // Handle PBFTRequest from Leader
            if (messageType == "PBFTRequest" && getState().getRole() == "Leader") {
                std::cout << "  Processing PBFTRequest as Leader" << std::endl;
                std::string operation = j["operation"].get<std::string>();
                if (!operation.empty() && !hasProcessedOperation(std::stoi(operation.substr(9)))) {
                    int seq = getNextSequenceNumber();
                    sequenceStates.emplace(seq, EntityState(getState().getRole(), "PBFTRequest", getState().getViewNumber(), seq));
                    
                    // Create PrePrepare message
                    json preprepareMsg;
                    preprepareMsg["type"] = "PrePrepare";
                    preprepareMsg["view"] = getState().getViewNumber();
                    preprepareMsg["sequence"] = seq;
                    preprepareMsg["operation"] = operation;
                    preprepareMsg["sender"] = getNodeId();
                    
                    Message protocolMsg(preprepareMsg.dump());
                    sendToAll(protocolMsg);
                }
                return;
            }
            
            // For protocol messages, ensure sequence state exists
            if (j.contains("sequence")) {
                int seq = j["sequence"].get<int>();
                if (sequenceStates.find(seq) == sequenceStates.end()) {
                    std::cout << "  Creating new sequence state for seq: " << seq << std::endl;
                    sequenceStates.emplace(seq, EntityState(getState().getRole(), "PBFTRequest", getState().getViewNumber(), seq));
                }
            }
            
        } catch (const json::exception& e) {
            std::cerr << "[Node " << getNodeId() << "] JSON parsing error: " << e.what() << "\n";
        }
    }
}
void Entity::sendToAll(const Message& message) {
    json j = json::parse(message.getContent());
    std::string type = j.contains("type") ? j["type"].get<std::string>() : "";

    for (int peer : peerPorts) {
        sendTo(peer, message);
    }
}
void Entity::sendTo(int peerId, const Message& message) {
    TcpConnection client(5000 + peerId, false);
    client.send(message.getContent());
}
EntityState& Entity::getState() { return _entityState; }
YAML::Node Entity::getPhaseConfig(const std::string& phase) const { return protocolConfig["phases"][phase]; }
void Entity::removeSequenceState(int seq) {
    sequenceStates.erase(seq);
    prePrepareMessages.erase(seq);
    prepareMessages.erase(seq);
    commitMessages.erase(seq);
    prePrepareOperations.erase(seq);
    prepareOperations.erase(seq);
    commitOperations.erase(seq);
}
void Entity::markOperationProcessed(const int operation) {
    processedOperations.insert(operation);
    std::cout << "[Node " << getNodeId() << "] Marked operation as processed: " << operation << "\n";
    printCommittedMessages();

    const int TOTAL_OPERATIONS = 3;
    std::lock_guard<std::mutex> lock(timerMtx);
    if (processedOperations.size() >= TOTAL_OPERATIONS) {
        if (timeKeeper) {
            timeKeeper->stop();
            timeKeeper.reset();
        }
        std::cout << "[Node " << getNodeId() << "] All operations processed. Timer stopped.\n";
    }
}
void Entity::printCommittedMessages() {
    std::cout << "\n========== Committed Messages ==========\n";
    std::cout << "[Node " << getNodeId() << "] Processed Operations:\n";
    for (const auto& op : processedOperations) {
        std::cout << "  - " << op << "\n";
    }
    std::cout << "====================================\n\n";
}
bool Entity::runVerification(const std::string& verifyType, const json& msg, EntityState* context) {
    if (verifyType == "none") return true;
    if (verifyType == "view_match") {
        int expected = context->getViewNumber();
        int actual = msg.contains("view") ? msg["view"].get<int>() : -1;
        if (!msg.contains("view") || actual != expected) {
            std::cout << "[Node " << getNodeId() << "] view_match failed: expected " << expected << ", got " << actual << "\n";
            return false;
        }
        return true;
    }
    if (verifyType == "valid_signature") return true;
    if (verifyType == "unique_digest") return true;
    if (verifyType == "preprepare_exists") return true;
    if (verifyType == "prepare_exists") return true;
    return true;
}

