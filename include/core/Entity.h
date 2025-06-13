#pragma once

#include "events/EventHandler.h"
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
#include <set>
#include <thread>
#include <nlohmann/json.hpp>
#include <map>
#include "TimeKeeper.h"
#include <atomic>

class Event;
class Message;

class Entity : public EventHandler<EntityState> {
    friend class Event; // <-- Add this line
public:
    Entity(const std::string& role, int id, const std::vector<int>& peers);
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

    // Protocol config access
    //const YAML::Node& getPhaseConfig(const std::string& phase) const;
    YAML::Node getPhaseConfig(const std::string& phase) const;

    // Protocol-agnostic verification
    bool runVerification(const std::string& verifyType, const nlohmann::json& msg, EntityState* context);

    int getNextSequenceNumber() { return nextSequenceNumber++; }

    void removeSequenceState(int seq);

    const std::unordered_map<int, std::string>& getPrePrepareOperations() const { return prePrepareOperations; }

    std::set<int> prePrepareBroadcasted;

    bool hasProcessedOperation(const int operation) const {
        return processedOperations.find(operation) != processedOperations.end();
    }
    
    void markOperationProcessed(const int operation);

    std::vector<int> peerPorts;
    std::map<int, EntityState> sequenceStates;
    std::unordered_map<int, std::set<int>> prePrepareMessages;
    std::unordered_map<int, std::set<int>> prepareMessages;
    std::unordered_map<int, std::set<int>> commitMessages;
    std::unordered_map<int, std::string> prePrepareOperations;
    std::unordered_map<int, std::string> prepareOperations;
    std::unordered_map<int, std::string> commitOperations;
    std::unordered_map<int, std::set<int>> viewChangeMessages;
    bool inViewChange = false;
    std::unique_ptr<TimeKeeper> timeKeeper;
    std::mutex timerMtx;

    // Track received messages: [sequence][type][sender]
    std::unordered_map<int, std::unordered_map<std::string, std::set<int>>> receivedMessages;
    std::set<int> processedOperations;  // Track completed operations
    std::unordered_map<int, std::atomic<bool>> preparePhaseTimerRunning;
    std::unordered_map<std::string, std::unique_ptr<Event>> actions;

private:
    int nodeId;
    int f;
    EntityState _entityState;
    TcpConnection connection;
    std::thread processingThread;

    YAML::Node protocolConfig;
    

    DataSet dataset;

    int nextSequenceNumber = 0;

    

    void onTimeout();
};

