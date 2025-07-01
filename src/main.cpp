#include "../include/coordination/CoordinationServer.h"
#include "../include/coordination/CoordinationUnit.h"
#include <iostream>
#include <thread>

int main() {
    CoordinationServer server;
    CoordinationUnit unit;

    //change path in entity.cpp to change protocol
    server.loadConfig("config.pbft.yaml");
    server.sendStartSignal();

    // Start CoordinationUnit in a separate thread
    std::thread unitThread([&unit]() {
        unit.start();
    });

    // Main thread continues to run
    std::cout << "Main thread continues execution while server is listening..." << std::endl;
    
    // Simulate some other task
    for (int i = 0; i < 10; i++) {
        //std::cout << "Main is doing work..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    server.sendStopSignal();
    unit.stop();

    // Ensure the unit thread finishes before exiting
    if (unitThread.joinable()) {
        unitThread.join();
    }

    return 0;
}
