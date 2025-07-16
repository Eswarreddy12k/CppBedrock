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
        if (scenario < 1 || scenario > 5) {
            std::cerr << "Invalid scenario. Use 1 (single), 2 (sequential), 3 (concurrent), 4 (randomized), or 5 (failure test)." << std::endl;
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

    std::map<std::string, int> initialBalances = {
        {"A", 100},
        {"B", 100},
        {"C", 100},
        {"D", 100}
    };
    std::vector<std::tuple<std::string, std::string, int>> transactions;

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
        // Example: A->B, B->C, C->D
        transactions = {
            {"A", "B", 30},
            {"B", "C", 20},
            {"C", "D", 10}
        };
        NUM_REQUESTS = transactions.size();
        int txnIdx = 0;
        for (const auto& [from, to, amount] : transactions) {
            json transaction = {{"from", from}, {"to", to}, {"amount", amount}};
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            std::string timestamp = std::to_string(ms) + "_" + std::to_string(txnIdx++);
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
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } else if (scenario == 2) {
        // Example: C->A, D->B, A->C, B->D, repeated
        NUM_REQUESTS = 10;
        transactions.clear();
        for (int i = 0; i < NUM_REQUESTS; ++i) {
            if (i % 4 == 0) transactions.push_back({"C", "A", 15});
            else if (i % 4 == 1) transactions.push_back({"D", "B", 25});
            else if (i % 4 == 2) transactions.push_back({"A", "C", 10});
            else transactions.push_back({"B", "D", 20});
        }
        int txnIdx = 0;
        for (const auto& [from, to, amount] : transactions) {
            json transaction = {{"from", from}, {"to", to}, {"amount", amount}};
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            std::string timestamp = std::to_string(ms) + "_" + std::to_string(txnIdx++);
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
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } else if (scenario == 3) {
        // Example: D->A, C->B, B->C, A->D
        transactions = {
            {"D", "A", 5},
            {"C", "B", 10},
            {"B", "C", 20},
            {"A", "D", 15}
        };
        NUM_REQUESTS = transactions.size();
        std::vector<std::thread> clientThreads;
        int txnIdx = 0;
        for (const auto& txn : transactions) {
            clientThreads.emplace_back([&, txnIdx, txn]() {
                auto [from, to, amount] = txn;
                json transaction = {{"from", from}, {"to", to}, {"amount", amount}};
                auto now = std::chrono::system_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                std::string timestamp = std::to_string(ms) + "_" + std::to_string(txnIdx);
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
                try {
                    TcpConnection clientConn(leaderPort, false);
                    clientConn.send(strtoSend);
                    clientConn.closeConnection();
                } catch (...) {
                    std::cout << "[PBFTClient] Could not connect to leader.\n";
                }
                {
                    std::lock_guard<std::mutex> lock(txnMutex);
                    txnResponses[timestamp] = 0;
                }
            });
            txnIdx++;
        }
        for (auto& t : clientThreads) t.join();
    }
    else if (scenario == 4) {
        NUM_REQUESTS = 20;
        transactions.clear();
        for (int i = 0; i < NUM_REQUESTS; ++i) {
            // Randomize or cycle through clients
            std::string from = (i % 4 == 0) ? "A" : (i % 4 == 1) ? "B" : (i % 4 == 2) ? "C" : "D";
            std::string to = (i % 4 == 0) ? "B" : (i % 4 == 1) ? "C" : (i % 4 == 2) ? "D" : "A";
            int amount = 5 + (i % 5) * 5;
            transactions.push_back({from, to, amount});
        }
        std::vector<std::thread> clientThreads;
        int txnIdx = 0;
        for (const auto& txn : transactions) {
            clientThreads.emplace_back([&, txnIdx, txn]() {
                auto [from, to, amount] = txn;
                json transaction = {{"from", from}, {"to", to}, {"amount", amount}};
                auto now = std::chrono::system_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                std::string timestamp = std::to_string(ms) + "_" + std::to_string(txnIdx);
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
                // std::cout << "[PBFTClient] Preparing transaction: " << timestamp << "\n";
                std::string msgToSign = j["transaction"].dump() + j["timestamp"].get<std::string>();
                std::string signature = crypto.sign(msgToSign);
                j["signature"] = signature;
                std::string strtoSend = j.dump();
                try {
                    TcpConnection clientConn(leaderPort, false);
                    clientConn.send(strtoSend);
                    clientConn.closeConnection();
                } catch (...) {
                    std::cout << "[PBFTClient] Could not connect to leader.\n";
                }
                {
                    std::lock_guard<std::mutex> lock(txnMutex);
                    txnResponses[timestamp] = 0;
                }
            });
            txnIdx++;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (auto& t : clientThreads) t.join();
    }
    else if (scenario == 5) {
        // Example: A->B, B->C, C->D, D->A
        transactions = {
            {"A", "B", 25},
            {"B", "C", 15},
            {"C", "D", 10},
            {"D", "A", 5}
        };
        NUM_REQUESTS = transactions.size();
        int txnIdx = 0;
        for (const auto& [from, to, amount] : transactions) {
            json transaction = {{"from", from}, {"to", to}, {"amount", amount}};
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            std::string timestamp = std::to_string(ms) + "_" + std::to_string(txnIdx++);
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
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            
        }
    }

    // === PBFT client logic: send, wait, retry ===
    int sentCount = 0;
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
            sentCount++;
            // Turn off node 5001 after the second transaction is sent
            if (scenario == 5 && sentCount == 2) {
                std::cout << "[IntegrationTest] Turning off node on port 5001 mid-transaction...\n";
                try {
                    TcpConnection nodeConn(5001, false);
                    json statusMsg = {
                        {"type", "changeServerStatus"},
                        {"server_status", 0}
                    };
                    nodeConn.send(statusMsg.dump());
                    nodeConn.closeConnection();
                } catch (...) {
                    std::cout << "[IntegrationTest] Could not connect to node 5001 to turn off.\n";
                }
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

    

    // Wait for responses for up to maxWaitSeconds, but exit early if enough responses are received
    const int maxWaitSeconds = 20;
    auto waitStart = std::chrono::steady_clock::now();
    while (true) {
        {
            std::lock_guard<std::mutex> lock(responsesMutex);
            if (responses.size() >= 7*transactions.size()) break;
        }
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - waitStart).count() > maxWaitSeconds) {
            std::cout << "[IntegrationTest] Timeout waiting for responses.\n";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    //std::this_thread::sleep_for(std::chrono::seconds(1)); // Give time for balance queries to be processed

    // Print all received responses (already printed in listener, but you can print again if needed)
    // std::cout << "\n[IntegrationTest] All received responses:\n";
    // for (const auto& resp : responses) {
    //     std::cout << resp << std::endl;
    // }

    // Expected balances (adjust as needed)
    std::map<std::string, int> expectedBalances = initialBalances;
    for (const auto& [from, to, amount] : transactions) {
        expectedBalances[from] -= amount;
        expectedBalances[to] += amount;
    }

    // std::cout << "[IntegrationTest] Expected balances:\n";
    // for (const auto& [client, expected] : expectedBalances) {
    //     std::cout << client << ": " << expected << std::endl;
    // }

    // std::cout << "[IntegrationTest] Transactions are...\n";
    // for (const auto& [from, to, amount] : transactions) {
    //     std::cout << from << " -> " << to << ": " << amount << std::endl;
    // }
    bool allBalancesCorrect = true;

    for (const auto& resp : responses) {
        try {
            auto j = json::parse(resp);
            if (j.contains("balances")) {
                // std::cout << "[Validation] Received balances response: " << j.dump() << std::endl;
                int senderId = j["message_sender_id"].get<int>();
                std::cout << "[Validation] message_sender_id: " << senderId << std::endl;
                auto balances = j["balances"];
                for (const auto& [client, expected] : expectedBalances) {
                    if (!balances.contains(client)) {
                        std::cout << "[Validation] Missing balance for client " << client << std::endl;
                        allBalancesCorrect = false;
                        continue;
                    }
                    int actual = balances[client].get<int>();
                    if (actual != expected) {
                        std::cout << "[Validation] Balance mismatch for " << client << ": expected " << expected << ", got " << actual << std::endl;
                        allBalancesCorrect = false;
                    }
                }
            }
        } catch (...) {
            // Ignore parse errors for non-balance responses
        }
    }

    if (!allBalancesCorrect) {
        std::cout << "[IntegrationTest] Balance validation failed!\n";
    } else {
        std::cout << "[IntegrationTest] All balances are correct.\n";
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