#pragma once

#include <string>

class IConnection {
public:
    virtual ~IConnection() = default;

    virtual void send(const std::string& message) = 0;
    virtual std::string receive() = 0;
};
