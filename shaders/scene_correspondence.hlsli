// SPDX-License-Identifier: Apache-2.0

static const uint object_motion_valid = 1u;
static const uint object_surface_marks_changed = 2u;
static const uint deferred_motion_valid = 4u;
static const uint deferred_surface_marks_changed = 32u;

// Preserve the existing validity threshold while giving FSR a separate
// material-change signal in the same R8_UNORM attachment.
static const float reactive_correspondence = 128.0/255.0;

float encode_correspondence(bool valid,bool marks_changed) {
    return valid ? (marks_changed ? reactive_correspondence : 1.0) : 0.0;
}

bool correspondence_is_valid(float value) {
    return value>.5;
}

bool correspondence_is_reactive(float value) {
    return correspondence_is_valid(value) && value<.75;
}
