#include "coordination/grpc/NodeServiceImpl.h"
#include "core/Entity.h"
#include <nlohmann/json.hpp>
#include <thread>

using json = nlohmann::json;

NodeServiceImpl::NodeServiceImpl(Entity& entity) : entity_(entity) {}

grpc::Status NodeServiceImpl::SubmitRequest(grpc::ServerContext*,
                                            const bedrock::ClientRequest* req,
                                            bedrock::Ack* ack) {
    json j = {
        {"type", "Request"},
        {"message_sender_id", req->message_sender_id()},
        {"timestamp", req->timestamp()},
        {"transaction", {
            {"from", req->transaction().from()},
            {"to", req->transaction().to()},
            {"amount", req->transaction().amount()}
        }},
        {"view", req->view()},
        {"operation", req->operation()},
        {"client_listen_port", req->client_listen_port()},
        {"signature", req->signature()}
    };
    // Async dispatch to avoid re-entrancy/deadlocks
    std::string payload = j.dump();
    std::thread([this, payload]{
        (void)entity_.processJsonFromGrpc(payload);
    }).detach();

    ack->set_ok(true);
    ack->set_msg("accepted");
    return grpc::Status::OK;
}

grpc::Status NodeServiceImpl::SendRawJson(grpc::ServerContext*,
                                          const bedrock::RawJson* request,
                                          bedrock::RawJson* response) {
    // Fire-and-forget to avoid blocking cycles
    std::string payload = request->json();
    std::thread([this, payload]{
        (void)entity_.processJsonFromGrpc(payload);
    }).detach();

    response->set_json(R"({"status":"accepted"})");
    return grpc::Status::OK;
}

grpc::Status NodeServiceImpl::SendProtocol(grpc::ServerContext*,
                                           const bedrock::ProtocolEnvelope* env,
                                           bedrock::Ack* ack) {
    json j;
    if (env->has_pre_prepare()) {
        const auto& m = env->pre_prepare();
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
            {"message_sender_id", m.message_sender_id()}
        };
    } else if (env->has_prepare()) {
        const auto& m = env->prepare();
        j = {
            {"type","Prepare"},
            {"view", m.view()},
            {"sequence", m.sequence()},
            {"operation", m.operation()},
            {"message_sender_id", m.message_sender_id()}
        };
    } else if (env->has_commit()) {
        const auto& m = env->commit();
        j = {
            {"type","Commit"},
            {"view", m.view()},
            {"sequence", m.sequence()},
            {"operation", m.operation()},
            {"message_sender_id", m.message_sender_id()}
        };
    } else {
        ack->set_ok(false);
        ack->set_msg("unknown envelope");
        return grpc::Status::OK;
    }

    // Async to avoid re-entrancy
    std::string payload = j.dump();
    std::thread([this, payload]{
        (void)entity_.processJsonFromGrpc(payload);
    }).detach();

    ack->set_ok(true);
    ack->set_msg("accepted");
    return grpc::Status::OK;
}