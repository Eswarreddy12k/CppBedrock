#pragma once

class Coordinator {
public:
    virtual ~Coordinator() = default;  // Ensure it's public

    virtual void initialize() = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
};
