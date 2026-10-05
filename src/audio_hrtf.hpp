// SPDX-License-Identifier: Apache-2.0
#pragma once
#if POIMA_AUDIO
#include <phonon.h>
#include <mutex>
namespace poima::audio_detail {
// Steam Audio 4.8.1 documents HRTF creation as non-thread-safe even when
// different contexts are used. All engine creation sites share this lock.
inline IPLerror create_hrtf(IPLContext context,IPLAudioSettings* audio,IPLHRTFSettings* settings,IPLHRTF* result) {
    static std::mutex creation;
    const std::lock_guard lock(creation);
    return iplHRTFCreate(context,audio,settings,result);
}
}
#endif
