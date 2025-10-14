#pragma once

#include <grpcpp/grpcpp.h>
#include "proto/bedrock.grpc.pb.h"

class Entity;

class NodeServiceImpl final : public bedrock::Node::Service {
public:
    explicit NodeServiceImpl(Entity& entity);

    grpc::Status SubmitRequest(grpc::ServerContext*,
                               const bedrock::ClientRequest* request,
                               bedrock::Ack* response) override;

    grpc::Status SendRawJson(grpc::ServerContext*,
                             const bedrock::RawJson* request,
                             bedrock::RawJson* response) override;

    // New typed handler
    grpc::Status SendProtocol(grpc::ServerContext*,
                              const bedrock::ProtocolEnvelope* env,
                              bedrock::Ack* ack) override;

private:
    Entity& entity_;
};