#include <gtest/gtest.h>

#include <base/mediarouter/media_buffer.h>
#include <modules/ovt_packetizer/ovt_depacketizer.h>
#include <modules/ovt_packetizer/ovt_packetizer.h>

TEST(OvtMediaPacketTest, PreservesCaptionSeiAndPacketType)
{
	const uint8_t access_unit[] = {
		0x00, 0x00, 0x00, 0x01,
		0x06, 0x04, 0x0B,
		0xB5, 0x00, 0x31, 'G', 'A', '9', '4', 0x03,
		0x41, 0x00, 0xFC, 0x80, 0x80, 0xFF, 0x80,
		0x00, 0x00, 0x00, 0x01,
		0x65, 0x88, 0x84
	};
	auto input = std::make_shared<MediaPacket>(
		cmn::MediaType::Video,
		7,
		std::make_shared<ov::Data>(access_unit, sizeof(access_unit)),
		9000,
		9000,
		3000,
		MediaPacketFlag::Key,
		cmn::BitstreamFormat::H264_ANNEXB,
		cmn::PacketType::NALU);

	OvtPacketizer packetizer;
	ASSERT_TRUE(packetizer.PacketizeMediaPacket(input->GetPts(), input));

	OvtDepacketizer depacketizer;
	while (packetizer.IsAvailablePackets())
	{
		auto packet = packetizer.PopPacket();
		ASSERT_NE(packet, nullptr);
		ASSERT_TRUE(depacketizer.AppendPacket(packet->GetData()));
	}

	ASSERT_TRUE(depacketizer.IsAvailableMediaPacket());
	auto output = depacketizer.PopMediaPacket();
	ASSERT_NE(output, nullptr);
	EXPECT_EQ(output->GetPacketType(), cmn::PacketType::NALU);
	EXPECT_EQ(output->GetBitstreamFormat(), cmn::BitstreamFormat::H264_ANNEXB);
	ASSERT_EQ(output->GetDataLength(), sizeof(access_unit));
	EXPECT_EQ(::memcmp(output->GetData()->GetData(), access_unit, sizeof(access_unit)), 0);
}
