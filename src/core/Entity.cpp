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

// ===================== Protocol-Agnostic Handler =====================
class ProtocolHandler : public MessageHandler {
public:
    void handle(Entity* entity, const Message* message, EntityState* context) {
        json j = json::parse(message->getContent());
        std::string currentState = context->getState();
        YAML::Node phaseConfig = entity->getPhaseConfig(currentState);
        std::string operation = j.contains("operation") ? j["operation"].get<std::string>() : "";
        int seq = context->getSequenceNumber();
        int senderId = j["sender"].get<int>();
        std::string messageType = j["type"].get<std::string>();

        // Skip if already processed (except for leader's initial request)
        if (!operation.empty() && messageType != "PBFTRequest" && entity->hasProcessedOperation(operation)) {
            return;
        }

        // Handle message based on type and state
        int quorum = computeQuorum(phaseConfig["quorum"].as<std::string>(), entity->getF());
        bool quorumMet = false;

        if (messageType == "PrePrepare") {
            // Only leader can send PrePrepare
            if (senderId != 1) return;
            
            entity->storePrePrepareMessage(senderId, seq, operation);
            quorumMet = true;  // PrePrepare always transitions
            
            // Move to prepare phase
            context->setState("prepare");
            
            // Send prepare message
            json prepareMsg;
            prepareMsg["type"] = "Prepare";
            prepareMsg["view"] = context->getViewNumber();
            prepareMsg["sequence"] = seq;
            prepareMsg["operation"] = operation;
            prepareMsg["sender"] = entity->getNodeId();
            
            Message protocolMsg(prepareMsg.dump());
            entity->sendToAll(protocolMsg);
        }
        else if (messageType == "Prepare") {
            entity->storePrepareMessage(senderId, seq, operation);
            quorumMet = entity->getPrepareCount(seq) >= quorum;
            
            if (quorumMet) {
                // Move to commit phase
                context->setState("commit");
                
                // Send commit message
                json commitMsg;
                commitMsg["type"] = "Commit";
                commitMsg["view"] = context->getViewNumber();
                commitMsg["sequence"] = seq;
                commitMsg["operation"] = operation;
                commitMsg["sender"] = entity->getNodeId();
                
                Message protocolMsg(commitMsg.dump());
                entity->sendToAll(protocolMsg);
            }
        }
        else if (messageType == "Commit") {
            entity->storeCommitMessage(senderId, seq, operation);
            if (entity->getCommitCount(seq) >= quorum) {
                std::cout << "[Node " << entity->getNodeId() << "] Consensus reached for seq " << seq << "\n";
                entity->markOperationProcessed(operation);
                entity->removeSequenceState(seq);
                context->setState("idle");
                return;
            }
        }
    }
};

// ===================== Null Handler =====================
class NullMessageHandler : public MessageHandler {
public:
    void handle(Entity*, const Message*, EntityState*) override {
        std::cout << "[Entity] No handler registered for this message type\n";
    }
};

// ===================== Message Handler Factory =====================
class MessageHandlerFactory {
private:
    MessageHandlerFactory() { registerHandlers(); }
    using HandlerCreator = std::function<std::unique_ptr<MessageHandler>()>;
    std::unordered_map<std::string, HandlerCreator> _registry;
    void registerHandlers() {
        _registry["PBFTRequest"] = []() { return std::make_unique<ProtocolHandler>(); };
        _registry["PrePrepare"] = []() { return std::make_unique<ProtocolHandler>(); };
        _registry["Prepare"] = []() { return std::make_unique<ProtocolHandler>(); };
        _registry["Commit"] = []() { return std::make_unique<ProtocolHandler>(); };
        _registry["Reply"] = []() { return std::make_unique<ProtocolHandler>(); };
    }
public:
    static MessageHandlerFactory& getInstance() {
        static MessageHandlerFactory instance;
        return instance;
    }
    std::unique_ptr<MessageHandler> createHandler(const std::string& messageType) {
        auto it = _registry.find(messageType);
        if (it != _registry.end()) return it->second();
        return std::make_unique<NullMessageHandler>();
    }
};

// ===================== Entity Methods =====================
Entity::Entity(const std::string& role, int id, const std::vector<int>& peers)
    : _entityState(role, "idle", 0, 0), nodeId(id), peerPorts(peers), connection(5000+id,true), processingThread(), f(2), prePrepareBroadcasted() {
    EventFactory::getInstance().initialize();
    loadProtocolConfig("/Users/eswar/Downloads/CppBedrock/config/config.pbft.yaml");
    timeKeeper = std::make_unique<TimeKeeper>(1200, [this] {
        std::lock_guard<std::mutex> lock(timerMtx);
        this->onTimeout();
    });
    
}

void Entity::storePrePrepareMessage(int nodeId, int sequence, const std::string& operation) {
    prePrepareMessages[sequence].insert(nodeId);
    if (!operation.empty()) prePrepareOperations[sequence] = operation;
    if (timeKeeper) {
        std::cout << "[Node " << getNodeId() << "] Starting timer for seq " << sequence << " after PrePrepare\n";
        timeKeeper->start(); // Start timer on PrePrepare
    }
}
void Entity::storePrepareMessage(int nodeId, int sequence, const std::string& operation) {
    prepareMessages[sequence].insert(nodeId);
    if (!operation.empty()) prepareOperations[sequence] = operation;
    if (timeKeeper) timeKeeper->reset(); // Reset timer on 
    //std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // Simulate processing delay
}
void Entity::storeCommitMessage(int nodeId, int sequence, const std::string& operation) {
    commitMessages[sequence].insert(nodeId);
    if (!operation.empty()) commitOperations[sequence] = operation;

    std::lock_guard<std::mutex> lock(timerMtx);
    if (timeKeeper) {
        if (prePrepareMessages.size() == commitMessages.size()) {
            timeKeeper->stop();
            timeKeeper.reset();
            std::cout << "[Node  " << getNodeId() << "] All operations processed. Timer stopped.\n";
        } else {
            timeKeeper->reset();
        }
    }
}


int Entity::getPrepareCount(int seq) const {
    auto it = prepareMessages.find(seq);
    if (it != prepareMessages.end()) return static_cast<int>(it->second.size());
    return 0;
}

int Entity::getCommitCount(int seq) const {
    auto it = commitMessages.find(seq);
    if (it != commitMessages.end()) return static_cast<int>(it->second.size());
    return 0;
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
void Entity::handleEvent(const Event* event, EntityState*) {
    if (const Message* message = dynamic_cast<const Message*>(event)) {
        try {
            json j = json::parse(message->getContent());
            std::string messageType = j["type"].get<std::string>();

            // Ignore all except ViewChange and NewView if in view change mode
            if (inViewChange && messageType != "ViewChange" && messageType != "NewView") {
                std::cout << "[Node " << getNodeId() << "] In view change, ignoring message type: " << messageType << "\n";
                return;
            }

            // --- ViewChange logic ---
            if (messageType == "ViewChange") {
                int newView = j["new_view"].get<int>();
                int senderId = j["sender"].get<int>();
                viewChangeMessages[newView].insert(senderId);

                int quorum = computeQuorum("2f+1", getF());
                if ((int)viewChangeMessages[newView].size() >= quorum) {
                    std::cout << "[Node " << getNodeId() << "] View change quorum reached for view " << newView << "\n";
                    getState().setViewNumber(newView);
                    inViewChange = false; // Exit view change mode

                    int leaderId = newView % (peerPorts.size() + 1);
                    if (getNodeId() == leaderId) {
                        nlohmann::json newViewMsg;
                        newViewMsg["type"] = "NewView";
                        newViewMsg["new_view"] = newView;
                        newViewMsg["sender"] = getNodeId();
                        Message msg(newViewMsg.dump());
                        sendToAll(msg);
                        std::cout << "[Node " << getNodeId() << "] Sent NewView message as new leader.\n";
                        inViewChange = false;
                        std::lock_guard<std::mutex> lock(timerMtx);
                        if (timeKeeper) {
                            timeKeeper->stop();
                            timeKeeper.reset();
                        }

                        // ---- Re-propose all uncommitted sequences ----
                        for (const auto& [seq, state] : sequenceStates) {
                            // Only re-propose if not committed
                            if (commitMessages[seq].size() < computeQuorum("2f+1", getF())) {
                                std::string operation;
                                if (prePrepareOperations.count(seq))
                                    operation = prePrepareOperations[seq];
                                else if (prepareOperations.count(seq))
                                    operation = prepareOperations[seq];
                                else if (commitOperations.count(seq))
                                    operation = commitOperations[seq];
                                else
                                    continue; // No operation to re-propose

                                nlohmann::json preprepareMsg;
                                preprepareMsg["type"] = "PrePrepare";
                                preprepareMsg["view"] = getState().getViewNumber();
                                preprepareMsg["sequence"] = seq;
                                preprepareMsg["operation"] = operation;
                                preprepareMsg["sender"] = getNodeId();

                                Message protocolMsg(preprepareMsg.dump());
                                sendToAll(protocolMsg);
                                std::cout << "[Node " << getNodeId() << "] Re-proposed PrePrepare for seq " << seq << " in view " << getState().getViewNumber() << "\n";
                            }
                        }
                        // Optionally: If there are no uncommitted sequences, wait for new client requests as usual.
                    }
                }
                return;
            }

            if (messageType == "NewView") {
                int newView = j["new_view"].get<int>();
                std::cout << "[Node " << getNodeId() << "] Received NewView for view " << newView << "\n";
                getState().setViewNumber(newView);
                inViewChange = false; // Exit view change mode

                // Stop timer for new view
                std::lock_guard<std::mutex> lock(timerMtx);
                if (timeKeeper) {
                    timeKeeper->stop();
                    timeKeeper.reset();
                }

                viewChangeMessages.erase(newView);
                return;
            }

            // Handle PBFTRequest from Leader
            if (messageType == "PBFTRequest" && getState().getRole() == "Leader") {
                std::string operation = j["operation"].get<std::string>();
                if (!operation.empty() && !hasProcessedOperation(operation)) {
                    int seq = getNextSequenceNumber();
                    sequenceStates.emplace(seq, EntityState(getState().getRole(), "idle", getState().getViewNumber(), seq));
                    
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
            
            // Handle protocol messages
            if (j.contains("sequence")) {
                int seq = j["sequence"].get<int>();
                if (sequenceStates.find(seq) == sequenceStates.end()) {
                    sequenceStates.emplace(seq, EntityState(getState().getRole(), "idle", getState().getViewNumber(), seq));
                }
                
                auto it = sequenceStates.find(seq);
                if (it != sequenceStates.end()) {
                    auto handler = MessageHandlerFactory::getInstance().createHandler(messageType);
                    handler->handle(this, message, &it->second);
                }
            }
            
        } catch (const json::exception& e) {
            std::cerr << "[Entity] JSON parsing error: " << e.what() << "\n";
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
void Entity::markOperationProcessed(const std::string& operation) {
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

