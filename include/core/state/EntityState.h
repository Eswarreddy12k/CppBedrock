#pragma once

#include <string>

class EntityState {
public:
    EntityState() : _role(""), _state("PBFTRequest"), _view(0), _sequence(0), lockedQC("0") {} // <-- Add this line
    EntityState(const std::string& role, const std::string& state, int view, int sequence);

    // Getters
    std::string getRole() const;
    std::string getState() const;
    int getSequenceNumber() const;
    int getViewNumber() const;
    std::string getLockedQC() const { return lockedQC; } // Used for Hotstuff protocol
    

    // Setters
    void setRole(const std::string& newRole);
    void setState(const std::string& newState);
    void setSequenceNumber(int newSequenceNumber);
    void incrementSequenceNumber();
    void setViewNumber(int newViewNumber);
    int getView() const { return _view; }
    void setView(int view) { _view = view; }
    void setLockedQC(const std::string& qc) { lockedQC = qc; } // Used for Hotstuff protocol
    int getNextSequence() { return ++_sequence;}

private:
    std::string _role;
    std::string _state;
    int sequenceNumber;
    int viewNumber;
    int _view;
    int _sequence;
    std::string lockedQC; // Used for Hotstuff protocol
};