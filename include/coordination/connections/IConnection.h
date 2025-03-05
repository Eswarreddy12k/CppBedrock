#pragma once

#include <string>

using namespace std;

class IConnection {
public:
    virtual ~IConnection() = default;

    virtual void send(const string& message) = 0;

    virtual string 
}