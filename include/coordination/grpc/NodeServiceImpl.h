#pragma once

#include <grpcpp/grpcpp.h>
#include "proto/bedrock.grpc.pb.h"

class Entity;

class NodeServiceImpl final : public bedrock::Node::Service {
public:
    explicit NodeServiceImpl(Entity& entity);

    grpc::Status SubmitRequest(grpc::ServerContext*,
                               const bedrock::ClientRequest*,
                               bedrock::Ack*) override;

    grpc::Status SendRawJson(grpc::ServerContext*,
                             const bedrock::RawJson*,
                             bedrock::RawJson*) override;

    grpc::Status SendProtocol(grpc::ServerContext*,
                              const bedrock::ProtocolEnvelope*,
                              bedrock::Ack*) override;

private:
    Entity& entity_;
};