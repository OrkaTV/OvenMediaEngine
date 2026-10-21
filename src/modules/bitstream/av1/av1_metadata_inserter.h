//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#pragma once

#include <base/mediarouter/media_buffer.h>

class Av1MetadataInserter
{
public:
	// Inserts an OBU_METADATA carrying metadata_type ITUT_T35 before the coded frame.
	// `payload` is the complete ITU-T T.35 body beginning with the country code.
	static bool InsertItuTT35(const std::shared_ptr<MediaPacket> &packet,
							 const std::shared_ptr<const ov::Data> &payload);
};
