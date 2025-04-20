#pragma once
#include <thread>
#include <atomic>
#include <functional>
#include <chrono>
#include <condition_variable>
#include <mutex>

class TimeKeeper {
public:
    using Callback = std::function<void()>;

    explicit TimeKeeper(int timeoutMs, Callback cb);
    ~TimeKeeper();

    TimeKeeper(const TimeKeeper&) = delete;
    TimeKeeper& operator=(const TimeKeeper&) = delete;

    void start();
    void reset();
    void stop();

private:
    void run();

    const int timeoutMs;
    const Callback callback;
    std::thread timerThread;
    std::atomic<bool> running{false};
    std::atomic<bool> stopped{false};
    mutable std::mutex mtx;
    std::condition_variable cv;
    bool resetFlag{false};
};