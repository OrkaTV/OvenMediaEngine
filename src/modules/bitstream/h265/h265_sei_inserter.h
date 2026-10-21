//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#pragma once

#include <base/mediarouter/media_buffer.h>

class H265SeiInserter
{
public:
	// Inserts a prefix SEI user_data_registered_itu_t_t35 message before the first VCL NAL.
	// `payload` is the complete ITU-T T.35 body beginning with the country code.
	static bool InsertRegisteredUserData(const std::shared_ptr<MediaPacket> &packet,
										 const std::shared_ptr<const ov::Data> &payload);
};
