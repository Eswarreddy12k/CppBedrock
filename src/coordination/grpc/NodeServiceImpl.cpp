#include "coordination/grpc/NodeServiceImpl.h"
#include "core/Entity.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <iostream>

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
    // Debug log
    std::string kind = env->has_pre_prepare() ? "PrePrepare" :
                       env->has_prepare()     ? "Prepare" :
                       env->has_commit()      ? "Commit" : "Unknown";
    // std::cout << "[Node " << entity_.getNodeId() << "] Received " << kind << " via gRPC\n";

    bedrock::ProtocolEnvelope copy = *env;
    std::thread([this, envCopy = std::move(copy)]() mutable {
        entity_.processProtocolEnvelope(envCopy);
    }).detach();
    ack->set_ok(true);
    ack->set_msg("accepted");
    return grpc::Status::OK;
}