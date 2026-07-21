#include "../recorder/gstreamer_recorder.hpp"

#include <gtest/gtest.h>

TEST(GStreamerRecorderTest, TargetsPortalStreamByObjectSerial) {
  clipdeck::RecorderConfig config;
  config.audio_enabled = false;
  clipdeck::GStreamerRecorder recorder(config);

  recorder.SetPortalTestTargetObject(42, 103, "987654321");

  const std::string pipeline = recorder.BuildPipelineDescriptionForTest();

  EXPECT_NE(
      pipeline.find(
          "pipewiresrc name=clipdeck_screen_source client-name=ClipDeck fd=42"),
      std::string::npos);
  EXPECT_NE(pipeline.find("target-object=\"987654321\""), std::string::npos);
  EXPECT_EQ(pipeline.find(" path="), std::string::npos);
  EXPECT_EQ(pipeline.find("autoconnect=false"), std::string::npos);
  EXPECT_EQ(pipeline.find("pulsesrc"), std::string::npos);
  EXPECT_NE(
      pipeline.find(
          "video/x-raw,format=I420,width=1920,height=1080,framerate=60/1"),
      std::string::npos);
  EXPECT_NE(pipeline.find("max-files=0"), std::string::npos);
  EXPECT_NE(pipeline.find("send-keyframe-requests=true"), std::string::npos);
  EXPECT_EQ(pipeline.find("tune=zerolatency"), std::string::npos);
}

TEST(GStreamerRecorderTest, UsesPortalNodePathForLegacyNodeFallback) {
  clipdeck::RecorderConfig config;
  config.audio_enabled = false;
  clipdeck::GStreamerRecorder recorder(config);

  recorder.SetPortalTestPath(42, 103);

  const std::string pipeline = recorder.BuildPipelineDescriptionForTest();

  EXPECT_NE(
      pipeline.find(
          "pipewiresrc name=clipdeck_screen_source client-name=ClipDeck fd=42"),
      std::string::npos);
  EXPECT_NE(pipeline.find("path=\"103\""), std::string::npos);
  EXPECT_EQ(pipeline.find("target-object="), std::string::npos);
  EXPECT_EQ(pipeline.find("autoconnect=false"), std::string::npos);
}

TEST(GStreamerRecorderTest, AddsAudioOnlyWhenEnabledAndResolved) {
  clipdeck::RecorderConfig config;
  config.audio_enabled = true;
  config.audio_source = "alsa_output.test.monitor";
  clipdeck::GStreamerRecorder recorder(config);

  recorder.SetPortalTestTargetObject(42, 103, "987654321");

  const std::string pipeline = recorder.BuildPipelineDescriptionForTest();

  EXPECT_NE(pipeline.find("pulsesrc name=clipdeck_output_audio_source "
                          "device=\"alsa_output.test.monitor\""),
            std::string::npos);
  EXPECT_NE(pipeline.find("audioconvert"), std::string::npos);
  EXPECT_NE(pipeline.find("audioresample"), std::string::npos);
  EXPECT_NE(pipeline.find("audiorate"), std::string::npos);
  EXPECT_NE(pipeline.find("rate=48000"), std::string::npos);
  EXPECT_NE(pipeline.find("channels=2"), std::string::npos);
  EXPECT_NE(pipeline.find("aacparse"), std::string::npos);
  const bool uses_fdk = pipeline.find("fdkaacenc") != std::string::npos;
  const bool uses_avenc = pipeline.find("avenc_aac") != std::string::npos;
  EXPECT_TRUE(uses_fdk || uses_avenc);
  if (uses_fdk) {
    EXPECT_NE(pipeline.find("audio/x-raw,format=S16LE"), std::string::npos);
  } else {
    EXPECT_NE(pipeline.find("audio/x-raw,format=F32LE"), std::string::npos);
  }
}

TEST(GStreamerRecorderTest, ReportsPortalCaptureSourceType) {
  clipdeck::RecorderConfig config;
  config.audio_enabled = false;
  clipdeck::GStreamerRecorder recorder(config);

  recorder.SetPortalTestCaptureSourceType("window");

  EXPECT_EQ(recorder.Status().capture_source_type, "window");
}
