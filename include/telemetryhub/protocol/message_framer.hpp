#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace telemetryhub::protocol {

inline constexpr std::size_t frame_header_size_bytes = 4U;
inline constexpr std::size_t max_payload_size_bytes = 64U * 1024U;

enum class FramingError
{
    zero_length_payload,
    payload_too_large,
    already_failed
};

struct FrameEncodingResult
{
    std::vector<std::byte> frame;
    std::optional<FramingError> error;
};

struct MessageFramerResult
{
    std::vector<std::vector<std::byte>> payloads;
    std::optional<FramingError> error;
};

// Encodes a nonempty payload as a four-byte big-endian length followed by the
// payload. Invalid lengths are reported through error; allocation failures
// remain exceptional.
[[nodiscard]] FrameEncodingResult encode_frame(std::span<const std::byte> payload);

// Incrementally reconstructs payloads from arbitrary input chunks. An empty
// result without an error means more input is needed. A fatal framing error
// discards the partial frame, preserves any frames completed earlier in that
// call, and permanently fails this framer; later calls return already_failed
// without consuming input.
class MessageFramer
{
public:
    [[nodiscard]] MessageFramerResult consume(std::span<const std::byte> input);

private:
    void discard_partial_frame() noexcept;

    std::array<std::byte, frame_header_size_bytes> header_{};
    std::size_t header_bytes_received_{};
    std::size_t expected_payload_size_{};
    std::vector<std::byte> payload_;
    std::optional<FramingError> fatal_error_;
};

} // namespace telemetryhub::protocol
