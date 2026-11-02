/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// NOLINTBEGIN(readability-magic-numbers)

// Runs only where the host test binary links the real libdisplay-info
// (.ci/Makefile.unittests); other builds compile an empty translation unit.
#if HAS_LIBDISPLAY_INFO

#include <gtest/gtest.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "drm/DrmUnique.h"
#include "utils/EdidWrapper.h"

namespace android::drm_hwcomposer {
namespace {

constexpr size_t kEdidBlockSize = 128;
constexpr size_t kManufacturerIdOffset = 8;
constexpr size_t kProductCodeOffset = 10;
constexpr size_t kVersionOffset = 18;
constexpr size_t kVideoInputDefOffset = 20;
constexpr size_t kFirstDescriptorOffset = 0x36;
constexpr size_t kDescriptorSize = 18;
constexpr size_t kDescriptorCount = 4;
constexpr size_t kDescriptorTagOffset = 3;
constexpr size_t kDescriptorTextOffset = 5;
constexpr size_t kChecksumOffset = 127;
constexpr uint8_t kProductNameTag = 0xFC;
constexpr uint8_t kDummyDescriptorTag = 0x10;
constexpr uint8_t kDigitalVideoInput = 0x80;
constexpr char kTestProductName[] = "TestPanel";
// "GSM" packed as three 5-bit letters ('A' == 1), listed in hwdata pnp.ids.
constexpr std::array<uint8_t, 2> kGsmManufacturerId = {0x1E, 0x6D};
constexpr char kGsmVendorName[] = "LG Electronics";

// Builds a checksummed EDID 1.4 base block whose first display descriptor is
// a Display Product Name and whose remaining descriptors are dummies.
std::vector<uint8_t> MakeTestEdid() {
  std::vector<uint8_t> edid(kEdidBlockSize, 0);
  std::fill(edid.begin() + 1, edid.begin() + 7, 0xFF);
  edid[kManufacturerIdOffset] = kGsmManufacturerId[0];
  edid[kManufacturerIdOffset + 1] = kGsmManufacturerId[1];
  edid[kProductCodeOffset] = 0x34;
  edid[kProductCodeOffset + 1] = 0x12;
  edid[kVersionOffset] = 1;
  edid[kVersionOffset + 1] = 4;
  edid[kVideoInputDefOffset] = kDigitalVideoInput;

  for (size_t i = 0; i < kDescriptorCount; ++i) {
    edid[kFirstDescriptorOffset + (i * kDescriptorSize) +
         kDescriptorTagOffset] = kDummyDescriptorTag;
  }
  edid[kFirstDescriptorOffset + kDescriptorTagOffset] = kProductNameTag;
  const std::string name = std::string(kTestProductName) + "\n";
  auto text = edid.begin() + kFirstDescriptorOffset + kDescriptorTextOffset;
  std::fill(text, text + (kDescriptorSize - kDescriptorTextOffset), ' ');
  std::copy(name.begin(), name.end(), text);

  uint8_t sum = 0;
  for (size_t i = 0; i < kChecksumOffset; ++i) {
    sum = static_cast<uint8_t>(sum + edid[i]);
  }
  edid[kChecksumOffset] = static_cast<uint8_t>(0U - sum);
  return edid;
}

class LibdisplayEdidWrapperTest : public ::testing::Test {
 protected:
  // The returned blob borrows edid_bytes_ and blob_res_ from the fixture.
  DrmModePropertyBlobUnique MakeEdidBlob() {
    blob_res_ = drmModePropertyBlobRes{
        .id = 1,
        .length = static_cast<uint32_t>(edid_bytes_.size()),
        .data = edid_bytes_.data(),
    };
    return {&blob_res_, [](drmModePropertyBlobRes * /*blob*/) {}};
  }

  std::vector<uint8_t> edid_bytes_ = MakeTestEdid();
  drmModePropertyBlobRes blob_res_{};
};

TEST_F(LibdisplayEdidWrapperTest, CreateReturnsNullWhenBlobIsNull) {
  EXPECT_EQ(LibdisplayEdidWrapper::Create(nullptr), nullptr);
}

TEST_F(LibdisplayEdidWrapperTest, GetVendorProductInfoReturnsPnpVendorAsMake) {
  auto wrapper = LibdisplayEdidWrapper::Create(MakeEdidBlob());
  ASSERT_NE(wrapper, nullptr);

  EXPECT_EQ(wrapper->GetVendorProductInfo().make, kGsmVendorName);
}

TEST_F(LibdisplayEdidWrapperTest,
       GetVendorProductInfoReturnsProductNameAsModel) {
  auto wrapper = LibdisplayEdidWrapper::Create(MakeEdidBlob());
  ASSERT_NE(wrapper, nullptr);

  EXPECT_EQ(wrapper->GetVendorProductInfo().model, kTestProductName);
}

// Records the strings handed out by the real libdisplay-info getters and every
// pointer released through VendorStringOps::free_string. Function pointers
// cannot capture, so the recorder is file-static and reset per test.
struct StringOpsRecorder {
  bool null_make = false;
  bool null_model = false;
  void *make = nullptr;
  void *model = nullptr;
  std::vector<void *> freed;
};

StringOpsRecorder &GetRecorder() {
  static StringOpsRecorder recorder;
  return recorder;
}

char *RecordingGetMake(const di_info *info) {
  if (GetRecorder().null_make) {
    return nullptr;
  }
  char *make = di_info_get_make(info);
  GetRecorder().make = make;
  return make;
}

char *RecordingGetModel(const di_info *info) {
  if (GetRecorder().null_model) {
    return nullptr;
  }
  char *model = di_info_get_model(info);
  GetRecorder().model = model;
  return model;
}

void RecordingFree(void *ptr) {
  GetRecorder().freed.push_back(ptr);
  std::free(ptr);
}

}  // namespace

class LibdisplayEdidWrapperStringOpsTest : public LibdisplayEdidWrapperTest {
 protected:
  void SetUp() override {
    GetRecorder() = StringOpsRecorder{};
  }

  EdidWrapper::VendorProductInfo GetVendorProductInfo() {
    auto wrapper = LibdisplayEdidWrapper::
        Create(MakeEdidBlob(), {
                                   .get_make = RecordingGetMake,
                                   .get_model = RecordingGetModel,
                                   .free_string = RecordingFree,
                               });
    EXPECT_NE(wrapper, nullptr);
    return wrapper ? wrapper->GetVendorProductInfo()
                   : EdidWrapper::VendorProductInfo{};
  }

  static size_t FreeCount(const void *ptr) {
    const auto &freed = GetRecorder().freed;
    return static_cast<size_t>(std::count(freed.begin(), freed.end(), ptr));
  }
};

TEST_F(LibdisplayEdidWrapperStringOpsTest,
       GetVendorProductInfoFreesMakeString) {
  const auto info = GetVendorProductInfo();

  EXPECT_EQ(info.make, kGsmVendorName);
  ASSERT_NE(GetRecorder().make, nullptr);
  EXPECT_EQ(FreeCount(GetRecorder().make), 1U);
}

TEST_F(LibdisplayEdidWrapperStringOpsTest,
       GetVendorProductInfoFreesModelString) {
  const auto info = GetVendorProductInfo();

  EXPECT_EQ(info.model, kTestProductName);
  ASSERT_NE(GetRecorder().model, nullptr);
  EXPECT_EQ(FreeCount(GetRecorder().model), 1U);
}

TEST_F(LibdisplayEdidWrapperStringOpsTest,
       GetVendorProductInfoReturnsEmptyMakeWhenGetterReturnsNull) {
  GetRecorder().null_make = true;

  const auto info = GetVendorProductInfo();

  EXPECT_EQ(info.make, "");
  EXPECT_EQ(info.model, kTestProductName);
}

TEST_F(LibdisplayEdidWrapperStringOpsTest,
       GetVendorProductInfoReturnsEmptyModelWhenGetterReturnsNull) {
  GetRecorder().null_model = true;

  const auto info = GetVendorProductInfo();

  EXPECT_EQ(info.make, kGsmVendorName);
  EXPECT_EQ(info.model, "");
}

}  // namespace android::drm_hwcomposer

#endif  // HAS_LIBDISPLAY_INFO

// NOLINTEND(readability-magic-numbers)
