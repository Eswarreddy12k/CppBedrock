#pragma once
#include <string>
#include "proto/bedrock.pb.h"
#include "../../../include/core/events/Message.h"
#include <nlohmann/json.hpp>

class ProtoMessage : public Message {
public:
    explicit ProtoMessage(const bedrock::ProtocolEnvelope& env)
    : Message(toJsonString(env)), env_(env) {}

    static std::string toJsonString(const bedrock::ProtocolEnvelope& env) {
        nlohmann::json j;
        if (env.has_pre_prepare()) {
            const auto& m = env.pre_prepare();
            j = {
                {"type","PrePrepare"},
                {"view", m.view()},
                {"sequence", m.sequence()},
                {"timestamp", m.timestamp()},
                {"operation", m.operation()},
                {"transaction", {
                    {"from", m.transaction().from()},
                    {"to", m.transaction().to()},
                    {"amount", m.transaction().amount()}
                }},
                {"client_listen_port", m.client_listen_port()},
                {"signature", m.signature()},
                {"client_id", m.client_id()},
                {"message_sender_id", m.message_sender_id()}
            };
        } else if (env.has_prepare()) {
            const auto& m = env.prepare();
            j = {
                {"type","Prepare"},
                {"view", m.view()},
                {"sequence", m.sequence()},
                {"operation", m.operation()},
                {"message_sender_id", m.message_sender_id()}
            };
        } else if (env.has_commit()) {
            const auto& m = env.commit();
            j = {
                {"type","Commit"},
                {"view", m.view()},
                {"sequence", m.sequence()},
                {"operation", m.operation()},
                {"message_sender_id", m.message_sender_id()}
            };
        } else {
            return "{}";
        }
        return j.dump();
    }

    std::string phase() const {
        if (env_.has_pre_prepare()) return "PrePrepare";
        if (env_.has_prepare()) return "Prepare";
        if (env_.has_commit()) return "Commit";
        return "";
    }
    int view() const {
        if (env_.has_pre_prepare()) return env_.pre_prepare().view();
        if (env_.has_prepare()) return env_.prepare().view();
        if (env_.has_commit()) return env_.commit().view();
        return 0;
    }
    int sequence() const {
        if (env_.has_pre_prepare()) return env_.pre_prepare().sequence();
        if (env_.has_prepare()) return env_.prepare().sequence();
        if (env_.has_commit()) return env_.commit().sequence();
        return 0;
    }
    std::string operation() const {
        if (env_.has_pre_prepare()) return env_.pre_prepare().operation();
        if (env_.has_prepare()) return env_.prepare().operation();
        if (env_.has_commit()) return env_.commit().operation();
        return "";
    }
    int sender_id() const {
        if (env_.has_pre_prepare()) return env_.pre_prepare().message_sender_id();
        if (env_.has_prepare()) return env_.prepare().message_sender_id();
        if (env_.has_commit()) return env_.commit().message_sender_id();
        return 0;
    }

    // PrePrepare-only
    int client_listen_port() const { return env_.has_pre_prepare() ? env_.pre_prepare().client_listen_port() : -1; }
    std::string timestamp() const { return env_.has_pre_prepare() ? env_.pre_prepare().timestamp() : ""; }
    bool has_tx() const { return env_.has_pre_prepare(); }
    std::string tx_from() const { return env_.pre_prepare().transaction().from(); }
    std::string tx_to() const { return env_.pre_prepare().transaction().to(); }
    int tx_amount() const { return env_.pre_prepare().transaction().amount(); }
    std::string signature() const { return env_.has_pre_prepare() ? env_.pre_prepare().signature() : ""; }
    std::string client_id() const { return env_.has_pre_prepare() ? env_.pre_prepare().client_id() : ""; }

    const bedrock::ProtocolEnvelope& envelope() const { return env_; }

    bool execute(Entity*, const Message*, EntityState*) override { return true; }

private:
    bedrock::ProtocolEnvelope env_;
};