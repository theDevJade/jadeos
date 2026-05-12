#pragma once

#include <cstddef>
#include <cstdint>

namespace jade::audio {

// Host-side stereo PCM ring (interleaved int16 L,R,), 48 kHz by default.
void init(std::uint32_t sample_rate_hz = 48000, std::size_t ring_seconds = 3);
void shutdown() noexcept;

// Push decoded / mixed stereo S16 PCM into the ring (drops oldest on overflow).
void submit_stereo_i16(const std::int16_t* interleaved, std::uint32_t frames) noexcept;

// Copy up to max_frames stereo frames out; returns frames actually copied.
[[nodiscard]] std::uint32_t dequeue_stereo_i16(std::int16_t* interleaved_out,
                                               std::uint32_t max_frames) noexcept;

[[nodiscard]] std::uint32_t sample_rate() noexcept;

// Stereo frames currently waiting in the ring (for pacing decoders).
[[nodiscard]] std::uint32_t queued_stereo_frames() noexcept;

}  // namespace jade::audio

extern "C" void          jade_pcm_submit_stereo_i16(const std::int16_t* interleaved,
                                                    std::uint32_t frames) noexcept;
extern "C" std::uint32_t jade_pcm_queued_frames(void) noexcept;
extern "C" std::uint32_t jade_pcm_sample_rate(void) noexcept;
