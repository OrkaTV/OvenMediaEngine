//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#include "av1_metadata_inserter.h"

#include "av1_parser.h"

#define OV_LOG_TAG "Av1MetadataInserter"

bool Av1MetadataInserter::InsertItuTT35(const std::shared_ptr<MediaPacket> &packet,
										const std::shared_ptr<const ov::Data> &payload)
{
	if (packet == nullptr || packet->GetData() == nullptr || payload == nullptr ||
		payload->GetLength() == 0 ||
		packet->GetBitstreamFormat() != cmn::BitstreamFormat::AV1_OBU)
	{
		return false;
	}

	const auto *buffer = packet->GetData()->GetDataAs<uint8_t>();
	const size_t length = packet->GetData()->GetLength();
	size_t offset = 0;
	size_t insert_offset = SIZE_MAX;
	Av1ObuHeader target_header;
	while (offset < length)
	{
		Av1ObuSpan obu;
		if (Av1Parser::ReadObu(buffer, length, offset, obu) == false ||
			obu.next_offset <= offset)
		{
			return false;
		}

		if (obu.header.type == Av1ObuType::FrameHeader ||
			obu.header.type == Av1ObuType::Frame ||
			obu.header.type == Av1ObuType::RedundantFrameHeader)
		{
			insert_offset = obu.obu_offset;
			target_header = obu.header;
			break;
		}

		offset = obu.next_offset;
	}

	if (insert_offset == SIZE_MAX)
	{
		return false;
	}

	constexpr uint8_t kMetadataTypeItuTT35 = 4;
	const size_t metadata_payload_size = 1 + payload->GetLength();
	uint8_t size_field[Av1Parser::LEB128_MAX_SIZE] = {0};
	const size_t size_field_length =
		Av1Parser::EncodeLeb128(metadata_payload_size, size_field);
	if (size_field_length == 0)
	{
		return false;
	}

	auto metadata_obu = std::make_shared<ov::Data>();
	// forbidden(0), type OBU_METADATA(5), frame-matched extension, has_size_field(1), reserved(0).
	const uint8_t obu_header =
		(static_cast<uint8_t>(Av1ObuType::Metadata) << 3) |
		(target_header.extension_flag ? 0x04 : 0x00) | 0x02;
	metadata_obu->Append(&obu_header, sizeof(obu_header));
	if (target_header.extension_flag)
	{
		const uint8_t extension_header =
			(target_header.temporal_id << 5) | (target_header.spatial_id << 3);
		metadata_obu->Append(&extension_header, sizeof(extension_header));
	}
	metadata_obu->Append(size_field, size_field_length);
	metadata_obu->Append(&kMetadataTypeItuTT35, sizeof(kMetadataTypeItuTT35));
	metadata_obu->Append(payload);

	auto data = std::make_shared<ov::Data>(length + metadata_obu->GetLength());
	data->Append(buffer, insert_offset);
	data->Append(metadata_obu);
	data->Append(buffer + insert_offset, length - insert_offset);
	packet->SetData(data);
	return true;
}
