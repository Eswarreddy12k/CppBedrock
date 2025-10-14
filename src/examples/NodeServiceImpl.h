#pragma once
#include "bedrock.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <atomic>

class NodeServiceImpl final : public bedrock::NodeService::Service {
public:
    explicit NodeServiceImpl(int nodeId) : selfId(nodeId) {}
    ::grpc::Status PingPong(::grpc::ServerContext*,
                            const bedrock::Ping* req,
                            bedrock::Pong* resp) override;
private:
    int selfId;
};