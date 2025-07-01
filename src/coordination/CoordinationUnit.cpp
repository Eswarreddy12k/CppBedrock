#include "../../include/coordination/CoordinationUnit.h"
#include "../../include/core/state/StateMachine.h"
#include "../../include/core/Entity.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <memory>

using json = nlohmann::json;

CoordinationUnit::CoordinationUnit()
    : server(50001, true), stateMachine(), entityState("Replica", "Idle", 0, 0), entitiesStarted(false) {
}

void CoordinationUnit::initialize() {
    std::cout << "Coordination Unit initialized." << std::endl;
}

void CoordinationUnit::start() {
    std::cout << "Coordination Unit started." << std::endl;
    server.startListening(); // Start TCP listener in a separate thread
    running = true;
    // Start a new thread to continuously receive messages
    std::thread listeningThread = std::thread([this]() {
        while (running) {  // Ensure we can stop the loop when needed
            receiveMessage();
        }
    });

    listeningThread.detach(); // Run independently
}

void CoordinationUnit::stop() {
    std::cout << "Coordination Unit stopped." << std::endl;
    running = false; // Ensure the loop in start() exits
    server.stopListening(); // Stop the listener thread gracefully
}

void CoordinationUnit::initializeEntities() {
    std::cout << "Initializing entities in the coordination unit." << std::endl;
}

void CoordinationUnit::receiveMessage() {
    std::string receivedMessage = server.receive();
    std::cout << "Coordination Unit received message: " << receivedMessage << std::endl;

    if (receivedMessage == "start the units" && !entitiesStarted) {
        entitiesStarted = true;
        entity1 = std::make_unique<Entity>("Leader", 1, std::vector<int>{1, 2, 3, 4, 5, 6, 7});
        entity2 = std::make_unique<Entity>("Node2", 2, std::vector<int>{1, 2, 3, 4, 5, 6, 7});
        entity3 = std::make_unique<Entity>("Node3", 3, std::vector<int>{1, 2, 3, 4, 5, 6, 7});
        entity4 = std::make_unique<Entity>("Node4", 4, std::vector<int>{1, 2, 3, 4, 5, 6, 7});
        entity5 = std::make_unique<Entity>("Node5", 5, std::vector<int>{1, 2, 3, 4, 5, 6, 7});
        entity6 = std::make_unique<Entity>("Node6", 6, std::vector<int>{1, 2, 3, 4, 5, 6, 7});
        entity7 = std::make_unique<Entity>("Node7", 7, std::vector<int>{1, 2, 3, 4, 5, 6, 7});
        entity1->start();
        entity2->start();
        entity3->start();
        entity4->start();
        entity5->start();
        entity6->start();
        entity7->start();
        std::cout << "[CoordinationUnit] Entities started.\n";
    } else if (receivedMessage == "stop the units" && entitiesStarted) {
        entity1->stop();
        entity2->stop();
        entity3->stop();
        entity4->stop();
        entity5->stop();
        entity6->stop();
        entity7->stop();
        entitiesStarted = false;
        std::cout << "[CoordinationUnit] Entities stopped.\n";
    } else if (entitiesStarted) {
        // Assume any other message is a client request (JSON string)
        try {
            nlohmann::json j = nlohmann::json::parse(receivedMessage);
            Message msg(receivedMessage);
            entity1->handleEvent(&msg, &entity1->getState());
            std::cout << "[CoordinationUnit] Forwarded client request to leader.\n";
        } catch (...) {
            std::cout << "[CoordinationUnit] Received non-JSON message while running.\n";
        }
    }
}


