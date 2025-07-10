#pragma once

#include "events/EventHandler.h"
#include "events/BaseEvent.h"
#include "state/StateMachine.h"
#include "state/DataSet.h"
#include "state/EntityState.h"
#include "pipeline/Pipeline.h"
#include "../coordination/connections/TcpConnection.h"
#include <vector>
#include <memory>
#include <yaml-cpp/yaml.h>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <thread>
#include <nlohmann/json.hpp>
#include <map>
#include "TimeKeeper.h"
#include <atomic>
#include "crypto/CryptoProvider.h"
#include "crypto/OpenSSLCryptoProvider.h"
#include "crypto/CryptoUtils.h"
#include <string>
#include <mutex>

class Event;
class Message;

struct ProtocolMessageRecord {
    int seq;
    int senderId;
    std::string operation;
    std::string phase;         // e.g., "prepare", "commit"
    std::string protocolName;  // e.g., "Hotstuff"
    std::map<std::string, std::string> customData; // default: empty
    ProtocolMessageRecord(int s, int sid, const std::string& op, const std::string& ph, const std::string& proto,
                         const std::map<std::string, std::string>& custom = {})
        : seq(s), senderId(sid), operation(op), phase(ph), protocolName(proto), customData(custom) {}
    ProtocolMessageRecord() = default;
};

// Store all view change data for each view as an array of structs
struct ViewChangeData {
    int sender;
    int last_sequence;
    std::string last_operation;
    nlohmann::json locked_qc;
};



class Entity : public EventHandler<EntityState> {
    friend class Event; // <-- Add this line
public:
    Entity(const std::string& role, int id, const std::vector<int>& peers, bool byzantine = false);
    ~Entity(); // <-- Add this line
    void start();
    void stop();
    void handleEvent(const Event* event, EntityState* context) override;
    EntityState& getState();
    void sendToAll(const Message& message);
    void sendTo(int peerId, const Message& message);
    void processMessages();
    void loadProtocolConfig(const std::string& configFile);

    int getNodeId() const { return nodeId; }
    int getF() const { return f; }
    int getView() const { return _entityState.getView(); }
    void storePrepareMessage(int nodeId, int sequence);
    void storeCommitMessage(int nodeId, int sequence);
    void storePrePrepareMessage(int nodeId, int sequence);
    void storePrePrepareMessage(int nodeId, int sequence, const std::string& operation);
    void storePrepareMessage(int nodeId, int sequence, const std::string& operation);
    void storeCommitMessage(int nodeId, int sequence, const std::string& operation);
    int getPrepareCount(int sequence) const;
    int getCommitCount(int sequence) const;
    int getPrePrepareCount(int sequence) const;
    void printDataStore();
    void printCommittedMessages();
    void loadOrInitDataset();
    void updateEntityInfoField(const std::string& key, const nlohmann::json& value);
    
    void saveEntityInfo();
    // Protocol config access
    //const YAML::Node& getPhaseConfig(const std::string& phase) const;
    YAML::Node getPhaseConfig(const std::string& phase) const;

    // Protocol-agnostic verification
    bool runVerification(const std::string& verifyType, const nlohmann::json& msg, EntityState* context);

    int getNextSequenceNumber() { return nextSequenceNumber++; }
    int assignSequenceNumber() {
        return nextSequenceNumber+1;
    }

    void removeSequenceState(int seq);

    const std::unordered_map<int, std::string>& getPrePrepareOperations() const { return prePrepareOperations; }

    std::set<int> prePrepareBroadcasted;

    bool hasProcessedOperation(const int operation) const {
        return processedOperations.find(operation) != processedOperations.end();
    }
    
    void markOperationProcessed(const int operation);

    void updateBalances(const std::string& from, const std::string& to, int amount) {
        std::lock_guard<std::mutex> lock(balancesMutex);
        if (balances.find(from) == balances.end()) balances[from] = 100;
        if (balances.find(to) == balances.end()) balances[to] = 100;
        balances[from] -= amount;
        balances[to] += amount;
    }

    std::vector<int> peerPorts;
    std::map<std::string, int> balances;
    std::mutex balancesMutex;
    std::map<int, EntityState> sequenceStates;
    std::unordered_map<int, std::set<int>> prePrepareMessages;
    std::unordered_map<int, std::set<int>> prepareMessages;
    std::unordered_map<int, std::set<int>> commitMessages;
    std::unordered_map<int, std::string> prePrepareOperations;
    std::unordered_map<int, std::string> prepareOperations;
    std::unordered_map<int, std::string> commitOperations;
    std::unordered_map<int, std::vector<nlohmann::json>> viewChangeMessages;
    bool inViewChange = false;
    std::unique_ptr<TimeKeeper> timeKeeper;
    std::mutex timerMtx;

    // Track received messages: [sequence][type][sender]
    std::unordered_map<int, std::unordered_map<std::string, std::set<int>>> receivedMessages;
    std::set<int> processedOperations;  // Track completed operations
    std::unordered_map<int, std::atomic<bool>> preparePhaseTimerRunning;
    std::unordered_map<std::string, std::unique_ptr<BaseEvent>> actions;

    std::map<int, std::vector<ProtocolMessageRecord>> allMessagesBySeq;
    std::unordered_map<int, std::vector<ViewChangeData>> viewChangeDataArray;

    DataSet dataset;
    nlohmann::json entityInfo;
    std::unique_ptr<CryptoProvider> cryptoProvider;
    YAML::Node protocolConfig;
    void onTimeout();

    bool isByzantine;
    std::unordered_map<int, std::unique_ptr<TimeKeeper>> prepareTimers;

    // Add this line:
    std::unordered_map<std::string, std::unordered_set<int>> keyToSenderIds;

private:
    int nodeId;
    int f;
    EntityState _entityState;
    TcpConnection connection;
    std::thread processingThread;

    
    

    
    std::atomic<bool> running = false;
    int nextSequenceNumber = 0;

    

    

    
};

