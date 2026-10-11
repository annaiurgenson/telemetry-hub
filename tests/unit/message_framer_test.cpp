#include <telemetryhub/protocol/message_framer.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace {

using telemetryhub::protocol::FramingError;
using telemetryhub::protocol::MessageFramer;
using telemetryhub::protocol::encode_frame;
using telemetryhub::protocol::frame_header_size_bytes;
using telemetryhub::protocol::max_payload_size_bytes;

[[nodiscard]] std::vector<std::byte> bytes_from(const std::initializer_list<unsigned int> values)
{
    std::vector<std::byte> bytes;
    bytes.reserve(values.size());
    for (const unsigned int value : values)
    {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

[[nodiscard]] std::vector<std::byte> make_header(const std::uint32_t length)
{
    return {
        static_cast<std::byte>((length >> 24U) & 0xffU),
        static_cast<std::byte>((length >> 16U) & 0xffU),
        static_cast<std::byte>((length >> 8U) & 0xffU),
        static_cast<std::byte>(length & 0xffU)};
}

TEST(MessageFramer, ReconstructsCompleteSingleFrame)
{
    const auto payload = bytes_from({0x54U, 0x48U, 0x01U});
    const auto encoded = encode_frame(payload);
    ASSERT_FALSE(encoded.error.has_value());
    MessageFramer framer;

    const auto result = framer.consume(encoded.frame);

    ASSERT_FALSE(result.error.has_value());
    ASSERT_EQ(result.payloads.size(), 1U);
    EXPECT_EQ(result.payloads.front(), payload);
}

TEST(MessageFramer, AcceptsLengthPrefixSplitAtEveryBoundary)
{
    const auto payload = bytes_from({0x11U, 0x22U, 0x33U});
    const auto encoded = encode_frame(payload);
    ASSERT_FALSE(encoded.error.has_value());

    for (std::size_t split = 1U; split < frame_header_size_bytes; ++split)
    {
        MessageFramer framer;
        const auto first = framer.consume(
            std::span<const std::byte>{encoded.frame}.first(split));
        EXPECT_FALSE(first.error.has_value()) << "split=" << split;
        EXPECT_TRUE(first.payloads.empty()) << "split=" << split;

        const auto second = framer.consume(
            std::span<const std::byte>{encoded.frame}.subspan(split));
        ASSERT_FALSE(second.error.has_value()) << "split=" << split;
        ASSERT_EQ(second.payloads.size(), 1U) << "split=" << split;
        EXPECT_EQ(second.payloads.front(), payload) << "split=" << split;
    }
}

TEST(MessageFramer, ReconstructsPayloadFragmentedAcrossInputs)
{
    const auto payload = bytes_from({0x00U, 0xffU, 0x7fU, 0x42U});
    const auto encoded = encode_frame(payload);
    ASSERT_FALSE(encoded.error.has_value());
    MessageFramer framer;

    const auto first = framer.consume(
        std::span<const std::byte>{encoded.frame}.first(frame_header_size_bytes + 1U));
    EXPECT_FALSE(first.error.has_value());
    EXPECT_TRUE(first.payloads.empty());

    const auto second = framer.consume(
        std::span<const std::byte>{encoded.frame}.subspan(frame_header_size_bytes + 1U, 2U));
    EXPECT_FALSE(second.error.has_value());
    EXPECT_TRUE(second.payloads.empty());

    const auto third = framer.consume(
        std::span<const std::byte>{encoded.frame}.last(1U));
    ASSERT_FALSE(third.error.has_value());
    ASSERT_EQ(third.payloads.size(), 1U);
    EXPECT_EQ(third.payloads.front(), payload);
}

TEST(MessageFramer, EmitsMultipleFramesFromOneInput)
{
    const auto first_payload = bytes_from({0x01U, 0x02U});
    const auto second_payload = bytes_from({0x03U});
    const auto first_frame = encode_frame(first_payload);
    const auto second_frame = encode_frame(second_payload);
    ASSERT_FALSE(first_frame.error.has_value());
    ASSERT_FALSE(second_frame.error.has_value());
    std::vector<std::byte> input = first_frame.frame;
    input.insert(input.end(), second_frame.frame.begin(), second_frame.frame.end());
    MessageFramer framer;

    const auto result = framer.consume(input);

    ASSERT_FALSE(result.error.has_value());
    ASSERT_EQ(result.payloads.size(), 2U);
    EXPECT_EQ(result.payloads[0], first_payload);
    EXPECT_EQ(result.payloads[1], second_payload);
}

TEST(MessageFramer, RetainsIncompleteFrameAfterCompleteFrame)
{
    const auto first_payload = bytes_from({0x31U});
    const auto second_payload = bytes_from({0x41U, 0x42U, 0x43U});
    const auto first_frame = encode_frame(first_payload);
    const auto second_frame = encode_frame(second_payload);
    ASSERT_FALSE(first_frame.error.has_value());
    ASSERT_FALSE(second_frame.error.has_value());
    std::vector<std::byte> input = first_frame.frame;
    input.insert(input.end(), second_frame.frame.begin(), second_frame.frame.begin() + 6);
    MessageFramer framer;

    const auto first = framer.consume(input);
    ASSERT_FALSE(first.error.has_value());
    ASSERT_EQ(first.payloads.size(), 1U);
    EXPECT_EQ(first.payloads.front(), first_payload);

    const auto second = framer.consume(
        std::span<const std::byte>{second_frame.frame}.subspan(6U));
    ASSERT_FALSE(second.error.has_value());
    ASSERT_EQ(second.payloads.size(), 1U);
    EXPECT_EQ(second.payloads.front(), second_payload);
}

TEST(MessageFramer, AcceptsMaximumPayloadSize)
{
    std::vector<std::byte> payload(max_payload_size_bytes, std::byte{0xa5U});
    const auto encoded = encode_frame(payload);
    ASSERT_FALSE(encoded.error.has_value());
    EXPECT_EQ(encoded.frame.size(), frame_header_size_bytes + max_payload_size_bytes);
    MessageFramer framer;

    const auto result = framer.consume(encoded.frame);

    ASSERT_FALSE(result.error.has_value());
    ASSERT_EQ(result.payloads.size(), 1U);
    EXPECT_EQ(result.payloads.front(), payload);
}

TEST(MessageFramer, RejectsPayloadOneByteOverMaximum)
{
    const auto header = make_header(static_cast<std::uint32_t>(max_payload_size_bytes + 1U));
    MessageFramer framer;

    const auto result = framer.consume(header);

    EXPECT_EQ(result.error, FramingError::payload_too_large);
    EXPECT_TRUE(result.payloads.empty());
}

TEST(MessageFramer, RejectsZeroLengthPayload)
{
    const auto header = make_header(0U);
    MessageFramer framer;

    const auto result = framer.consume(header);

    EXPECT_EQ(result.error, FramingError::zero_length_payload);
    EXPECT_TRUE(result.payloads.empty());
}

TEST(MessageFramer, UsesFourByteBigEndianLengthForEncodingAndDecoding)
{
    std::vector<std::byte> payload(0x0102U, std::byte{0x5aU});
    const auto encoded = encode_frame(payload);
    ASSERT_FALSE(encoded.error.has_value());
    ASSERT_EQ(encoded.frame.size(), frame_header_size_bytes + payload.size());
    EXPECT_EQ(encoded.frame[0], std::byte{0x00U});
    EXPECT_EQ(encoded.frame[1], std::byte{0x00U});
    EXPECT_EQ(encoded.frame[2], std::byte{0x01U});
    EXPECT_EQ(encoded.frame[3], std::byte{0x02U});
    MessageFramer framer;

    const auto result = framer.consume(encoded.frame);

    ASSERT_FALSE(result.error.has_value());
    ASSERT_EQ(result.payloads.size(), 1U);
    EXPECT_EQ(result.payloads.front(), payload);
}

TEST(MessageFramer, PreservesArbitraryBinaryPayloadBytes)
{
    const auto payload = bytes_from({0x00U, 0x80U, 0xffU, 0x00U, 0x7fU});
    const auto encoded = encode_frame(payload);
    ASSERT_FALSE(encoded.error.has_value());
    MessageFramer framer;

    const auto result = framer.consume(encoded.frame);

    ASSERT_EQ(result.payloads.size(), 1U);
    EXPECT_EQ(result.payloads.front(), payload);
}

TEST(MessageFramer, DoesNotEmitIncompleteInputPrematurely)
{
    const auto payload = bytes_from({0x01U, 0x02U, 0x03U});
    const auto encoded = encode_frame(payload);
    ASSERT_FALSE(encoded.error.has_value());
    MessageFramer framer;

    const auto result = framer.consume(
        std::span<const std::byte>{encoded.frame}.first(encoded.frame.size() - 1U));

    EXPECT_FALSE(result.error.has_value());
    EXPECT_TRUE(result.payloads.empty());
}

TEST(MessageFramer, EncoderRejectsInvalidPayloadSizes)
{
    EXPECT_EQ(encode_frame(std::span<const std::byte>{}).error,
        FramingError::zero_length_payload);
    const std::vector<std::byte> oversized(max_payload_size_bytes + 1U);
    EXPECT_EQ(encode_frame(oversized).error, FramingError::payload_too_large);
}

TEST(MessageFramer, FatalErrorClearsPartialStateAndPoisonsFramer)
{
    const auto partial_header = bytes_from({0x00U, 0x00U});
    MessageFramer framer;
    const auto partial = framer.consume(partial_header);
    EXPECT_FALSE(partial.error.has_value());
    EXPECT_TRUE(partial.payloads.empty());

    const auto invalid_tail = bytes_from({0x00U, 0x00U, 0x00U, 0x00U});
    const auto failure = framer.consume(invalid_tail);
    EXPECT_EQ(failure.error, FramingError::zero_length_payload);
    EXPECT_TRUE(failure.payloads.empty());

    const auto after_failure = framer.consume(
        bytes_from({0x00U, 0x00U, 0x00U, 0x01U, 0x7fU}));
    EXPECT_EQ(after_failure.error, FramingError::already_failed);
    EXPECT_TRUE(after_failure.payloads.empty());
}

TEST(MessageFramer, KeepsEarlierFramesWhenLaterFrameHasFatalLength)
{
    const auto payload = bytes_from({0x44U});
    const auto frame = encode_frame(payload);
    ASSERT_FALSE(frame.error.has_value());
    std::vector<std::byte> input = frame.frame;
    const auto invalid_header = make_header(0U);
    input.insert(input.end(), invalid_header.begin(), invalid_header.end());
    MessageFramer framer;

    const auto result = framer.consume(input);

    EXPECT_EQ(result.error, FramingError::zero_length_payload);
    ASSERT_EQ(result.payloads.size(), 1U);
    EXPECT_EQ(result.payloads.front(), payload);
}

} // namespace
