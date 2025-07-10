#include "core/crypto/OpenSSLCryptoProvider.h"
#include "coordination/connections/TcpConnection.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <iostream>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <atomic>
#include <vector>
#include <mutex>
#include <queue>
#include <map>
#include <set>
#include <arpa/inet.h>

using json = nlohmann::json;

struct Transaction {
    std::string id;      // timestamp
    std::string payload; // serialized json
    int retries = 0;
};

int main(int argc, char* argv[]) {
    int leaderPort = 5001;
    int clientListenPort = 6000;
    int scenario = 1; // Default to 1=single

    // Parse scenario from command line argument if provided
    if (argc > 1) {
        scenario = std::stoi(argv[1]);
        if (scenario < 1 || scenario > 4) {
            std::cerr << "Invalid scenario. Use 1 (single), 2 (sequential), or 3 (concurrent)." << std::endl;
            return 1;
        }
    }

    std::atomic<bool> running{true};
    std::vector<std::string> responses;
    std::mutex responsesMutex;

    std::queue<Transaction> txnQueue;
    std::map<std::string, int> txnResponses;
    std::set<std::string> completedTxns;
    std::mutex txnMutex;

    std::vector<int> nodePorts = {5001, 5002, 5003, 5004, 5005, 5006, 5007};
    const int n = nodePorts.size();
    const int f = (n - 1) / 3;
    const int requiredResponses = 2 * f + 1;
    const int maxRetries = 5;
    const int responseTimeoutSec = 2;
    int NUM_REQUESTS = 3;

    // Start a server thread to listen for incoming responses
    std::thread serverThread([&]() {
        int listenSock = socket(AF_INET, SOCK_STREAM, 0);
        if (listenSock < 0) {
            std::cerr << "[Listener] Failed to create socket\n";
            return;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(clientListenPort);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(listenSock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "[Listener] Bind failed\n";
            close(listenSock);
            return;
        }
        listen(listenSock, 10);
        std::cout << "[Listener] Listening on port " << clientListenPort << std::endl;

        while (running) {
            sockaddr_in clientAddr;
            socklen_t clientAddrLen = sizeof(clientAddr);
            int respSock = accept(listenSock, (struct sockaddr*)&clientAddr, &clientAddrLen);
            if (respSock < 0) {
                if (running) std::cerr << "[Listener] Accept failed\n";
                continue;
            }
            char buffer[4096] = {0};
            int bytesReceived = recv(respSock, buffer, sizeof(buffer), 0);
            if (bytesReceived > 0) {
                std::string response(buffer, bytesReceived);
                {
                    std::lock_guard<std::mutex> lock(responsesMutex);
                    responses.push_back(response);
                }
                // Parse operation/timestamp from response and count
                try {
                    auto j = json::parse(response);
                    std::string op;
                    if (j.contains("operation")) {
                        op = j["operation"].get<std::string>();
                    } else if (j.contains("timestamp")) {
                        op = j["timestamp"].get<std::string>();
                    }
                    if (!op.empty()) {
                        std::lock_guard<std::mutex> txnLock(txnMutex);
                        txnResponses[op]++;
                        if (txnResponses[op] >= requiredResponses) {
                            completedTxns.insert(op);
                        }
                    }
                } catch (...) {}
                // std::cout << "[Listener] Received response: " << response << std::endl;
            }
            close(respSock);
        }
        close(listenSock);
    });

    OpenSSLCryptoProvider crypto("../keys/client_private.pem");
    auto startTime = std::chrono::high_resolution_clock::now();

    // === Transaction Preparation based on scenario ===
    if (scenario == 1) {
        // Multiple sequential requests
        NUM_REQUESTS = 3;
        for (int i = 0; i < NUM_REQUESTS; i++) {
            json transaction = {{"from", "A"}, {"to", "B"}, {"amount", 50}};
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            std::string timestamp = std::to_string(ms);
            std::string clientId = "client";
            json j = {
                {"type", "Request"},
                {"message_sender_id", clientId},
                {"timestamp", timestamp},
                {"transaction", transaction},
                {"view", 0},
                {"operation", timestamp},
                {"client_listen_port", clientListenPort}
            };
            std::string msgToSign = j["transaction"].dump() + j["timestamp"].get<std::string>();
            std::string signature = crypto.sign(msgToSign);
            j["signature"] = signature;
            std::string strtoSend = j.dump();
            txnQueue.push(Transaction{timestamp, strtoSend, 0});
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); // ensure unique timestamps
        }
    } else if (scenario == 2) {
        // Multiple sequential requests
        NUM_REQUESTS = 30;
        for (int i = 0; i < NUM_REQUESTS; i++) {
            json transaction = {{"from", "A"}, {"to", "B"}, {"amount", 50}};
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            std::string timestamp = std::to_string(ms);
            std::string clientId = "client";
            json j = {
                {"type", "Request"},
                {"message_sender_id", clientId},
                {"timestamp", timestamp},
                {"transaction", transaction},
                {"view", 0},
                {"operation", timestamp},
                {"client_listen_port", clientListenPort}
            };
            std::string msgToSign = j["transaction"].dump() + j["timestamp"].get<std::string>();
            std::string signature = crypto.sign(msgToSign);
            j["signature"] = signature;
            std::string strtoSend = j.dump();
            txnQueue.push(Transaction{timestamp, strtoSend, 0});
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); // ensure unique timestamps
        }
    } else if (scenario == 3) {
        // Concurrent requests: send all at once using threads
        std::vector<std::thread> clientThreads;
        NUM_REQUESTS = 3; // Number of concurrent requests
        for (int i = 0; i < NUM_REQUESTS; i++) {
            clientThreads.emplace_back([&, i]() {
                json transaction = {{"from", "A"}, {"to", "B"}, {"amount", 50}};
                auto now = std::chrono::system_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                std::string timestamp = std::to_string(ms) + "_" + std::to_string(i); // ensure uniqueness
                std::string clientId = "client";
                json j = {
                    {"type", "Request"},
                    {"message_sender_id", clientId},
                    {"timestamp", timestamp},
                    {"transaction", transaction},
                    {"view", 0},
                    {"operation", timestamp},
                    {"client_listen_port", clientListenPort}
                };
                std::string msgToSign = j["transaction"].dump() + j["timestamp"].get<std::string>();
                std::string signature = crypto.sign(msgToSign);
                j["signature"] = signature;
                std::string strtoSend = j.dump();

                // Send immediately in this thread
                try {
                    TcpConnection clientConn(leaderPort, false);
                    clientConn.send(strtoSend);
                    clientConn.closeConnection();
                } catch (...) {
                    std::cout << "[PBFTClient] Could not connect to leader.\n";
                }

                // Track for response counting
                {
                    std::lock_guard<std::mutex> lock(txnMutex);
                    txnResponses[timestamp] = 0;
                }
            });
        }
        for (auto& t : clientThreads) t.join();
    }
    else if (scenario == 4) {
        NUM_REQUESTS = 30;
        // Concurrent requests: send all at once using threads
        std::vector<std::thread> clientThreads;
        for (int i = 0; i < NUM_REQUESTS; i++) {
            clientThreads.emplace_back([&, i]() {
                json transaction = {{"from", "A"}, {"to", "B"}, {"amount", 50}};
                auto now = std::chrono::system_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                std::string timestamp = std::to_string(ms) + "_" + std::to_string(i); // ensure uniqueness
                std::string clientId = "client";
                json j = {
                    {"type", "Request"},
                    {"message_sender_id", clientId},
                    {"timestamp", timestamp},
                    {"transaction", transaction},
                    {"view", 0},
                    {"operation", timestamp},
                    {"client_listen_port", clientListenPort}
                };
                std::string msgToSign = j["transaction"].dump() + j["timestamp"].get<std::string>();
                std::string signature = crypto.sign(msgToSign);
                j["signature"] = signature;
                std::string strtoSend = j.dump();

                // Send immediately in this thread
                try {
                    TcpConnection clientConn(leaderPort, false);
                    clientConn.send(strtoSend);
                    clientConn.closeConnection();
                    std::cout << "[PBFTClient] Sent txn " << timestamp << " to leader\n";
                } catch (...) {
                    std::cout << "[PBFTClient] Could not connect to leader.\n";
                }

                // Track for response counting
                {
                    std::lock_guard<std::mutex> lock(txnMutex);
                    txnResponses[timestamp] = 0;
                }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (auto& t : clientThreads) t.join();
    }

    // === PBFT client logic: send, wait, retry ===
    while (!txnQueue.empty()) {
        Transaction txn = txnQueue.front();
        txnQueue.pop();

        bool enoughResponses = false;
        {
            std::lock_guard<std::mutex> lock(txnMutex);
            if (completedTxns.count(txn.id)) continue;
        }

        if (txn.retries == 0) {
            std::cout << "\n[PBFTClient] Sending txn " << txn.id << " to leader\n";
            try {
                TcpConnection clientConn(leaderPort, false);
                clientConn.send(txn.payload);
                clientConn.closeConnection();
            } catch (...) {
                std::cout << "[PBFTClient] Could not connect to leader.\n";
            }
        } else {
            std::cout << "\n[PBFTClient] Retrying txn " << txn.id << " to all nodes (retry " << txn.retries << ")\n";
            for (int port : nodePorts) {
                try {
                    TcpConnection nodeConn(port, false);
                    nodeConn.send(txn.payload);
                    nodeConn.closeConnection();
                    std::cout << "[PBFTClient] Retried txn " << txn.id << " to node on port " << port << "\n";
                } catch (...) {
                    std::cout << "[PBFTClient] Could not connect to node " << port << ".\n";
                }
            }
        }

        // Wait for responses for this transaction
        auto waitStart = std::chrono::steady_clock::now();
        while (true) {
            {
                std::lock_guard<std::mutex> lock(txnMutex);
                if (txnResponses[txn.id] >= requiredResponses) {
                    std::cout << "[PBFTClient] Got " << txnResponses[txn.id] << " responses for txn " << txn.id << "\n";
                    enoughResponses = true;
                    completedTxns.insert(txn.id);
                    break;
                }
            }
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - waitStart).count() > responseTimeoutSec) {
                std::cout << "[PBFTClient] Timeout waiting for txn " << txn.id << " responses, will retry if possible...\n";
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!enoughResponses && txn.retries < maxRetries) {
            Transaction retryTxn = txn;
            retryTxn.retries++;
            txnQueue.push(retryTxn); // retry
        } else if (!enoughResponses) {
            std::cout << "[PBFTClient] Failed to get enough responses for txn " << txn.id << " after " << maxRetries << " retries.\n";
        }
    }

    // Query balances from all nodes
    std::cout << "\n[IntegrationTest] Querying balances from all nodes...\n";
    for (int port : nodePorts) {
        try {
            TcpConnection nodeConn(port, false);
            json query = {
                {"type", "QueryBalances"},
                {"message_sender_id", "client"},
                {"timestamp", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count())},
                {"client_listen_port", clientListenPort}
            };
            std::string queryStr = query.dump();
            nodeConn.send(queryStr);
            nodeConn.closeConnection();
            std::cout << "[IntegrationTest] Sent balance query to node on port " << port << "\n";
        } catch (...) {
            std::cout << "[IntegrationTest] Could not connect to node " << port << ".\n";
        }
    }

    // Wait for responses for up to maxWaitSeconds, but exit early if enough responses are received
    const int maxWaitSeconds = 5;
    auto waitStart = std::chrono::steady_clock::now();
    while (true) {
        {
            std::lock_guard<std::mutex> lock(responsesMutex);
            if (responses.size() >= 5000) break;
        }
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - waitStart).count() > maxWaitSeconds) {
            std::cout << "[IntegrationTest] Timeout waiting for responses.\n";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Print all received responses (already printed in listener, but you can print again if needed)
    std::cout << "\n[IntegrationTest] All received responses:\n";
    for (const auto& resp : responses) {
        std::cout << resp << std::endl;
    }

    running = false;
    // Connect to self to unblock accept if needed
    int dummySock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in dummyAddr{};
    dummyAddr.sin_family = AF_INET;
    dummyAddr.sin_port = htons(clientListenPort);
    dummyAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    connect(dummySock, (struct sockaddr*)&dummyAddr, sizeof(dummyAddr));
    close(dummySock);

    if (serverThread.joinable()) serverThread.join();

    auto endTime = std::chrono::high_resolution_clock::now();
    auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
    std::cout << "[IntegrationTest] Time taken for " << NUM_REQUESTS << " requests: " << durationMs << " ms" << std::endl;

    std::cout << "Integration test complete.\n";
    return 0;
}