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


class SpeculativeCompleteEvent : public BaseEvent {
public:
    SpeculativeCompleteEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState* state) override {
        auto j = nlohmann::json::parse(message->getContent());
        std::string operation = j.value("operation", "");
        int seq = j.value("sequence", -1);

        // Use timestamp as unique transaction ID
        std::string txnId = j.value("timestamp", "");
        if (!txnId.empty() && !entity->hasExecutedSpeculativeTransaction(txnId)) {
            if (j.contains("transaction")) {
                auto tx = j["transaction"];
                std::string from = tx.value("from", "");
                std::string to = tx.value("to", "");
                int amount = tx.value("amount", 0);

                if (!from.empty() && !to.empty() && amount > 0) {
                    entity->updateSpeculativeBalances(from, to, amount);
                    entity->appendSpeculativeEntry(seq, txnId, from, to, amount); // NEW: log it
                    entity->markSpeculativeTransactionExecuted(txnId);
                    std::cout << "[Node " << entity->getNodeId() << "] SPECULATIVE Transaction: "
                              << from << " -> " << to << " : " << amount
                              << " Sequence: " << seq << std::endl;
                }
            }
        }

        entity->markOperationProcessed(seq);

        // Send speculative response to client
        if (j.contains("client_listen_port")) {
            int clientPort = j.value("client_listen_port", -1);
            nlohmann::json response;
            response["type"] = "Response";
            response["view"] = j.value("view", -1);
            response["sequence"] = seq;
            response["timestamp"] = j.value("timestamp", "");
            response["message_sender_id"] = entity->getNodeId();
            response["result"] = "speculative";
            response["clientid"] = j.value("clientid", "");
            Message BalancesReply(response.dump());
            if (clientPort != -1) {
                entity->sendTo(clientPort - 5000, BalancesReply);
            }
        }
        return true;
    }
};

// Commit-certificate handler for Zyzzyva slow-path
class CommitCertificateEvent : public BaseEvent {
public:
    CommitCertificateEvent(const nlohmann::json& params = {}) : BaseEvent(params) {}
    bool execute(Entity* entity, const Message* message, EntityState*) override {
        auto j = nlohmann::json::parse(message->getContent());
        // Prefer explicit sequence; else derive from txnId (timestamp)
        int s = j.value("sequence", -1);
        std::string txnId = j.value("timestamp", "");
        if (s < 0 && !txnId.empty()) {
            s = entity->findSeqByTxnId(txnId);
        }
        if (s < 0) {
            std::cout << "[Node " << entity->getNodeId() << "] CommitCertificateEvent: missing sequence, ignoring\n";
            return false;
        }

        // 1) Commit all entries with seq ≤ s
        std::vector<int> toErase;
        {
            std::lock_guard<std::mutex> g(entity->speculativeLogMtx);
            for (const auto& [seq, e] : entity->speculativeLog) {
                if (seq <= s) {
                    entity->updateBalances(e.from, e.to, e.amount);        // durable
                    toErase.push_back(seq);
                }
            }
            for (int seq : toErase) {
                auto it = entity->speculativeLog.find(seq);
                if (it != entity->speculativeLog.end()) {
                    entity->executedSpeculativeTransactions.erase(it->second.txnId);
                    entity->speculativeLog.erase(it);
                }
            }
        }
        entity->committedSeq = std::max(entity->committedSeq, s);

        // 2) Rebuild speculative balances from remaining suffix
        entity->rebuildSpeculativeBalancesFromLog();

        // Optional: ack client
        if (j.contains("client_listen_port")) {
            int clientPort = j.value("client_listen_port", -1);
            if (clientPort != -1) {
                nlohmann::json resp{
                    {"type","Response"},
                    {"sequence", s},
                    {"timestamp", txnId},
                    {"message_sender_id", entity->getNodeId()}
                };
                Message ack(resp.dump());
                entity->sendTo(clientPort - 5000, ack);
            }
        }
        std::cout << "[Node " << entity->getNodeId() << "] Committed up to seq " << s << " (slow-path)\n";
        return true;
    }
};

// Registration function for uncommon events
void registerUncommonEvents(EventFactory& factory) {
    factory.registerEvent<UncommonEvent>("uncommonEvent");
    factory.registerEvent<VerifyPBFTConditionsEvent>("verifyPBFTConditions");
    factory.registerEvent<StoreNewViewMessageEvent>("storeNewViewMessage");
    factory.registerEvent<CheckNewViewQuorumEvent>("checkNewViewQuorum");
    factory.registerEvent<SendNewViewToNextLeaderEvent>("sendNewViewToNextLeader");
    factory.registerEvent<SpeculativeCompleteEvent>("speculativeComplete");
    factory.registerEvent<CommitCertificateEvent>("commitCertificate"); // NEW
}

// Explicit template instantiation
template void EventFactory::registerEvent<UncommonEvent>(const std::string&);
template void EventFactory::registerEvent<VerifyPBFTConditionsEvent>(const std::string&);
template void EventFactory::registerEvent<StoreNewViewMessageEvent>(const std::string&);
template void EventFactory::registerEvent<CheckNewViewQuorumEvent>(const std::string&);
template void EventFactory::registerEvent<SendNewViewToNextLeaderEvent>(const std::string&);
template void EventFactory::registerEvent<SpeculativeCompleteEvent>(const std::string&);
template void EventFactory::registerEvent<CommitCertificateEvent>(const std::string&); // NEW