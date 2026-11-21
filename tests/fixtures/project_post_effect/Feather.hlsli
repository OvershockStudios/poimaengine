// SPDX-License-Identifier: Apache-2.0
float edge_gain(float radius_squared) {
    return lerp(1.0, 0.72, smoothstep(0.32, 1.6, radius_squared));
}
