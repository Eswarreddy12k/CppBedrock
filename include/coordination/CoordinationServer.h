#pragma once

#include "Coordinator.h"
#include "connections/TcpConnection.h"
#include <unordered_map>
#include <string>

class CoordinationServer : public Coordinator {
public:
    CoordinationServer();

    void initialize() override;
    void start() override;
    void stop() override;
    
    void loadConfig(const std::string& configFile);
    void sendStartSignal();
    void sendStopSignal();

private:
    struct Transition {
        std::string targetState;
        std::string message;
    };

    std::unordered_map<std::string, std::unordered_map<std::string, Transition>> transitions;
};
