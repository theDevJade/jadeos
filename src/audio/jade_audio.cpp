// Emscripten-only in meson; general-purpose stereo PCM ring for WASM Web Audio.

#include "jade_audio.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

std::vector<std::int16_t> g_ring;  // capacity_frames * 2 interleaved
std::size_t               g_head  = 0;  // read index
std::size_t               g_tail  = 0;  // write index
std::size_t               g_count = 0;  // stereo frames queued
std::size_t               g_cap   = 0;  // max stereo frames
std::uint32_t             g_rate  = 48000;

}  // namespace

extern "C" void jade_pcm_submit_stereo_i16(const std::int16_t* interleaved,
                                           std::uint32_t frames) noexcept
{
    jade::audio::submit_stereo_i16(interleaved, frames);
}

namespace jade::audio {

void init(std::uint32_t sample_rate_hz, std::size_t ring_seconds)
{
    shutdown();
    g_rate = sample_rate_hz ? sample_rate_hz : 48000;
    g_cap  = std::max<std::size_t>(g_rate * ring_seconds, 4096);
    g_ring.assign(g_cap * 2u, 0);
    g_head = g_tail = g_count = 0;
}

void shutdown() noexcept
{
    g_ring.clear();
    g_head = g_tail = g_count = g_cap = 0;
}

void submit_stereo_i16(const std::int16_t* interleaved, std::uint32_t frames) noexcept
{
    if (!interleaved || frames == 0 || g_cap == 0)
        return;
    for (std::uint32_t f = 0; f < frames; ++f) {
        if (g_count >= g_cap) {
            // Drop oldest frame.
            g_head = (g_head + 1u) % g_cap;
            --g_count;
        }
        const std::size_t i0 = g_tail * 2u;
        g_ring[i0]     = interleaved[f * 2u];
        g_ring[i0 + 1] = interleaved[f * 2u + 1];
        g_tail         = (g_tail + 1u) % g_cap;
        ++g_count;
    }
}

std::uint32_t dequeue_stereo_i16(std::int16_t* interleaved_out,
                                 std::uint32_t max_frames) noexcept
{
    if (!interleaved_out || max_frames == 0 || g_cap == 0)
        return 0;
    const std::uint32_t n = static_cast<std::uint32_t>(
        std::min<std::size_t>(g_count, max_frames));
    for (std::uint32_t f = 0; f < n; ++f) {
        const std::size_t i0 = g_head * 2u;
        interleaved_out[f * 2u]     = g_ring[i0];
        interleaved_out[f * 2u + 1] = g_ring[i0 + 1];
        g_head                        = (g_head + 1u) % g_cap;
    }
    g_count -= n;
    return n;
}

std::uint32_t sample_rate() noexcept
{
    return g_rate;
}

std::uint32_t queued_stereo_frames() noexcept
{
    return static_cast<std::uint32_t>(g_count);
}

}  // namespace jade::audio
