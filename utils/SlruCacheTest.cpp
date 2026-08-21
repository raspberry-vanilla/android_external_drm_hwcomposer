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

#include "utils/SlruCache.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// NOLINTBEGIN(readability-magic-numbers)

namespace android::drm_hwcomposer {

TEST(SlruCacheTest, BasicInsertAndFind) {
  SlruCache<int, std::shared_ptr<int>, 2, 2> cache;

  EXPECT_EQ(cache.Find(1), nullptr);

  auto val1 = std::make_shared<int>(100);
  cache.Insert(1, val1);
  EXPECT_EQ(cache.Size(), 1U);
  EXPECT_EQ(cache.ProbationarySize(), 1U);
  EXPECT_EQ(cache.ProtectedSize(), 0U);

  auto found = cache.Find(1);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(*found, 100);
  // After Find, promoted to protected segment
  EXPECT_EQ(cache.ProbationarySize(), 0U);
  EXPECT_EQ(cache.ProtectedSize(), 1U);
}

TEST(SlruCacheTest, ProbationaryEvictionDoesNotEvictProtected) {
  // Capacity: 2 probationary, 2 protected
  SlruCache<std::string, std::shared_ptr<int>, 2, 2> cache;

  // Insert "hot" steady-state item and promote it to protected
  auto hot_val = std::make_shared<int>(999);
  cache.Insert("hot", hot_val);
  EXPECT_EQ(cache.Find("hot"), hot_val);
  EXPECT_EQ(cache.ProtectedSize(), 1U);
  EXPECT_EQ(cache.ProbationarySize(), 0U);

  // Stream transient one-off items through probationary
  for (int i = 0; i < 10; ++i) {
    std::string key = "transient_" + std::to_string(i);
    cache.Insert(key, std::make_shared<int>(i));
    // Each transient item enters probationary and is never looked up again
  }

  // "hot" item must still reside safely in protected segment!
  EXPECT_EQ(cache.ProtectedSize(), 1U);
  auto found_hot = cache.Find("hot");
  ASSERT_NE(found_hot, nullptr);
  EXPECT_EQ(*found_hot, 999);

  // Probationary only holds the latest 2 transient items
  EXPECT_EQ(cache.ProbationarySize(), 2U);
  EXPECT_EQ(cache.Find("transient_0"), nullptr);
  EXPECT_EQ(cache.Find("transient_7"), nullptr);
  EXPECT_NE(cache.Find("transient_8"), nullptr);
  EXPECT_NE(cache.Find("transient_9"), nullptr);
}

TEST(SlruCacheTest, ProtectedDemotesToProbationaryOnOverflow) {
  // Capacity: 2 probationary, 2 protected
  SlruCache<int, std::shared_ptr<int>, 2, 2> cache;

  // Fill protected with 1 and 2
  cache.Insert(1, std::make_shared<int>(1));
  cache.Find(1);  // promoted to protected
  cache.Insert(2, std::make_shared<int>(2));
  cache.Find(2);  // promoted to protected
  EXPECT_EQ(cache.ProtectedSize(), 2U);
  EXPECT_EQ(cache.ProbationarySize(), 0U);

  // Insert 3 and promote it to protected -> item 1 should be demoted to
  // probationary
  cache.Insert(3, std::make_shared<int>(3));
  cache.Find(3);  // promoted to protected
  EXPECT_EQ(cache.ProtectedSize(), 2U);
  EXPECT_EQ(cache.ProbationarySize(), 1U);

  // 1 is still in cache (in probationary)
  EXPECT_NE(cache.Find(1), nullptr);
  // Re-finding 1 promotes it back to protected, demoting 2
  EXPECT_EQ(cache.ProtectedSize(), 2U);
  EXPECT_EQ(cache.ProbationarySize(), 1U);
}

TEST(SlruCacheTest, ClearEmptiesBothSegments) {
  SlruCache<int, std::shared_ptr<int>, 2, 2> cache;
  cache.Insert(1, std::make_shared<int>(1));
  cache.Find(1);                              // protected
  cache.Insert(2, std::make_shared<int>(2));  // probationary

  EXPECT_EQ(cache.Size(), 2U);
  cache.Clear();
  EXPECT_EQ(cache.Size(), 0U);
  EXPECT_EQ(cache.ProbationarySize(), 0U);
  EXPECT_EQ(cache.ProtectedSize(), 0U);
  EXPECT_EQ(cache.Find(1), nullptr);
}

TEST(SlruCacheTest, ConcurrentAccessStressTest) {
  SlruCache<int, std::shared_ptr<int>, 16, 48> cache;
  constexpr int kNumThreads = 8;
  constexpr int kIterations = 1000;

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);

  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&cache, t]() {
      for (int i = 0; i < kIterations; ++i) {
        int key = (t * 100) + (i % 20);
        if (i % 2 == 0) {
          cache.Insert(key, std::make_shared<int>(key));
        } else {
          cache.Find(key);
        }
      }
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_LE(cache.Size(), 64U);
}

TEST(SlruCacheTest, OverwriteExistingKey) {
  SlruCache<int, std::shared_ptr<int>, 2, 2> cache;

  // Insert into probationary and overwrite
  cache.Insert(1, std::make_shared<int>(10));
  EXPECT_EQ(cache.ProbationarySize(), 1U);
  cache.Insert(1, std::make_shared<int>(11));
  EXPECT_EQ(cache.ProbationarySize(), 1U);
  EXPECT_EQ(*cache.Find(1), 11);
  EXPECT_EQ(cache.ProtectedSize(), 1U);

  // Overwrite while in protected
  cache.Insert(1, std::make_shared<int>(12));
  EXPECT_EQ(cache.ProtectedSize(), 1U);
  EXPECT_EQ(cache.ProbationarySize(), 0U);
  EXPECT_EQ(*cache.Find(1), 12);
}

TEST(SlruCacheTest, CapacityOneByOne) {
  SlruCache<int, std::shared_ptr<int>, 1, 1> cache;

  cache.Insert(1, std::make_shared<int>(1));
  EXPECT_EQ(cache.ProbationarySize(), 1U);
  EXPECT_EQ(cache.ProtectedSize(), 0U);

  EXPECT_EQ(*cache.Find(1), 1);
  EXPECT_EQ(cache.ProbationarySize(), 0U);
  EXPECT_EQ(cache.ProtectedSize(), 1U);

  cache.Insert(2, std::make_shared<int>(2));
  EXPECT_EQ(cache.ProbationarySize(), 1U);
  EXPECT_EQ(cache.ProtectedSize(), 1U);

  // Promoting 2 demotes 1 to probationary
  EXPECT_EQ(*cache.Find(2), 2);
  EXPECT_EQ(cache.ProbationarySize(), 1U);
  EXPECT_EQ(cache.ProtectedSize(), 1U);

  // Inserting 3 evicts demoted 1 from probationary
  cache.Insert(3, std::make_shared<int>(3));
  EXPECT_EQ(cache.Find(1), nullptr);
  EXPECT_NE(cache.Find(2), nullptr);
}

TEST(SlruCacheTest, EvictionOfDemotedItems) {
  SlruCache<int, std::shared_ptr<int>, 2, 2> cache;

  // Populate and promote 1 and 2 to protected
  cache.Insert(1, std::make_shared<int>(1));
  cache.Find(1);
  cache.Insert(2, std::make_shared<int>(2));
  cache.Find(2);

  // Promote 3, demoting 1 to probationary
  cache.Insert(3, std::make_shared<int>(3));
  cache.Find(3);
  EXPECT_EQ(cache.ProtectedSize(), 2U);
  EXPECT_EQ(cache.ProbationarySize(), 1U);

  // Fill probationary with transient items 4 and 5, which evicts demoted 1
  cache.Insert(4, std::make_shared<int>(4));
  cache.Insert(5, std::make_shared<int>(5));
  EXPECT_EQ(cache.ProbationarySize(), 2U);
  EXPECT_EQ(cache.Find(1), nullptr);
  EXPECT_NE(cache.Find(2), nullptr);
  EXPECT_NE(cache.Find(3), nullptr);
}

TEST(SlruCacheTest, HitInProbationaryWhenBothSegmentsFull) {
  // Capacity: 2 probationary, 2 protected
  SlruCache<int, std::shared_ptr<int>, 2, 2> cache;

  // Fill protected with 1 and 2
  cache.Insert(1, std::make_shared<int>(1));
  cache.Find(1);
  cache.Insert(2, std::make_shared<int>(2));
  cache.Find(2);
  EXPECT_EQ(cache.ProtectedSize(), 2U);
  EXPECT_EQ(cache.ProbationarySize(), 0U);

  // Fill probationary with 3 and 4
  cache.Insert(3, std::make_shared<int>(3));
  cache.Insert(4, std::make_shared<int>(4));
  EXPECT_EQ(cache.ProbationarySize(), 2U);
  EXPECT_EQ(cache.Size(), 4U);

  // Finding 3 (front of probationary) promotes 3, demotes 1, and preserves 4
  EXPECT_NE(cache.Find(3), nullptr);
  EXPECT_EQ(cache.Size(), 4U);
  EXPECT_EQ(cache.ProtectedSize(), 2U);
  EXPECT_EQ(cache.ProbationarySize(), 2U);
  EXPECT_NE(cache.Find(1), nullptr);
  EXPECT_NE(cache.Find(2), nullptr);
  EXPECT_NE(cache.Find(4), nullptr);
}

TEST(SlruCacheTest, DumpFormatting) {
  SlruCache<int, std::shared_ptr<int>, 2, 2> cache;
  cache.Insert(1, std::make_shared<int>(10));
  cache.Find(1);                               // protected
  cache.Insert(2, std::make_shared<int>(20));  // probationary

  auto dump = cache.Dump(
      [](const int &k, const std::shared_ptr<int> &v) {
        return "key=" + std::to_string(k) + " val=" + std::to_string(*v);
      },
      "Test Cache");

  EXPECT_NE(dump.find("Test Cache: 2/4 (Probationary: 1/2, Protected: 1/2)"),
            std::string::npos);
  EXPECT_NE(dump.find("[Protected]    key=1 val=10"), std::string::npos);
  EXPECT_NE(dump.find("[Probationary] key=2 val=20"), std::string::npos);
}

TEST(SlruCacheTest, DumpFormattingEmptyCache) {
  SlruCache<int, std::shared_ptr<int>, 2, 2> cache;
  auto dump = cache.Dump(
      [](const int &, const std::shared_ptr<int> &) { return ""; });
  EXPECT_EQ(dump, "SLRU Cache: 0/4 (Probationary: 0/2, Protected: 0/2)\n");
}
}  // namespace android::drm_hwcomposer

// NOLINTEND(readability-magic-numbers)
