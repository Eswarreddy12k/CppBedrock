#include "../../../include/core/events/EventFactory.h"
#include "../../../include/core/events/Event.h"
#include <iostream>

// Example derived event class for incrementing sequence
class IncrementSequenceEvent : public Event {
public:
    void execute(EntityState& state) override {  // Removed const
        // Example execution logic: Increment sequence number
        state.incrementSequenceNumber();
        std::cout << "Sequence incremented" << std::endl;
    }
};

// Add more event classes as needed
class AddLogEvent : public Event {
public:
    void execute(EntityState& state) override {  // Removed const
        std::cout << "Log entry added" << std::endl;
    }
};

class UpdateLogEvent : public Event {
public:
    void execute(EntityState& state) override {  // Removed const
        std::cout << "Log entry updated" << std::endl;
    }
};

class SendToClientEvent : public Event {
public:
    void execute(EntityState& state) override {  // Removed const
        std::cout << "Sent to client" << std::endl;
    }
};

// Singleton instance of the EventFactory
EventFactory& EventFactory::getInstance() {
    static EventFactory instance;
    return instance;
}

// Register event dynamically (this is defined in the .cpp file)
template<typename T>
void EventFactory::registerEvent(const std::string& name) {
    factoryMap[name] = []() { return std::make_unique<T>(); };
}

// Create event based on name
std::unique_ptr<Event> EventFactory::createEvent(const std::string& name) {
    auto it = factoryMap.find(name);
    if (it != factoryMap.end()) {
        return it->second();
    }
    return nullptr;
}

// The initialize method should be a member of EventFactory
void EventFactory::initialize() {
    // Register all event types with proper scope resolution
    this->registerEvent<IncrementSequenceEvent>("incrementSequence");
    this->registerEvent<AddLogEvent>("addLog");
    this->registerEvent<UpdateLogEvent>("updateLog");
    this->registerEvent<SendToClientEvent>("sendtoClient");
}

// Explicit template instantiations to avoid linker errors (for all events you register)
template void EventFactory::registerEvent<IncrementSequenceEvent>(const std::string&);
template void EventFactory::registerEvent<AddLogEvent>(const std::string&);
template void EventFactory::registerEvent<UpdateLogEvent>(const std::string&);
template void EventFactory::registerEvent<SendToClientEvent>(const std::string&);