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
    void process(std::span<float> stereo,bool wet_only=false) noexcept;

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

// One diffuse return remains alive while its door is sealed. The route gates
// the listener's output after delay, independently of the room excitation.
class RoomReturnMixer {
public:
    explicit RoomReturnMixer(const AudioRoomReturn& initial);
    bool configure(const AudioRoomReturn& target);
    void mix(std::span<const float> excitation,std::span<float> destination);

private:
    using Stereo=std::array<float,2>;
    using Bands=std::array<std::array<float,3>,2>;
    RoomReverb reverb_;
    std::vector<Stereo> history_;
    std::size_t cursor_=0;
    Stereo low_{},mid_{};
    Bands gains_{},target_{};
    std::uint32_t fade_remaining_=0;
    double delay_=0,target_delay_=0;
    bool has_route_=false,seen_route_=false;
};
}
