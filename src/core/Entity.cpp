#include "../../include/core/Entity.h"
// RE-ENABLE TcpConnection for client replies
#include "../../include/coordination/connections/TcpConnection.h"
#include "../../include/core/events/EventFactory.h"
#include "../../include/core/events/MessageHandler.h"
#include "../../include/core/TimeKeeper.h"
#include "coordination/grpc/NodeServiceImpl.h"
#include "proto/bedrock.grpc.pb.h"
#include "proto/bedrock.pb.h"
#include <grpcpp/grpcpp.h>
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
// Fix: don't map 1000 to gRPC; only map valid node ids or legacy TCP ports
static inline int grpcPortForPeer(int peerEntry) {
    if (peerEntry == 1000) return -1;                 // special client marker
    if (peerEntry >= 5001 && peerEntry <= 5999) return peerEntry + 10000; // legacy TCP -> +10000
    if (peerEntry > 0 && peerEntry < 1000) return 15000 + peerEntry;      // node id
    return -1; // invalid
}

// Helper: send to client via TCP using client_listen_port in payload
static bool sendToClientViaTcp(const std::string& jsonPayload) {
    try {
        nlohmann::json j = nlohmann::json::parse(jsonPayload);
        int clientPort;
        if (!j.contains("client_listen_port")) {
            clientPort = 6000;
        }
        else{
            clientPort= j["client_listen_port"].get<int>();
        }
        
        TcpConnection clientConn(clientPort, /*isServer*/ false);
        clientConn.send(jsonPayload);
        clientConn.closeConnection();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ClientSend] Exception: " << e.what() << "\n";
        return false;
    }
}

// Helper: build typed envelope from a protocol JSON message for basic phases
static bool buildEnvelopeFromJson(const std::string& jsonStr, bedrock::ProtocolEnvelope& env) {
    try {
        nlohmann::json j = nlohmann::json::parse(jsonStr);
        if (!j.contains("type") || !j["type"].is_string()) return false;
        const std::string t = j["type"].get<std::string>();

        if (t == "PrePrepare") {
            auto* m = env.mutable_pre_prepare();
            m->set_view(j.value("view", 0));
            m->set_sequence(j.value("sequence", 0));
            m->set_timestamp(j.value("timestamp", ""));
            m->set_operation(j.value("operation", ""));
            if (j.contains("transaction")) {
                const auto& txj = j["transaction"];
                auto* tx = m->mutable_transaction();
                tx->set_from(txj.value("from",""));
                tx->set_to(txj.value("to",""));
                tx->set_amount(txj.value("amount", 0));
            }
            m->set_client_listen_port(j.value("client_listen_port", 0));
            m->set_signature(j.value("signature", ""));
            m->set_message_sender_id(j.value("message_sender_id", getpid())); // fallback
            return true;
        }
        if (t == "Prepare") {
            auto* m = env.mutable_prepare();
            m->set_view(j.value("view", 0));
            m->set_sequence(j.value("sequence", 0));
            m->set_operation(j.value("operation", ""));
            m->set_message_sender_id(j.value("message_sender_id", 0));
            return true;
        }
        if (t == "Commit") {
            auto* m = env.mutable_commit();
            m->set_view(j.value("view", 0));
            m->set_sequence(j.value("sequence", 0));
            m->set_operation(j.value("operation", ""));
            m->set_message_sender_id(j.value("message_sender_id", 0));
            return true;
        }
    } catch (...) {}
    return false;
}

// ===================== Entity Methods =====================
Entity::Entity(const std::string& role, int id, const std::vector<int>& peers, bool byzantine)
    : _entityState(role, "Request", 0, 0),
      nodeId(id),
      peerPorts(peers),
      isByzantine(byzantine),
      connection(5000 + id, true),
      processingThread(),
      f(2),
      prePrepareBroadcasted()
{
    EventFactory::getInstance().initialize();
    loadProtocolConfig("/Users/eswar/Downloads/CppBedrock/config/config.pbft.yaml");
    timeKeeper = std::make_unique<TimeKeeper>(1500, [this] {
        this->onTimeout();
    });
    entityInfo["server_name"] = getNodeId();
    entityInfo["view"] = 0;
    entityInfo["sequence"] = 0;
    entityInfo["server_status"] = 1;
    cryptoProvider = std::make_unique<OpenSSLCryptoProvider>("../keys/server_" + std::to_string(id) + "_private.pem");
    
    // Example: in Entity constructor or init
    std::string protocol = protocolConfig["protocol"] ? protocolConfig["protocol"].as<std::string>() : "";
    if(protocol=="ChainedHotstuff"){
        auto event = EventFactory::getInstance().createEvent("periodicPiggybackBroadcast");
        if (event) event->execute(this, nullptr, nullptr);
    }
    
}

Entity::~Entity() {
    stop(); // Ensure thread is joined before destruction
    //std::cout << "[Entity] Destructor called for role: " << _entityState.getRole() << std::endl;
}

int Entity::getMaxSpeculativeSeq() const {
    int maxSeq = committedSeq;
    std::lock_guard<std::mutex> g(speculativeLogMtx);
    if (!speculativeLog.empty()) {
        maxSeq = std::max(maxSeq, speculativeLog.rbegin()->first);
    }
    return maxSeq;
}

void Entity::cachePrePrepare(int seq, const nlohmann::json& msg) {
    preprepareCache[seq] = msg;
}

void Entity::sendFillHole(int fromSeq, int toSeq, bool broadcast) {
    nlohmann::json fh{
        {"type","FillHole"},
        {"view", entityInfo["view"]},
        {"from_seq", fromSeq},
        {"to_seq", toSeq},
        {"message_sender_id", getNodeId()},
        {"broadcast", broadcast}
    };
    Message m(fh.dump());
    if (broadcast) {
        sendToAll(m);
        std::cout << "[Node " << getNodeId() << "] Broadcast FillHole request for [" << fromSeq << "," << toSeq << "]\n";
    } else {
        // current primary id determination: view % N mapping to peerPorts vector
        if (!peerPorts.empty()) {
            int n = (int)peerPorts.size();
            int primaryId = peerPorts[entityInfo["view"].get<int>() % n];
            sendTo(primaryId, m);
            std::cout << "[Node " << getNodeId() << "] Sent FillHole to primary " << primaryId
                      << " for [" << fromSeq << "," << toSeq << "]\n";
        }
    }
    fillHolePending = true;
    fillHoleFromSeq = fromSeq;
    fillHoleToSeq = toSeq;
    fillHoleDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(fillHoleTimeoutMs);
}

void Entity::replayRangeTo(int fromSeq, int toSeq, int targetNodeId) {
    for (int s = fromSeq; s <= toSeq; ++s) {
        auto it = preprepareCache.find(s);
        if (it == preprepareCache.end()) continue;
        Message resend(it->second.dump());
        sendTo(targetNodeId, resend);
        std::cout << "[Node " << getNodeId() << "] Replay seq " << s << " to node " << targetNodeId << "\n";
    }
}

void Entity::tryHandleFillHoleTimeout() {
    if (!fillHolePending) return;
    if (std::chrono::steady_clock::now() < fillHoleDeadline) return;
    // Escalate: broadcast fill-hole and start view change
    sendFillHole(fillHoleFromSeq, fillHoleToSeq, true);
    std::cout << "[Node " << getNodeId() << "] FillHole timeout -> initiating view change\n";
    fillHolePending = false;
    initiateViewChange();
}

void Entity::onTimeout() {
    if (!timeKeeper) return;
    // First check pending fill-hole escalation
    tryHandleFillHoleTimeout();
    // If still pending we already escalated; optionally early return
    if (fillHolePending) return;

    std::cout << "[Node " << getNodeId() << "] Timeout occurred! Initiating view change.\n";
    int newView = entityInfo["view"].get<int>() + 1;
    entityInfo["view"] = newView;

    inViewChange = true;
    std::string protocol = protocolConfig["protocol"] ? protocolConfig["protocol"].as<std::string>() : "";
    nlohmann::json viewChangeMsg;
    viewChangeMsg["type"] = "ViewChange";
    viewChangeMsg["new_view"] = newView;
    viewChangeMsg["view"] = newView; // for Zyzzyva handlers
    viewChangeMsg["message_sender_id"] = getNodeId();

    // Collect one prepare/status per sequence (status report)
    std::unordered_map<int, nlohmann::json> preparePerSeq;
    std::string fileName = "messages_" + std::to_string(getNodeId()) + ".json";
    dataset.loadFromFile(fileName);
    auto records = dataset.getRecords();

    for (const auto& [key, record] : records) {
        try {
            if (!record.is_object()) continue;
            std::string t = record.value("type", "");
            if ((t == "prepare" || t == "Prepare") && record.contains("sequence") && record["sequence"].is_number_integer()) {
                int seq = record["sequence"].get<int>();
                if (!preparePerSeq.count(seq)) {
                    preparePerSeq[seq] = record;
                }
            }
        } catch (const nlohmann::json::exception&) {
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
        if (record.contains("sequence"))          filtered["sequence"]          = record["sequence"];
        prepareArray.push_back(filtered);
    }
    viewChangeMsg["prepare_messages"] = prepareArray;

    if (protocol == "Zyzzyva") {
        viewChangeMsg["committed_seq"] = committedSeq;
        Message msg(viewChangeMsg.dump());
        if (!peerPorts.empty()) {
            int idx = newView % static_cast<int>(peerPorts.size());
            int nextLeaderPeerId = peerPorts[idx];
            sendTo(nextLeaderPeerId, msg);
        } else {
            sendToAll(msg);
        }
        if (timeKeeper) timeKeeper->start();
        return;
    }

    if (protocol == "Hotstuff") {
        int lastSeq = -1;
        std::string lastOp;
        nlohmann::json lastQC;

        std::string fileName = "messages_" + std::to_string(getNodeId()) + ".json";
        dataset.loadFromFile(fileName);
        auto records = dataset.getRecords();

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

    if (timeKeeper) {
        timeKeeper->start();
    }
}

void Entity::sendNewViewToNextLeader() {
    int currentView = entityInfo["view"].get<int>();
    if(currentView==0){
        currentView-=1;
    }
    currentView+=1;
    entityInfo["view"] = currentView;
    int nextLeader = (currentView + 1) % peerPorts.size();
    
    nlohmann::json newViewMsg;
    newViewMsg["type"] = "NewViewforHotstuff";
    newViewMsg["new_view"] = currentView;
    newViewMsg["message_sender_id"] = getNodeId();
    Message msg(newViewMsg.dump());
    sendTo(nextLeader, msg);
    std::cout << "[Node " << getNodeId() << "] Sent NewView message to node " << nextLeader << "\n";

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

    // No TCP listener
    // connection.startListening();

    // Start in-entity gRPC server and init client stubs
    startGrpcServer();
    initGrpcStubs();

    processingThread = std::thread(&Entity::processMessages, this);
    //std::this_thread::sleep_for(std::chrono::milliseconds(0)); 
    std::string protocol = protocolConfig["protocol"] ? protocolConfig["protocol"].as<std::string>() : "";
    if(protocol=="Hotstuff" || protocol=="ChainedHotstuff"){
        sendNewViewToNextLeader();
    }
    
}
void Entity::stop() {
    std::cout << "[Entity] Stopping entity: " << _entityState.getRole() << "\n";
    running = false;

    // connection.stopListening();  // removed to avoid TCP use

    stopGrpcServer();

    if (processingThread.joinable() && std::this_thread::get_id() != processingThread.get_id()) {
        processingThread.join();
    }
}
void Entity::processMessages() {
    // No TCP receive loop anymore. Keep thread lightweight or remove if unused.
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
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

            // Handle TriggerViewChange from the client
            if (messageType == "TriggerViewChange") {
                std::cout << "[Node " << getNodeId() << "] Received TriggerViewChange from client.\n";
                initiateViewChange();
                return;
            }

            if(j["type"]=="changeServerStatus"){
                if(j.contains("server_status")) {
                    entityInfo["server_status"] = j["server_status"].get<int>();
                    std::cout << "[Node " << getNodeId() << "] Server status changed to: " << entityInfo["server_status"] << "\n";
                } else {
                    std::cerr << "[Node " << getNodeId() << "] Invalid changeServerStatus message: missing server_status field.\n";
                }
                return;
            }
            if(entityInfo["server_status"]!=1) {
                // std::cout << "[Node " << getNodeId() << "] Ignoring message while server is down.\n";
                return;
            }

            else if (messageType == "FillHole") {
                auto ev = EventFactory::getInstance().createEvent("fillHoleRequest");
                if (ev) ev->execute(this, message, &_entityState);
                return;
            }

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
            // if (inViewChange && messageType != "ViewChange" && messageType != "NewView") {
            //     std::cout << "  [IGNORED] Node " << getNodeId() << "in view change\n";
            //     return;
            // }
            
            // if(inViewChange && messageType == "NewView") {
            //     //std::lock_guard<std::mutex> lock(timerMtx);
            //     if (timeKeeper) {
            //         timeKeeper->stop();
            //     }
            // }

            // --- PiggybackBroadcast handling ---
            if (messageType == "PiggybackBroadcast" && j.contains("piggyback") && j["piggyback"].is_array()) {
                // std::cout << "[Node " << getNodeId() << "] PiggybackBroadcast received. Types in piggyback:\n";
                // for (const auto& piggyMsg : j["piggyback"]) {
                //     if (piggyMsg.contains("type")) {
                //         std::cout << "  - " << piggyMsg["type"].get<std::string>() << " seq -" << piggyMsg["sequence"] << "\n";
                //     } else {
                //         std::cout << "  - (no type field)\n";
                //     }
                // }
                for (const auto& piggyMsg : j["piggyback"]) {
                    Message protocolMsg(piggyMsg.dump());
                    handleEvent(&protocolMsg, context);
                }
                return;
            }
            // --- End PiggybackBroadcast handling ---

            std::string phase = messageType;
            
            // std::cout << "[Node " << getNodeId() << "] Processing message of type: " << messageType << " for seq: " << seq << "\n";
            
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
                    // std::cout << "[Node " << getNodeId() << "] Executing action: " << actionName << " for seq " << seq << "type: " << j["type"] << "\n";
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
    // Broadcast to all peers (skip self)
    for (int peer : peerPorts) {
        if (peer == nodeId || peer == (5000 + nodeId)) continue;
        if (grpcPortForPeer(peer) < 0) continue;
        sendTo(peer, message);
    }
}
void Entity::sendTo(int peer, const Message& message) {
    // Peer 1000 -> client via TCP
    if (peer == 1000) {
        if (!sendToClientViaTcp(message.getContent())) {
            std::cerr << "[Node " << getNodeId() << "] Failed to send to client via TCP (peer==1000)\n";
        }
        return;
    }
    if (peer == nodeId || peer == (5000 + nodeId)) return;
    if (grpcPortForPeer(peer) < 0) {
        std::cerr << "[Node " << getNodeId() << "] Skipping invalid peer " << peer << "\n";
        return;
    }

    // Prefer typed gRPC for basic phases
    bedrock::ProtocolEnvelope env;
    const bool isTyped = buildEnvelopeFromJson(message.getContent(), env);

    try {
        auto* stub = getStub(peer);
        if (!stub) {
            std::cerr << "[Node " << getNodeId() << "] No gRPC stub for peer " << peer << "\n";
            return;
        }
        grpc::ClientContext ctx;
        bedrock::Ack ack;

        if (isTyped) {
            auto status = stub->SendProtocol(&ctx, env, &ack);
            if (!status.ok() || !ack.ok()) {
                std::cerr << "[Node " << getNodeId() << "] gRPC SendProtocol to peer "
                          << peer << " failed: " << status.error_code() << " "
                          << status.error_message() << " | ack=" << ack.msg() << "\n";
            }
        } else {
            bedrock::RawJson req;
            req.set_json(message.getContent());
            bedrock::RawJson resp;
            auto status = stub->SendRawJson(&ctx, req, &resp);
            if (!status.ok()) {
                std::cerr << "[Node " << getNodeId() << "] gRPC SendRawJson to peer "
                          << peer << " failed: " << status.error_code() << " "
                          << status.error_message() << "\n";
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[Node " << getNodeId() << "] gRPC send exception to peer "
                  << peer << ": " << e.what() << "\n";
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
    //printCommittedMessages();

    const int TOTAL_OPERATIONS = 50;
    //std::lock_guard<std::mutex> lock(timerMtx);
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
                //saveEntityInfo();
            }
        }
    } else {
        info["server_name"] = getNodeId();
        info["view"] = 0;
        info["sequence"] = 0;
        info["server_status"] = 1;
        //saveEntityInfo();
    }
    this->entityInfo = info;
}

void Entity::updateEntityInfoField(const std::string& key, const nlohmann::json& value) {
    entityInfo[key] = value;
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

void Entity::initiateViewChange() {
    std::cout << "[Node " << getNodeId() << "] Initiating view change.\n";

    int newView = entityInfo["view"].get<int>() + 1;
    entityInfo["view"] = newView;

    inViewChange = true;
    nlohmann::json viewChangeMsg;
    viewChangeMsg["type"] = "ViewChange";
    viewChangeMsg["view"] = newView;
    viewChangeMsg["message_sender_id"] = getNodeId();
    viewChangeMsg["committed_seq"] = committedSeq;

    // Collect speculative log for sequences > committed_seq
    nlohmann::json speculativeLogArray = nlohmann::json::array();
    nlohmann::json prepareMsgs = nlohmann::json::array(); // NEW
    {
        std::lock_guard<std::mutex> lock(speculativeLogMtx);
        for (const auto& [seq, entry] : speculativeLog) { // Use structured bindings for std::map
            if (seq > committedSeq) {
                nlohmann::json logEntry = {
                    {"sequence", seq},
                    {"txnId", entry.txnId},
                    {"from", entry.from},
                    {"to", entry.to},
                    {"amount", entry.amount}
                };
                speculativeLogArray.push_back(logEntry);

                // Zyzzyva VC status entry compatible with leader logic
                nlohmann::json pm = {
                    {"sequence", seq},
                    {"timestamp", entry.txnId},
                    {"transaction", {
                        {"from", entry.from},
                        {"to", entry.to},
                        {"amount", entry.amount}
                    }},
                    {"operation", entry.txnId} // optional
                };
                prepareMsgs.push_back(pm);
            }
        }
    }
    viewChangeMsg["speculative_log"] = speculativeLogArray;
    viewChangeMsg["prepare_messages"] = prepareMsgs; // NEW

    // Send ViewChange to the next leader
    if (!peerPorts.empty()) {
        int nextLeader = newView % peerPorts.size();
        Message msg(viewChangeMsg.dump());
        sendTo(peerPorts[nextLeader], msg);
        std::cout << "[Node " << getNodeId() << "] Sent ViewChange(view=" << newView
                  << ", committed_seq=" << committedSeq << ") to next leader (Node " << peerPorts[nextLeader] << ").\n";
    } else {
        sendToAll(Message(viewChangeMsg.dump()));
        std::cout << "[Node " << getNodeId() << "] Broadcasted ViewChange(view=" << newView
                  << ", committed_seq=" << committedSeq << ").\n";
    }
}

int Entity::allocateNextSequence() {
    std::lock_guard<std::mutex> g(clientRequestMtx);
    int seq = nextSequenceNumber.fetch_add(1) + 1; // start at 1
    int curr = entityInfo["sequence"].get<int>();
    if (seq > curr) entityInfo["sequence"] = seq;
    return seq;
}

bool Entity::processJsonFromGrpc(const std::string& jsonPayload) {
    try {
        std::lock_guard<std::mutex> lk(eventMtx);
        Message msg(jsonPayload);
        handleEvent(&msg, &_entityState);
        return true;
    } catch (...) {
        return false;
    }
}

void Entity::startGrpcServer() {
    int grpcPort = 15000 + nodeId;
    grpcSvc_ = std::make_unique<NodeServiceImpl>(*this);  // changed to pass Entity&

    grpc::ServerBuilder builder;
    std::string addr = "0.0.0.0:" + std::to_string(grpcPort);
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(grpcSvc_.get());
    grpcServer_ = builder.BuildAndStart();
    if (!grpcServer_) {
        std::cerr << "[Node " << getNodeId() << "] Failed to start gRPC server on " << addr << "\n";
        grpcSvc_.reset();
        return;
    }
    grpcThread_ = std::thread([this, grpcPort]{
        std::cout << "[Node " << getNodeId() << "] gRPC listening on " << grpcPort << "\n";
        grpcServer_->Wait();
    });
}

void Entity::stopGrpcServer() {
    if (grpcServer_) grpcServer_->Shutdown();
    if (grpcThread_.joinable()) grpcThread_.join();
    grpcServer_.reset();
    grpcSvc_.reset();
}

// Create gRPC stubs to peers (skip self and invalid entries)
void Entity::initGrpcStubs() {
    std::lock_guard<std::mutex> lk(grpcStubsMtx_);
    grpcStubs_.clear();

    auto makeStub = [&](int idOrPort) {
        int port = grpcPortForPeer(idOrPort);
        if (port < 0) {
            std::cerr << "[Node " << getNodeId() << "] Skipping invalid peer entry " << idOrPort << "\n";
            return;
        }
        const std::string address = "127.0.0.1:" + std::to_string(port);
        auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
        grpcStubs_[idOrPort] = bedrock::Node::NewStub(channel);
    };

    for (int peer : peerPorts) {
        if (peer == nodeId) continue;            // skip self by nodeId form
        if (peer == (5000 + nodeId)) continue;   // skip self by TCP form
        if (grpcStubs_.find(peer) == grpcStubs_.end()) {
            makeStub(peer);
        }
    }
}

bedrock::Node::Stub* Entity::getStub(int peer) {
    std::lock_guard<std::mutex> lk(grpcStubsMtx_);
    auto it = grpcStubs_.find(peer);
    if (it != grpcStubs_.end()) return it->second.get();

    int port = grpcPortForPeer(peer);
    if (port < 0) {
        std::cerr << "[Node " << getNodeId() << "] Invalid peer " << peer << " (no gRPC mapping)\n";
        return nullptr;
    }
    const std::string address = "127.0.0.1:" + std::to_string(port);
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    grpcStubs_[peer] = bedrock::Node::NewStub(channel);
    return grpcStubs_[peer].get();
}

