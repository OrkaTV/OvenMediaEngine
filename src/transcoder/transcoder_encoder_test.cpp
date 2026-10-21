//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#include <gtest/gtest.h>

#include <base/info/stream.h>
#include <modules/ffmpeg/compat.h>
#include <modules/ffmpeg/ffmpeg_filter_graph.h>

#include "codec/encoder/encoder_avcodec_video.h"
#include "transcoder_encoder.h"

#include <algorithm>
#include <cstring>

// KeyframeGridRestore: after a pipeline disturbance (track change, codec
// session reinitialization) the encoder forces one keyframe where the previous
// cadence would have produced one, so the cadence does not shift permanently.
// FRAME mode mirrors the codec's own frame counting; TIME mode projects the
// next position from the last output keyframe timestamp.

namespace
{
	class GridRestoreTestEncoder : public TranscodeEncoder
	{
	public:
		GridRestoreTestEncoder(const std::shared_ptr<MediaTrack> &track)
			: TranscodeEncoder(info::Stream(StreamSourceType::Transcoder))
		{
			_track = track;
		}

		cmn::AudioSample::Format GetSupportAudioFormat() const noexcept override
		{
			return cmn::AudioSample::Format::None;
		}
		cmn::VideoPixelFormatId GetSupportVideoFormat() const noexcept override
		{
			return cmn::VideoPixelFormatId::YUV420P;
		}
		cmn::BitstreamFormat GetBitstreamFormat() const noexcept override
		{
			return cmn::BitstreamFormat::H264_ANNEXB;
		}
		cmn::MediaCodecId GetCodecID() const noexcept override
		{
			return cmn::MediaCodecId::H264;
		}
		cmn::MediaCodecModuleId GetModuleID() const noexcept override
		{
			return cmn::MediaCodecModuleId::DEFAULT;
		}
		cmn::MediaType GetMediaType() const noexcept override
		{
			return cmn::MediaType::Video;
		}
		bool IsHWAccel() const noexcept override
		{
			return false;
		}
		bool Initialize() override
		{
			return true;
		}

		// One frame through the per-frame cadence check, as ThreadLoop does
		bool Step(int64_t frame_pts)
		{
			auto frame = std::make_shared<MediaFrame>();
			frame->SetPts(frame_pts);
			return ComputeKeyframeGridRestore(frame);
		}

		// The last output keyframe position is normally tracked in Complete();
		// seed it the same way with a fake key packet
		void SeedLastKeyframe(int64_t pts)
		{
			auto data = std::make_shared<ov::Data>();
			uint8_t byte = 0x00;
			data->Append(&byte, 1);
			auto packet = std::make_shared<MediaPacket>(cmn::MediaType::Video, 0, data, pts, pts, 0,
														MediaPacketFlag::Key, cmn::BitstreamFormat::H264_ANNEXB, cmn::PacketType::NALU);
			Complete(TranscodeResult::DataReady, std::move(packet));
		}

		bool IsArmed() const
		{
			return _keyframe_grid_restore_armed;
		}
	};

	class A53CaptionTestEncoder : public AVCodecVideoEncoder
	{
	public:
		explicit A53CaptionTestEncoder(cmn::MediaCodecId codec_id = cmn::MediaCodecId::H264)
			: AVCodecVideoEncoder(info::Stream(StreamSourceType::Transcoder),
								  codec_id,
								  cmn::MediaCodecModuleId::X264)
		{
		}

		void Preserve(const std::shared_ptr<const MediaFrame> &frame,
					  const std::shared_ptr<MediaPacket> &packet)
		{
			CaptureA53Captions(frame);
			RestoreA53Captions(packet);
		}
	};

#ifdef THIRDP_LIBX265_ENABLED
	class X265TestEncoder : public AVCodecVideoEncoder
	{
	public:
		X265TestEncoder()
			: AVCodecVideoEncoder(info::Stream(StreamSourceType::Transcoder),
								  cmn::MediaCodecId::H265,
								  cmn::MediaCodecModuleId::X265)
		{
		}

		bool Open(const std::shared_ptr<MediaTrack> &track)
		{
			_track = track;
			return Initialize();
		}

		EncodeResult Encode(const std::shared_ptr<const MediaFrame> &frame)
		{
			auto sent = SendFrame(frame, false);
			if (sent.result == TranscodeResult::DataError)
			{
				return sent;
			}
			return ReceivePacket();
		}
	};
#endif

#ifdef THIRDP_LIBSVTAV1_ENABLED
	class SvtAv1TestEncoder : public AVCodecVideoEncoder
	{
	public:
		SvtAv1TestEncoder()
			: AVCodecVideoEncoder(info::Stream(StreamSourceType::Transcoder),
								  cmn::MediaCodecId::Av1,
								  cmn::MediaCodecModuleId::SVTAV1)
		{
		}

		bool Open(const std::shared_ptr<MediaTrack> &track)
		{
			_track = track;
			return Initialize();
		}

		EncodeResult Encode(const std::shared_ptr<const MediaFrame> &frame)
		{
			auto sent = SendFrame(frame, false);
			if (sent.result == TranscodeResult::DataError)
			{
				return sent;
			}
			return ReceivePacket();
		}

		void Finish()
		{
			SendFrame(nullptr, false);
			while (ReceivePacket().result == TranscodeResult::DataReady)
			{
			}
		}
	};
#endif

	constexpr int32_t kIntervalFrames = 30;
	constexpr int64_t kFrameTicks = 3000;  // 1/90000 timebase, 30 fps

	bool ContainsHevcNalType(const uint8_t *data, size_t size, uint8_t expected_type)
	{
		for (size_t offset = 0; offset + 5 < size; offset++)
		{
			size_t header_offset = 0;
			if (data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1)
			{
				header_offset = offset + 3;
			}
			else if (offset + 6 < size && data[offset] == 0 && data[offset + 1] == 0 &&
					 data[offset + 2] == 0 && data[offset + 3] == 1)
			{
				header_offset = offset + 4;
			}
			else
			{
				continue;
			}

			if (((data[header_offset] >> 1) & 0x3F) == expected_type)
			{
				return true;
			}
		}
		return false;
	}

	bool ContainsAv1ObuType(const uint8_t *data, size_t size, uint8_t expected_type)
	{
		size_t offset = 0;
		while (offset < size)
		{
			const uint8_t header = data[offset++];
			const uint8_t obu_type = (header >> 3) & 0x0F;
			const bool extension_flag = (header & 0x04) != 0;
			const bool has_size_field = (header & 0x02) != 0;
			if ((header & 0x80) != 0 || !has_size_field || (extension_flag && offset >= size))
			{
				return false;
			}
			if (extension_flag)
			{
				offset++;
			}

			uint64_t payload_size = 0;
			unsigned shift = 0;
			uint8_t byte = 0;
			do
			{
				if (offset >= size || shift >= 56)
				{
					return false;
				}
				byte = data[offset++];
				payload_size |= static_cast<uint64_t>(byte & 0x7F) << shift;
				shift += 7;
			} while ((byte & 0x80) != 0);

			if (payload_size > size - offset)
			{
				return false;
			}
			if (obu_type == expected_type)
			{
				return true;
			}
			offset += static_cast<size_t>(payload_size);
		}
		return false;
	}

	std::shared_ptr<MediaTrack> MakeVideoTrack(cmn::KeyFrameIntervalType type = cmn::KeyFrameIntervalType::FRAME)
	{
		auto track = std::make_shared<MediaTrack>();
		track->SetId(0);
		track->SetMediaType(cmn::MediaType::Video);
		track->SetCodecId(cmn::MediaCodecId::H264);
		track->SetTimeBase(1, 90000);
		track->SetFrameRateByConfig(30.0);
		track->SetKeyFrameIntervalTypeByConfig(type);
		// FRAME: 30 frames, TIME: 1000 ms; both are one second at 30 fps
		track->SetKeyFrameIntervalByConfig(type == cmn::KeyFrameIntervalType::FRAME ? kIntervalFrames : 1000);
		return track;
	}
}  // namespace

TEST(A53CaptionTest, RestoresGa94SeiByFramePts)
{
	AVFrame *av_frame = ::av_frame_alloc();
	ASSERT_NE(av_frame, nullptr);
	av_frame->pts = 9000;
	auto *side_data = ::av_frame_new_side_data(av_frame, AV_FRAME_DATA_A53_CC, 3);
	ASSERT_NE(side_data, nullptr);
	const uint8_t cc_data[] = {0xFC, 0x80, 0x80};
	::memcpy(side_data->data, cc_data, sizeof(cc_data));

	auto frame = ffmpeg::compat::ToMediaFrame(cmn::MediaType::Video, av_frame);
	::av_frame_free(&av_frame);
	ASSERT_NE(frame, nullptr);

	const uint8_t access_unit[] = {
		0x00, 0x00, 0x00, 0x01,
		0x65, 0x88, 0x84, 0x00, 0x21, 0xFF
	};
	auto packet_data = std::make_shared<ov::Data>(access_unit, sizeof(access_unit));
	auto packet = std::make_shared<MediaPacket>(
		cmn::MediaType::Video, 1, packet_data, 9000, 9000, 3000,
		MediaPacketFlag::Key, cmn::BitstreamFormat::H264_ANNEXB, cmn::PacketType::NALU);

	A53CaptionTestEncoder encoder;
	encoder.Preserve(frame, packet);

	const uint8_t signature[] = {0xB5, 0x00, 0x31, 'G', 'A', '9', '4', 0x03};
	const auto *begin = packet->GetData()->GetDataAs<uint8_t>();
	const auto *end = begin + packet->GetData()->GetLength();
	EXPECT_NE(std::search(begin, end, std::begin(signature), std::end(signature)), end);
}

TEST(A53CaptionTest, RestoresGa94IntoHevcWhenChangingCodec)
{
	AVFrame *av_frame = ::av_frame_alloc();
	ASSERT_NE(av_frame, nullptr);
	av_frame->pts = 9000;
	auto *side_data = ::av_frame_new_side_data(av_frame, AV_FRAME_DATA_A53_CC, 3);
	ASSERT_NE(side_data, nullptr);
	const uint8_t cc_data[] = {0xFC, 0x80, 0x80};
	::memcpy(side_data->data, cc_data, sizeof(cc_data));
	auto frame = ffmpeg::compat::ToMediaFrame(cmn::MediaType::Video, av_frame);
	::av_frame_free(&av_frame);

	const uint8_t access_unit[] = {
		0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xBB
	};
	auto packet = std::make_shared<MediaPacket>(
		cmn::MediaType::Video, 1,
		std::make_shared<ov::Data>(access_unit, sizeof(access_unit)),
		9000, 9000, 3000, MediaPacketFlag::Key,
		cmn::BitstreamFormat::H265_ANNEXB, cmn::PacketType::NALU);

	A53CaptionTestEncoder encoder(cmn::MediaCodecId::H265);
	encoder.Preserve(frame, packet);

	const uint8_t signature[] = {0xB5, 0x00, 0x31, 'G', 'A', '9', '4', 0x03};
	const auto *begin = packet->GetData()->GetDataAs<uint8_t>();
	const auto *end = begin + packet->GetData()->GetLength();
	EXPECT_NE(std::search(begin, end, std::begin(signature), std::end(signature)), end);
}

TEST(A53CaptionTest, RestoresGa94IntoAv1WhenChangingCodec)
{
	AVFrame *av_frame = ::av_frame_alloc();
	ASSERT_NE(av_frame, nullptr);
	av_frame->pts = 9000;
	auto *side_data = ::av_frame_new_side_data(av_frame, AV_FRAME_DATA_A53_CC, 3);
	ASSERT_NE(side_data, nullptr);
	const uint8_t cc_data[] = {0xFC, 0x80, 0x80};
	::memcpy(side_data->data, cc_data, sizeof(cc_data));
	auto frame = ffmpeg::compat::ToMediaFrame(cmn::MediaType::Video, av_frame);
	::av_frame_free(&av_frame);

	const uint8_t frame_obu[] = {0x32, 0x01, 0x00};
	auto packet = std::make_shared<MediaPacket>(
		cmn::MediaType::Video, 1,
		std::make_shared<ov::Data>(frame_obu, sizeof(frame_obu)),
		9000, 9000, 3000, MediaPacketFlag::Key,
		cmn::BitstreamFormat::AV1_OBU, cmn::PacketType::RAW);

	A53CaptionTestEncoder encoder(cmn::MediaCodecId::Av1);
	encoder.Preserve(frame, packet);

	const uint8_t signature[] = {0xB5, 0x00, 0x31, 'G', 'A', '9', '4', 0x03};
	const auto *begin = packet->GetData()->GetDataAs<uint8_t>();
	const auto *end = begin + packet->GetData()->GetLength();
	EXPECT_NE(std::search(begin, end, std::begin(signature), std::end(signature)), end);
}

TEST(A53CaptionTest, SurvivesScaleFilter)
{
	ffmpeg::FFmpegFilterGraph graph;
	ASSERT_TRUE(graph.Alloc(cmn::MediaType::Video, 1));
	ASSERT_TRUE(graph.CreateBufferSource(
		"video_size=16x16:pix_fmt=yuv420p:time_base=1/90000:pixel_aspect=1/1"));
	ASSERT_TRUE(graph.CreateBufferSink());
	ASSERT_TRUE(graph.Parse("scale=8x8:flags=bilinear"));
	ASSERT_TRUE(graph.Config());

	AVFrame *av_frame = ::av_frame_alloc();
	ASSERT_NE(av_frame, nullptr);
	av_frame->format = AV_PIX_FMT_YUV420P;
	av_frame->width = 16;
	av_frame->height = 16;
	av_frame->pts = 9000;
	ASSERT_EQ(::av_frame_get_buffer(av_frame, 0), 0);
	auto *side_data = ::av_frame_new_side_data(av_frame, AV_FRAME_DATA_A53_CC, 3);
	ASSERT_NE(side_data, nullptr);
	const uint8_t cc_data[] = {0xFC, 0x80, 0x80};
	::memcpy(side_data->data, cc_data, sizeof(cc_data));

	auto input = ffmpeg::compat::ToMediaFrame(cmn::MediaType::Video, av_frame);
	::av_frame_free(&av_frame);
	ASSERT_NE(input, nullptr);
	ASSERT_EQ(graph.PushFrame(input, false), ffmpeg::CodecResult::Ok);

	auto [result, output] = graph.PullFrame();
	ASSERT_EQ(result, ffmpeg::CodecResult::Ok);
	ASSERT_NE(output, nullptr);
	const auto *filtered = static_cast<const AVFrame *>(output->GetPrivData());
	ASSERT_NE(filtered, nullptr);
	const auto *filtered_side_data =
		::av_frame_get_side_data(filtered, AV_FRAME_DATA_A53_CC);
	ASSERT_NE(filtered_side_data, nullptr);
	ASSERT_EQ(filtered_side_data->size, sizeof(cc_data));
	EXPECT_EQ(::memcmp(filtered_side_data->data, cc_data, sizeof(cc_data)), 0);
}

TEST(X265EncoderTest, ModuleNameRoundTrips)
{
	EXPECT_EQ(cmn::GetCodecModuleIdByName("X265"), cmn::MediaCodecModuleId::X265);
	EXPECT_STREQ(cmn::GetCodecModuleIdString(cmn::MediaCodecModuleId::X265), "x265");
}

#ifdef THIRDP_LIBX265_ENABLED
TEST(X265EncoderTest, EncodesHevcAnnexB)
{
	auto track = MakeVideoTrack();
	track->SetCodecId(cmn::MediaCodecId::H265);
	track->SetBitrateByConfig(500000);
	track->SetResolution(64, 64);
	track->SetBFrames(0);
	track->SetThreadCount(1);
	track->SetPreset("faster");
	track->SetProfile("main");

	X265TestEncoder encoder;
	ASSERT_TRUE(encoder.Open(track));

	AVFrame *av_frame = ::av_frame_alloc();
	ASSERT_NE(av_frame, nullptr);
	av_frame->format = AV_PIX_FMT_YUV420P;
	av_frame->width = 64;
	av_frame->height = 64;
	av_frame->pts = 0;
	ASSERT_EQ(::av_frame_get_buffer(av_frame, 0), 0);
	ASSERT_EQ(::av_frame_make_writable(av_frame), 0);
	for (int plane = 0; plane < 3; plane++)
	{
		const int height = (plane == 0) ? 64 : 32;
		const int width = (plane == 0) ? 64 : 32;
		for (int y = 0; y < height; y++)
		{
			::memset(av_frame->data[plane] + y * av_frame->linesize[plane],
					 plane == 0 ? 0x10 : 0x80, width);
		}
	}

	auto frame = ffmpeg::compat::ToMediaFrame(cmn::MediaType::Video, av_frame);
	::av_frame_free(&av_frame);
	ASSERT_NE(frame, nullptr);

	auto result = encoder.Encode(frame);
	ASSERT_EQ(result.result, TranscodeResult::DataReady);
	ASSERT_NE(result.packet, nullptr);
	EXPECT_EQ(result.packet->GetBitstreamFormat(), cmn::BitstreamFormat::H265_ANNEXB);
	ASSERT_GE(result.packet->GetDataLength(), 6U);
	const auto *data = result.packet->GetData()->GetDataAs<uint8_t>();
	EXPECT_EQ(data[0], 0x00);
	EXPECT_EQ(data[1], 0x00);
	EXPECT_TRUE((data[2] == 0x01) || (data[2] == 0x00 && data[3] == 0x01));
	EXPECT_TRUE(ContainsHevcNalType(data, result.packet->GetDataLength(), 32)) << "missing VPS";
	EXPECT_TRUE(ContainsHevcNalType(data, result.packet->GetDataLength(), 33)) << "missing SPS";
	EXPECT_TRUE(ContainsHevcNalType(data, result.packet->GetDataLength(), 34)) << "missing PPS";
	EXPECT_TRUE(ContainsHevcNalType(data, result.packet->GetDataLength(), 20) ||
				ContainsHevcNalType(data, result.packet->GetDataLength(), 19))
		<< "missing IDR VCL NAL";
}
#endif

TEST(SvtAv1EncoderTest, ModuleNameRoundTrips)
{
	EXPECT_EQ(cmn::GetCodecModuleIdByName("SVTAV1"), cmn::MediaCodecModuleId::SVTAV1);
	EXPECT_EQ(cmn::GetCodecModuleIdByName("SVT_AV1"), cmn::MediaCodecModuleId::SVTAV1);
	EXPECT_STREQ(cmn::GetCodecModuleIdString(cmn::MediaCodecModuleId::SVTAV1), "svtav1");
}

#ifdef THIRDP_LIBSVTAV1_ENABLED
TEST(SvtAv1EncoderTest, DefaultAndExplicitFactoryUseSvtAv1)
{
	info::Stream stream(StreamSourceType::Transcoder);
	auto default_encoder = TranscodeEncoder::Instantiate(
		cmn::MediaCodecId::Av1, cmn::MediaCodecModuleId::DEFAULT, stream);
	auto explicit_encoder = TranscodeEncoder::Instantiate(
		cmn::MediaCodecId::Av1, cmn::MediaCodecModuleId::SVTAV1, stream);

	ASSERT_NE(default_encoder, nullptr);
	ASSERT_NE(explicit_encoder, nullptr);
	EXPECT_EQ(default_encoder->GetModuleID(), cmn::MediaCodecModuleId::SVTAV1);
	EXPECT_EQ(explicit_encoder->GetModuleID(), cmn::MediaCodecModuleId::SVTAV1);
}

TEST(SvtAv1EncoderTest, EncodesAv1Obu)
{
	auto track = MakeVideoTrack();
	track->SetCodecId(cmn::MediaCodecId::Av1);
	track->SetBitrateByConfig(500000);
	track->SetResolution(64, 64);
	track->SetBFrames(0);
	track->SetThreadCount(1);
	track->SetPreset("faster");
	track->SetProfile("main");

	SvtAv1TestEncoder encoder;
	ASSERT_TRUE(encoder.Open(track));

	EncodeResult result = EncodeResult::NoOutput();
	for (int64_t pts = 0; pts < 4 && result.result != TranscodeResult::DataReady; pts++)
	{
		AVFrame *av_frame = ::av_frame_alloc();
		ASSERT_NE(av_frame, nullptr);
		av_frame->format = AV_PIX_FMT_YUV420P;
		av_frame->width = 64;
		av_frame->height = 64;
		av_frame->pts = pts * kFrameTicks;
		ASSERT_EQ(::av_frame_get_buffer(av_frame, 0), 0);
		ASSERT_EQ(::av_frame_make_writable(av_frame), 0);
		for (int plane = 0; plane < 3; plane++)
		{
			const int height = (plane == 0) ? 64 : 32;
			const int width = (plane == 0) ? 64 : 32;
			for (int y = 0; y < height; y++)
			{
				::memset(av_frame->data[plane] + y * av_frame->linesize[plane],
						 plane == 0 ? 0x10 : 0x80, width);
			}
		}

		auto frame = ffmpeg::compat::ToMediaFrame(cmn::MediaType::Video, av_frame);
		::av_frame_free(&av_frame);
		ASSERT_NE(frame, nullptr);
		result = encoder.Encode(frame);
		ASSERT_NE(result.result, TranscodeResult::DataError);
	}

	ASSERT_EQ(result.result, TranscodeResult::DataReady);
	ASSERT_NE(result.packet, nullptr);
	EXPECT_EQ(result.packet->GetBitstreamFormat(), cmn::BitstreamFormat::AV1_OBU);
	ASSERT_GT(result.packet->GetDataLength(), 2U);
	const auto *data = result.packet->GetData()->GetDataAs<uint8_t>();
	EXPECT_TRUE(ContainsAv1ObuType(data, result.packet->GetDataLength(), 1))
		<< "missing sequence header OBU";
	EXPECT_TRUE(ContainsAv1ObuType(data, result.packet->GetDataLength(), 6) ||
				ContainsAv1ObuType(data, result.packet->GetDataLength(), 3))
		<< "missing frame OBU";
	encoder.Finish();
}
#endif

TEST(KeyframeGridRestoreTest, FrameModeFiresAtNextCadencePosition)
{
	GridRestoreTestEncoder encoder(MakeVideoTrack());

	// Build up mid-cycle cadence state: 10 frames into the cycle
	int64_t pts = 0;
	for (int step = 0; step < 10; step++)
	{
		EXPECT_FALSE(encoder.Step(pts));
		pts += kFrameTicks;
	}

	encoder.ArmKeyframeGridRestore();

	// The cadence position is the 31st frame of the cycle: 20 more pass first
	for (int step = 0; step < 20; step++)
	{
		EXPECT_FALSE(encoder.Step(pts)) << "fired early at step " << step;
		pts += kFrameTicks;
	}

	EXPECT_TRUE(encoder.Step(pts));
	EXPECT_FALSE(encoder.IsArmed());
}

TEST(KeyframeGridRestoreTest, FrameModeFiresOnlyWhileArmed)
{
	GridRestoreTestEncoder encoder(MakeVideoTrack());

	// Two full cadence cycles without arming: the counter runs, nothing fires
	int64_t pts = 0;
	for (int step = 0; step < kIntervalFrames * 2 + 1; step++)
	{
		EXPECT_FALSE(encoder.Step(pts)) << "fired unarmed at step " << step;
		pts += kFrameTicks;
	}

	// Armed now: the next cadence position fires exactly once
	encoder.ArmKeyframeGridRestore();
	bool fired = false;
	for (int step = 0; step <= kIntervalFrames; step++)
	{
		if (encoder.Step(pts))
		{
			fired = true;
			break;
		}
		pts += kFrameTicks;
	}
	EXPECT_TRUE(fired);
	EXPECT_FALSE(encoder.IsArmed());
}

TEST(KeyframeGridRestoreTest, FrameModeDisarmsWithoutInterval)
{
	auto track = MakeVideoTrack();
	track->SetKeyFrameIntervalByConfig(0);	// encoder-decided cadence
	GridRestoreTestEncoder encoder(track);

	encoder.ArmKeyframeGridRestore();
	EXPECT_FALSE(encoder.Step(0));
	EXPECT_FALSE(encoder.IsArmed());
}

TEST(KeyframeGridRestoreTest, TimeModeFiresAtProjectedPosition)
{
	GridRestoreTestEncoder encoder(MakeVideoTrack(cmn::KeyFrameIntervalType::TIME));

	// The last keyframe went out at 10 s
	encoder.SeedLastKeyframe(10 * 90000);
	encoder.ArmKeyframeGridRestore();

	EXPECT_FALSE(encoder.Step(10 * 90000 + kFrameTicks));
	EXPECT_FALSE(encoder.Step(11 * 90000 - kFrameTicks));
	EXPECT_TRUE(encoder.Step(11 * 90000));
	EXPECT_FALSE(encoder.IsArmed());
	EXPECT_FALSE(encoder.Step(12 * 90000));
}

TEST(KeyframeGridRestoreTest, TimeModeDisarmsWithoutCadenceHistory)
{
	GridRestoreTestEncoder encoder(MakeVideoTrack(cmn::KeyFrameIntervalType::TIME));

	// Armed but no keyframe has ever been produced: nothing to restore
	encoder.ArmKeyframeGridRestore();
	EXPECT_FALSE(encoder.Step(90000));
	EXPECT_FALSE(encoder.IsArmed());
}
