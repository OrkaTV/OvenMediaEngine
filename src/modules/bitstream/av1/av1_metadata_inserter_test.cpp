//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#include <gtest/gtest.h>

#include "av1_metadata_inserter.h"
#include "av1_parser.h"

#include <cstring>

TEST(Av1MetadataInserter, InsertsItuTT35BeforeFrame)
{
	const uint8_t obu_stream[] = {
		0x0A, 0x01, 0x00,        // OBU_SEQUENCE_HEADER
		0x36, 0x48, 0x01, 0x00  // OBU_FRAME, temporal id 2, spatial id 1
	};
	const uint8_t a53_payload[] = {
		0xB5, 0x00, 0x31, 'G', 'A', '9', '4', 0x03,
		0x41, 0x00, 0xFC, 0x80, 0x80, 0xFF
	};
	auto packet = std::make_shared<MediaPacket>(
		cmn::MediaType::Video, 1,
		std::make_shared<ov::Data>(obu_stream, sizeof(obu_stream)),
		9000, 9000, 3000, MediaPacketFlag::Key,
		cmn::BitstreamFormat::AV1_OBU, cmn::PacketType::RAW);

	ASSERT_TRUE(Av1MetadataInserter::InsertItuTT35(
		packet, std::make_shared<ov::Data>(a53_payload, sizeof(a53_payload))));

	const auto *buffer = packet->GetData()->GetDataAs<uint8_t>();
	const auto length = packet->GetData()->GetLength();
	Av1ObuSpan sequence;
	Av1ObuSpan metadata;
	Av1ObuSpan frame;
	ASSERT_TRUE(Av1Parser::ReadObu(buffer, length, 0, sequence));
	ASSERT_TRUE(Av1Parser::ReadObu(buffer, length, sequence.next_offset, metadata));
	ASSERT_TRUE(Av1Parser::ReadObu(buffer, length, metadata.next_offset, frame));
	EXPECT_EQ(sequence.header.type, Av1ObuType::SequenceHeader);
	EXPECT_EQ(metadata.header.type, Av1ObuType::Metadata);
	EXPECT_TRUE(metadata.header.extension_flag);
	EXPECT_EQ(metadata.header.temporal_id, 2);
	EXPECT_EQ(metadata.header.spatial_id, 1);
	EXPECT_EQ(frame.header.type, Av1ObuType::Frame);
	ASSERT_EQ(metadata.payload_size, sizeof(a53_payload) + 1);
	EXPECT_EQ(buffer[metadata.payload_offset], 4);
	EXPECT_EQ(::memcmp(buffer + metadata.payload_offset + 1,
					   a53_payload, sizeof(a53_payload)), 0);
}
