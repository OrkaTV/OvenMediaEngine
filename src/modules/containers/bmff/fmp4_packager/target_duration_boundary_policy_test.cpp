//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#include <gtest/gtest.h>

#include <cmath>

#include <base/modules/data_format/cue_event/cue_event.h>

#include "fmp4_test_fixtures.h"
#include "target_duration_boundary_policy.h"

// TargetDurationBoundaryPolicy: each segment is capped at the configured target
// and, for video, at a whole number of GOPs. Pacing can shorten a segment but
// never stretch it past that cap.

namespace
{
	constexpr uint64_t kSegmentDurationMs = 4000;

	std::shared_ptr<bmff::TargetDurationBoundaryPolicy> MakePolicy(int64_t initial_segment_number = 0, bool wall_clock_slot_pacing = false, double keyframe_interval_ms = 0.0)
	{
		bmff::SegmentBoundaryPolicy::Config config;
		config.segment_duration_ms = kSegmentDurationMs;
		config.chunk_duration_ms = 400.0;
		config.keyframe_interval_ms = keyframe_interval_ms;
		config.enable_max_duration = true;
		config.initial_segment_number = initial_segment_number;
		config.log_context = "test";
		return std::make_shared<bmff::TargetDurationBoundaryPolicy>(config, wall_clock_slot_pacing);
	}

	bmff::CompletionResult Complete(const std::shared_ptr<bmff::TargetDurationBoundaryPolicy> &policy, int64_t number, double start_ms, double duration_ms)
	{
		bmff::CompletedSegment completed_segment;
		completed_segment.number = number;
		completed_segment.start_timestamp_us = std::llround(start_ms * 1000.0);
		completed_segment.duration_us = std::llround(duration_ms * 1000.0);
		return policy->OnSegmentCompleted(completed_segment);
	}

	// The target the policy plans from a segment starting at 0, in milliseconds
	double TargetMs(const std::shared_ptr<bmff::TargetDurationBoundaryPolicy> &policy)
	{
		return static_cast<double>(policy->GetSegmentBoundary(static_cast<int64_t>(0)).end_us) / 1000.0;
	}
}  // namespace

TEST(TargetDurationBoundaryPolicyTest, InitialTargetIsConfiguredDuration)
{
	auto policy = MakePolicy();

	EXPECT_DOUBLE_EQ(TargetMs(policy), 4000.0);
}

TEST(TargetDurationBoundaryPolicyTest, PlanIsRelativeToTheSegmentStart)
{
	auto policy = MakePolicy();

	auto boundary = policy->GetSegmentBoundary(static_cast<int64_t>(12000000));
	EXPECT_EQ(boundary.end_us, 16000000);
	EXPECT_FALSE(boundary.exact);

	// An unknown start plans only the numbering
	EXPECT_LT(policy->GetSegmentBoundary(std::nullopt).end_us, 0);

	// A negative start is a real position, not "unknown"
	EXPECT_EQ(policy->GetSegmentBoundary(static_cast<int64_t>(-100000)).end_us, 3900000);
}

TEST(TargetDurationBoundaryPolicyTest, NumberingStartsAtTheInitialAndFollowsSettlements)
{
	auto policy = MakePolicy(100);

	EXPECT_EQ(policy->GetSegmentBoundary(static_cast<int64_t>(0)).segment_number, 100);

	Complete(policy, 100, 0.0, 4000.0);
	EXPECT_EQ(policy->GetSegmentBoundary(static_cast<int64_t>(4000000)).segment_number, 101);

	// A force-completed segment names the next one too
	bmff::CompletedSegment completed_segment;
	completed_segment.number = 101;
	completed_segment.duration_us = 2000000;
	policy->OnDiscontinuity(completed_segment);
	EXPECT_EQ(policy->GetSegmentBoundary(static_cast<int64_t>(10000000)).segment_number, 102);

	// A discontinuity with nothing in progress changes no numbering
	policy->OnDiscontinuity({});
	EXPECT_EQ(policy->GetSegmentBoundary(static_cast<int64_t>(10000000)).segment_number, 102);
}

TEST(TargetDurationBoundaryPolicyTest, LongSegmentShortensNextTarget)
{
	auto policy = MakePolicy();

	Complete(policy, 0, 0.0, 4200.0);
	EXPECT_DOUBLE_EQ(TargetMs(policy), 3800.0);

	// The shorter segment pays the debt back and the cadence returns
	Complete(policy, 1, 4200.0, 3800.0);
	EXPECT_DOUBLE_EQ(TargetMs(policy), 4000.0);
}

TEST(TargetDurationBoundaryPolicyTest, ShortSegmentDoesNotStretchNextTargetPastTheCap)
{
	auto policy = MakePolicy();

	Complete(policy, 0, 0.0, 3900.0);
	EXPECT_DOUBLE_EQ(TargetMs(policy), 4000.0);
}

TEST(TargetDurationBoundaryPolicyTest, GopCadenceCapsTheTarget)
{
	auto policy = MakePolicy(0, false, 1900.0);

	EXPECT_DOUBLE_EQ(TargetMs(policy), 3800.0);

	Complete(policy, 0, 0.0, 3800.0);
	EXPECT_DOUBLE_EQ(TargetMs(policy), 3800.0);
}

TEST(TargetDurationBoundaryPolicyTest, LearnedGopCapsTheFirstSegment)
{
	constexpr uint64_t kTargetDurationMs = 6000;
	constexpr double kFrameDurationMs = 100.0;
	constexpr int64_t kGopFrames = 19;

	bmff::SegmentBoundaryPolicy::Config config;
	config.segment_duration_ms = kTargetDurationMs;
	config.chunk_duration_ms = 500.0;
	config.enable_max_duration = true;
	config.log_context = "learned_gop_pipeline_test";

	auto policy = std::make_shared<bmff::TargetDurationBoundaryPolicy>(config, false);
	auto track = fmp4_test::MakeVideoTrack(0, 10.0, kGopFrames);
	auto pipeline = fmp4_test::MakePipeline(track, std::make_shared<fmp4_test::NullStorageObserver>(), policy,
											kTargetDurationMs, config.chunk_duration_ms, config.log_context);

	for (int64_t index = 0; index < 140; index++)
	{
		const int64_t dts = index * static_cast<int64_t>(kFrameDurationMs);
		const int64_t local = index % kGopFrames;
		const bool keyframe = local == 0;
		if (index == kGopFrames)
		{
			policy->UpdateKeyframeInterval(kGopFrames * kFrameDurationMs);
		}

		ASSERT_TRUE(pipeline.packager->AppendSample(
			fmp4_test::MakeVideoFrame(0, dts, static_cast<int64_t>(kFrameDurationMs), keyframe, dts)));
	}

	auto first_segment = pipeline.storage->GetSegment(0);
	ASSERT_NE(first_segment, nullptr);
	ASSERT_TRUE(first_segment->IsCompleted());
	EXPECT_DOUBLE_EQ(first_segment->GetDurationMs(), 5700.0);
}

TEST(TargetDurationBoundaryPolicyTest, FractionalFrameRateGopStaysWithinTheTarget)
{
	bmff::SegmentBoundaryPolicy::Config config;
	config.segment_duration_ms = 6000;
	config.chunk_duration_ms = 500.0;
	config.keyframe_interval_ms = 2002.0;
	config.enable_max_duration = true;
	config.log_context = "fractional-frame-rate-test";

	auto policy = std::make_shared<bmff::TargetDurationBoundaryPolicy>(config, false);
	EXPECT_DOUBLE_EQ(TargetMs(policy), 4004.0);
}

TEST(TargetDurationBoundaryPolicyTest, BFramesWithA1900MsGopStayWithinA2000MsTarget)
{
	constexpr uint64_t kTargetDurationMs = 2000;
	constexpr double kFrameDurationMs = 100.0;
	constexpr int64_t kGopFrames = 19;

	bmff::SegmentBoundaryPolicy::Config config;
	config.segment_duration_ms = kTargetDurationMs;
	config.chunk_duration_ms = 500.0;
	config.keyframe_interval_ms = kGopFrames * kFrameDurationMs;
	config.enable_max_duration = true;
	config.log_context = "target_duration_pipeline_test";

	auto policy = std::make_shared<bmff::TargetDurationBoundaryPolicy>(config, false);
	auto track = fmp4_test::MakeVideoTrack(0, 10.0, kGopFrames);
	auto pipeline = fmp4_test::MakePipeline(track, std::make_shared<fmp4_test::NullStorageObserver>(), policy,
											kTargetDurationMs, config.chunk_duration_ms, config.log_context);

	for (int64_t index = 0; index < 80; index++)
	{
		const int64_t dts = index * static_cast<int64_t>(kFrameDurationMs);
		const int64_t local = index % kGopFrames;
		const bool keyframe = local == 0;
		const int64_t pts = keyframe ? dts : ((local - 1) % 3 == 0 ? dts + 200 : dts - 100);
		ASSERT_TRUE(pipeline.packager->AppendSample(
			fmp4_test::MakeVideoFrame(0, dts, static_cast<int64_t>(kFrameDurationMs), keyframe, pts)));
	}

	int completed = 0;
	for (int64_t number = 0; number <= pipeline.storage->GetLastSegmentNumber(); number++)
	{
		auto segment = pipeline.storage->GetSegment(number);
		if (segment == nullptr || segment->IsCompleted() == false)
		{
			continue;
		}

		EXPECT_LE(segment->GetDurationMs(), static_cast<double>(kTargetDurationMs));
		EXPECT_DOUBLE_EQ(segment->GetDurationMs(), 1900.0);
		completed++;
	}
	EXPECT_GE(completed, 3);
}

TEST(TargetDurationBoundaryPolicyTest, MarkerSegmentIsDiscounted)
{
	auto policy = MakePolicy();

	// A real marker: the discount applies when the settlement consumes one
	auto marker = Marker::CreateMarker(cmn::BitstreamFormat::CUE, 1400, 1400, CueEvent::Create(CueEvent::CueType::OUT, 10000, 0, false)->Serialize());
	ASSERT_TRUE(policy->InsertMarker(marker));

	// A marker cut ends the segment early; the extra boundary is discounted so
	// the next segment fills only up to the original cadence position
	auto completion_result = Complete(policy, 0, 0.0, 1400.0);
	ASSERT_EQ(completion_result.markers.size(), 1u);
	EXPECT_EQ(completion_result.markers.front(), marker);
	EXPECT_DOUBLE_EQ(TargetMs(policy), 2600.0);

	Complete(policy, 1, 1400.0, 2600.0);
	EXPECT_DOUBLE_EQ(TargetMs(policy), 4000.0);
}

TEST(TargetDurationBoundaryPolicyTest, MarkerLandingOnTheTargetKeepsTheCadence)
{
	auto policy = MakePolicy();

	// A keyframe-mode cut can land exactly where the cadence was going to cut.
	// No extra boundary appeared, so the step counts as usual and the next
	// target stays on the grid; counting it as an extra boundary would push the
	// following segments past EXT-X-TARGETDURATION.
	auto marker = Marker::CreateMarker(cmn::BitstreamFormat::CUE, 4000, 4000, CueEvent::Create(CueEvent::CueType::OUT, 10000, 0, false)->Serialize());
	ASSERT_TRUE(policy->InsertMarker(marker));

	auto completion_result = Complete(policy, 0, 0.0, 4000.0);
	ASSERT_EQ(completion_result.markers.size(), 1u);
	EXPECT_DOUBLE_EQ(TargetMs(policy), 4000.0);

	Complete(policy, 1, 4000.0, 4000.0);
	EXPECT_DOUBLE_EQ(TargetMs(policy), 4000.0);
}

TEST(TargetDurationBoundaryPolicyTest, SlotPacingCountsAMarkerSegmentAsAFullSlot)
{
	auto policy = MakePolicy(0, true);

	// Server-time numbering: every number is one wall-clock slot, so the marker
	// segment is not discounted and the next segment stretches to finish the
	// slot. Discounting would leave the numbering one slot ahead of the wall
	// clock per marker, and a server started later never pairs with it again.
	auto marker = Marker::CreateMarker(cmn::BitstreamFormat::CUE, 1400, 1400, CueEvent::Create(CueEvent::CueType::OUT, 10000, 0, false)->Serialize());
	ASSERT_TRUE(policy->InsertMarker(marker));

	auto completion_result = Complete(policy, 0, 0.0, 1400.0);
	ASSERT_EQ(completion_result.markers.size(), 1u);
	EXPECT_DOUBLE_EQ(TargetMs(policy), 4000.0);

	Complete(policy, 1, 1400.0, 4000.0);
	EXPECT_DOUBLE_EQ(TargetMs(policy), 4000.0);
}

TEST(TargetDurationBoundaryPolicyTest, DiscontinuityRealignsPacing)
{
	auto policy = MakePolicy();

	Complete(policy, 0, 0.0, 4200.0);
	EXPECT_DOUBLE_EQ(TargetMs(policy), 3800.0);

	// The force-completed segment counts, then the pacing is realigned so the
	// next segment is not stretched to catch up across the break
	bmff::CompletedSegment completed_segment;
	completed_segment.number = 1;
	completed_segment.duration_us = 2000000;
	policy->OnDiscontinuity(completed_segment);
	EXPECT_DOUBLE_EQ(TargetMs(policy), 4000.0);

	Complete(policy, 2, 6200.0, 4000.0);
	EXPECT_DOUBLE_EQ(TargetMs(policy), 4000.0);
}

TEST(TargetDurationBoundaryPolicyTest, ServerTimeNumberingKeepsPacingOverDiscontinuity)
{
	auto policy = MakePolicy(0, true);

	Complete(policy, 0, 0.0, 4200.0);
	EXPECT_DOUBLE_EQ(TargetMs(policy), 3800.0);

	// Server-time based numbering relies on the catch-up pacing to keep segment
	// numbers aligned to the wall clock, so a discontinuity must not reset it
	policy->OnDiscontinuity({});
	EXPECT_DOUBLE_EQ(TargetMs(policy), 3800.0);
}

TEST(TargetDurationBoundaryPolicyTest, ForceCompletedSegmentCannotStretchNextTargetPastTheCap)
{
	auto policy = MakePolicy(0, true);

	// Server-time numbering: every segment number is one wall-clock slot, so a
	// force-completed segment still counts a full cadence step and the next
	// target stretches to finish its slot
	bmff::CompletedSegment completed_segment;
	completed_segment.number = 0;
	completed_segment.duration_us = 2000000;
	policy->OnDiscontinuity(completed_segment);
	EXPECT_DOUBLE_EQ(TargetMs(policy), 4000.0);

	Complete(policy, 1, 2000.0, 4000.0);
	EXPECT_DOUBLE_EQ(TargetMs(policy), 4000.0);
}

TEST(TargetDurationBoundaryPolicyTest, AnOverlongSegmentIsRepaidByAimingShort)
{
	auto policy = MakePolicy();

	// A keyframe interval longer than the segment duration already breaks the
	// configured pacing, and closing at every keyframe until the ledger is
	// repaid is the intended answer: the segments stay playable, and the
	// stream returns to its cadence as soon as keyframes allow. Forgiving the
	// shortfall instead would leave the ledger describing a timeline the
	// segments never had.
	Complete(policy, 0, 0.0, 20000.0);

	EXPECT_LT(TargetMs(policy), 0.0);
}
