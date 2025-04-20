#include "../../include/coordination/CoordinationUnit.h"
#include "../../include/core/state/StateMachine.h"
#include "../../include/core/Entity.h"
#include <nlohmann/json.hpp>
#include <iostream>

using json = nlohmann::json;

CoordinationUnit::CoordinationUnit()
    : server(50001, true), stateMachine(), entityState("Replica", "Idle", 0, 0) {
}

void CoordinationUnit::initialize() {
    std::cout << "Coordination Unit initialized." << std::endl;
}

void CoordinationUnit::start() {
    std::cout << "Coordination Unit started." << std::endl;
    std::cout << "Coordination Unit started." << std::endl;
    server.startListening(); // Start TCP listener in a separate thread
    running=true;
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

    if (receivedMessage == "start the units") {
        Entity entity1("Leader",1,{1,2,3,4,5,6,7});
        Entity entity2("Node2",2,{1,2,3,4,5,6,7});
        Entity entity3("Node3",3,{1,2,3,4,5,6,7});
        Entity entity4("Node4",4,{1,2,3,4,5,6,7});
        Entity entity5("Node5",5,{1,2,3,4,5,6,7});
        Entity entity6("Node6",6,{1,2,3,4,5,6,7});
        Entity entity7("Node7",7,{1,2,3,4,5,6,7});
        
        // Start all entities
        entity1.start();
        entity2.start();
        entity3.start();
        entity4.start();
        entity5.start();
        entity6.start();
        entity7.start();

        // Wait for network setup
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // Send multiple PBFT client requests
        const int NUM_REQUESTS = 3;
        for (int i = 0; i < NUM_REQUESTS; i++) {
            json j = {
                {"type", "PBFTRequest"},
                {"client", "Client-" + std::to_string(i)},
                {"operation", "Operation-" + std::to_string(i)},
                {"view", 0}
            };
            
            std::string strtoSend = j.dump();
            Message msg(strtoSend);
            
            std::cout << "\n[CoordinationUnit] Sending request " << i + 1 << " of " << NUM_REQUESTS << "\n";
            entity1.handleEvent(&msg, &entity1.getState());
            
            // Wait for consensus to complete before sending next request
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        
        // Allow time for final consensus to complete
        std::this_thread::sleep_for(std::chrono::seconds(10));
        
        // Stop all entities
        entity1.stop();
        entity2.stop();
        entity3.stop();
        entity4.stop();
        entity5.stop();
        entity6.stop();
        entity7.stop();
    }
}

