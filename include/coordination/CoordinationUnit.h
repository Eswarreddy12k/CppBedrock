#pragma once

#include "Coordinator.h"
#include "../core/state/StateMachine.h"
#include "../core/state/EntityState.h"
#include "connections/TcpConnection.h"
#include "../core/Entity.h"

class CoordinationUnit : public Coordinator {
public:
    CoordinationUnit();
    
    void initialize() override;
    void start() override;
    void stop() override;
    void initializeEntities();
    void receiveMessage();

private:
    TcpConnection server;
    StateMachine stateMachine;
    EntityState entityState;
    std::unique_ptr<Entity> entity;
    bool running;
};
