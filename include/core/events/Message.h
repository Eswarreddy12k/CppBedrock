#pragma once

#include "Event.h"
#include <string>
#include <iostream>

class Message : public Event {
public:
    explicit Message(const std::string& content) : _content(content) {}
    std::string getContent() const { return _content; }
    void execute(EntityState& state) {
        // For now, this can be an empty implementation or a simple action
        std::cout << "[Message] Executing action for content: " << _content << "\n";
    }
private:
    std::string _content;
};