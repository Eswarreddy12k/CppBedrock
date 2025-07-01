#pragma once

#include "Coordinator.h"
#include "../core/state/StateMachine.h"
#include "../core/state/EntityState.h"
#include "connections/TcpConnection.h"
#include "../core/Entity.h"
#include "../core/crypto/OpenSSLCryptoProvider.h"

class CoordinationUnit : public Coordinator {
public:
    CoordinationUnit();
    
    void initialize() override;
    void start() override;
    void stop() override;
    void initializeEntities();
    void receiveMessage();

    std::unique_ptr<Entity> entity1, entity2, entity3, entity4, entity5, entity6, entity7;
    bool entitiesStarted = false;

private:
    TcpConnection server;
    StateMachine stateMachine;
    EntityState entityState;
    std::unique_ptr<Entity> entity;
    bool running;
};
