#include <telemetryhub/protocol/message_framer.hpp>

#include <algorithm>
#include <utility>

namespace telemetryhub::protocol {
namespace {

[[nodiscard]] std::uint32_t decode_payload_size(
    const std::array<std::byte, frame_header_size_bytes>& header) noexcept
{
    return (std::to_integer<std::uint32_t>(header[0]) << 24U) |
        (std::to_integer<std::uint32_t>(header[1]) << 16U) |
        (std::to_integer<std::uint32_t>(header[2]) << 8U) |
        std::to_integer<std::uint32_t>(header[3]);
}

} // namespace

FrameEncodingResult encode_frame(const std::span<const std::byte> payload)
{
    if (payload.empty())
    {
        return {{}, FramingError::zero_length_payload};
    }
    if (payload.size() > max_payload_size_bytes)
    {
        return {{}, FramingError::payload_too_large};
    }

    const auto payload_size = static_cast<std::uint32_t>(payload.size());
    std::vector<std::byte> frame;
    frame.reserve(frame_header_size_bytes + payload.size());
    frame.push_back(static_cast<std::byte>((payload_size >> 24U) & 0xffU));
    frame.push_back(static_cast<std::byte>((payload_size >> 16U) & 0xffU));
    frame.push_back(static_cast<std::byte>((payload_size >> 8U) & 0xffU));
    frame.push_back(static_cast<std::byte>(payload_size & 0xffU));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return {std::move(frame), std::nullopt};
}

MessageFramerResult MessageFramer::consume(const std::span<const std::byte> input)
{
    MessageFramerResult result;
    if (fatal_error_.has_value())
    {
        result.error = FramingError::already_failed;
        return result;
    }

    std::size_t input_offset = 0U;
    while (input_offset < input.size())
    {
        if (expected_payload_size_ == 0U)
        {
            const std::size_t header_bytes_to_copy = std::min(
                frame_header_size_bytes - header_bytes_received_, input.size() - input_offset);
            std::copy_n(
                input.begin() + static_cast<std::ptrdiff_t>(input_offset),
                header_bytes_to_copy,
                header_.begin() + static_cast<std::ptrdiff_t>(header_bytes_received_));
            input_offset += header_bytes_to_copy;
            header_bytes_received_ += header_bytes_to_copy;
            if (header_bytes_received_ != frame_header_size_bytes)
            {
                continue;
            }

            const std::uint32_t decoded_size = decode_payload_size(header_);
            if (decoded_size == 0U)
            {
                fatal_error_ = FramingError::zero_length_payload;
            }
            else if (decoded_size > max_payload_size_bytes)
            {
                fatal_error_ = FramingError::payload_too_large;
            }

            if (fatal_error_.has_value())
            {
                discard_partial_frame();
                result.error = fatal_error_;
                return result;
            }

            expected_payload_size_ = static_cast<std::size_t>(decoded_size);
            payload_.clear();
            payload_.reserve(expected_payload_size_);
        }

        const std::size_t payload_bytes_to_copy = std::min(
            expected_payload_size_ - payload_.size(), input.size() - input_offset);
        const auto payload_begin =
            input.begin() + static_cast<std::ptrdiff_t>(input_offset);
        const auto payload_end =
            payload_begin + static_cast<std::ptrdiff_t>(payload_bytes_to_copy);
        payload_.insert(payload_.end(), payload_begin, payload_end);
        input_offset += payload_bytes_to_copy;

        if (payload_.size() == expected_payload_size_)
        {
            result.payloads.push_back(std::move(payload_));
            payload_.clear();
            expected_payload_size_ = 0U;
            header_bytes_received_ = 0U;
        }
    }

    return result;
}

void MessageFramer::discard_partial_frame() noexcept
{
    header_bytes_received_ = 0U;
    expected_payload_size_ = 0U;
    payload_.clear();
}

} // namespace telemetryhub::protocol
