#pragma once

#include "Event.h"
#include <string>
#include <iostream>

class Message : public Event {
public:
    explicit Message(std::string content) : content_(std::move(content)) {}
    virtual ~Message() = default;

    virtual std::string getContent() const { return content_; }

    virtual bool execute(Entity*, const Message*, EntityState*) { return true; }
protected:
    std::string content_;
};