// Bridge 契约测试：Frame / IFrameStorage。
// 业务层（event.cpp 的帧缓冲、analyzer.cpp）只应该见到 Frame，
// 不应该知道像素在 Host 还是 Device —— 这组测试把这件事钉死。
#include <gtest/gtest.h>

#include "imaging/frame.h"
#include "imaging/frame_storage.h"

#include "fake_frame_migrator.h"
#include "test_utils.h"

namespace avs_analyzer::imaging {
namespace {

std::shared_ptr<HostFrameStorage> MakeHost(Size s, PixelFormat f, int64_t pts,
                                           uchar fill = 0x5A) {
  std::vector<std::byte> buf(FrameBytes(s, f));
  std::fill(buf.begin(), buf.end(), std::byte{fill});
  return std::make_shared<HostFrameStorage>(std::move(buf), s, f, pts);
}

// ---------------------------------------------------------------------------
// HostFrameStorage
// ---------------------------------------------------------------------------

TEST(HostFrameStorageTest, ReportsMetadataAndLocation) {
  auto s = MakeHost(Size{320, 240}, PixelFormat::kBgr24, 1234);
  EXPECT_EQ(s->location(), MemoryLocation::kHost);
  EXPECT_EQ(s->size().w, 320);
  EXPECT_EQ(s->size().h, 240);
  EXPECT_EQ(s->format(), PixelFormat::kBgr24);
  EXPECT_EQ(s->pts(), 1234);
  EXPECT_EQ(s->byte_size(), FrameBytes(Size{320, 240}, PixelFormat::kBgr24));
  EXPECT_NE(s->data(), nullptr);
  EXPECT_EQ(s->device_id(), -1);
}

TEST(HostFrameStorageTest, Nv12SizeIsOneAndHalfTimesPixels) {
  EXPECT_EQ(FrameBytes(Size{640, 640}, PixelFormat::kNv12),
            static_cast<size_t>(640 * 640 * 3 / 2));
}

// ---------------------------------------------------------------------------
// DeviceFrameStorage（不接触真实 NPU，只验证元数据与"非持有"语义）
// ---------------------------------------------------------------------------

TEST(DeviceFrameStorageTest, ReportsDeviceLocationAndId) {
  std::vector<std::byte> backing(640 * 640 * 3 / 2);
  DeviceFrameStorage s(backing.data(), backing.size(), Size{640, 640},
                       PixelFormat::kNv12, /*pts=*/7, /*device_id=*/3);
  EXPECT_EQ(s.location(), MemoryLocation::kDevice);
  EXPECT_EQ(s.device_id(), 3);
  EXPECT_EQ(s.size().w, 640);
  EXPECT_EQ(s.format(), PixelFormat::kNv12);
  EXPECT_EQ(s.pts(), 7);
  EXPECT_EQ(s.data(), backing.data());
}

// ---------------------------------------------------------------------------
// Frame：按需搬运 + 只搬一次
// ---------------------------------------------------------------------------

TEST(FrameTest, HostPreferredHostStorageNeverMigrates) {
  auto migrator = std::make_shared<test::FakeFrameMigrator>();
  Frame f(MakeHost(Size{1920, 1080}, PixelFormat::kNv12, 0),
          MemoryLocation::kHost, migrator);

  auto p1 = f.pixels();
  auto p2 = f.pixels();

  EXPECT_EQ(migrator->to_host_calls, 0);
  EXPECT_EQ(migrator->to_device_calls, 0);
  EXPECT_EQ(p1, p2) << "同位置重复取用应返回同一份存储";
  EXPECT_EQ(p1->location(), MemoryLocation::kHost);
}

TEST(FrameTest, DeviceFrameMigratesToHostExactlyOnce) {
  auto migrator = std::make_shared<test::FakeFrameMigrator>();
  std::vector<std::byte> backing(FrameBytes(Size{320, 240}, PixelFormat::kBgr24),
                                 std::byte{0x11});
  auto device = std::make_shared<DeviceFrameStorage>(
      backing.data(), backing.size(), Size{320, 240}, PixelFormat::kBgr24,
      /*pts=*/42, /*device_id=*/1);

  Frame f(device, MemoryLocation::kHost, migrator);

  auto a = f.pixels();
  auto b = f.pixels();
  auto c = f.pixels();

  EXPECT_EQ(migrator->to_host_calls, 1) << "必须缓存，不能每帧重复 D2H";
  EXPECT_EQ(a.get(), b.get());
  EXPECT_EQ(b.get(), c.get());
  EXPECT_EQ(a->location(), MemoryLocation::kHost);
  ASSERT_EQ(a->byte_size(), backing.size());
  EXPECT_EQ(static_cast<uchar>(std::to_integer<uchar>(*(a->data()))), 0x11)
      << "搬运必须真的把数据带过来";
}

TEST(FrameTest, DevicePreferredDeviceStorageNeverMigrates) {
  auto migrator = std::make_shared<test::FakeFrameMigrator>();
  std::vector<std::byte> backing(FrameBytes(Size{320, 240}, PixelFormat::kNv12));
  auto device = std::make_shared<DeviceFrameStorage>(
      backing.data(), backing.size(), Size{320, 240}, PixelFormat::kNv12, 0, 2);

  Frame f(device, MemoryLocation::kDevice, migrator);
  (void)f.pixels();

  EXPECT_EQ(migrator->to_host_calls, 0);
  EXPECT_EQ(migrator->to_device_calls, 0);
  EXPECT_EQ(f.pixels()->location(), MemoryLocation::kDevice);
}

TEST(FrameTest, HostFrameMigratesToDeviceWhenPreferredIsDevice) {
  auto migrator = std::make_shared<test::FakeFrameMigrator>();
  Frame f(MakeHost(Size{320, 240}, PixelFormat::kBgr24, 0),
          MemoryLocation::kDevice, migrator);

  auto p = f.pixels();
  EXPECT_EQ(migrator->to_device_calls, 1);
  EXPECT_EQ(migrator->last_device_id, -1) << "未指定 device 时不应臆造";
  EXPECT_EQ(p->location(), MemoryLocation::kDevice);
}

TEST(FrameTest, ExposesUnderlyingMetadataRegardlessOfLocation) {
  auto migrator = std::make_shared<test::FakeFrameMigrator>();
  std::vector<std::byte> backing(FrameBytes(Size{1280, 720}, PixelFormat::kNv12));
  auto device = std::make_shared<DeviceFrameStorage>(
      backing.data(), backing.size(), Size{1280, 720}, PixelFormat::kNv12,
      /*pts=*/99, /*device_id=*/0);

  Frame f(device, MemoryLocation::kDevice, migrator);
  EXPECT_EQ(f.size().w, 1280);
  EXPECT_EQ(f.size().h, 720);
  EXPECT_EQ(f.format(), PixelFormat::kNv12);
  EXPECT_EQ(f.pts(), 99);
}

}  // namespace
}  // namespace avs_analyzer::imaging
