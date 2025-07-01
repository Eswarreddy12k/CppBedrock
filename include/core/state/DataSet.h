#pragma once

#include <unordered_map>
#include <mutex>
#include <string>
#include <nlohmann/json.hpp>

class DataSet {
public:
    DataSet() = default;
    DataSet(const DataSet&) = delete;
    DataSet& operator=(const DataSet&) = delete;

    void setRecords(const std::unordered_map<std::string, nlohmann::json>& records);
    std::unordered_map<std::string, nlohmann::json> getRecords() const;
    void update(const std::string& key, const nlohmann::json& value);
    nlohmann::json get(const std::string& key) const;

    void saveToFile(const std::string& filename = "dataset.json") const;
    void loadFromFile(const std::string& filename = "dataset.json");
    bool exists(const std::string& key) const;

private:
    std::unordered_map<std::string, nlohmann::json> _records;
    mutable std::mutex _mtx;
};