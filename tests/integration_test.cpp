#include "core/Entity.h"
#include "core/crypto/OpenSSLCryptoProvider.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>
#include <iostream>
#include <chrono>

using json = nlohmann::json;

int main() {
    // // Create 7 entities (nodes)
    // std::vector<int> peerIds = {1,2,3,4,5,6,7};
    // Entity entity1("Leader",1,peerIds);
    // Entity entity2("Node2",2,peerIds);
    // Entity entity3("Node3",3,peerIds);
    // Entity entity4("Node4",4,peerIds);
    // Entity entity5("Node5",5,peerIds);
    // Entity entity6("Node6",6,peerIds);
    // Entity entity7("Node7",7,peerIds);

    // // Start all entities
    // entity1.start();
    // entity2.start();
    // entity3.start();
    // entity4.start();
    // entity5.start();
    // entity6.start();
    // entity7.start();

    // // Wait for network setup
    // std::this_thread::sleep_for(std::chrono::seconds(2));

    // // Start timing
    // auto startTime = std::chrono::high_resolution_clock::now();

    // // Send multiple PBFT client requests
    // OpenSSLCryptoProvider crypto("../keys/client_private.pem");

    // const int NUM_REQUESTS = 3;
    // for (int i = 0; i < NUM_REQUESTS; i++) {
    //     // Prepare transaction details
    //     json transaction = {
    //         {"from", "A"},
    //         {"to", "B"},
    //         {"amount", 50}
    //     };
    //     auto now = std::chrono::system_clock::now();
    //     auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    //     std::string timestamp = std::to_string(ms);
    //     std::string clientId = "client";
    //     json j = {
    //         {"type", "Request"},
    //         {"message_sender_id", clientId},
    //         {"timestamp", timestamp},
    //         {"transaction", transaction},
    //         {"view", 0},
    //         {"operation", timestamp}
    //     };

    //     // Sign the request (excluding the signature field)
    //     std::string msgToSign = j["transaction"].dump() + j["timestamp"].get<std::string>();
    //     std::string signature = crypto.sign(msgToSign);
    //     j["signature"] = signature;

    //     std::string strtoSend = j.dump();
    //     Message msg(strtoSend);

    //     std::cout << "\n[IntegrationTest] Sending request " << i + 1 << " of " << NUM_REQUESTS << "\n";
    //     entity1.handleEvent(&msg, &entity1.getState());

    //     // Wait for consensus to complete before sending next request
    //     std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // }

    // // End timing
    // auto endTime = std::chrono::high_resolution_clock::now();
    // auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
    // std::cout << "[IntegrationTest] Time taken for " << NUM_REQUESTS << " requests: " << durationMs << " ms" << std::endl;

    // // Sleep to make sure all messages are processed and entities ready to be stopped
    // std::this_thread::sleep_for(std::chrono::seconds(20));

    // // Stop all entities
    // entity1.stop();
    // entity2.stop();
    // entity3.stop();
    // entity4.stop();
    // entity5.stop();
    // entity6.stop();
    // entity7.stop();

    // std::cout << "Integration test complete.\n";
    return 0;
}