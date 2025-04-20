#pragma once

#include <unordered_map>
#include <mutex>

class DataSet {
public:
    // Constructor
    DataSet() = default;

    // Delete the copy constructor and copy assignment operator
    DataSet(const DataSet&) = delete;
    DataSet& operator=(const DataSet&) = delete;

    // Set records
    void setRecords(const std::unordered_map<int, int>& records);

    // Get records
    std::unordered_map<int, int> getRecords() const;

    // Update a record
    void update(const int& key, const int& value);

private:
    std::unordered_map<int, int> _records;  // The actual records
    mutable std::mutex _mtx;  // Mutex for thread-safety
};
