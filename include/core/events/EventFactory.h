#pragma once
#include "Event.h"
#include <unordered_map>
#include <memory>
#include <functional>
#include <string>

class EventFactory {
public:
    static EventFactory& getInstance();

    // Register event dynamically
    template<typename T>
    void registerEvent(const std::string& name);

    // Create event based on name
    std::unique_ptr<Event> createEvent(const std::string& name);

    void initialize();

private:
    std::unordered_map<std::string, std::function<std::unique_ptr<Event>()>> factoryMap;

    EventFactory() = default;
};
