#include "NodeServiceImpl.h"
#include <iostream>

::grpc::Status NodeServiceImpl::PingPong(::grpc::ServerContext*,
                                         const bedrock::Ping* req,
                                         bedrock::Pong* resp) {
    std::cout << "[gRPC] Ping logical_time=" << req->logical_time() << std::endl;
    resp->set_node_id(selfId);
    resp->set_logical_time(req->logical_time());
    return ::grpc::Status::OK;
}