#pragma once

#include "Rect.h"
#include "KalmanFilter.h"

#include <cstddef>
#include <vector>
#include <utility>

namespace byte_track
{
enum class STrackState {
    New = 0,
    Tracked = 1,
    Lost = 2,
    Removed = 3,
};

class STrack
{
public:
    STrack(const Rect<float>& rect, const float& score, int idx);
    ~STrack();

    const Rect<float>& getRect() const;
    const STrackState& getSTrackState() const;

    const bool& isActivated() const;
    const float& getScore() const;
    const size_t& getTrackId() const;
    const size_t& getFrameId() const;
    const size_t& getStartFrameId() const;
    const size_t& getTrackletLength() const;
    const uint64_t getFrameDuration() const;

    void activate(const size_t frame_id, const uint64_t frame_ms, const size_t track_id);
    void reActivate(const STrack &new_track, const size_t frame_id, const uint64_t frame_ms, const int new_track_id = -1);

    void predict();
    void update(const STrack &new_track, const size_t frame_id, const uint64_t frame_ms);

    void markAsLost();
    void markAsRemoved();

    [[nodiscard]] std::pair<size_t, int> result() const;

private:
    KalmanFilter kalman_filter_;
    KalmanFilter::StateMean mean_;
    KalmanFilter::StateCov covariance_;

    Rect<float> rect_;
    STrackState state_;

    bool is_activated_;
    float score_;
    size_t track_id_;
    size_t frame_id_;
    size_t start_frame_id_;
    size_t tracklet_len_;
    int idx_;
    uint64_t frame_ms_;
    uint64_t start_frame_ms_;

    void updateRect();
};
}