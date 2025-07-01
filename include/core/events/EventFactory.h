#pragma once
#include "BaseEvent.h"
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <functional>
#include <string>
#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>
#include "../crypto/CryptoUtils.h"



class EventFactory {
public:
    static EventFactory& getInstance();

    // Register event dynamically
    template<typename T>
    // Register event dynamically (this is defined in the .cpp file)

    void registerEvent(const std::string& name) {
        factoryMap[name] = [](const nlohmann::json& params) {
            return std::make_unique<T>(params);
        };
    }

    // Create event based on name
    std::unique_ptr<BaseEvent> createEvent(const std::string& name, const nlohmann::json& params = {});

    void initialize();

private:
    std::unordered_map<std::string, std::function<std::unique_ptr<BaseEvent>(const nlohmann::json&)>> factoryMap;
    EventFactory() = default;
};
