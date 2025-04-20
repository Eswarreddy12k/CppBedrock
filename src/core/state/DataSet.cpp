#include "../../../include/core/state/DataSet.h"
#include <iostream>

void DataSet::setRecords(const std::unordered_map<int, int>& records) {
    std::lock_guard<std::mutex> lock(_mtx);  // Ensure thread-safety
    _records = records;  // Set all records at once
}

std::unordered_map<int, int> DataSet::getRecords() const {
    std::lock_guard<std::mutex> lock(_mtx);  // Ensure thread-safety
    return _records;  // Return a copy of the records
}

void DataSet::update(const int& key, const int& value) {
    std::lock_guard<std::mutex> lock(_mtx);  // Ensure thread-safety
    _records[key] = value;  // Update the record with the given key
    std::cout << "Updated record: " << key << " = " << value << std::endl;
}
