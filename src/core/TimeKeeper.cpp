#include "../../include/core/TimeKeeper.h"

TimeKeeper::TimeKeeper(int timeoutMs, Callback cb)
    : timeoutMs(timeoutMs), callback(cb) {}

TimeKeeper::~TimeKeeper() {
    stop();
}

void TimeKeeper::start() {
    std::lock_guard<std::mutex> lock(mtx);
    if (running) return;
    running = true;
    timerThread = std::thread(&TimeKeeper::run, this);
}

void TimeKeeper::reset() {
    std::lock_guard<std::mutex> lock(mtx);
    if (!running) return;
    resetFlag = true;
    cv.notify_one();
}

void TimeKeeper::stop() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (!running) return;
        running = false;
        resetFlag = true;
        cv.notify_one();
    }
    if (timerThread.joinable()) {
        timerThread.join();
    }
}

void TimeKeeper::run() {
    Callback cbCopy = callback; // Copy the callback to avoid use-after-free
    std::unique_lock<std::mutex> lock(mtx);
    while (running) {
        resetFlag = false;
        auto status = cv.wait_for(lock, 
            std::chrono::milliseconds(timeoutMs),
            [this] { return resetFlag || !running; });
        if (!running) break;
        if (status) continue; // Timer was reset
        // Timeout occurred - release lock while calling callback
        if (running && cbCopy) {
            lock.unlock();
            cbCopy();
            lock.lock();
        }
    }
}