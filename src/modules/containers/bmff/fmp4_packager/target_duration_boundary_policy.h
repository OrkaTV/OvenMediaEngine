//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include "segment_boundary_policy.h"

namespace bmff
{
	// Keeps every ordinary segment at or below the configured target duration.
	// Every rendition targets the largest whole number of the reference video's
	// keyframe intervals that fits. Pacing may shorten a later segment to repay
	// an overrun, but it never stretches one past that cap.
	class TargetDurationBoundaryPolicy : public SegmentBoundaryPolicy
	{
	public:
		// wall_clock_slot_pacing: server-time based segment numbering treats
		// every segment number as one wall-clock slot, so every completion counts
		// a full cadence step (no marker discount, no reset on a discontinuity)
		// and the catch-up pacing finishes each slot. Otherwise a server started
		// later derives its numbers fresh from the wall clock and never matches
		// the ones already running.
		TargetDurationBoundaryPolicy(const Config &config, bool wall_clock_slot_pacing);

		SegmentBoundary GetSegmentBoundary(std::optional<int64_t> segment_start_us) override;
		void UpdateKeyframeInterval(double keyframe_interval_ms);

	protected:
		CompletionResult DoOnSegmentCompleted(const CompletedSegment &completed, const std::vector<std::shared_ptr<Marker>> &covered_markers) override;
		CompletionResult DoOnDiscontinuity(const CompletedSegment &completed, const std::vector<std::shared_ptr<Marker>> &covered_markers) override;

	private:
		bool _wall_clock_slot_pacing = false;
		bool _keyframe_interval_known = false;
		int64_t _maximum_segment_duration_us = 0;

		// The pacing ledger: the ideal timeline advances by the configured
		// duration per segment, the actual one by what each segment really was;
		// their difference may shorten the next target, but the configured/GOP
		// cap prevents it from stretching past EXT-X-TARGETDURATION.
		int64_t _target_segment_duration_us = 0;
		int64_t _total_segment_duration_us = 0;
		int64_t _total_expected_duration_us = 0;
	};
}  // namespace bmff
