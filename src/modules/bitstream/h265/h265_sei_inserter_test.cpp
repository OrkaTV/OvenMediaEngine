//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#include <gtest/gtest.h>

#include <modules/bitstream/nalu/nal_unit_fragment_header.h>
#include <modules/bitstream/nalu/nal_unit_insertor.h>

#include "h265_sei_inserter.h"
#include "h265_types.h"

#include <cstring>

TEST(H265SeiInserter, InsertsRegisteredA53UserDataBeforeVcl)
{
	const uint8_t access_unit[] = {
		0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0xAA,  // VPS
		0x00, 0x00, 0x00, 0x01, 0x26, 0x8B, 0xBB   // IDR_W_RADL, layer 17, temporal id 2
	};
	const uint8_t a53_payload[] = {
		0xB5, 0x00, 0x31, 'G', 'A', '9', '4', 0x03,
		0x41, 0x00, 0xFC, 0x80, 0x80, 0xFF
	};
	auto packet = std::make_shared<MediaPacket>(
		cmn::MediaType::Video, 1,
		std::make_shared<ov::Data>(access_unit, sizeof(access_unit)),
		9000, 9000, 3000, MediaPacketFlag::Key,
		cmn::BitstreamFormat::H265_ANNEXB, cmn::PacketType::NALU);

	ASSERT_TRUE(H265SeiInserter::InsertRegisteredUserData(
		packet, std::make_shared<ov::Data>(a53_payload, sizeof(a53_payload))));

	NalUnitFragmentHeader fragments;
	ASSERT_TRUE(NalUnitFragmentHeader::Parse(packet->GetData(), fragments));
	const auto *fragment = fragments.GetFragmentHeader();
	ASSERT_NE(fragment, nullptr);
	ASSERT_EQ(fragment->GetCount(), 3U);

	auto sei_fragment = fragment->GetFragment(1);
	ASSERT_TRUE(sei_fragment.has_value());
	const auto offset = std::get<0>(sei_fragment.value());
	const auto length = std::get<1>(sei_fragment.value());
	const auto *buffer = packet->GetData()->GetDataAs<uint8_t>();
	EXPECT_EQ((buffer[offset] >> 1) & 0x3F,
			  static_cast<uint8_t>(H265NALUnitType::PREFIX_SEI_NUT));
	EXPECT_EQ(buffer[offset] & 0x01, 0);
	EXPECT_EQ(buffer[offset + 1], 0x8B);

	auto sei = NalUnitInsertor::RemoveEmulationPreventionBytes(
		std::make_shared<ov::Data>(buffer + offset, length));
	ASSERT_NE(sei, nullptr);
	ASSERT_EQ(sei->GetLength(), sizeof(a53_payload) + 5);
	const auto *bytes = sei->GetDataAs<uint8_t>();
	EXPECT_EQ(bytes[2], 4);
	EXPECT_EQ(bytes[3], sizeof(a53_payload));
	EXPECT_EQ(::memcmp(bytes + 4, a53_payload, sizeof(a53_payload)), 0);
	EXPECT_EQ(bytes[4 + sizeof(a53_payload)], 0x80);
}
