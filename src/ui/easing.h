#pragma once
#include <algorithm>
#include <cmath>

// Pure math easing functions. t in [0,1], returns [0,1] (except overshoot).
// No dependencies. Usage: float v = ease(t, EaseOutCubic);

using EaseFunc = float(*)(float);

inline float easeLinear(float t) { return t; }

inline float easeInQuad(float t) { return t * t; }
inline float easeOutQuad(float t) { return t * (2.0f - t); }
inline float easeInOutQuad(float t) {
    return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
}

inline float easeInCubic(float t) { return t * t * t; }
inline float easeOutCubic(float t) {
    float f = t - 1.0f;
    return f * f * f + 1.0f;
}
inline float easeInOutCubic(float t) {
    return t < 0.5f ? 4.0f * t * t * t
                     : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;
}

inline float easeOutExpo(float t) {
    return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
}

// Overshoot: goes slightly past 1.0 then settles back. Good for "pop" effects.
inline float easeOutBack(float t) {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
}

// Elastic: spring-like oscillation. Returns ~1.0 at t=1.
inline float easeOutElastic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * (2.0f * 3.14159f / 3.0f)) + 1.0f;
}

// Utility: lerp with easing
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// SmoothStep: classic Hermite interpolation (ease-in-out like)
inline float smoothstep(float edge0, float edge1, float x) {
    float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
