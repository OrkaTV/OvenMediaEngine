//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================

#include "descriptors/cea608_caption_service.h"

#include <gtest/gtest.h>

#include <cstring>

TEST(Cea608CaptionServiceDescriptor, BuildsEnglishCc1Service)
{
	mpegts::Cea608CaptionServiceDescriptor descriptor;
	auto data = descriptor.Build();

	const uint8_t expected[] = {
		0x86, 0x07, 0xE1, 'e', 'n', 'g', 0x7E, 0x3F, 0xFF
	};
	ASSERT_NE(data, nullptr);
	ASSERT_EQ(data->GetLength(), sizeof(expected));
	EXPECT_EQ(std::memcmp(data->GetData(), expected, sizeof(expected)), 0);
}
