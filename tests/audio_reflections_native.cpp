// SPDX-License-Identifier: Apache-2.0
#include "poima/audio.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace poima;

namespace {
constexpr std::uint32_t frames=6*audio_tick_frames;
void check(bool value,const char* reason) {
    if(!value)throw std::runtime_error(reason);
}

Matrix4 pose(double x,double y,double z,double sx=1,double sy=1,double sz=1) {
    auto world=identity_matrix();
    world[0]=sx;world[5]=sy;world[10]=sz;
    world[12]=x;world[13]=y;world[14]=z;
    return world;
}

AudioSnapshot fixture() {
    AudioSnapshot snapshot;
    auto clip=std::make_shared<AudioClip>();
    clip->samples={.5f};
    AudioEmitter emitter{"pulse",clip};
    snapshot.sources.push_back({"source",std::move(emitter),pose(0,0,-2)});
    AcousticGeometry wall;
    wall.entity="wall";wall.world=pose(2,0,0,1,8,8);wall.material.absorption={.25f,.5f,.75f};
    wall.material.scattering=.1f;
    snapshot.geometry.push_back(std::move(wall));
    return snapshot;
}

double wet_energy(const std::vector<float>& enabled,const std::vector<float>& disabled,std::size_t channel,
                  std::size_t begin=0,std::size_t end=frames) {
    double sum=0;
    for(std::size_t i=begin;i<end;++i) {
        const double diff=static_cast<double>(enabled[2*i+channel])-disabled[2*i+channel];
        sum+=diff*diff;
    }
    return sum;
}

std::vector<float> stream(const AudioSnapshot& snapshot) {
    SoundState logical;
    logical.play("source",snapshot.sources.front().emitter,0,1);
    AudioStream owner(0,snapshot);
    std::vector<float> result;
    for(std::uint64_t tick=1;tick<=6;++tick) {
        const auto pcm=owner.advance(tick,snapshot,logical.voices(),tick==6);
        result.insert(result.end(),pcm.begin(),pcm.end());
    }
    check(owner.stats().frames==frames && owner.stats().voices_started==1,"The reflection altered logical voice or stream timing.");
    return result;
}

void test_handoffs() {
    auto right_side=fixture();
    auto clip=std::make_shared<AudioClip>();
    // An asymmetric repeating signal makes a restarted or incorrectly
    // delayed clip distinguishable from continuous playback.
    for(int i=0;i<911;++i)clip->samples.push_back(.16f*std::sin(.13f*i)+.07f*std::cos(.042f*i));
    auto& emitter=right_side.sources.front().emitter;
    emitter.clip=clip;
    emitter.loop=true;
    right_side.early_reflections=true;
    auto left_side=right_side;
    left_side.geometry.front().entity="far_wall";
    left_side.geometry.front().world=pose(-4,0,0,1,8,8);
    auto no_bounce=right_side;
    no_bounce.geometry.front().material.enabled=false;
    const auto right=observe_audio(right_side).paths.at(0).reflection;
    const auto left=observe_audio(left_side).paths.at(0).reflection;
    check(right && left && left->propagation_delay_samples>right->propagation_delay_samples,
          "The handoff fixture needs two arrivals with unequal delays.");
    SoundState logical;
    logical.play("source",emitter,0,1);
    AudioStream moving(0,no_bounce),dry(0,no_bounce),fixed_right(0,right_side),fixed_left(0,left_side);
    std::array<std::vector<float>,4> pcm;
    const auto append=[&](std::size_t index,AudioStream& owner,const AudioSnapshot& snapshot,std::uint64_t tick) {
        const auto block=owner.advance(tick,snapshot,logical.voices(),tick==12);
        pcm[index].insert(pcm[index].end(),block.begin(),block.end());
    };
    for(std::uint64_t tick=1;tick<=12;++tick) {
        const auto& geometry=tick<=2 ? no_bounce : (tick<8 ? right_side : left_side);
        append(0,moving,geometry,tick);
        append(1,dry,no_bounce,tick);
        append(2,fixed_right,right_side,tick);
        append(3,fixed_left,left_side,tick);
    }
    check(moving.stats().reflection_handoffs==2 && moving.stats().voices_started==1 &&
          moving.stats().frames==12*audio_tick_frames,"An acquired or replaced reflection restarted or lost the voice.");
    check(std::equal(pcm[0].begin(),pcm[0].begin()+2*(3*audio_block),pcm[1].begin()),
          "A reflection reached a block before the new geometry was sampled.");
    const auto matches=[&](std::size_t reference,std::size_t begin,std::size_t end) {
        for(std::size_t i=2*begin;i<2*end;++i)
            if(std::abs(pcm[0][i]-pcm[reference][i])>2e-6f)return false;
        return true;
    };
    check(matches(2,3200,4500) && matches(3,7400,9600),
          "The audible branch disagrees with the independent steady, phase-aligned path.");
    check(wet_energy(pcm[0],pcm[1],1,3200,4500)>wet_energy(pcm[0],pcm[1],0,3200,4500) &&
          wet_energy(pcm[0],pcm[1],0,7400,9600)>wet_energy(pcm[0],pcm[1],1,7400,9600),
          "The replacement reflection did not move the wet signal toward the new reflecting surface.");
    // Separately check that both branches contribute within the handoff block.
    const std::size_t handoff=(7*audio_tick_frames/audio_block)*audio_block;
    check(wet_energy(pcm[0],pcm[1],0,handoff,handoff+audio_block)>1e-6 &&
          !matches(2,handoff,handoff+audio_block) && !matches(3,handoff,handoff+audio_block),
          "The handoff block did not blend the two arrivals.");
}
}

int main() {
    try {
        if(!audio_available()) {
            std::cout<<"Steam Audio unavailable. Early reflection DSP requires an audio-enabled build.\n";
            return 77;
        }
        const auto dry_snapshot=fixture();
        auto snapshot=dry_snapshot;
        snapshot.early_reflections=true;
        const auto dry=observe_audio(dry_snapshot,frames);
        const auto wet=observe_audio(snapshot,frames);
        const auto& direct=wet.paths.at(0);
        check(!dry.paths.at(0).reflection && direct.reflection.has_value() && !wet.reflection_budget_exceeded,
              "The opt-in did not produce exactly one supported reflection.");
        const auto& reflection=*direct.reflection;
        check(reflection.entity=="wall" && reflection.face==4 && std::abs(reflection.point[0]-1.5)<1e-10 &&
              std::abs(reflection.point[1])<1e-10 && std::abs(reflection.point[2]+1)<1e-10,
              "The image source did not hit the finite reflecting face.");
        const auto expected_delay=static_cast<std::uint32_t>(std::llround(std::sqrt(13.)*audio_rate/343));
        check(reflection.propagation_delay_samples==expected_delay && expected_delay>direct.propagation_delay_samples &&
              std::abs(reflection.distance-std::sqrt(13.))<1e-10 &&
              std::abs(reflection.distance_gain-1/std::sqrt(13.))<1e-7 &&
              std::abs(reflection.bands[2]-std::sqrt(.25*.9))<1e-7 && reflection.direction[0]>.8,
              "Reflection delay, band absorption, distance or direction disagrees with the analytic path.");
        check(wet.samples.size()==2*frames && std::equal(dry.samples.begin(),dry.samples.begin()+2*expected_delay,wet.samples.begin()),
              "Wet sound arrived earlier than the reflected path or changed the direct signal.");
        const double right=wet_energy(wet.samples,dry.samples,1);
        const double left=wet_energy(wet.samples,dry.samples,0);
        check(right>1e-6 && right>left,"The reflected impulse did not favor the near ear.");
        check(observe_audio(snapshot,frames).samples==wet.samples,"The frozen two-path mix is not repeatable.");

        auto blocked=snapshot;
        AcousticGeometry absorber;
        absorber.entity="absorber";absorber.world=pose(.75,0,-.5,.2,.4,.4);absorber.material.scattering=1;
        blocked.geometry.push_back(absorber);
        const auto blocked_report=observe_audio(blocked,frames);
        check(!blocked_report.paths.at(0).reflection && blocked_report.samples==dry.samples,
              "A blocker on the listener-side leg leaked through or changed the direct path.");
        blocked.geometry.back().world[14]=-1.5;
        check(!observe_audio(blocked).paths.at(0).reflection,
              "A blocker on the source-side leg leaked through.");
        blocked.geometry.back().material.enabled=false;
        check(observe_audio(blocked).paths.at(0).reflection.has_value(),"Disabled acoustic geometry still blocks a reflected leg.");
        auto clipped=snapshot;
        clipped.geometry.front().world=pose(2,0,0,1,8,1);
        check(!observe_audio(clipped).paths.at(0).reflection,"An image source escaped the finite face.");

        auto reversed=snapshot;
        reversed.geometry.front().world=pose(-2,0,0,1,8,8);
        const auto opposite=observe_audio(reversed,frames);
        check(opposite.paths.at(0).reflection && opposite.paths.at(0).reflection->direction[0]<-.8 &&
              wet_energy(opposite.samples,dry.samples,0)>wet_energy(opposite.samples,dry.samples,1),
              "The opposite reflection did not move the stereo image.");

        auto long_path=snapshot;
        long_path.geometry.front().world=pose(10,0,0,1,8,8);
        const auto long_report=observe_audio(long_path);
        check(long_report.paths.at(0).reflection.has_value(),"The long-delay wall was not selected.");
        const auto long_delay=long_report.paths.at(0).reflection->propagation_delay_samples;
        check(long_delay>4*audio_block,"The delayed-voice fixture does not outlast its direct impulse.");
        const auto direct_stream=stream(dry_snapshot),reflection_stream=stream(long_path);
        check(direct_stream.size()==2*frames && reflection_stream.size()==2*frames &&
              wet_energy(reflection_stream,direct_stream,0,0,long_delay)==0 &&
              wet_energy(reflection_stream,direct_stream,1,long_delay)>1e-7 &&
              stream(long_path)==reflection_stream,"The stream lost or repeated its delayed reflection tail.");

        auto off=snapshot;
        off.geometry.front().material.enabled=false;
        // Remove the reflecting wall during one continuing loop. Set
        // the start and end windows well outside the one-block wet fade.
        auto looping=snapshot.sources.front().emitter;
        const auto clip=std::make_shared<AudioClip>();
        clip->samples={.25f,-.125f,.1f,-.3f};
        looping.clip=clip;looping.loop=true;
        snapshot.sources.front().emitter=looping;off.sources.front().emitter=looping;
        SoundState logical;
        logical.play("source",looping,0,1);
        AudioStream switched(0,snapshot),direct_owner(0,off);
        std::vector<float> switched_pcm,direct_pcm;
        for(std::uint64_t tick=1;tick<=8;++tick) {
            const auto mixed=switched.advance(tick,tick>=4 && tick<=6 ? off : snapshot,logical.voices(),tick==8);
            const auto direct_only=direct_owner.advance(tick,off,logical.voices(),tick==8);
            switched_pcm.insert(switched_pcm.end(),mixed.begin(),mixed.end());
            direct_pcm.insert(direct_pcm.end(),direct_only.begin(),direct_only.end());
        }
        check(wet_energy(switched_pcm,direct_pcm,1,800,1600)>1e-6 &&
              wet_energy(switched_pcm,direct_pcm,1,3400,4500)==0 &&
              wet_energy(switched_pcm,direct_pcm,1,5700,6400)>1e-6,
              "The timed voice did not fade and resume its selected reflection during a geometry update.");
        test_handoffs();
        std::cout<<"First-order audio reflection checks passed. Analytic delay: "<<expected_delay
                 <<" samples. Stream delayed tail: "<<long_delay<<" samples.\n";
        return 0;
    }catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
