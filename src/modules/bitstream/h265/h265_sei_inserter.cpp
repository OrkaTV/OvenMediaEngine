//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#include "h265_sei_inserter.h"

#include <modules/bitstream/nalu/nal_unit_fragment_header.h>
#include <modules/bitstream/nalu/nal_unit_insertor.h>

#include "h265_types.h"

#define OV_LOG_TAG "H265SeiInserter"

namespace
{
	void AppendExtendedValue(const std::shared_ptr<ov::Data> &data, size_t value)
	{
		const uint8_t extension = 0xFF;
		while (value >= extension)
		{
			data->Append(&extension, sizeof(extension));
			value -= extension;
		}

		const uint8_t remainder = static_cast<uint8_t>(value);
		data->Append(&remainder, sizeof(remainder));
	}
}  // namespace

bool H265SeiInserter::InsertRegisteredUserData(const std::shared_ptr<MediaPacket> &packet,
												const std::shared_ptr<const ov::Data> &payload)
{
	if (packet == nullptr || packet->GetData() == nullptr || payload == nullptr ||
		payload->GetLength() == 0 ||
		packet->GetBitstreamFormat() != cmn::BitstreamFormat::H265_ANNEXB)
	{
		return false;
	}

	NalUnitFragmentHeader fragment_header;
	if (NalUnitFragmentHeader::Parse(packet->GetData(), fragment_header) == false)
	{
		return false;
	}

	const auto *fragment = fragment_header.GetFragmentHeader();
	if (fragment == nullptr || fragment->GetCount() == 0)
	{
		return false;
	}

	size_t nal_index = NalUnitInsertor::APPEND_TO_END;
	uint8_t target_header[2] = {0x00, 0x01};
	const auto *buffer = packet->GetData()->GetDataAs<uint8_t>();
	for (size_t i = 0; i < fragment->GetCount(); i++)
	{
		auto nal = fragment->GetFragment(i);
		if (nal.has_value() == false || std::get<1>(nal.value()) < 2)
		{
			continue;
		}

		const auto nal_type =
			static_cast<H265NALUnitType>((buffer[std::get<0>(nal.value())] >> 1) & 0x3F);
		if (static_cast<uint8_t>(nal_type) < 32)
		{
			nal_index = i;
			target_header[0] = buffer[std::get<0>(nal.value())];
			target_header[1] = buffer[std::get<0>(nal.value()) + 1];
			break;
		}
	}

	// Preserve the coded picture's nuh_layer_id and nuh_temporal_id_plus1.
	const uint8_t prefix_sei_header[] = {
		static_cast<uint8_t>(
			(static_cast<uint8_t>(H265NALUnitType::PREFIX_SEI_NUT) << 1) |
			(target_header[0] & 0x01)),
		target_header[1]
	};
	auto sei_nal = std::make_shared<ov::Data>();
	sei_nal->Append(prefix_sei_header, sizeof(prefix_sei_header));
	AppendExtendedValue(sei_nal, 4);  // user_data_registered_itu_t_t35
	AppendExtendedValue(sei_nal, payload->GetLength());
	sei_nal->Append(payload);
	const uint8_t rbsp_trailing_bits = 0x80;
	sei_nal->Append(&rbsp_trailing_bits, sizeof(rbsp_trailing_bits));

	auto inserted = NalUnitInsertor::Insert(packet->GetData(), fragment, sei_nal,
											packet->GetBitstreamFormat(), nal_index);
	if (inserted.has_value() == false)
	{
		return false;
	}

	auto data = std::get<0>(inserted.value());
	packet->SetData(data);
	return true;
}
