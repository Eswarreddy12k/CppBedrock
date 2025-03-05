#pragma once

#include <unordered_map>
#include <mutex>

class DataSet {
public:
    void setRecords(std::unordered_map<int, int>& records);
    std::unordered_map<int, int> getRecords();
    void update(const int& key, const int& value);

private:
    std::unordered_map<int, int> _records;
    mutable std::mutex _mtx;
};