#pragma once

#include <memory>

namespace bedrockgrpc {
bool StartGrpcServer(int grpcPort, int backendTcpPort);
void StopGrpcServer();
}