#define MINIAUDIO_IMPLEMENTATION
#include "alarm_sound.h"
#include <algorithm>
#include <chrono>

AlarmController::AlarmController(const std::string& soundPath, int initialCooldownSec)
        : m_active(false),
            m_cooldownSeconds(std::max(0, initialCooldownSec)),
            m_lastStopMs(0) {
    
    if (ma_engine_init(NULL, &m_engine) != MA_SUCCESS) {
        throw std::runtime_error("Failed to init audio engine");
    }

    // Memory Cache: MA_SOUND_FLAG_DECODE loads raw PCM into RAM
    ma_result result = ma_sound_init_from_file(&m_engine, soundPath.c_str(), 
                                               MA_SOUND_FLAG_DECODE, NULL, NULL, &m_sound);
    if (result != MA_SUCCESS) {
        ma_engine_uninit(&m_engine);
        throw std::runtime_error("Failed to load sound to RAM");
    }

    ma_sound_set_looping(&m_sound, MA_TRUE);
}

AlarmController::~AlarmController() {
    ma_sound_uninit(&m_sound);
    ma_engine_uninit(&m_engine);
}

void AlarmController::trigger() {
    if (m_active.load(std::memory_order_relaxed)) return;
    if (isInCooldown()) return;

    bool expected = false;
    if (m_active.compare_exchange_strong(expected, true)) {
        ma_sound_start(&m_sound);
    }
}

void AlarmController::stop() {
    bool expected = true;
    if (m_active.compare_exchange_strong(expected, false)) {
        m_active.store(false);
        m_lastStopMs.store(getCurrentMs());
        ma_sound_stop(&m_sound);
        ma_sound_seek_to_pcm_frame(&m_sound, 0);
    }
}

bool AlarmController::isInCooldown() const {
    return (getCurrentMs() - m_lastStopMs.load()) < (m_cooldownSeconds.load() * 1000);
}

long long AlarmController::getCurrentMs() const {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void AlarmController::setCooldown(int seconds) {
    m_cooldownSeconds.store(std::max(0, seconds));
}
bool AlarmController::isActive() const { return m_active.load(); }