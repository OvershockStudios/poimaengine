// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/audio.hpp"

namespace poima::audio_detail {
void select_room(std::optional<AudioRoomSelection>& selected, const Matrix4& listener,
                 std::string_view entity, const Matrix4& world, const AudioRoom& room);

class RoomReverb {
public:
    static constexpr std::uint32_t transition_frames=audio_rate/20;

    explicit RoomReverb(const AudioRoom& initial, bool fade_in=false);
    void target(const AudioRoom* room);
    void process(std::span<float> stereo) noexcept;

private:
    struct Delay {
        std::vector<float> samples;
        std::size_t cursor=0;
        float previous=0;
        float low=0, high=0, low_target=0, high_target=0;
    };
    std::array<Delay,8> delays_;
    double decay_=0, high_decay_=0;
    float send_=0, send_target_=0;
};
}
