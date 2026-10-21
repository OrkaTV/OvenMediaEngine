//==============================================================================
//
//  CEA-608 caption service descriptor
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================

#pragma once

#include "descriptor.h"

namespace mpegts
{
	class Cea608CaptionServiceDescriptor : public Descriptor
	{
	public:
		Cea608CaptionServiceDescriptor()
			: Descriptor(0x86)
		{
		}

	protected:
		std::shared_ptr<const ov::Data> GetData() const override
		{
			// ATSC A/65: one English analog Line 21 field-1 service (CC1).
			static constexpr uint8_t payload[] = {
				0xE1, 'e', 'n', 'g', 0x7E, 0x3F, 0xFF
			};
			return std::make_shared<ov::Data>(payload, sizeof(payload));
		}
	};
}  // namespace mpegts
