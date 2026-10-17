/*
 * Raymarch Tech Demo - real-time CPU raymarching demo for native PS5
 * homebrew, built from source with the open ps5-payload-sdk.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * VideoOut setup and the bitmap font follow ps5-native-app-boilerplate
 * (GPL-3.0-or-later). Everything is rendered on the CPU by worker threads:
 * a reflective checkerboard floor, smoothly merging metal blobs, soft
 * shadows, ambient occlusion, fog and a sun-lit sky.
 *
 * The scene is traced at a dynamic internal resolution that keeps the
 * render time inside a 60 Hz budget, optionally with 4x rotated-grid
 * supersampling, and bilinearly upscaled to 1920x1080.
 *
 * DualSense: right stick orbits the camera, left stick zooms, L1/R1 pick a
 * fixed internal resolution, Triangle toggles dynamic resolution, Square
 * toggles 4x antialiasing, Cross switches the colour mood, Circle switches
 * between the blob scene and the neon tunnel (also every 30 seconds while
 * the pad is idle). In the tunnel the left stick (or, optionally, the
 * DualSense motion sensor) steers the spaceship and R2 fires lasers at the
 * comets, with optional rumble feedback.
 *
 * An original cracktro-style chiptune is synthesized on a thread and played
 * through AudioOut; the kick drives the tunnel's light pulse.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C"
{
    std::size_t sceKernelGetDirectMemorySize();
    int sceKernelAllocateDirectMemory(std::int64_t search_start, std::int64_t search_end,
                                      std::size_t length, std::size_t alignment, int memory_type,
                                      std::int64_t *physical_address);
    int sceKernelMapDirectMemory(void **address, std::size_t length, int protection, int flags,
                                 std::int64_t physical_address, std::size_t alignment);
    int sceKernelUsleep(std::uint32_t microseconds);
    std::uint64_t sceKernelGetProcessTime();
    int sceSystemServiceHideSplashScreen();
    int sceVideoOutOpen(std::int32_t user_id, std::int32_t bus_type, std::int32_t index,
                        const void *param);
    int sceVideoOutSetFlipRate(std::int32_t handle, std::int32_t rate);
    int sceVideoOutSubmitFlip(std::int32_t handle, std::int32_t buffer_index,
                              std::uint32_t flip_mode, std::int64_t flip_argument);
    int sceVideoOutWaitVblank(std::int32_t handle);
    int sceVideoOutClose(std::int32_t handle);
    int scePthreadCreate(void **thread, const void *attr, void *(*entry)(void *), void *arg,
                         const char *name);
    int scePthreadYield();
    int scePthreadJoin(void *thread, void **result);
    int sceUserServiceInitialize(const void *params);
    int sceUserServiceGetInitialUser(std::int32_t *user_id);
    int scePadInit();
    int scePadOpen(std::int32_t user_id, std::int32_t type, std::int32_t index,
                   const void *param);
    int scePadReadState(std::int32_t handle, void *data);
    int scePadSetMotionSensorState(std::int32_t handle, bool enable);
    int scePadSetVibration(std::int32_t handle, const void *param);
    long write(int descriptor, const void *buffer, std::size_t size);
    int sysctlbyname(const char *name, void *old_value, std::size_t *old_length,
                     const void *new_value, std::size_t new_length);
    int sceKernelGetSystemSwVersion(void *version);
    int sceAudioOutInit();
    int sceAudioOutOpen(std::int32_t user_id, std::int32_t type, std::int32_t index,
                        std::uint32_t length, std::uint32_t frequency, std::uint32_t format);
    int sceAudioOutOutput(std::int32_t handle, const void *samples);
}

namespace
{
constexpr unsigned kWidth = 1920;
constexpr unsigned kHeight = 1080;
constexpr std::size_t kFrameBytes = 0x1000000;
constexpr std::size_t kMemoryBytes = kFrameBytes * 2;
constexpr std::size_t kMemoryAlignment = 0x200000;
constexpr int kMemoryTypeWcGarlic = 3;
constexpr int kMapProtection = 0x33;
constexpr std::uint64_t kPixelFormatRgba8Srgb = UINT64_C(0x8000000022000000);
constexpr std::uint32_t kTilingLinear = 1;
constexpr int kWorkers = 9;
// Dynamic resolution keeps the CPU render time below this budget.
constexpr float kBudgetMs = 13.0f;
constexpr int kMinWidth = 160;
constexpr int kMaxWidth = 1920;
// Fixed internal widths for L1/R1 (height follows 16:9).
constexpr int kStepWidths[] = {240, 320, 384, 480, 640, 960};
constexpr int kStepCount = sizeof(kStepWidths) / sizeof(kStepWidths[0]);

struct VideoBuffer
{
    void *data;
    void *metadata;
    void *reserved0;
    void *reserved1;
};

struct VideoAttribute
{
    std::uint8_t reserved[80];
};

extern "C" void sceVideoOutSetBufferAttribute2(VideoAttribute *attribute,
                                               std::uint64_t pixel_format,
                                               std::uint32_t tiling_mode, std::uint32_t width,
                                               std::uint32_t height, std::uint64_t option,
                                               std::uint32_t dcc_control,
                                               std::uint64_t dcc_clear_color);
extern "C" int sceVideoOutRegisterBuffers2(std::int32_t handle, std::int32_t set_index,
                                           std::int32_t buffer_index_start, VideoBuffer *buffers,
                                           std::int32_t buffer_count, VideoAttribute *attribute,
                                           std::int32_t category, void *option);

// ---------------------------------------------------------------- math

inline float Sqrt(float x) { return __builtin_sqrtf(x > 0.0f ? x : 0.0f); }
inline float Abs(float x) { return __builtin_fabsf(x); }
inline float Min(float a, float b) { return a < b ? a : b; }
inline float Max(float a, float b) { return a > b ? a : b; }
inline float Clamp(float x, float lo, float hi) { return Min(Max(x, lo), hi); }
inline float Mix(float a, float b, float t) { return a + (b - a) * t; }
inline float Floor(float x) { return __builtin_floorf(x); }

// Sine for animation values only (a few calls per frame).
float Sin(float x)
{
    constexpr float kTwoPi = 6.28318531f;
    x -= kTwoPi * Floor(x / kTwoPi + 0.5f);
    const float x2 = x * x;
    return x * (1.0f + x2 * (-1.0f / 6 + x2 * (1.0f / 120 + x2 * (-1.0f / 5040 + x2 / 362880))));
}
float Cos(float x) { return Sin(x + 1.57079633f); }

struct V3
{
    float x, y, z;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Length(V3 a) { return Sqrt(Dot(a, a)); }
inline V3 Normalize(V3 a) { return a * (1.0f / Max(Length(a), 1e-6f)); }
inline V3 Cross(V3 a, V3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// ---------------------------------------------------------------- scene

struct Mood
{
    const char *name;
    V3 sky_top, sky_horizon, sun_color, metal, floor_a, floor_b, fog;
};

constexpr Mood kMoods[] = {
    {"CHROME", {0.18f, 0.32f, 0.62f}, {0.75f, 0.82f, 0.92f}, {1.2f, 1.05f, 0.85f},
     {0.80f, 0.86f, 0.95f}, {0.85f, 0.85f, 0.88f}, {0.12f, 0.13f, 0.16f},
     {0.70f, 0.78f, 0.90f}},
    {"GOLD SUNSET", {0.20f, 0.10f, 0.30f}, {1.00f, 0.55f, 0.30f}, {1.4f, 0.80f, 0.45f},
     {1.00f, 0.72f, 0.30f}, {0.55f, 0.35f, 0.25f}, {0.08f, 0.05f, 0.06f},
     {0.95f, 0.55f, 0.35f}},
    {"NEON NIGHT", {0.01f, 0.01f, 0.04f}, {0.10f, 0.02f, 0.20f}, {0.6f, 0.9f, 1.4f},
     {0.95f, 0.20f, 0.85f}, {0.05f, 0.70f, 0.80f}, {0.01f, 0.01f, 0.03f},
     {0.08f, 0.02f, 0.15f}},
};
constexpr int kMoodCount = sizeof(kMoods) / sizeof(kMoods[0]);

struct FrameState
{
    std::uint32_t *pixels;    // 1920x1080 display buffer
    std::uint32_t *internal;  // width x height render target
    bool antialias;
    int scene;   // 0 = blobs, 1 = neon tunnel
    float time;  // seconds since start
    float beat;  // kick envelope 0..1 from the music thread
    V3 ship;     // spaceship centre in tunnel space
    V3 comets[4][6];  // per comet: head, then five tail points
    V3 bolts[8];      // laser bolts (z < 0 means unused)
    V3 blast;         // last comet explosion
    float blast_strength;
    int width;
    int height;
    V3 eye, forward, right, up;
    V3 blobs[5];
    V3 sun;
    const Mood *mood;
    int panel_count;      // dimmed overlay panels, applied while upscaling
    int panels[4][4];     // x0, y0, x1, y1 in display pixels
};

FrameState g_state;
// Render target in cached memory; the display buffer is write-combined.
std::uint32_t g_internal[kMaxWidth * kHeight];

// ---- 4-wide SIMD types: four neighbouring pixels are traced together.

typedef float F4 __attribute__((ext_vector_type(4)));
typedef int I4 __attribute__((ext_vector_type(4)));

inline F4 S4(float v) { return F4{v, v, v, v}; }
inline F4 Sqrt4(F4 x) { return __builtin_elementwise_sqrt(__builtin_elementwise_max(x, S4(0.0f))); }
inline F4 Min4(F4 a, F4 b) { return __builtin_elementwise_min(a, b); }
inline F4 Max4(F4 a, F4 b) { return __builtin_elementwise_max(a, b); }
inline F4 Clamp4(F4 x, float lo, float hi) { return Min4(Max4(x, S4(lo)), S4(hi)); }
inline F4 Sel4(I4 mask, F4 a, F4 b)
{
    const I4 ia = __builtin_bit_cast(I4, a);
    const I4 ib = __builtin_bit_cast(I4, b);
    return __builtin_bit_cast(F4, (ia & mask) | (ib & ~mask));
}
inline bool Any4(I4 mask) { return (mask.x | mask.y | mask.z | mask.w) != 0; }

struct P3
{
    F4 x, y, z;
};
inline P3 Splat(V3 v) { return {S4(v.x), S4(v.y), S4(v.z)}; }
inline P3 operator+(P3 a, P3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline P3 operator-(P3 a, P3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline P3 operator*(P3 a, F4 s) { return {a.x * s, a.y * s, a.z * s}; }
inline P3 operator*(P3 a, P3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline F4 Dot4(P3 a, P3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline F4 Length4(P3 a) { return Sqrt4(Dot4(a, a)); }
inline P3 Normalize4(P3 a) { return a * (S4(1.0f) / Max4(Length4(a), S4(1e-6f))); }
inline P3 Mix4(P3 a, P3 b, F4 t) { return a + (b - a) * t; }
inline P3 Sel3(I4 mask, P3 a, P3 b)
{
    return {Sel4(mask, a.x, b.x), Sel4(mask, a.y, b.y), Sel4(mask, a.z, b.z)};
}
inline P3 Reflect4(P3 d, P3 n) { return d - n * (S4(2.0f) * Dot4(d, n)); }

inline F4 SmoothMin4(F4 a, F4 b, float k)
{
    const F4 h = Clamp4(S4(0.5f) + S4(0.5f / k) * (b - a), 0.0f, 1.0f);
    return b + (a - b) * h - S4(k) * h * (S4(1.0f) - h);
}

// Distance to the scene; *metal is set for lanes where the blobs are closer
// than the floor.
inline F4 Map4(P3 p, I4 *metal)
{
    const FrameState &s = g_state;
    F4 blob = Length4(p - Splat(s.blobs[0])) - S4(0.9f);
    for (int i = 1; i < 5; ++i)
        blob = SmoothMin4(blob, Length4(p - Splat(s.blobs[i])) - S4(0.55f), 0.65f);
    *metal = blob < p.y;
    return Min4(blob, p.y);
}

inline F4 MapDistance4(P3 p)
{
    I4 metal;
    return Map4(p, &metal);
}

P3 Normal4(P3 p)
{
    constexpr float e = 0.0015f;
    const P3 a = Splat({e, -e, -e}), b = Splat({-e, -e, e});
    const P3 c = Splat({-e, e, -e}), d = Splat({e, e, e});
    return Normalize4(a * MapDistance4(p + a) + b * MapDistance4(p + b) +
                      c * MapDistance4(p + c) + d * MapDistance4(p + d));
}

// Marches the active lanes; returns the distance, sets *hit and *metal per lane.
F4 March4(P3 origin, P3 dir, int steps, float max_distance, I4 active, I4 *hit, I4 *metal)
{
    F4 t = S4(0.02f);
    *hit = I4{0, 0, 0, 0};
    *metal = I4{0, 0, 0, 0};
    for (int i = 0; i < steps && Any4(active); ++i)
    {
        I4 lane_metal;
        const F4 d = Map4(origin + dir * t, &lane_metal);
        const I4 now_hit = (d < S4(0.0015f) * t) & active;
        *hit |= now_hit;
        *metal |= lane_metal & now_hit;
        active &= ~now_hit;
        t = Sel4(active, t + d, t);
        active &= t < S4(max_distance);
    }
    return t;
}

F4 SoftShadow4(P3 origin, P3 dir, I4 active)
{
    F4 result = S4(1.0f);
    F4 t = S4(0.05f);
    for (int i = 0; i < 22 && Any4(active); ++i)
    {
        const F4 d = MapDistance4(origin + dir * t);
        const I4 blocked = (d < S4(0.001f)) & active;
        result = Sel4(blocked, S4(0.0f), result);
        active &= ~blocked;
        result = Sel4(active, Min4(result, S4(10.0f) * d / t), result);
        t = t + Clamp4(d, 0.03f, 0.5f);
        active &= t < S4(12.0f);
    }
    return Clamp4(result, 0.0f, 1.0f);
}

F4 AmbientOcclusion4(P3 p, P3 n)
{
    F4 occlusion = S4(0.0f);
    float weight = 1.0f;
    for (int i = 1; i <= 4; ++i)
    {
        const float h = 0.08f * static_cast<float>(i);
        occlusion += S4(weight) * (S4(h) - MapDistance4(p + n * S4(h)));
        weight *= 0.6f;
    }
    return Clamp4(S4(1.0f) - S4(2.5f) * occlusion, 0.0f, 1.0f);
}

P3 Sky4(P3 dir)
{
    const Mood &m = *g_state.mood;
    const P3 color = Mix4(Splat(m.sky_horizon), Splat(m.sky_top), Clamp4(dir.y * S4(1.6f), 0.0f, 1.0f));
    F4 sun = Max4(Dot4(dir, Splat(g_state.sun)), S4(0.0f));
    sun *= sun;
    sun *= sun;
    sun *= sun;
    const F4 halo = sun;
    sun *= sun;
    sun *= sun;
    sun *= sun;
    sun *= sun;
    return color + Splat(m.sun_color) * (S4(0.25f) * halo + S4(1.6f) * sun);
}

P3 FloorAlbedo4(P3 p)
{
    const Mood &m = *g_state.mood;
    const I4 cx = __builtin_convertvector(__builtin_elementwise_floor(p.x * S4(0.8f)), I4);
    const I4 cz = __builtin_convertvector(__builtin_elementwise_floor(p.z * S4(0.8f)), I4);
    const I4 odd = ((cx + cz) & 1) != 0;
    return Sel3(odd, Splat(m.floor_a), Splat(m.floor_b));
}

P3 Fog4(P3 color, F4 distance)
{
    const F4 f = S4(1.0f) / (S4(1.0f) + distance * distance * S4(0.0025f));
    return Mix4(Splat(g_state.mood->fog), color, f);
}

inline F4 Pow32_4(F4 x)
{
    x *= x;
    x *= x;
    x *= x;
    x *= x;
    return x * x;
}

P3 Trace4(P3 origin, P3 dir)
{
    const Mood &m = *g_state.mood;
    const P3 sun = Splat(g_state.sun);
    I4 hit, metal;
    const I4 all{-1, -1, -1, -1};
    const F4 t = March4(origin, dir, 90, 45.0f, all, &hit, &metal);
    const P3 sky = Sky4(dir);
    if (!Any4(hit))
        return sky;
    const P3 p = origin + dir * t;
    const P3 up = Splat({0.0f, 1.0f, 0.0f});
    const P3 n = Any4(metal & hit) ? Sel3(metal, Normal4(p), up) : up;
    const P3 albedo = Sel3(metal, Splat(m.metal), FloorAlbedo4(p));
    const F4 reflectivity = Sel4(metal, S4(0.65f), S4(0.28f));
    const F4 diffuse = Max4(Dot4(n, sun), S4(0.0f));
    const F4 shadow = SoftShadow4(p + n * S4(0.01f), sun, hit & (diffuse > S4(0.0f)));
    const F4 shadow_lit = Sel4(diffuse > S4(0.0f), shadow, S4(0.0f));
    const F4 ao = AmbientOcclusion4(p, n);
    const P3 half = Normalize4(sun - dir);
    const F4 specular = Pow32_4(Max4(Dot4(n, half), S4(0.0f))) * shadow_lit;
    const F4 sky_light = S4(0.5f) + S4(0.5f) * n.y;
    P3 color = albedo * (Splat(m.sun_color) * (diffuse * shadow_lit) +
                         Splat(m.sky_top) * (S4(0.35f) * sky_light * ao));
    color = color + Splat(m.sun_color) * (specular * Sel4(metal, S4(1.5f), S4(0.4f)));

    // One reflection bounce, shaded without shadows to stay real-time.
    const P3 bounce = Reflect4(dir, n);
    const P3 r_origin = p + n * S4(0.02f);
    I4 hit2, metal2;
    const F4 t2 = March4(r_origin, bounce, 48, 30.0f, hit, &hit2, &metal2);
    P3 reflected = Sky4(bounce);
    if (Any4(hit2))
    {
        const P3 q = r_origin + bounce * t2;
        const P3 n2 = Any4(metal2) ? Sel3(metal2, Normal4(q), up) : up;
        const P3 albedo2 = Sel3(metal2, Splat(m.metal), FloorAlbedo4(q));
        const P3 lit = albedo2 * (Splat(m.sun_color) * Max4(Dot4(n2, sun), S4(0.0f)) +
                                  Splat(m.sky_top) * S4(0.3f));
        reflected = Sel3(hit2, Fog4(lit, t2), reflected);
    }
    color = Mix4(color, reflected, reflectivity);
    return Sel3(hit, Fog4(color, t), sky);
}

inline std::uint32_t PackLane(float r, float g, float b)
{
    return UINT32_C(0xff000000) | (static_cast<std::uint32_t>(b * 255.0f) << 16) |
           (static_cast<std::uint32_t>(g * 255.0f) << 8) | static_cast<std::uint32_t>(r * 255.0f);
}

// ---- Scene 2: analytic neon tunnel (camera on the tunnel axis).

inline F4 Floor4(F4 x) { return __builtin_elementwise_floor(x); }
inline F4 Fract4(F4 x) { return x - Floor4(x); }
inline F4 Abs4(F4 x) { return Max4(x, -x); }

F4 Sin4(F4 x)
{
    constexpr float kTwoPi = 6.28318531f;
    x = x - S4(kTwoPi) * Floor4(x / S4(kTwoPi) + S4(0.5f));
    const F4 x2 = x * x;
    return x * (S4(1.0f) +
                x2 * (S4(-1.0f / 6) + x2 * (S4(1.0f / 120) + x2 * (S4(-1.0f / 5040) +
                                                                  x2 * S4(1.0f / 362880)))));
}

// Additive glow of a point light seen along a ray from the origin.
inline F4 Glow4(P3 dir, V3 point, float size)
{
    const F4 along = Dot4(dir, Splat(point));
    const F4 miss2 = S4(Dot(point, point)) - along * along;
    return Sel4(along > S4(0.0f), S4(size) / (Max4(miss2, S4(0.0f)) + S4(size)), S4(0.0f));
}

// Ray from the origin against an axis-aligned ellipsoid; returns the hit
// distance or -1.
inline F4 HitEllipsoid4(P3 dir, V3 centre, V3 radii)
{
    const V3 o{-centre.x / radii.x, -centre.y / radii.y, -centre.z / radii.z};
    const P3 d{dir.x * S4(1.0f / radii.x), dir.y * S4(1.0f / radii.y), dir.z * S4(1.0f / radii.z)};
    const F4 a = Dot4(d, d);
    const F4 b = Dot4(Splat(o), d);
    const F4 c = S4(Dot(o, o) - 1.0f);
    const F4 disc = b * b - a * c;
    const F4 t = (-b - Sqrt4(disc)) / a;
    return Sel4((disc > S4(0.0f)) & (t > S4(0.0f)), t, S4(-1.0f));
}

P3 TraceTunnel4(P3 dir)
{
    const FrameState &s = g_state;
    const Mood &m = *s.mood;
    // Unit-radius cylinder: distance to the wall along the ray.
    const F4 t = S4(1.0f) / Max4(Sqrt4(dir.x * dir.x + dir.y * dir.y), S4(1e-3f));
    const F4 z = S4(s.time * 3.0f) + dir.z * t;
    // Twist the wall pattern along the tunnel.
    const F4 twist = z * S4(0.25f) + S4(s.time * 0.4f);
    const F4 wall_x = dir.x * t * Sin4(twist + S4(1.5707963f)) - dir.y * t * Sin4(twist);
    // cos(6 * angle) via the Chebyshev polynomial of cos(angle).
    const F4 c2 = wall_x * wall_x;
    const F4 cos6 = ((S4(32.0f) * c2 - S4(48.0f)) * c2 + S4(18.0f)) * c2 - S4(1.0f);
    const F4 ring = Fract4(z * S4(0.8f)) - S4(0.5f);
    const F4 ring_distance = S4(0.5f) - Abs4(ring);
    const F4 pulse = S4(1.0f + 1.5f * s.beat);
    const F4 ring_glow = S4(0.035f) / (ring_distance + S4(0.035f)) * pulse;
    const F4 panel_glow = S4(0.05f) / (S4(1.05f) - cos6);
    const F4 fog = S4(1.0f) / (S4(1.0f) + t * S4(0.12f));
    P3 color = Splat(m.sun_color) * ring_glow + Splat(m.sky_horizon) * (panel_glow * S4(0.6f)) +
               Splat(m.floor_a) * (S4(0.08f) * (S4(1.0f) + cos6));
    color = color * fog;
    // Light at the end of the tunnel (forward component to the 32nd power).
    F4 core = Max4(dir.z, S4(0.0f));
    for (int i = 0; i < 5; ++i)
        core = core * core;
    color = color + Splat(m.sun_color) * (core * S4(0.8f));

    // Comets: glowing heads with fading tails.
    for (int i = 0; i < 4; ++i)
    {
        F4 head = Glow4(dir, s.comets[i][0], 0.0006f);
        F4 tail = S4(0.0f);
        for (int k = 1; k < 6; ++k)
            tail = tail + Glow4(dir, s.comets[i][k], 0.0008f / static_cast<float>(k));
        color = color + Splat(V3{0.85f, 0.95f, 1.0f}) * (head * S4(1.5f)) +
                Splat(m.sky_horizon) * (tail * S4(0.6f));
    }

    // Laser bolts: two glow points each, for a short streak.
    for (int i = 0; i < 8; ++i)
    {
        if (s.bolts[i].z <= 0.0f)
            continue;
        const F4 bolt = Glow4(dir, s.bolts[i], 0.0004f) +
                        Glow4(dir, s.bolts[i] - V3{0.0f, 0.0f, 0.35f}, 0.0003f);
        color = color + Splat(V3{0.4f, 1.0f, 0.6f}) * (bolt * S4(1.4f));
    }
    if (s.blast_strength > 0.0f)
    {
        const F4 blast = Glow4(dir, s.blast, 0.02f * s.blast_strength);
        color = color + Splat(V3{1.0f, 0.7f, 0.3f}) * (blast * S4(2.0f * s.blast_strength));
    }

    // Spaceship: fuselage and wing ellipsoids, metallic with a neon rim.
    const V3 body_radii{0.16f, 0.09f, 0.5f};
    const V3 wing_radii{0.62f, 0.03f, 0.2f};
    F4 t_body = HitEllipsoid4(dir, s.ship, body_radii);
    F4 t_wing = HitEllipsoid4(dir, s.ship + V3{0.0f, -0.02f, -0.12f}, wing_radii);
    const I4 body_hit = t_body > S4(0.0f);
    const I4 wing_hit = t_wing > S4(0.0f);
    const I4 use_wing = wing_hit & ((~body_hit) | (t_wing < t_body));
    const I4 any_hit = body_hit | wing_hit;
    if (Any4(any_hit))
    {
        const F4 t_ship = Sel4(use_wing, t_wing, t_body);
        const P3 hit = dir * t_ship;
        const V3 centre_wing = s.ship + V3{0.0f, -0.02f, -0.12f};
        const P3 local = Sel3(use_wing, hit - Splat(centre_wing), hit - Splat(s.ship));
        const P3 inv2 = Sel3(use_wing,
                             Splat(V3{1.0f / (wing_radii.x * wing_radii.x),
                                      1.0f / (wing_radii.y * wing_radii.y),
                                      1.0f / (wing_radii.z * wing_radii.z)}),
                             Splat(V3{1.0f / (body_radii.x * body_radii.x),
                                      1.0f / (body_radii.y * body_radii.y),
                                      1.0f / (body_radii.z * body_radii.z)}));
        const P3 n = Normalize4(local * inv2);
        const P3 light = Splat(Normalize(V3{0.3f, 0.7f, -1.0f}));
        const F4 diffuse = Max4(Dot4(n, light), S4(0.0f));
        F4 spec = Max4(Dot4(Reflect4(dir, n), light), S4(0.0f));
        for (int i = 0; i < 4; ++i)
            spec = spec * spec;
        const F4 facing = S4(1.0f) - Abs4(Dot4(n, dir));
        const P3 ship_color = Splat(m.metal) * (S4(0.12f) + diffuse * S4(0.7f)) + Splat(V3{1, 1, 1}) * spec +
                              Splat(m.sky_horizon) * (facing * facing * S4(0.9f));
        color = Sel3(any_hit, ship_color, color);
    }
    // Engine flames behind the ship pulse with the kick.
    const float flame = 0.0012f * (1.0f + 1.5f * s.beat);
    const F4 engines = Glow4(dir, s.ship + V3{0.1f, 0.0f, -0.52f}, flame) +
                       Glow4(dir, s.ship + V3{-0.1f, 0.0f, -0.52f}, flame);
    return color + Splat(m.sun_color) * (engines * S4(1.2f));
}

inline P3 TraceScene4(P3 eye, P3 dir)
{
    return g_state.scene == 1 ? TraceTunnel4(dir) : Trace4(eye, dir);
}

// Rotated-grid sub-pixel offsets for 4x antialiasing.
constexpr float kRgssX[4] = {-0.125f, 0.375f, 0.125f, -0.375f};
constexpr float kRgssY[4] = {-0.375f, -0.125f, 0.375f, 0.125f};

inline float Average(F4 v) { return (v.x + v.y + v.z + v.w) * 0.25f; }

void RenderRow(int y)
{
    const FrameState &s = g_state;
    const float aspect = static_cast<float>(kWidth) / static_cast<float>(kHeight);
    const float du = 2.0f * aspect / static_cast<float>(s.width);
    const float dv = 2.0f / static_cast<float>(s.height);
    const float v_center = 1.0f - (static_cast<float>(y) + 0.5f) * dv;
    const P3 forward = Splat(s.forward * 1.6f);
    const P3 right = Splat(s.right);
    const P3 up = Splat(s.up);
    const P3 eye = Splat(s.eye);
    std::uint32_t *out = s.internal + y * s.width;
    if (s.antialias)
    {
        // One pixel per packet: the four lanes are its sub-samples.
        const F4 offset_u = F4{kRgssX[0], kRgssX[1], kRgssX[2], kRgssX[3]} * S4(du);
        const F4 v = S4(v_center) - F4{kRgssY[0], kRgssY[1], kRgssY[2], kRgssY[3]} * S4(dv);
        for (int x = 0; x < s.width; ++x)
        {
            const F4 u = S4((static_cast<float>(x) + 0.5f) * du - aspect) + offset_u;
            const P3 c = TraceScene4(eye, Normalize4(forward + right * u + up * v));
            // Clamp before averaging so bright highlights do not dominate.
            out[x] = PackLane(Sqrt(Average(Clamp4(c.x, 0.0f, 1.0f))),
                              Sqrt(Average(Clamp4(c.y, 0.0f, 1.0f))),
                              Sqrt(Average(Clamp4(c.z, 0.0f, 1.0f))));
        }
        return;
    }
    const F4 v = S4(v_center);
    for (int x = 0; x < s.width; x += 4)
    {
        const float u0 = (static_cast<float>(x) + 0.5f) * du - aspect;
        const F4 u = F4{u0, u0 + du, u0 + 2 * du, u0 + 3 * du};
        const P3 c = TraceScene4(eye, Normalize4(forward + right * u + up * v));
        // Approximate gamma 2.0 for the sRGB display buffer.
        const F4 r = Sqrt4(Clamp4(c.x, 0.0f, 1.0f));
        const F4 g = Sqrt4(Clamp4(c.y, 0.0f, 1.0f));
        const F4 b = Sqrt4(Clamp4(c.z, 0.0f, 1.0f));
        out[x] = PackLane(r.x, g.x, b.x);
        out[x + 1] = PackLane(r.y, g.y, b.y);
        out[x + 2] = PackLane(r.z, g.z, b.z);
        out[x + 3] = PackLane(r.w, g.w, b.w);
    }
}

// Blends two packed RGBA8 colours, weight t in [0, 256].
inline std::uint32_t LerpPacked(std::uint32_t a, std::uint32_t b, std::uint32_t t)
{
    const std::uint32_t rb =
        (((a & 0x00ff00ffu) * (256 - t) + (b & 0x00ff00ffu) * t) >> 8) & 0x00ff00ffu;
    const std::uint32_t ga =
        (((a >> 8) & 0x00ff00ffu) * (256 - t) + ((b >> 8) & 0x00ff00ffu) * t) & 0xff00ff00u;
    return rb | ga;
}

// Bilinear upscale of one display row from the internal render target.
void UpscaleRow(int y)
{
    const FrameState &s = g_state;
    int fy = ((2 * y + 1) * s.height * 256) / (2 * static_cast<int>(kHeight)) - 128;
    fy = fy < 0 ? 0 : fy;
    const int y0 = fy >> 8;
    const int y1 = y0 + 1 < s.height ? y0 + 1 : y0;
    const std::uint32_t wy = static_cast<std::uint32_t>(fy & 255);
    const std::uint32_t *row0 = s.internal + y0 * s.width;
    const std::uint32_t *row1 = s.internal + y1 * s.width;
    std::uint32_t *dst = s.pixels + y * static_cast<int>(kWidth);
    int spans[4][2];
    int span_count = 0;
    for (int i = 0; i < s.panel_count; ++i)
    {
        if (y >= s.panels[i][1] && y < s.panels[i][3])
        {
            spans[span_count][0] = s.panels[i][0];
            spans[span_count][1] = s.panels[i][2];
            ++span_count;
        }
    }
    const int step = (s.width << 16) / static_cast<int>(kWidth);
    int fx = step / 2 - 32768;
    for (int x = 0; x < static_cast<int>(kWidth); ++x, fx += step)
    {
        const int clamped = fx < 0 ? 0 : fx;
        const int x0 = clamped >> 16;
        const int x1 = x0 + 1 < s.width ? x0 + 1 : x0;
        const std::uint32_t wx = static_cast<std::uint32_t>((clamped >> 8) & 255);
        std::uint32_t value = LerpPacked(LerpPacked(row0[x0], row0[x1], wx),
                                         LerpPacked(row1[x0], row1[x1], wx), wy);
        for (int i = 0; i < span_count; ++i)
        {
            if (x >= spans[i][0] && x < spans[i][1])
            {
                // 3/8 brightness keeps the scene visible behind the text.
                value = UINT32_C(0xff000000) | (((value >> 2) & 0x003f3f3fu) + ((value >> 3) & 0x001f1f1fu));
                break;
            }
        }
        dst[x] = value;
    }
}

// ---------------------------------------------------------------- workers

// A job is a function run once per row. Tickets carry the job generation in
// the upper 32 bits, so a late worker can never take rows of a newer job.
struct Job
{
    void (*run)(int);
    int rows;
    std::atomic<int> done;
};

Job g_jobs[4];
std::atomic<std::uint64_t> g_ticket{0};
std::atomic<std::uint32_t> g_generation{0};
std::uint32_t g_next_generation = 1;
std::atomic<bool> g_stop_workers{false};

void WorkOnJob()
{
    for (;;)
    {
        const std::uint64_t ticket = g_ticket.fetch_add(1, std::memory_order_acq_rel);
        Job &job = g_jobs[(ticket >> 32) & 3];
        const int row = static_cast<int>(ticket & 0xffffffffu);
        if (row >= job.rows)
            return;
        job.run(row);
        job.done.fetch_add(1, std::memory_order_acq_rel);
    }
}

void *Worker(void *)
{
    std::uint32_t seen = 0;
    for (;;)
    {
        int spins = 0;
        while (g_generation.load(std::memory_order_acquire) == seen)
        {
            if (g_stop_workers.load(std::memory_order_acquire))
                return nullptr;
            if (++spins > 200)
            {
                (void)scePthreadYield();
                spins = 0;
            }
        }
        seen = g_generation.load(std::memory_order_acquire);
        WorkOnJob();
    }
    return nullptr;
}

void RunJob(void (*run)(int), int rows)
{
    const std::uint32_t generation = g_next_generation++;
    Job &job = g_jobs[generation & 3];
    job.run = run;
    job.rows = rows;
    job.done.store(0, std::memory_order_relaxed);
    g_ticket.store(static_cast<std::uint64_t>(generation) << 32, std::memory_order_release);
    g_generation.store(generation, std::memory_order_release);
    WorkOnJob();
    while (job.done.load(std::memory_order_acquire) < rows)
        (void)scePthreadYield();
}

void RenderFrame()
{
    RunJob(RenderRow, g_state.height);
    RunJob(UpscaleRow, static_cast<int>(kHeight));
}

// ---------------------------------------------------------------- text

struct Glyph
{
    char character;
    std::uint8_t rows[7];
};

constexpr Glyph kGlyphs[] = {
    {' ', {0, 0, 0, 0, 0, 0, 0}},        {'0', {14, 17, 19, 21, 25, 17, 14}},
    {'1', {4, 12, 4, 4, 4, 4, 14}},      {'2', {14, 17, 1, 2, 4, 8, 31}},
    {'3', {30, 1, 1, 14, 1, 1, 30}},     {'4', {2, 6, 10, 18, 31, 2, 2}},
    {'5', {31, 16, 16, 30, 1, 1, 30}},   {'6', {14, 16, 16, 30, 17, 17, 14}},
    {'7', {31, 1, 2, 4, 8, 8, 8}},       {'8', {14, 17, 17, 14, 17, 17, 14}},
    {'9', {14, 17, 17, 15, 1, 1, 14}},   {'A', {14, 17, 17, 31, 17, 17, 17}},
    {'B', {30, 17, 17, 30, 17, 17, 30}}, {'C', {14, 17, 16, 16, 16, 17, 14}},
    {'D', {30, 17, 17, 17, 17, 17, 30}}, {'E', {31, 16, 16, 30, 16, 16, 31}},
    {'F', {31, 16, 16, 30, 16, 16, 16}}, {'G', {14, 17, 16, 23, 17, 17, 14}},
    {'H', {17, 17, 17, 31, 17, 17, 17}}, {'I', {31, 4, 4, 4, 4, 4, 31}},
    {'J', {7, 2, 2, 2, 18, 18, 12}},     {'K', {17, 18, 20, 24, 20, 18, 17}},
    {'L', {16, 16, 16, 16, 16, 16, 31}}, {'M', {17, 27, 21, 21, 17, 17, 17}},
    {'N', {17, 25, 21, 19, 17, 17, 17}}, {'O', {14, 17, 17, 17, 17, 17, 14}},
    {'P', {30, 17, 17, 30, 16, 16, 16}}, {'Q', {14, 17, 17, 17, 21, 18, 13}},
    {'R', {30, 17, 17, 30, 20, 18, 17}}, {'S', {15, 16, 16, 14, 1, 1, 30}},
    {'T', {31, 4, 4, 4, 4, 4, 4}},       {'U', {17, 17, 17, 17, 17, 17, 14}},
    {'V', {17, 17, 17, 17, 17, 10, 4}},  {'W', {17, 17, 17, 21, 21, 21, 10}},
    {'X', {17, 17, 10, 4, 10, 17, 17}},  {'Y', {17, 17, 10, 4, 4, 4, 4}},
    {'Z', {31, 1, 2, 4, 8, 16, 31}},     {'.', {0, 0, 0, 0, 0, 12, 12}},
    {':', {0, 12, 12, 0, 12, 12, 0}},    {'-', {0, 0, 0, 31, 0, 0, 0}},
    {'/', {1, 1, 2, 4, 8, 16, 16}},       {'+', {0, 4, 4, 31, 4, 4, 0}},
    {'(', {2, 4, 8, 8, 8, 4, 2}},         {')', {8, 4, 2, 2, 2, 4, 8}},
    {',', {0, 0, 0, 0, 12, 4, 8}},        {'@', {14, 17, 23, 21, 23, 16, 14}},
};

void DrawText(std::uint32_t *pixels, int x, int y, const char *text, int scale,
              std::uint32_t color)
{
    for (; *text != '\0'; ++text, x += 6 * scale)
    {
        const Glyph *glyph = nullptr;
        for (const Glyph &candidate : kGlyphs)
        {
            if (candidate.character == (*text >= 'a' && *text <= 'z' ? *text - 32 : *text))
                glyph = &candidate;
        }
        if (glyph == nullptr)
            continue;
        for (int row = 0; row < 7; ++row)
        {
            for (int col = 0; col < 5; ++col)
            {
                if ((glyph->rows[row] & (16 >> col)) == 0)
                    continue;
                for (int dy = 0; dy < scale; ++dy)
                {
                    const int py = y + row * scale + dy;
                    const int px = x + col * scale;
                    if (py < 0 || py >= static_cast<int>(kHeight) || px < 0 ||
                        px + scale > static_cast<int>(kWidth))
                        continue;
                    for (int dx = 0; dx < scale; ++dx)
                        pixels[py * kWidth + px + dx] = color;
                }
            }
        }
    }
}

void DrawShadowedText(std::uint32_t *pixels, int x, int y, const char *text, int scale)
{
    DrawText(pixels, x + scale, y + scale, text, scale, UINT32_C(0xff000000));
    DrawText(pixels, x, y, text, scale, UINT32_C(0xffffffff));
}

// Writes a non-negative number with one decimal into buffer.
char *AppendNumber(char *out, float value, bool decimal)
{
    int whole = static_cast<int>(value);
    char digits[12];
    int count = 0;
    do
    {
        digits[count++] = static_cast<char>('0' + whole % 10);
        whole /= 10;
    } while (whole > 0 && count < 11);
    while (count > 0)
        *out++ = digits[--count];
    if (decimal)
    {
        *out++ = '.';
        *out++ = static_cast<char>('0' + static_cast<int>(value * 10.0f) % 10);
    }
    return out;
}

char *AppendText(char *out, const char *text)
{
    while (*text != '\0')
        *out++ = *text++;
    return out;
}

// ---------------------------------------------------------------- input

struct PadState
{
    std::uint32_t buttons;
    float left_x, left_y, right_x, right_y;
    float r2;                        // analog trigger 0..1
    float gyro_x, gyro_y, gyro_z;    // angular velocity in rad/s
};

constexpr std::uint32_t kButtonOptions = 0x0008;
constexpr std::uint32_t kButtonRight = 0x0020;
constexpr std::uint32_t kButtonLeft = 0x0080;
constexpr std::uint32_t kButtonR2 = 0x0200;
constexpr std::uint32_t kButtonTouchPad = 0x100000;

constexpr std::uint32_t kButtonL1 = 0x400;
constexpr std::uint32_t kButtonR1 = 0x800;
constexpr std::uint32_t kButtonTriangle = 0x1000;
constexpr std::uint32_t kButtonL3 = 0x0002;
constexpr std::uint32_t kButtonCircle = 0x2000;
constexpr std::uint32_t kButtonCross = 0x4000;
constexpr std::uint32_t kButtonSquare = 0x8000;

float Axis(std::uint8_t raw)
{
    const float value = (static_cast<float>(raw) - 128.0f) / 127.0f;
    return Abs(value) < 0.15f ? 0.0f : Clamp(value, -1.0f, 1.0f);
}

bool ReadPad(std::int32_t handle, PadState *state)
{
    alignas(16) std::uint8_t data[256] = {};
    if (handle < 0 || scePadReadState(handle, data) < 0)
        return false;
    state->buttons = static_cast<std::uint32_t>(data[0]) |
                     (static_cast<std::uint32_t>(data[1]) << 8) |
                     (static_cast<std::uint32_t>(data[2]) << 16) |
                     (static_cast<std::uint32_t>(data[3]) << 24);
    state->left_x = Axis(data[4]);
    state->left_y = Axis(data[5]);
    state->right_x = Axis(data[6]);
    state->right_y = Axis(data[7]);
    state->r2 = static_cast<float>(data[9]) / 255.0f;
    // ScePadData: angular velocity floats start at byte 40.
    float gyro[3];
    __builtin_memcpy(gyro, data + 40, sizeof(gyro));
    state->gyro_x = gyro[0];
    state->gyro_y = gyro[1];
    state->gyro_z = gyro[2];
    return true;
}

// ---------------------------------------------------------------- music
//
// Six original chiptunes in the spirit of 80s/90s cracktro music: pulse
// bass, fast chord arpeggios, a PWM lead with delayed vibrato and ping-pong
// echo, kick/snare/hi-hat. Each song starts with a four-bar intro without
// the lead. D-pad left/right switches songs.

constexpr std::uint32_t kAudioFrames = 256;
constexpr int kSampleRate = 48000;
constexpr int kEchoMax = 3 * 6429;  // dotted eighth at the slowest tempo
constexpr std::uint32_t kAudioStereoS16 = 1;

std::atomic<int> g_beat{0};  // kick envelope in 1/1000
std::atomic<int> g_shot{0};  // set by the game: play a laser sound
std::atomic<int> g_hit{0};   // set by the game: play an explosion

struct Voice
{
    std::uint32_t phase = 0;
    std::uint32_t step = 0;
    float envelope = 0.0f;
};

std::uint32_t PhaseStep(float frequency)
{
    return static_cast<std::uint32_t>(frequency / kSampleRate * 4294967296.0f);
}

std::uint32_t NoteStep(int midi_note)
{
    float frequency = 8.1757989f;  // MIDI note 0
    for (int i = 0; i < midi_note; ++i)
        frequency *= 1.0594631f;
    return PhaseStep(frequency);
}

inline float Saw(std::uint32_t phase)
{
    return static_cast<float>(static_cast<std::int32_t>(phase)) * (1.0f / 2147483648.0f);
}

inline float Pulse(std::uint32_t phase, std::uint32_t width)
{
    return phase < width ? 1.0f : -1.0f;
}

inline float SineApprox(std::uint32_t phase)
{
    const float x = Saw(phase);
    return 4.0f * x * (1.0f - Abs(x));
}

inline std::int16_t ToSample(float value)
{
    // Soft clip keeps the mix loud without harsh wrap-around.
    const float soft = value / (1.0f + Abs(value) * 0.6f) * 1.3f;
    return static_cast<std::int16_t>(Clamp(soft, -1.0f, 1.0f) * 32000.0f);
}

// A song: tempo, four one-bar chords (bass root + three chord tones), a
// 64-step lead melody (MIDI note, -1 = hold, 0 = rest) and drum masks
// (bit n = sixteenth n of the bar).
struct Song
{
    const char *name;
    int sixteenth;  // samples per sixteenth note
    int chords[4][4];
    int melody[64];
    int lead_shift;  // semitones added to the melody
    std::uint16_t kick, snare, hat;
    int bass_mode;  // 0 = octave jumps, 1 = eighth roots, 2 = root-fifth roll
    int arp_hz;     // chord arpeggio speed
    bool roll;      // snare roll at the end of every fourth bar
};

constexpr Song kSongs[] = {
    {"NEON SUNRISE", 5143,  // 140 BPM, Am F C G
     {{45, 57, 60, 64}, {41, 53, 57, 60}, {48, 55, 60, 64}, {43, 55, 59, 62}},
     {69, -1, 72, -1, 76, -1, 74, 72, 74, -1, -1, 72, 69, -1, 67, -1,
      69, -1, -1, -1, 72, -1, 69, -1, 65, -1, 67, 69, -1, -1, 0,  0,
      67, -1, 72, -1, 76, -1, 79, -1, 77, 76, -1, 74, 72, -1, 74, -1,
      74, -1, 71, -1, 67, -1, 71, 74, 79, -1, -1, -1, 76, -1, 74, -1},
     12, 0x4101, 0x1010, 0x5555, 0, 50, true},
    {"CRACKTRO 1987", 4800,  // 150 BPM, Dm Bb C A
     {{38, 62, 65, 69}, {46, 58, 62, 65}, {48, 55, 60, 64}, {45, 57, 61, 64}},
     {74, -1, 77, -1, 81, -1, 77, 74, 76, -1, 77, -1, 74, -1, -1, 0,
      70, -1, 74, -1, 77, -1, 74, 70, 72, -1, 74, -1, 70, -1, -1, 0,
      72, -1, 76, -1, 79, -1, 76, 72, 74, -1, 76, -1, 79, -1, 77, 76,
      73, -1, -1, -1, 76, -1, 73, -1, 69, -1, -1, -1, -1, -1, 0,  0},
     0, 0x1111, 0x1010, 0xffff, 0, 50, true},
    {"VECTOR BALLS", 5625,  // 128 BPM, Em C G D
     {{40, 55, 59, 64}, {48, 55, 60, 64}, {43, 55, 59, 62}, {38, 57, 62, 66}},
     {76, -1, -1, 79, -1, -1, 76, -1, 74, -1, 71, -1, 74, -1, 76, -1,
      72, -1, -1, 76, -1, -1, 72, -1, 71, -1, 67, -1, 71, -1, 72, -1,
      74, -1, -1, 79, -1, -1, 74, -1, 83, -1, 81, -1, 79, -1, 78, -1,
      78, -1, -1, -1, 74, -1, -1, -1, 69, -1, 74, -1, 78, -1, 81, -1},
     0, 0x0941, 0x1010, 0x5555, 2, 50, true},
    {"DISK MAGAZINE", 6429,  // 112 BPM, Cm Ab Eb Bb
     {{48, 60, 63, 67}, {44, 60, 63, 68}, {39, 58, 63, 67}, {46, 58, 62, 65}},
     {75, -1, -1, -1, 74, -1, 72, -1, 67, -1, -1, -1, -1, -1, 0,  0,
      72, -1, -1, -1, 75, -1, 77, -1, 79, -1, -1, -1, -1, -1, 0,  0,
      79, -1, 77, -1, 75, -1, 74, -1, 75, -1, -1, -1, 70, -1, -1, 0,
      74, -1, -1, -1, -1, -1, 72, 74, 70, -1, -1, -1, -1, -1, 0,  0},
     0, 0x0401, 0x0100, 0x4444, 1, 25, false},
    {"HYPERSPACE", 4235,  // 170 BPM, Am G F E
     {{45, 57, 60, 64}, {43, 55, 59, 62}, {41, 53, 57, 60}, {40, 56, 59, 64}},
     {81, 79, 76, 79, 81, -1, 84, -1, 81, 79, 76, 72, 76, -1, 79, -1,
      79, 77, 74, 77, 79, -1, 83, -1, 79, 77, 74, 71, 74, -1, 77, -1,
      77, 76, 72, 76, 77, -1, 81, -1, 77, 76, 72, 69, 72, -1, 76, -1,
      76, -1, 80, -1, 83, -1, 80, -1, 76, -1, -1, -1, 71, -1, 68, -1},
     0, 0x0401, 0x1010, 0xffff, 0, 60, true},
    {"SID HEARTS", 5455,  // 132 BPM, C G Am F
     {{48, 55, 60, 64}, {43, 55, 59, 62}, {45, 57, 60, 64}, {41, 53, 57, 60}},
     {72, -1, 72, 74, 76, -1, 72, -1, 79, -1, -1, -1, 76, -1, -1, -1,
      74, -1, 74, 76, 79, -1, 74, -1, 83, -1, -1, -1, 79, -1, -1, -1,
      76, -1, 76, 77, 79, -1, 76, -1, 84, -1, -1, -1, 81, -1, 79, -1,
      77, -1, -1, -1, 76, -1, 74, -1, 72, -1, -1, -1, -1, -1, 0,  0},
     0, 0x0501, 0x1010, 0x5555, 2, 50, true},
};
constexpr int kSongCount = sizeof(kSongs) / sizeof(kSongs[0]);
constexpr int kRootFifth[4] = {0, 7, 12, 7};

std::atomic<int> g_song{0};  // selected by the game, read by the music thread

void *Music(void *argument)
{
    const int port = static_cast<int>(reinterpret_cast<long>(argument));
    static std::int16_t block[kAudioFrames * 2];
    static float echo_left[kEchoMax];
    static float echo_right[kEchoMax];
    int song_index = 0;
    Voice bass, arp, lead, kick, snare;
    std::uint32_t lead_note_step = 0;
    std::uint32_t vibrato_phase = 0;
    std::uint32_t pwm_phase = 0;
    int lead_age = 0;
    float kick_frequency = 50.0f;
    float hat = 0.0f;
    float noise_value = 0.0f;
    Voice laser;
    float laser_frequency = 0.0f;
    float boom = 0.0f;
    float boom_filter = 0.0f;
    std::uint32_t noise = 0x12345678u;
    int echo_index = 0;
    std::int64_t sample = 0;
    for (;;)
    {
        if (g_shot.exchange(0, std::memory_order_relaxed) != 0)
        {
            laser.envelope = 1.0f;
            laser_frequency = 2400.0f;
        }
        if (g_hit.exchange(0, std::memory_order_relaxed) != 0)
            boom = 1.0f;
        const int wanted = g_song.load(std::memory_order_relaxed);
        if (wanted != song_index)
        {
            // Restart cleanly: new intro, silent voices, empty echo.
            song_index = wanted;
            sample = 0;
            echo_index = 0;
            bass.envelope = arp.envelope = lead.envelope = 0.0f;
            for (int k = 0; k < kEchoMax; ++k)
                echo_left[k] = echo_right[k] = 0.0f;
        }
        const Song &song = kSongs[song_index];
        const int echo_length = 3 * song.sixteenth;
        const int arp_tick = kSampleRate / song.arp_hz;
        for (std::uint32_t i = 0; i < kAudioFrames; ++i, ++sample)
        {
            const std::int64_t sixteenth = sample / song.sixteenth;
            const int step = static_cast<int>(sixteenth % 64);
            const int beat_step = step % 16;
            const int *chord = song.chords[step / 16];
            if (sample % song.sixteenth == 0)
            {
                if (song.bass_mode == 0)
                    bass.step = NoteStep(chord[0] + (beat_step % 2 == 1 ? 12 : 0));
                else if (song.bass_mode == 2)
                    bass.step = NoteStep(chord[0] + kRootFifth[beat_step % 4]);
                else
                    bass.step = NoteStep(chord[0]);
                if (song.bass_mode != 1 || beat_step % 2 == 0)
                    bass.envelope = 1.0f;
                arp.envelope = 1.0f;
                const bool intro = sixteenth < 64;
                const int note = song.melody[step];
                if (!intro && note > 0)
                {
                    lead_note_step = NoteStep(note + song.lead_shift);
                    lead.envelope = 1.0f;
                    lead_age = 0;
                }
                else if (!intro && note == 0)
                {
                    lead.envelope = 0.0f;
                }
                if (((song.kick >> beat_step) & 1) != 0)
                {
                    kick.phase = 0;
                    kick.envelope = 1.0f;
                    kick_frequency = 180.0f;
                }
                const bool roll = song.roll && step >= 60;
                if (((song.snare >> beat_step) & 1) != 0 || roll)
                {
                    snare.envelope = roll ? 0.6f + 0.1f * static_cast<float>(step - 60) : 1.0f;
                    snare.step = PhaseStep(190.0f);
                }
                if (((song.hat >> beat_step) & 1) != 0)
                    hat = 1.0f;
            }
            // Fast chord arpeggio over the chord tones, an octave up.
            arp.step = NoteStep(chord[1 + static_cast<int>((sample / arp_tick) % 3)] + 12);
            // Lead: vibrato fades in after a short delay.
            ++lead_age;
            vibrato_phase += PhaseStep(6.0f);
            const float vibrato_depth = lead_age > 6000 ? 0.008f : 0.0f;
            lead.step = static_cast<std::uint32_t>(
                static_cast<float>(lead_note_step) *
                (1.0f + vibrato_depth * SineApprox(vibrato_phase)));
            pwm_phase += PhaseStep(0.7f);
            const std::uint32_t width =
                static_cast<std::uint32_t>((0.5f + 0.35f * SineApprox(pwm_phase)) * 4294967295.0f);
            kick_frequency = 50.0f + (kick_frequency - 50.0f) * 0.999f;
            kick.step = PhaseStep(kick_frequency);
            bass.phase += bass.step;
            arp.phase += arp.step;
            lead.phase += lead.step;
            kick.phase += kick.step;
            snare.phase += snare.step;
            noise = noise * 1664525u + 1013904223u;
            noise_value = Saw(noise);

            const float drums = SineApprox(kick.phase) * kick.envelope * 0.55f +
                                (noise_value * 0.7f + SineApprox(snare.phase) * 0.5f) *
                                    snare.envelope * 0.35f +
                                noise_value * hat * 0.06f;
            const float bass_out = Pulse(bass.phase, 0x60000000u) * bass.envelope * 0.22f;
            const float arp_out = Pulse(arp.phase, 0x40000000u) * arp.envelope * 0.07f;
            const float lead_out = Pulse(lead.phase, width) * lead.envelope * 0.15f;
            // Ping-pong echo on arp and lead.
            const float wet = arp_out + lead_out;
            const float from_left = echo_left[echo_index];
            const float from_right = echo_right[echo_index];
            echo_left[echo_index] = wet * 0.6f + from_right * 0.4f;
            echo_right[echo_index] = from_left * 0.4f;
            echo_index = (echo_index + 1) % echo_length;
            // Sound effects: falling square "pew" and a filtered noise blast.
            laser_frequency *= 0.99985f;
            laser.step = PhaseStep(laser_frequency);
            laser.phase += laser.step;
            boom_filter += (noise_value - boom_filter) * 0.05f;
            const float effects = Pulse(laser.phase, 0x80000000u) * laser.envelope * 0.12f +
                                  boom_filter * boom * 0.9f;
            laser.envelope *= 0.9996f;
            boom *= 0.99988f;
            const float mono = drums + bass_out + wet + effects;
            block[2 * i] = ToSample(mono + from_left * 0.5f);
            block[2 * i + 1] = ToSample(mono + from_right * 0.5f);

            bass.envelope *= 0.9997f;
            arp.envelope *= 0.99993f;
            lead.envelope *= 0.99997f;
            kick.envelope *= 0.99975f;
            snare.envelope *= 0.9993f;
            hat *= 0.997f;
        }
        g_beat.store(static_cast<int>(kick.envelope * 1000.0f), std::memory_order_relaxed);
        (void)sceAudioOutOutput(port, block);  // blocks until the port has room
    }
    return nullptr;
}

// ---------------------------------------------------------------- frame

struct Controls
{
    float yaw = 0.6f;
    float pitch = 0.32f;
    float distance = 7.5f;
    int step = 3;
    bool dynamic = true;
    bool antialias = false;
    float width = 480.0f;  // dynamic internal width
    int mood = 0;
    int scene = 0;
    float scene_time = 0.0f;
    float ship_x = 0.0f;  // spaceship offset from the tunnel axis
    float ship_y = -0.2f;
    bool motion = false;  // steer with the DualSense motion sensor
    bool rumble = true;
    V3 bolts[8] = {};
    float fire_cooldown = 0.0f;
    int comet_dead_cycle[4] = {-1, -1, -1, -1};
    V3 blast = {};
    float blast_strength = 0.0f;
    float rumble_large = 0.0f;  // seconds left
    float rumble_small = 0.0f;
    int hits = 0;
    float help_time = 12.0f;  // seconds the help panel stays visible
    std::uint32_t previous_buttons = 0;
};

void UpdateControls(Controls *c, const PadState &pad, float dt)
{
    const std::uint32_t pressed = pad.buttons & ~c->previous_buttons;
    c->previous_buttons = pad.buttons;
    if ((pressed & (kButtonL1 | kButtonR1)) != 0)
    {
        // A fixed quality step turns dynamic resolution off.
        if (c->dynamic)
            c->dynamic = false;
        else if ((pressed & kButtonL1) != 0 && c->step > 0)
            --c->step;
        else if ((pressed & kButtonR1) != 0 && c->step < kStepCount - 1)
            ++c->step;
        c->width = static_cast<float>(kStepWidths[c->step]);
    }
    if ((pressed & kButtonTriangle) != 0)
        c->dynamic = !c->dynamic;
    if ((pressed & kButtonSquare) != 0)
        c->antialias = !c->antialias;
    if ((pressed & kButtonCross) != 0)
        c->mood = (c->mood + 1) % kMoodCount;
    if ((pressed & kButtonL3) != 0)
        c->help_time = c->help_time > 0.0f ? 0.0f : 1.0e9f;
    if ((pressed & kButtonOptions) != 0)
        c->motion = !c->motion;
    if ((pressed & (kButtonLeft | kButtonRight)) != 0)
    {
        const int song = g_song.load(std::memory_order_relaxed);
        const int next = (pressed & kButtonRight) != 0 ? song + 1 : song + kSongCount - 1;
        g_song.store(next % kSongCount, std::memory_order_relaxed);
    }
    if ((pressed & kButtonTouchPad) != 0)
        c->rumble = !c->rumble;
    if ((pressed & kButtonCircle) != 0)
    {
        c->scene ^= 1;
        c->scene_time = 0.0f;
    }
    c->yaw += pad.right_x * 1.8f * dt;
    c->pitch = Clamp(c->pitch - pad.right_y * 1.2f * dt, 0.05f, 1.2f);
    if (pad.buttons != 0 || pad.left_x != 0.0f || pad.left_y != 0.0f ||
        pad.right_x != 0.0f || pad.right_y != 0.0f)
        c->scene_time = 0.0f;  // no automatic scene change while playing
    if (c->scene == 1)
    {
        // Steer the ship; keep it well inside the tunnel wall.
        c->ship_x += pad.left_x * 1.4f * dt;
        c->ship_y -= pad.left_y * 1.4f * dt;
        if (c->motion)
        {
            // Gyro aiming: turning the pad moves the ship (yaw -> x, pitch -> y).
            c->ship_x -= pad.gyro_y * 0.6f * dt;
            c->ship_y += pad.gyro_x * 0.6f * dt;
        }
        const float radius = Sqrt(c->ship_x * c->ship_x + c->ship_y * c->ship_y);
        if (radius > 0.5f)
        {
            c->ship_x *= 0.5f / radius;
            c->ship_y *= 0.5f / radius;
        }
        // R2 fires laser bolts from both wing tips.
        if (((pad.buttons & kButtonR2) != 0 || pad.r2 > 0.3f) && c->fire_cooldown <= 0.0f)
        {
            int fired = 0;
            for (V3 &bolt : c->bolts)
            {
                if (bolt.z > 0.0f || fired == 2)
                    continue;
                bolt = V3{c->ship_x + (fired == 0 ? 0.3f : -0.3f), c->ship_y, 2.7f};
                ++fired;
            }
            if (fired > 0)
            {
                c->fire_cooldown = 0.14f;
                c->rumble_small = 0.06f;
                g_shot.store(1, std::memory_order_relaxed);
            }
        }
        return;
    }
    c->distance = Clamp(c->distance + pad.left_y * 6.0f * dt, 3.0f, 16.0f);
}

// Comet i at the given time: head (k = 0) or tail point k; also returns the
// pass number so a destroyed comet stays hidden until its next pass.
V3 CometPoint(int i, int k, float time, int *cycle)
{
    const float phase = time * 0.3f + 0.25f * static_cast<float>(i);
    const float progress = phase - Floor(phase);
    if (cycle != nullptr)
        *cycle = static_cast<int>(Floor(phase));
    const float angle = 1.7f * static_cast<float>(i) + time * 0.3f;
    const float spread = 0.62f + 0.01f * static_cast<float>(k);
    return V3{Cos(angle) * spread, Sin(angle) * spread,
              34.0f - progress * 36.0f + 0.4f * static_cast<float>(k)};
}

// Moves bolts, detects comet hits and drives the rumble motors.
void UpdateGame(Controls *c, float time, float dt, std::int32_t pad)
{
    c->fire_cooldown -= dt;
    c->blast_strength = Max(c->blast_strength - dt * 2.5f, 0.0f);
    for (V3 &bolt : c->bolts)
    {
        if (bolt.z <= 0.0f)
            continue;
        bolt.z += 30.0f * dt;
        if (bolt.z > 40.0f)
            bolt.z = 0.0f;
        for (int i = 0; i < 4 && bolt.z > 0.0f && c->scene == 1; ++i)
        {
            int cycle = 0;
            const V3 head = CometPoint(i, 0, time, &cycle);
            if (c->comet_dead_cycle[i] == cycle)
                continue;
            const V3 delta = head - bolt;
            if (delta.x * delta.x + delta.y * delta.y < 0.09f && Abs(delta.z) < 0.9f)
            {
                c->comet_dead_cycle[i] = cycle;
                c->blast = head;
                c->blast_strength = 1.0f;
                c->rumble_large = 0.25f;
                ++c->hits;
                bolt.z = 0.0f;
                g_hit.store(1, std::memory_order_relaxed);
            }
        }
    }
    c->rumble_large = Max(c->rumble_large - dt, 0.0f);
    c->rumble_small = Max(c->rumble_small - dt, 0.0f);
    // Only talk to the pad when the motor state changes.
    static int last = -1;
    const std::uint8_t motors[2] = {
        static_cast<std::uint8_t>(c->rumble && c->rumble_large > 0.0f ? 255 : 0),
        static_cast<std::uint8_t>(c->rumble && c->rumble_small > 0.0f ? 160 : 0)};
    const int state = motors[0] * 256 + motors[1];
    if (pad >= 0 && state != last)
    {
        (void)scePadSetVibration(pad, motors);
        last = state;
    }
}

void SetupFrame(const Controls &c, float time, std::uint32_t *pixels)
{
    FrameState &s = g_state;
    s.pixels = pixels;
    s.internal = g_internal;
    s.antialias = c.antialias;
    s.scene = c.scene;
    s.time = time;
    s.beat = static_cast<float>(g_beat.load(std::memory_order_relaxed)) * 1e-3f;
    s.width = c.dynamic ? static_cast<int>(c.width) & ~3 : kStepWidths[c.step];
    s.height = s.width * 9 / 16;
    s.mood = &kMoods[c.mood];
    // Slow automatic orbit on top of the player's camera.
    const float yaw = c.yaw + time * 0.15f;
    const V3 target{0.0f, 1.0f, 0.0f};
    s.eye = target + V3{Cos(yaw) * Cos(c.pitch), Sin(c.pitch), Sin(yaw) * Cos(c.pitch)} *
                         c.distance;
    s.forward = Normalize(target - s.eye);
    if (c.scene == 1)
    {
        // Tunnel: camera on the axis, gently swaying forward direction.
        s.eye = V3{0.0f, 0.0f, 0.0f};
        s.ship = V3{c.ship_x, c.ship_y + 0.03f * Sin(time * 2.3f), 2.6f};
        for (int i = 0; i < 4; ++i)
        {
            int cycle = 0;
            (void)CometPoint(i, 0, time, &cycle);
            const bool destroyed = c.comet_dead_cycle[i] == cycle;
            for (int k = 0; k < 6; ++k)
            {
                // A destroyed comet is parked behind the camera (no glow).
                s.comets[i][k] = destroyed ? V3{0.0f, 0.0f, -100.0f}
                                           : CometPoint(i, k, time, nullptr);
            }
        }
        for (int i = 0; i < 8; ++i)
            s.bolts[i] = c.bolts[i];
        s.blast = c.blast;
        s.blast_strength = c.blast_strength;
        // The view follows the ship a little, plus right-stick look-around.
        s.forward = Normalize(V3{0.1f * Sin(time * 0.5f) + 0.4f * Sin(c.yaw) + 0.35f * c.ship_x,
                                 0.08f * Sin(time * 0.37f) + 0.35f * c.ship_y, 1.0f});
    }
    s.right = Normalize(Cross(s.forward, V3{0.0f, 1.0f, 0.0f}));
    s.up = Cross(s.right, s.forward);
    s.sun = Normalize(V3{0.6f, 0.55f, -0.45f});
    s.blobs[0] = {0.0f, 1.15f + 0.25f * Sin(time * 1.3f), 0.0f};
    for (int i = 1; i < 5; ++i)
    {
        const float phase = time * (0.7f + 0.17f * static_cast<float>(i)) +
                            1.57f * static_cast<float>(i);
        const float radius = 1.25f + 0.25f * Sin(time * 0.9f + static_cast<float>(i));
        s.blobs[i] = {Cos(phase) * radius, 1.1f + 0.7f * Sin(phase * 1.7f),
                      Sin(phase) * radius};
    }
}

// Scales the internal width so the render time approaches the budget.
// Cost grows with the pixel count, so the width follows the square root.
void UpdateDynamicResolution(Controls *c, float render_ms)
{
    if (!c->dynamic)
        return;
    const float ratio = kBudgetMs / Max(render_ms, 0.25f);
    if (ratio > 0.92f && ratio < 1.08f)
        return;
    const float factor = Clamp(Sqrt(ratio), 0.85f, 1.04f);
    c->width = Clamp(c->width * factor, static_cast<float>(kMinWidth),
                     static_cast<float>(kMaxWidth));
}

int g_hits = 0;
bool g_motion = false;
bool g_rumble = true;
bool g_dynamic = true;
bool g_help = true;

// ---- Where the demo runs, detected once at start. Nothing is hard-coded: on AnyPS5 the
// host CPU comes from hw.model, on a console the system software version is shown.

struct SystemSwVersion
{
    std::size_t size;
    char version_string[0x1c];
    std::uint32_t version;
};

char g_platform[128] = "PLATFORM NOT DETECTED";
char g_build[128];

#ifndef SHOWCASE_VERSION
#define SHOWCASE_VERSION "dev"
#endif
#ifndef SHOWCASE_SOURCE
#define SHOWCASE_SOURCE "unknown"
#endif

bool Contains(const char *text, const char *part)
{
    for (; *text != '\0'; ++text)
    {
        const char *a = text;
        const char *b = part;
        while (*a != '\0' && *b != '\0' && (*a | 32) == (*b | 32))
            ++a, ++b;
        if (*b == '\0')
            return true;
    }
    return false;
}

bool StartsWith(const char *text, const char *prefix)
{
    while (*prefix != '\0')
        if (*text++ != *prefix++)
            return false;
    return true;
}

bool SysctlString(const char *name, char *buffer, std::size_t capacity)
{
    std::size_t length = capacity - 1;
    if (sysctlbyname(name, buffer, &length, nullptr, 0) != 0)
        return false;
    buffer[length < capacity ? length : capacity - 1] = '\0';
    return buffer[0] != '\0';
}

template <typename T> bool SysctlValue(const char *name, T *value)
{
    std::size_t length = sizeof(T);
    return sysctlbyname(name, value, &length, nullptr, 0) == 0 && length == sizeof(T);
}

// Upper-cases text for the bitmap font and folds runs of spaces.
char *AppendClean(char *out, const char *text, int limit)
{
    bool space = false;
    for (; *text != '\0' && limit > 0; ++text)
    {
        const char c = *text;
        if (c == ' ' || c == '\t')
        {
            space = true;
            continue;
        }
        if (space)
        {
            *out++ = ' ';
            --limit;
            space = false;
        }
        *out++ = c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
        --limit;
    }
    return out;
}

void DetectPlatform()
{
    char model[128] = "";
    char path[512] = "";
    const bool has_model = SysctlString("hw.model", model, sizeof(model));
    const bool has_path = SysctlString("kern.proc.pathname", path, sizeof(path));
    int cpus = 0;
    const bool has_cpus = SysctlValue("hw.ncpu", &cpus);
    unsigned long memory = 0;
    const bool has_memory = SysctlValue("hw.physmem", &memory);
    SystemSwVersion sw{};
    sw.size = sizeof(sw);
    const bool has_sw = sceKernelGetSystemSwVersion(&sw) == 0 && sw.version_string[0] != '\0';

    // A host path (not the console's /app0 sandbox) or a desktop CPU means AnyPS5.
    const bool apple = has_model && Contains(model, "apple");
    const bool host_path = has_path && !StartsWith(path, "/app0") && !Contains(path, "/sandbox/");
    const char *host = nullptr;
    if (apple || (host_path && (StartsWith(path, "/Users/") || StartsWith(path, "/Applications/") ||
                                StartsWith(path, "/Volumes/") || StartsWith(path, "/private/"))))
        host = "MACOS";
    else if (host_path && (path[1] == ':' || path[0] == '\\'))
        host = "WINDOWS";
    else if (host_path)
        host = "LINUX";

    char *out = g_platform;
    if (host != nullptr)
    {
        out = AppendText(out, "ANYPS5 ON ");
        out = AppendText(out, host);
        if (Contains(model, "virtualapple"))
            out = AppendText(out, " - APPLE SILICON (ROSETTA 2)");
        else if (has_model)
        {
            out = AppendText(out, " - ");
            out = AppendClean(out, model, 40);
            if (apple)
                out = AppendText(out, " (ROSETTA 2)");
        }
    }
    else if (has_path)
    {
        // Only the console reports its own /app0 sandbox path; AnyPS5 reports none or a host path.
        out = AppendText(out, "PLAYSTATION 5 HARDWARE");
        if (has_sw)
        {
            out = AppendText(out, " - SYSTEM SOFTWARE ");
            out = AppendClean(out, sw.version_string, 16);
        }
    }
    else if (has_model)
    {
        out = AppendText(out, "HOST ");
        out = AppendClean(out, model, 40);
    }
    else
    {
        out = AppendText(out, "PLATFORM NOT DETECTED");
    }
    if (has_cpus && cpus > 0)
    {
        out = AppendText(out, " - ");
        out = AppendNumber(out, static_cast<float>(cpus), false);
        out = AppendText(out, " CPUS");
    }
    if (has_memory && memory > 0)
    {
        out = AppendText(out, " - ");
        out = AppendNumber(out, static_cast<float>(memory >> 30), false);
        out = AppendText(out, " GB");
    }
    *out = '\0';

    out = AppendText(g_build, "BUILD " SHOWCASE_VERSION " - SOURCE " SHOWCASE_SOURCE " - CLANG ");
    out = AppendNumber(out, static_cast<float>(__clang_major__), false);
    out = AppendText(out, ".");
    out = AppendNumber(out, static_cast<float>(__clang_minor__), false);
    out = AppendText(out, ".");
    out = AppendNumber(out, static_cast<float>(__clang_patchlevel__), false);
    *out = '\0';

    // Raw values for the log (the path is reduced to its kind; it may name the user).
    char line[400];
    out = AppendText(line, "[showcase] platform: ");
    out = AppendText(out, g_platform);
    out = AppendText(out, " | hw.model=");
    out = AppendText(out, has_model ? model : "-");
    out = AppendText(out, " | path=");
    out = AppendText(out, !has_path ? "-" : host_path ? "host" : "console");
    out = AppendText(out, " | sw=");
    out = AppendText(out, has_sw ? sw.version_string : "-");
    out = AppendText(out, "\n[showcase] ");
    out = AppendText(out, g_build);
    *out++ = '\n';
    (void)write(1, line, static_cast<std::size_t>(out - line));
}

// ---- Overlay layout: three dimmed panels (identity, statistics, music) and an optional help
// panel. Panels are dimmed during the upscale pass so the display buffer is never read back.

constexpr int kMargin = 24;
constexpr int kPad = 16;

int TextWidth(const char *text, int scale)
{
    int count = 0;
    while (text[count] != '\0')
        ++count;
    return count * 6 * scale - scale;
}

struct OverlayText
{
    char title[96];
    char stats[3][64];
    char music[96];
    const char *help[2];
};

OverlayText BuildOverlayText(float fps, float render_ms)
{
    OverlayText t{};
    char *out = AppendText(t.title, "RAYMARCH TECH DEMO - ");
    out = AppendText(out, g_state.scene == 1 ? "NEON TUNNEL" : "METAL BLOBS");
    *out = '\0';
    out = AppendNumber(t.stats[0], fps, true);
    out = AppendText(out, " FPS");
    *out = '\0';
    out = AppendNumber(t.stats[1], static_cast<float>(g_state.width), false);
    out = AppendText(out, "X");
    out = AppendNumber(out, static_cast<float>(g_state.height), false);
    out = AppendText(out, "  ");
    out = AppendNumber(out, render_ms, true);
    out = AppendText(out, " MS");
    *out = '\0';
    out = AppendText(t.stats[2], g_dynamic ? "AUTO-RES" : "FIXED RES");
    out = AppendText(out, g_state.antialias ? "  AA 4X  " : "  ");
    out = AppendText(out, g_state.mood->name);
    if (g_state.scene == 1)
    {
        out = AppendText(out, "  HITS ");
        out = AppendNumber(out, static_cast<float>(g_hits), false);
    }
    *out = '\0';
    const int song = g_song.load(std::memory_order_relaxed);
    out = AppendText(t.music, "MUSIC ");
    out = AppendNumber(out, static_cast<float>(song + 1), false);
    out = AppendText(out, "/");
    out = AppendNumber(out, static_cast<float>(kSongCount), false);
    out = AppendText(out, "  ");
    out = AppendText(out, kSongs[song].name);
    *out = '\0';
    if (g_state.scene == 1)
    {
        t.help[0] = "L-STICK STEER   R2 FIRE   R-STICK LOOK   OPTIONS MOTION";
        t.help[1] = g_rumble ? (g_motion ? "TOUCHPAD RUMBLE OFF   X MOOD   O SCENE   D-PAD MUSIC   MOTION ON"
                                         : "TOUCHPAD RUMBLE OFF   X MOOD   O SCENE   D-PAD MUSIC")
                             : "TOUCHPAD RUMBLE ON   X MOOD   O SCENE   D-PAD MUSIC";
    }
    else
    {
        t.help[0] = "R-STICK CAMERA   L-STICK ZOOM   L1/R1 RESOLUTION   TRIANGLE AUTO-RES";
        t.help[1] = "SQUARE ANTIALIASING   X MOOD   O SCENE   D-PAD MUSIC";
    }
    return t;
}

// Panel rectangles for this frame; must be set before RenderFrame (the upscale dims them).
void LayoutPanels(float fps, float render_ms)
{
    const OverlayText t = BuildOverlayText(fps, render_ms);
    FrameState &s = g_state;
    s.panel_count = 0;
    const auto add = [&](int x0, int y0, int x1, int y1) {
        s.panels[s.panel_count][0] = x0;
        s.panels[s.panel_count][1] = y0;
        s.panels[s.panel_count][2] = x1;
        s.panels[s.panel_count][3] = y1;
        ++s.panel_count;
    };
    int width = TextWidth(t.title, 4);
    width = Max(static_cast<float>(width), static_cast<float>(TextWidth(g_platform, 2)));
    width = Max(static_cast<float>(width), static_cast<float>(TextWidth(g_build, 2)));
    add(kMargin, kMargin, kMargin + width + 2 * kPad, kMargin + 2 * kPad + 28 + 12 + 14 + 10 + 14);
    add(static_cast<int>(kWidth) - kMargin - 460, kMargin, static_cast<int>(kWidth) - kMargin,
        kMargin + 2 * kPad + 28 + 12 + 21 + 8 + 21);
    const int music = TextWidth(t.music, 3);
    add(kMargin, static_cast<int>(kHeight) - kMargin - 2 * kPad - 21, kMargin + music + 2 * kPad,
        static_cast<int>(kHeight) - kMargin);
    if (g_help)
    {
        const int help = static_cast<int>(Max(static_cast<float>(TextWidth(t.help[0], 2)),
                                              static_cast<float>(TextWidth(t.help[1], 2))));
        const int x0 = (static_cast<int>(kWidth) - help) / 2 - kPad;
        add(x0, static_cast<int>(kHeight) - kMargin - 2 * kPad - 14 - 10 - 14,
            x0 + help + 2 * kPad, static_cast<int>(kHeight) - kMargin);
    }
}

void DrawOverlay(std::uint32_t *pixels, float fps, float render_ms)
{
    const OverlayText t = BuildOverlayText(fps, render_ms);
    const int left = kMargin + kPad;
    DrawShadowedText(pixels, left, kMargin + kPad, t.title, 4);
    DrawShadowedText(pixels, left, kMargin + kPad + 40, g_platform, 2);
    DrawShadowedText(pixels, left, kMargin + kPad + 64, g_build, 2);
    const int right = static_cast<int>(kWidth) - kMargin - kPad;
    DrawShadowedText(pixels, right - TextWidth(t.stats[0], 4), kMargin + kPad, t.stats[0], 4);
    DrawShadowedText(pixels, right - TextWidth(t.stats[1], 3), kMargin + kPad + 40, t.stats[1], 3);
    DrawShadowedText(pixels, right - TextWidth(t.stats[2], 3), kMargin + kPad + 69, t.stats[2], 3);
    const int bottom = static_cast<int>(kHeight) - kMargin - kPad;
    DrawShadowedText(pixels, left, bottom - 21, t.music, 3);
    if (g_help)
    {
        for (int i = 0; i < 2; ++i)
        {
            DrawShadowedText(pixels, (static_cast<int>(kWidth) - TextWidth(t.help[i], 2)) / 2,
                             bottom - 38 + i * 24, t.help[i], 2);
        }
    }
    DrawShadowedText(pixels, right - TextWidth("L3 HELP", 2), bottom - 14, "L3 HELP", 2);
}

// Writes "[showcase] fps N.N res WxH" to stdout (unbuffered).
void LogFps(float fps, float render_ms, bool dynamic)
{
    char line[96];
    char *out = AppendText(line, "[showcase] fps ");
    out = AppendNumber(out, fps, true);
    out = AppendText(out, " res ");
    out = AppendNumber(out, static_cast<float>(g_state.width), false);
    out = AppendText(out, "x");
    out = AppendNumber(out, static_cast<float>(g_state.height), false);
    out = AppendText(out, " cpu_ms ");
    out = AppendNumber(out, render_ms, true);
    out = AppendText(out, dynamic ? " dyn" : " fixed");
    out = AppendText(out, g_state.antialias ? " aa4" : "");
    *out++ = '\n';
    (void)write(1, line, static_cast<std::size_t>(out - line));
}

[[noreturn]] void Halt()
{
    for (;;)
        (void)sceKernelUsleep(1000000);
}
} // namespace

int main(int argc, char **argv)
{
    const bool smoke = argc == 2 && std::strcmp(argv[1], "--smoke-test") == 0;
    if (argc > 1 && !smoke)
    {
        constexpr char message[] = "Usage: eboot [--smoke-test]\n";
        (void)write(2, message, sizeof(message) - 1);
        return 2;
    }
    (void)sceSystemServiceHideSplashScreen();
    const int video = sceVideoOutOpen(0xff, 0, 0, nullptr);
    if (video < 0)
        Halt();
    std::int64_t physical = 0;
    if (sceKernelAllocateDirectMemory(0, static_cast<std::int64_t>(sceKernelGetDirectMemorySize()),
                                      kMemoryBytes, kMemoryAlignment, kMemoryTypeWcGarlic,
                                      &physical) < 0)
        Halt();
    void *mapped = nullptr;
    if (sceKernelMapDirectMemory(&mapped, kMemoryBytes, kMapProtection, 0, physical,
                                 kMemoryAlignment) < 0)
        Halt();
    std::uint32_t *frames[2] = {
        static_cast<std::uint32_t *>(mapped),
        reinterpret_cast<std::uint32_t *>(static_cast<std::uint8_t *>(mapped) + kFrameBytes)};
    VideoBuffer buffers[2] = {{frames[0], nullptr, nullptr, nullptr},
                              {frames[1], nullptr, nullptr, nullptr}};
    VideoAttribute attribute{};
    (void)sceVideoOutSetFlipRate(video, 0);
    // Tiling mode 1 = linear: the CPU writes plain rows (0 would be GPU tiling).
    sceVideoOutSetBufferAttribute2(&attribute, kPixelFormatRgba8Srgb, kTilingLinear, kWidth,
                                   kHeight, 0, 0, 0);
    if (sceVideoOutRegisterBuffers2(video, 0, 0, buffers, 2, &attribute, 0, nullptr) < 0)
        Halt();

    std::int32_t pad = -1;
    std::int32_t user = -1;
    if (!smoke && sceUserServiceInitialize(nullptr) >= 0 &&
        sceUserServiceGetInitialUser(&user) >= 0 &&
        scePadInit() >= 0)
        pad = scePadOpen(user, 0, 0, nullptr);
    if (pad >= 0)
        (void)scePadSetMotionSensorState(pad, true);
    if (user >= 0 && sceAudioOutInit() >= 0)
    {
        const int audio = sceAudioOutOpen(user, 0, 0, kAudioFrames, kSampleRate, kAudioStereoS16);
        if (audio > 0)
        {
            void *thread = nullptr;
            (void)scePthreadCreate(&thread, nullptr, Music,
                                   reinterpret_cast<void *>(static_cast<long>(audio)), "music");
        }
    }

    void *workers[kWorkers]{};
    for (int i = 0; i < kWorkers; ++i)
    {
        if (scePthreadCreate(&workers[i], nullptr, Worker, nullptr, "raymarch") != 0)
            workers[i] = nullptr;
    }

    DetectPlatform();
    Controls controls;
    if (smoke)
        controls.dynamic = false;
    bool smoke_ok = true;
    const std::uint64_t start = sceKernelGetProcessTime();
    std::uint64_t last = start;
    float fps = 0.0f;
    float render_ms = 0.0f;
    for (int frame = 0; !smoke || frame < 180; ++frame)
    {
        const std::uint64_t now = sceKernelGetProcessTime();
        const float dt = Clamp(static_cast<float>(now - last) * 1e-6f, 0.0f, 0.1f);
        last = now;
        if (smoke)
            controls.scene = frame < 90 ? 0 : 1;
        if (dt > 0.0f)
            fps = fps == 0.0f ? 1.0f / dt : Mix(fps, 1.0f / dt, 0.1f);
        PadState state{};
        if (ReadPad(pad, &state))
            UpdateControls(&controls, state, dt);
        UpdateGame(&controls, static_cast<float>(now - start) * 1e-6f, dt, pad);
        controls.scene_time += dt;
        if (controls.scene_time > 30.0f)
        {
            controls.scene ^= 1;
            controls.scene_time = 0.0f;
        }
        const int index = frame & 1;
        controls.help_time -= dt;
        g_help = controls.help_time > 0.0f;
        g_dynamic = controls.dynamic;
        g_hits = controls.hits;
        g_motion = controls.motion;
        g_rumble = controls.rumble;
        SetupFrame(controls, static_cast<float>(now - start) * 1e-6f, frames[index]);
        LayoutPanels(fps, render_ms);
        const std::uint64_t render_start = sceKernelGetProcessTime();
        RenderFrame();
        const float frame_ms =
            static_cast<float>(sceKernelGetProcessTime() - render_start) * 1e-3f;
        render_ms = render_ms == 0.0f ? frame_ms : Mix(render_ms, frame_ms, 0.1f);
        UpdateDynamicResolution(&controls, frame_ms);
        DrawOverlay(frames[index], fps, render_ms);
        __asm__ volatile("sfence" ::: "memory");
        const int flip_result = sceVideoOutSubmitFlip(video, index, 1, frame);
        if (frame % 60 == 59)
            LogFps(fps, render_ms, controls.dynamic);
        const int wait_result = sceVideoOutWaitVblank(video);
        if (smoke && (flip_result != 0 || wait_result != 0))
        {
            smoke_ok = false;
            break;
        }
    }
    g_stop_workers.store(true, std::memory_order_release);
    for (void *thread : workers)
        if (thread != nullptr && scePthreadJoin(thread, nullptr) != 0)
            smoke_ok = false;
    if (sceVideoOutClose(video) != 0)
        smoke_ok = false;
    if (!smoke_ok)
        return 1;
    constexpr char complete[] = "[showcase-smoke] complete frames=180 scenes=2\n";
    (void)write(1, complete, sizeof(complete) - 1);
    return 0;
}
