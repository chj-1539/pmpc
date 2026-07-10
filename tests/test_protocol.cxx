//=============================================================================
// test_protocol.cxx — PEMP2.0 协议引擎单元测试
// 涵盖: FrameBuilder / FrameParser / CP56time2a
//=============================================================================

#include "mini_gtest.h"
#include "protocol.h"
#include <cstring>

// ==================== FrameBuilder ====================

TEST(FrameBuilderTest, MakeSimple) {
    auto frame = FrameBuilder::MakeSimple(FunCode::QueryStatus);
    ASSERT_EQ(frame.size(), 5);
    EXPECT_EQ(frame[0], FRAME_START);  // 0x7B
    EXPECT_EQ(frame[1], 0x01);        // FUN
    EXPECT_EQ(frame[2], 0x00);        // LEN low
    EXPECT_EQ(frame[3], 0x00);        // LEN high
    EXPECT_EQ(frame[4], FRAME_END);   // 0x7D
}

TEST(FrameBuilderTest, Make1Byte) {
    auto frame = FrameBuilder::Make1Byte(FunCode::ExecRemoteCtrl, 0xFF);
    ASSERT_EQ(frame.size(), 6);
    EXPECT_EQ(frame[0], FRAME_START);
    EXPECT_EQ(frame[1], 0x06);
    EXPECT_EQ(frame[2], 0x01);  // LEN = 1
    EXPECT_EQ(frame[3], 0x00);
    EXPECT_EQ(frame[4], 0xFF);  // DATA
    EXPECT_EQ(frame[5], FRAME_END);
}

TEST(FrameBuilderTest, BuildAndEnd) {
    FrameBuilder fb;
    fb.Begin(FunCode::CallTelemetry);
    fb.AppendU16(0x1234);
    auto frame = fb.End();
    ASSERT_EQ(frame.size(), 7);
    EXPECT_EQ(frame[0], FRAME_START);
    EXPECT_EQ(frame[1], 0x03);
    EXPECT_EQ(frame[2], 0x02);  // LEN = 2
    EXPECT_EQ(frame[3], 0x00);
    EXPECT_EQ(frame[4], 0x34);  // 0x1234 LE
    EXPECT_EQ(frame[5], 0x12);
    EXPECT_EQ(frame[6], FRAME_END);
}

TEST(FrameBuilderTest, AppendMultipleBytes) {
    FrameBuilder fb;
    fb.Begin(FunCode::CallTelemetry);
    uint8_t data[] = {0xAA, 0xBB, 0xCC};
    fb.Append(data, 3);
    auto frame = fb.End();
    ASSERT_EQ(frame.size(), 8);
    EXPECT_EQ(frame[2], 0x03);  // LEN = 3
    EXPECT_EQ(frame[4], 0xAA);
    EXPECT_EQ(frame[5], 0xBB);
    EXPECT_EQ(frame[6], 0xCC);
}

TEST(FrameBuilderTest, MakeTelemetryFrame) {
    int32_t vals[] = {100, -200, 30000};
    auto frame = FrameBuilder::MakeTelemetryFrame(
        FunCode::UploadTelemetry, 1, 2, vals, 3);

    ASSERT_TRUE(FrameParser::Validate(frame));
    EXPECT_EQ(FrameParser::GetFun(frame), FunCode::UploadTelemetry);
    EXPECT_EQ(FrameParser::GetDataLen(frame), 16);
    EXPECT_EQ(FrameParser::ReadByte(frame, 0), 1);     // ch
    EXPECT_EQ(FrameParser::ReadByte(frame, 1), 2);     // dev
    EXPECT_EQ(FrameParser::ReadU16(frame, 2), 3);      // count
    EXPECT_EQ(FrameParser::ReadI32(frame, 4), 100);    // vals[0]
    EXPECT_EQ(FrameParser::ReadI32(frame, 8), -200);   // vals[1]
    EXPECT_EQ(FrameParser::ReadI32(frame, 12), 30000); // vals[2]
}

TEST(FrameBuilderTest, MakeTelemetryFrameZeroCount) {
    auto frame = FrameBuilder::MakeTelemetryFrame(
        FunCode::UploadTelemetry, 1, 1, nullptr, 0);
    ASSERT_TRUE(FrameParser::Validate(frame));
    EXPECT_EQ(FrameParser::GetDataLen(frame), 4);
    EXPECT_EQ(FrameParser::ReadByte(frame, 0), 1);
    EXPECT_EQ(FrameParser::ReadByte(frame, 1), 1);
    EXPECT_EQ(FrameParser::ReadU16(frame, 2), 0);
}

TEST(FrameBuilderTest, MakeTeleIndFrame) {
    uint8_t packed[] = {0b10101010, 0b11001100};
    auto frame = FrameBuilder::MakeTeleIndFrame(
        FunCode::UploadTeleInd, 1, 1, 16, packed, 2);
    ASSERT_TRUE(FrameParser::Validate(frame));
    EXPECT_EQ(FrameParser::GetFun(frame), FunCode::UploadTeleInd);
    // Data: ch(1) + dev(1) + totalPoints(2) + packed(N) = 6 bytes
    EXPECT_EQ(FrameParser::GetDataLen(frame), 6);
    EXPECT_EQ(FrameParser::ReadByte(frame, 0), 1);     // ch
    EXPECT_EQ(FrameParser::ReadByte(frame, 1), 1);     // dev
    EXPECT_EQ(FrameParser::ReadU16(frame, 2), 16);     // totalPoints
    EXPECT_EQ(FrameParser::ReadByte(frame, 4), 0xAA);  // packed[0]
    EXPECT_EQ(FrameParser::ReadByte(frame, 5), 0xCC);  // packed[1]
}

TEST(FrameBuilderTest, MakeErrorFrame) {
    auto frame = FrameBuilder::MakeErrorFrame(
        FunCode::CallTelemetry, ErrorCode::Busy);
    ASSERT_TRUE(FrameParser::Validate(frame));
    EXPECT_EQ(FrameParser::GetFun(frame), FunCode::ErrorFrame);
    // Data: origFun(U16 LE) + err(1) = 3 bytes
    EXPECT_EQ(FrameParser::GetDataLen(frame), 3);
    EXPECT_EQ(FrameParser::ReadByte(frame, 0),
              static_cast<uint8_t>(FunCode::CallTelemetry));  // origFun low byte
    EXPECT_EQ(FrameParser::ReadByte(frame, 2),
              static_cast<uint8_t>(ErrorCode::Busy));         // error code
}

TEST(FrameBuilderTest, MakeSOEFrame) {
    SOEEvent events[2];
    events[0].channel = 1;   events[0].device = 1;
    events[0].soeId = 100;   events[0].status = 1;
    events[0].timestamp = CP56time2a::FromTimeT(1000000);

    events[1].channel = 2;   events[1].device = 3;
    events[1].soeId = 200;   events[1].status = 2;
    events[1].timestamp = CP56time2a::FromTimeT(2000000);

    auto frame = FrameBuilder::MakeSOEFrame(FunCode::UploadSOE, events, 2);

    ASSERT_TRUE(FrameParser::Validate(frame));
    EXPECT_EQ(FrameParser::GetFun(frame), FunCode::UploadSOE);
    // count(2) + 2 * (ch+dev+soeId+timestamp+status) = 2 + 2*12 = 26
    EXPECT_EQ(FrameParser::GetDataLen(frame), 26);
    EXPECT_EQ(FrameParser::ReadU16(frame, 0), 2);   // event count

    // Event 0
    EXPECT_EQ(FrameParser::ReadByte(frame, 2), 1);
    EXPECT_EQ(FrameParser::ReadByte(frame, 3), 1);
    EXPECT_EQ(FrameParser::ReadU16(frame, 4), 100);
    EXPECT_EQ(FrameParser::ReadByte(frame, 13), 1); // status @ offset 13

    // Event 1 starts at offset 14
    EXPECT_EQ(FrameParser::ReadByte(frame, 14), 2);
    EXPECT_EQ(FrameParser::ReadByte(frame, 15), 3);
    EXPECT_EQ(FrameParser::ReadU16(frame, 16), 200);
    EXPECT_EQ(FrameParser::ReadByte(frame, 25), 2); // status @ offset 25
}

TEST(FrameBuilderTest, AllFunCodesBuildable) {
    for (uint8_t code = 0x01; code <= 0x55; code++) {
        // Skip unused codes
        if (code == 0x00) continue;
        auto frame = FrameBuilder::MakeSimple(static_cast<FunCode>(code));
        EXPECT_TRUE(FrameParser::Validate(frame));
        EXPECT_EQ(FrameParser::GetFun(frame), static_cast<FunCode>(code));
    }
}

// ==================== FrameParser ====================

TEST(FrameParserTest, FeedSingleFrame) {
    auto frame = FrameBuilder::MakeSimple(FunCode::QueryStatus);
    FrameParser parser;
    auto frames = parser.Feed(frame.data(), frame.size());
    ASSERT_EQ(frames.size(), 1);
    EXPECT_EQ(FrameParser::GetFun(frames[0]), FunCode::QueryStatus);
}

TEST(FrameParserTest, FeedMultipleFrames) {
    auto f1 = FrameBuilder::MakeSimple(FunCode::CallTelemetry);
    auto f2 = FrameBuilder::MakeSimple(FunCode::SyncClock);

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), f1.begin(), f1.end());
    combined.insert(combined.end(), f2.begin(), f2.end());

    FrameParser parser;
    auto frames = parser.Feed(combined.data(), combined.size());
    ASSERT_EQ(frames.size(), 2);
    EXPECT_EQ(FrameParser::GetFun(frames[0]), FunCode::CallTelemetry);
    EXPECT_EQ(FrameParser::GetFun(frames[1]), FunCode::SyncClock);
}

TEST(FrameParserTest, FeedPartialThenRemaining) {
    auto frame = FrameBuilder::Make1Byte(FunCode::ExecRemoteCtrl, 0xAB);

    FrameParser parser;
    auto frames = parser.Feed(frame.data(), 3);
    EXPECT_EQ(frames.size(), 0);  // incomplete

    frames = parser.Feed(frame.data() + 3, frame.size() - 3);
    ASSERT_EQ(frames.size(), 1);
    EXPECT_EQ(FrameParser::GetFun(frames[0]), FunCode::ExecRemoteCtrl);
    EXPECT_EQ(FrameParser::ReadByte(frames[0], 0), 0xAB);
}

TEST(FrameParserTest, FeedByteByByte) {
    auto frame = FrameBuilder::MakeSimple(FunCode::CallHistorySOE);

    FrameParser parser;
    std::vector<std::vector<uint8_t>> result;
    for (size_t i = 0; i < frame.size(); i++) {
        auto f = parser.Feed(&frame[i], 1);
        result.insert(result.end(), f.begin(), f.end());
    }
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(FrameParser::GetFun(result[0]), FunCode::CallHistorySOE);
}

TEST(FrameParserTest, ResetClearsBuffer) {
    auto frame = FrameBuilder::MakeSimple(FunCode::CallTelemetry);
    FrameParser parser;

    // Feed partial
    EXPECT_EQ(parser.Feed(frame.data(), 2).size(), 0);

    // Reset should discard buffered bytes
    parser.Reset();

    // Feed remaining bytes (starting at byte 2 = LEN field, not a valid start)
    auto frames = parser.Feed(frame.data() + 2, frame.size() - 2);
    EXPECT_EQ(frames.size(), 0);
}

TEST(FrameParserTest, FeedGarbageBeforeValidFrame) {
    std::vector<uint8_t> data = {0x00, 0xFF, 0xAA};  // garbage
    auto frame = FrameBuilder::MakeSimple(FunCode::SyncClock);
    data.insert(data.end(), frame.begin(), frame.end());

    FrameParser parser;
    auto frames = parser.Feed(data.data(), data.size());
    ASSERT_EQ(frames.size(), 1);
    EXPECT_EQ(FrameParser::GetFun(frames[0]), FunCode::SyncClock);
}

TEST(FrameParserTest, FeedAllGarbage) {
    std::vector<uint8_t> garbage = {0x00, 0xFF, 0xAA, 0x55, 0x01};
    FrameParser parser;
    auto frames = parser.Feed(garbage.data(), garbage.size());
    EXPECT_EQ(frames.size(), 0);
}

// ==================== Frame field extraction ====================

TEST(FrameParserTest, GetFun) {
    auto frame = FrameBuilder::MakeSimple(FunCode::CallHistorySOE);
    EXPECT_EQ(FrameParser::GetFun(frame), FunCode::CallHistorySOE);
}

TEST(FrameParserTest, GetDataLen) {
    auto frame = FrameBuilder::Make1Byte(FunCode::ExecRemoteCtrl, 0x42);
    EXPECT_EQ(FrameParser::GetDataLen(frame), 1);
}

TEST(FrameParserTest, GetData) {
    auto frame = FrameBuilder::Make1Byte(FunCode::ExecRemoteCtrl, 0x42);
    const uint8_t* data = FrameParser::GetData(frame);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data[0], 0x42);
}

TEST(FrameParserTest, ReadByte) {
    FrameBuilder fb;
    fb.Begin(FunCode::CallTelemetry);
    fb.Append(0xA5);
    auto frame = fb.End();
    EXPECT_EQ(FrameParser::ReadByte(frame, 0), 0xA5);
}

TEST(FrameParserTest, ReadU16) {
    FrameBuilder fb;
    fb.Begin(FunCode::CallTelemetry);
    fb.AppendU16(0xAABB);
    auto frame = fb.End();
    EXPECT_EQ(FrameParser::ReadU16(frame, 0), 0xAABB);
}

TEST(FrameParserTest, ReadU32) {
    FrameBuilder fb;
    fb.Begin(FunCode::CallTelemetry);
    fb.AppendU32(0x12345678);
    auto frame = fb.End();
    EXPECT_EQ(FrameParser::ReadU32(frame, 0), 0x12345678);
}

TEST(FrameParserTest, ReadI32) {
    FrameBuilder fb;
    fb.Begin(FunCode::CallTelemetry);
    fb.AppendI32(-12345);
    auto frame = fb.End();
    EXPECT_EQ(FrameParser::ReadI32(frame, 0), -12345);
}

TEST(FrameParserTest, ReadCP56) {
    auto cp56 = CP56time2a::FromTimeT(1000000000);
    FrameBuilder fb;
    fb.Begin(FunCode::CallTelemetry);
    // Manually write the 7 CP56 bytes
    fb.Append(cp56.ms_low);
    fb.Append(cp56.ms_high);
    fb.Append(cp56.minute);
    fb.Append(cp56.hour);
    fb.Append(cp56.day);
    fb.Append(cp56.month);
    fb.Append(cp56.year);
    auto frame = fb.End();

    auto readBack = FrameParser::ReadCP56(frame, 0);
    EXPECT_EQ(readBack.ToTimeT(), cp56.ToTimeT());
}

// ==================== Validation ====================

TEST(FrameParserTest, ValidateValidFrame) {
    auto frame = FrameBuilder::MakeSimple(FunCode::QueryStatus);
    EXPECT_TRUE(FrameParser::Validate(frame));
}

TEST(FrameParserTest, ValidateTooShort) {
    std::vector<uint8_t> frame = {0x7B, 0x01};
    EXPECT_FALSE(FrameParser::Validate(frame));
}

TEST(FrameParserTest, ValidateMissingStart) {
    auto frame = FrameBuilder::MakeSimple(FunCode::CallTelemetry);
    frame[0] = 0x00;
    EXPECT_FALSE(FrameParser::Validate(frame));
}

TEST(FrameParserTest, ValidateMissingEnd) {
    auto frame = FrameBuilder::MakeSimple(FunCode::CallTelemetry);
    frame.back() = 0x00;
    EXPECT_FALSE(FrameParser::Validate(frame));
}

TEST(FrameParserTest, ValidateBadLength) {
    auto frame = FrameBuilder::MakeSimple(FunCode::CallTelemetry);
    frame[2] = 10;  // Claim data len = 10, but only 0 bytes follow
    EXPECT_FALSE(FrameParser::Validate(frame));
}

// ==================== CP56time2a ====================

TEST(CP56time2aTest, RoundTrip) {
    std::time_t original = 1000000000;  // 2001-09-09
    auto cp56 = CP56time2a::FromTimeT(original);
    EXPECT_EQ(cp56.ToTimeT(), original);
}

// CP56time2a: year = year-2000 (uint8_t), only valid for years >=2000.
// Pre-2000 timestamps like epoch 0 (1970) cannot roundtrip.
TEST(CP56time2aTest, RoundTrip2000Plus) {
    std::time_t times[] = {
        946684800,    // 2000-01-01
        1000000000,   // 2001-09-09
        1234567890,   // 2009-02-13
        1700000000,   // 2023-11-14
    };
    for (auto t : times) {
        auto cp56 = CP56time2a::FromTimeT(t);
        EXPECT_EQ(cp56.ToTimeT(), t);
    }
}

TEST(CP56time2aTest, NowIsCloseToSystemTime) {
    auto cp56 = CP56time2a::Now();
    std::time_t now = std::time(nullptr);
    std::time_t cp56time = cp56.ToTimeT();
    // Allow 10s tolerance for test execution
    EXPECT_NEAR(now, cp56time, 10);
}

TEST(CP56time2aTest, ToStringNonEmpty) {
    auto cp56 = CP56time2a::FromTimeT(1000000);
    EXPECT_FALSE(cp56.ToString().empty());
}

// ==================== Integration: Build then Parse ====================

TEST(ProtocolIntegrationTest, BuildThenParseSimple) {
    auto frame = FrameBuilder::MakeSimple(FunCode::SyncClock);
    ASSERT_TRUE(FrameParser::Validate(frame));

    FrameParser parser;
    auto frames = parser.Feed(frame.data(), frame.size());
    ASSERT_EQ(frames.size(), 1);
    EXPECT_EQ(FrameParser::GetFun(frames[0]), FunCode::SyncClock);
}

TEST(ProtocolIntegrationTest, BuildThenParseDataFrame) {
    int32_t vals[] = {10, 20, 30, 40, 50};
    auto frame = FrameBuilder::MakeTelemetryFrame(
        FunCode::UploadTelemetry, 2, 3, vals, 5);
    ASSERT_TRUE(FrameParser::Validate(frame));

    FrameParser parser;
    auto frames = parser.Feed(frame.data(), frame.size());
    ASSERT_EQ(frames.size(), 1);

    auto& f = frames[0];
    EXPECT_EQ(FrameParser::ReadByte(f, 0), 2);
    EXPECT_EQ(FrameParser::ReadByte(f, 1), 3);
    EXPECT_EQ(FrameParser::ReadU16(f, 2), 5);

    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(FrameParser::ReadI32(f, 4 + i * 4), vals[i]);
    }
}

TEST(ProtocolIntegrationTest, MaxSizeFrameHandling) {
    // Build a frame with max allowed data (4092 bytes)
    // This tests the MAX_FRAME_SIZE check in FrameParser::Feed
    std::vector<uint8_t> largeData(4092, 0x42);
    FrameBuilder fb;
    fb.Begin(FunCode::UploadTelemetry);
    fb.Append(largeData.data(), largeData.size());
    auto frame = fb.End();

    ASSERT_TRUE(FrameParser::Validate(frame));

    FrameParser parser;
    auto frames = parser.Feed(frame.data(), frame.size());
    ASSERT_EQ(frames.size(), 1);
    EXPECT_EQ(FrameParser::GetDataLen(frames[0]), 4092);
}
