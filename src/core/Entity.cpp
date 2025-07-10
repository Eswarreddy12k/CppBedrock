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
#include <fstream>
#include <filesystem> // C++17

using json = nlohmann::json;

// ===================== Utility =====================
int computeQuorum(const std::string& quorumStr, int f) {
    if (quorumStr == "2f") return 2 * f;
    if (quorumStr == "2f+1") return 2 * f + 1;
    try { return std::stoi(quorumStr); } catch (...) { return 1; }
}


// ===================== Entity Methods =====================
Entity::Entity(const std::string& role, int id, const std::vector<int>& peers, bool byzantine)
    : _entityState(role, "Request", 0, 0),
      nodeId(id),
      peerPorts(peers),
      isByzantine(byzantine), // <-- set the member variable
      connection(5000 + id, true),
      processingThread(),
      f(2),
      prePrepareBroadcasted()
{
    EventFactory::getInstance().initialize();
    loadProtocolConfig("/Users/eswar/Downloads/CppBedrock/config/config.pbft.yaml");
    timeKeeper = std::make_unique<TimeKeeper>(1500, [this] {
        std::lock_guard<std::mutex> lock(timerMtx);
        this->onTimeout();
    });
    loadOrInitDataset();
    cryptoProvider = std::make_unique<OpenSSLCryptoProvider>("../keys/server_" + std::to_string(id) + "_private.pem");
}

Entity::~Entity() {
    stop(); // Ensure thread is joined before destruction
    //std::cout << "[Entity] Destructor called for role: " << _entityState.getRole() << std::endl;
}

void Entity::onTimeout() {
    if (!timeKeeper) return;
    std::cout << "[Node " << getNodeId() << "] Timeout occurred! Initiating view change.\n";
    // Update view number in entityInfo and persist it
    int newView = entityInfo["view"].get<int>() + 1;
    entityInfo["view"] = newView;
    saveEntityInfo();

    inViewChange = true;
    std::string protocol = protocolConfig["protocol"] ? protocolConfig["protocol"].as<std::string>() : "";
    std::cout << "[Node " << getNodeId() << "] Initiating view change to view " << newView << " for protocol " << protocol << "\n";
    nlohmann::json viewChangeMsg;
    viewChangeMsg["type"] = "ViewChange";
    viewChangeMsg["new_view"] = newView;
    viewChangeMsg["message_sender_id"] = getNodeId();

    // --- Collect only one prepare message per sequence for the current view ---
    std::unordered_map<int, nlohmann::json> preparePerSeq;
    std::string fileName = "messages_" + std::to_string(getNodeId()) + ".json";
    dataset.loadFromFile(fileName);
    auto records = dataset.getRecords();

    for (const auto& [key, record] : records) {
        try {
            // std::cout << "[Node " << getNodeId() << "] Processing record: " << key << "\n";
            // Defensive: skip if record is not valid JSON object
            if (!record.is_object()) continue;
            if (record.contains("type") && record["type"] == "prepare" && record.contains("sequence")) {
                int seq = record["sequence"].get<int>();
                // Only keep the first prepare message per sequence (or replace for the last)
                // preparePerSeq[seq] = record; // last wins
                if (preparePerSeq.find(seq) == preparePerSeq.end()) {
                    preparePerSeq[seq] = record; // first wins
                }
            }
        } catch (const nlohmann::json::exception& e) {
            std::cerr << "[Node " << getNodeId() << "] Skipping invalid record: " << e.what() << "\n";
            continue;
        }
    }
    nlohmann::json prepareArray = nlohmann::json::array();
    for (const auto& [seq, record] : preparePerSeq) {
        nlohmann::json filtered;
        if (record.contains("message_sender_id")) filtered["message_sender_id"] = record["message_sender_id"];
        if (record.contains("timestamp"))         filtered["timestamp"]         = record["timestamp"];
        if (record.contains("transaction"))       filtered["transaction"]       = record["transaction"];
        if (record.contains("operation"))         filtered["operation"]         = record["operation"];
        if (record.contains("sequence"))         filtered["sequence"]         = record["sequence"];
        prepareArray.push_back(filtered);
    }
    viewChangeMsg["prepare_messages"] = prepareArray;
    // --- End Prepare collection ---

    if (protocol == "Hotstuff") {
        int lastSeq = -1;
        std::string lastOp;
        nlohmann::json lastQC;
    
        // Load messages from file
        std::string fileName = "messages_" + std::to_string(getNodeId()) + ".json";
        dataset.loadFromFile(fileName);
        auto records = dataset.getRecords();
    
        // Find the message with the highest sequence
        for (const auto& [key, record] : records) {
            if (record.contains("sequence") && record["sequence"].is_number_integer()) {
                int seq = record["sequence"].get<int>();
                if (seq > lastSeq) {
                    lastSeq = seq;
                    lastOp = record.value("operation", "");
                    if (record.contains("qc")) {
                        lastQC = record["qc"];
                    } else {
                        lastQC = nullptr;
                    }
                }
            }
        }
    
        viewChangeMsg["last_sequence"] = lastSeq;
        viewChangeMsg["last_operation"] = lastOp;
        viewChangeMsg["locked_qc"] = (lastSeq != -1 && sequenceStates.count(lastSeq))
            ? nlohmann::json(sequenceStates[lastSeq].getLockedQC())
            : nlohmann::json{};
        Message msg(viewChangeMsg.dump());
        int nextLeader = (newView + 1) % (peerPorts.size());
        sendTo(nextLeader, msg);
    } else {
        Message msg(viewChangeMsg.dump());
        sendToAll(msg);
    }

    if(true){
        std::lock_guard<std::mutex> lock(timerMtx);
        if (timeKeeper) {
            timeKeeper->start();
        }
    }
    
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
    running = true;
    connection.startListening();
    processingThread = std::thread(&Entity::processMessages, this);
}
void Entity::stop() {
    std::cout << "[Entity] Stopping entity: " << _entityState.getRole() << "\n";
    running = false;
    connection.stopListening();
    if (processingThread.joinable() && std::this_thread::get_id() != processingThread.get_id()) {
        processingThread.join();
    }
}
void Entity::processMessages() {
    while (running) {
        std::string receivedData = connection.receive();
        // std::cout << "[Node " << getNodeId() << "] Received data: " << receivedData << "\n";
        if (!running || receivedData.empty()) break; // <-- Add this check
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
                    std::string actionName;
                    nlohmann::json params;

                    if (actionNode.IsScalar()) {
                        actionName = actionNode.as<std::string>();
                    } else if (actionNode.IsMap()) {
                        // Only one key-value pair per map node (the action and its params)
                        auto it = actionNode.begin();
                        actionName = it->first.as<std::string>();
                        const YAML::Node& paramNode = it->second;
                        for (const auto& param : paramNode) {
                            params[param.first.as<std::string>()] = param.second.as<std::string>();
                        }
                    }
                    std::unique_ptr<BaseEvent> event = EventFactory::getInstance().createEvent(actionName, params);
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

            // // --- Special handling for QueryBalances ---
            // if (messageType == "QueryBalances" && j.contains("client_listen_port")) {
            //     int clientPort = j["client_listen_port"];
            //     json response = {
            //         {"type", "BalancesReply"},
            //         {"balances", balances} // or whatever your balances map is called
            //     };
            //     std::string respStr = response.dump();

            //     // Connect to client and send response
            //     TcpConnection clientConn(clientPort, false);
            //     clientConn.send(respStr);
            //     clientConn.closeConnection(); // If you have this method
            //     std::cout << "[Node " << getNodeId() << "] Sent balances to client on port " << clientPort << std::endl;
            //     return;
            // }
            // // --- End special handling ---

            int seq = assignSequenceNumber();
            if(j.contains("sequence")) {
                seq = j["sequence"].get<int>();
            }
            if (inViewChange && messageType != "ViewChange" && messageType != "NewView") {
                std::cout << "  [IGNORED] Node in view change\n";
                return;
            }
            
            // if(inViewChange && messageType == "NewView") {
            //     std::lock_guard<std::mutex> lock(timerMtx);
            //     if (timeKeeper) {
            //         timeKeeper->stop();
            //     }
            // }

            std::string phase = messageType;
            
            
            
            YAML::Node phaseConfig = getPhaseConfig(phase);
            if (phaseConfig && phaseConfig["actions"] && phaseConfig["actions"].IsSequence()) {
                // std::cout << "  Phase Configuration found for: " << phase << std::endl;
                // std::cout << "  Executing actions:" << std::endl;
                bool actionsSucceeded = true;
                // Execute all actions
                bool quorumMet = false;
                for (const auto& actionNode : phaseConfig["actions"]) {
                    
                    std::string actionName;
                    nlohmann::json params;

                    if (actionNode.IsScalar()) {
                        actionName = actionNode.as<std::string>();
                    } else if (actionNode.IsMap()) {
                        // Only one key-value pair per map node (the action and its params)
                        auto it = actionNode.begin();
                        actionName = it->first.as<std::string>();
                        const YAML::Node& paramNode = it->second;
                        for (auto paramIt = paramNode.begin(); paramIt != paramNode.end(); ++paramIt) {
                            params[paramIt->first.as<std::string>()] = paramIt->second.as<std::string>();
                        }
                        // std::cout << "[Node " << getNodeId() << "] Executing action: "  << actionName << " with params: " << params.dump() << "\n";
                    }
                    // std::cout << "[Node " << getNodeId() << "] Executing action: " << actionName << " for seq " << seq << "\n";
                    auto eventPtr = EventFactory::getInstance().createEvent(actionName, params);
                    if (eventPtr) {
                        bool shouldContinue = eventPtr->execute(this, message, &sequenceStates[seq]);
                        if (!shouldContinue) {
                            actionsSucceeded = false;
                            break;
                        }
                    }
                }

                // Transition state only if all actions succeeded
                if (actionsSucceeded && phaseConfig["next_state"] && context) {
                    std::string nextState = phaseConfig["next_state"].as<std::string>();
                    sequenceStates[seq].setState(nextState);
                    // std::cout << "\n[Node " << getNodeId() << "] " << "Transitioning to state: " << nextState << std::endl;
                }
                
            } else {
                std::cout << " No phase configuration found for: " << phase << std::endl;
            }

            
            // For protocol messages, ensure sequence state exists
            if (j.contains("sequence")) {
                
                int seq = j["sequence"].get<int>();
                if (sequenceStates.find(seq) == sequenceStates.end()) {
                    std::cout << "  Creating new sequence state for seq: " << seq << std::endl;
                    sequenceStates.emplace(seq, EntityState(getState().getRole(), "Request", getState().getViewNumber(), seq));
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
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Wait for all nodes to receive the new view message
}
void Entity::sendTo(int peerId, const Message& message) {
    try {
        TcpConnection client(5000 + peerId, false);
        client.send(message.getContent());
        // client.closeConnection(); // Close connection after sending
    } catch (const std::exception& e) {
        std::cerr << "[Node " << getNodeId() << "] Failed to connect send to peer " << peerId << ": " << e.what() << std::endl;
    }
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
    // dataset.loadFromFile("test" + std::to_string(getNodeId()) + ".json");
    // // Add or update a record
    // nlohmann::json user;
    // user["name"] = "Alice";
    // user["balance"] = 100;
    // user["active"] = true;
    // dataset.update(std::to_string(operation), user);

    // Save to file
    // dataset.saveToFile("test" + std::to_string(getNodeId()) + ".json");

    // Retrieve and print a record
    // nlohmann::json loaded = dataset.get("user1");
    // std::cout << loaded.dump(4) << std::endl;
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

void Entity::loadOrInitDataset() {
    std::string filename = "entity_info_" + std::to_string(getNodeId()) + ".json";
    nlohmann::json info;

    if (std::filesystem::exists(filename)) {
        std::ifstream in(filename);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        if (!content.empty()) {
            try {
                info = nlohmann::json::parse(content);
            } catch (const nlohmann::json::parse_error& e) {
                std::cerr << "[Node " << getNodeId() << "] Failed to parse JSON from " << filename << ": " << e.what() << std::endl;
                info["server_name"] = getNodeId();
                info["view"] = 0;
                info["sequence"] = 0;
                info["server_status"] = 1;
                saveEntityInfo();
            }
        }
    } else {
        info["server_name"] = getNodeId();
        info["view"] = 0;
        info["sequence"] = 0;
        info["server_status"] = 1;
        saveEntityInfo();
    }
    this->entityInfo = info;
}

void Entity::updateEntityInfoField(const std::string& key, const nlohmann::json& value) {
    entityInfo[key] = value;
    saveEntityInfo();
}

void Entity::saveEntityInfo() {
    std::string filename = "entity_info_" + std::to_string(getNodeId()) + ".json";
    std::string tmpFilename = filename + ".tmp";
    {
        std::ofstream out(tmpFilename, std::ios::trunc);
        out << entityInfo.dump(4) << std::endl;
    }
    std::filesystem::rename(tmpFilename, filename);
}

