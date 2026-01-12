#ifndef ALARM_CONTROLLER_H
#define ALARM_CONTROLLER_H

#include <atomic>
#include <string>
#include "miniaudio.h"

class AlarmController {
public:
    AlarmController(const std::string& soundPath, int initialCooldownSec = 10);
    ~AlarmController();

    // Triggered by detector threads
    void trigger();
    
    // Triggered by HTTP callback
    void stop();
    
    void setCooldown(int seconds);
    bool isActive() const;

private:
    bool isInCooldown() const;
    long long getCurrentMs() const;

    ma_engine m_engine;
    ma_sound  m_sound;

    std::atomic<bool>      m_active;
    std::atomic<int>       m_cooldownSeconds;
    std::atomic<long long> m_lastStopMs;
};

#endif