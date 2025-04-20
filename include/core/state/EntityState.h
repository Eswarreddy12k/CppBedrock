#pragma once

#include <string>

class EntityState {
public:
    // Constructor
    EntityState(const std::string& role, const std::string& state, int view, int sequence);

    // Getters
    std::string getRole() const;
    std::string getState() const;
    int getSequenceNumber() const;
    int getViewNumber() const;

    // Setters
    void setRole(const std::string& newRole);
    void setState(const std::string& newState);
    void setSequenceNumber(int newSequenceNumber);
    void incrementSequenceNumber();
    void setViewNumber(int newViewNumber);
    int getView() const { return _view; }
    void setView(int view) { _view = view; }
    int getNextSequence() { return ++_sequence;}

private:
    std::string _role;
    std::string _state;
    int sequenceNumber;
    int viewNumber;
    int _view;
    int _sequence;
};