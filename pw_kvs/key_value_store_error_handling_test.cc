// Copyright 2020 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#include <string_view>

#include "gtest/gtest.h"
#include "pw_kvs/crc16_checksum.h"
#include "pw_kvs/in_memory_fake_flash.h"
#include "pw_kvs/internal/hash.h"
#include "pw_kvs/key_value_store.h"
#include "pw_kvs_private/byte_utils.h"

namespace pw::kvs {
namespace {

using std::byte;
using std::string_view;

constexpr size_t kMaxEntries = 256;
constexpr size_t kMaxUsableSectors = 256;

constexpr uint32_t SimpleChecksum(span<const byte> data, uint32_t state = 0) {
  for (byte b : data) {
    state += uint32_t(b);
  }
  return state;
}

class SimpleChecksumAlgorithm final : public ChecksumAlgorithm {
 public:
  SimpleChecksumAlgorithm()
      : ChecksumAlgorithm(as_bytes(span(&checksum_, 1))) {}

  void Reset() override { checksum_ = 0; }

  void Update(span<const byte> data) override {
    checksum_ = SimpleChecksum(data, checksum_);
  }

 private:
  uint32_t checksum_;
} checksum;

// Returns a buffer containing the necessary padding for an entry.
template <size_t kAlignmentBytes, size_t kKeyLength, size_t kValueSize>
constexpr auto EntryPadding() {
  constexpr size_t content =
      sizeof(internal::EntryHeader) + kKeyLength + kValueSize;
  return std::array<byte, Padding(content, kAlignmentBytes)>{};
}

// Creates a buffer containing a valid entry at compile time.
template <size_t kAlignmentBytes = sizeof(internal::EntryHeader),
          size_t kKeyLengthWithNull,
          size_t kValueSize>
constexpr auto MakeValidEntry(uint32_t magic,
                              uint32_t id,
                              const char (&key)[kKeyLengthWithNull],
                              const std::array<byte, kValueSize>& value) {
  constexpr size_t kKeyLength = kKeyLengthWithNull - 1;

  auto data = AsBytes(magic,
                      uint32_t(0),
                      uint8_t(kAlignmentBytes / 16 - 1),
                      uint8_t(kKeyLength),
                      uint16_t(kValueSize),
                      id,
                      ByteStr(key),
                      span(value),
                      EntryPadding<kAlignmentBytes, kKeyLength, kValueSize>());

  // Calculate the checksum
  uint32_t checksum = SimpleChecksum(data);
  for (size_t i = 0; i < sizeof(checksum); ++i) {
    data[4 + i] = byte(checksum & 0xff);
    checksum >>= 8;
  }

  return data;
}

constexpr uint32_t kMagic = 0xc001beef;
constexpr EntryFormat kFormat{.magic = kMagic, .checksum = &checksum};
constexpr Options kNoGcOptions{
    .gc_on_write = GargbageCollectOnWrite::kDisabled,
    .verify_on_read = true,
    .verify_on_write = true,
};

constexpr auto kEntry1 = MakeValidEntry(kMagic, 1, "key1", ByteStr("value1"));
constexpr auto kEntry2 = MakeValidEntry(kMagic, 3, "k2", ByteStr("value2"));
constexpr auto kEntry3 = MakeValidEntry(kMagic, 4, "k3y", ByteStr("value3"));
constexpr auto kEntry4 = MakeValidEntry(kMagic, 5, "4k", ByteStr("value4"));

class KvsErrorHandling : public ::testing::Test {
 protected:
  KvsErrorHandling()
      : flash_(internal::Entry::kMinAlignmentBytes),
        partition_(&flash_),
        kvs_(&partition_, kFormat, kNoGcOptions) {}

  void InitFlashTo(span<const byte> contents) {
    partition_.Erase();
    std::memcpy(flash_.buffer().data(), contents.data(), contents.size());
  }

  FakeFlashBuffer<512, 4> flash_;
  FlashPartition partition_;
  KeyValueStoreBuffer<kMaxEntries, kMaxUsableSectors> kvs_;
};

TEST_F(KvsErrorHandling, Init_Ok) {
  InitFlashTo(AsBytes(kEntry1, kEntry2));

  EXPECT_EQ(Status::OK, kvs_.Init());
  byte buffer[64];
  EXPECT_EQ(Status::OK, kvs_.Get("key1", buffer).status());
  EXPECT_EQ(Status::OK, kvs_.Get("k2", buffer).status());
}

TEST_F(KvsErrorHandling, Init_DuplicateEntries_ReturnsDataLossButReadsEntry) {
  InitFlashTo(AsBytes(kEntry1, kEntry1));

  EXPECT_EQ(Status::DATA_LOSS, kvs_.Init());
  byte buffer[64];
  EXPECT_EQ(Status::OK, kvs_.Get("key1", buffer).status());
  EXPECT_EQ(Status::NOT_FOUND, kvs_.Get("k2", buffer).status());
}

TEST_F(KvsErrorHandling, Init_CorruptEntry_FindsSubsequentValidEntry) {
  // Corrupt each byte in the first entry once.
  for (size_t i = 0; i < kEntry1.size(); ++i) {
    InitFlashTo(AsBytes(kEntry1, kEntry2));
    flash_.buffer()[i] = byte(int(flash_.buffer()[i]) + 1);

    ASSERT_EQ(Status::DATA_LOSS, kvs_.Init());
    byte buffer[64];
    ASSERT_EQ(Status::NOT_FOUND, kvs_.Get("key1", buffer).status());
    ASSERT_EQ(Status::OK, kvs_.Get("k2", buffer).status());

    auto stats = kvs_.GetStorageStats();
    // One valid entry.
    ASSERT_EQ(32u, stats.in_use_bytes);
    // Rest of space is reclaimable as the sector is corrupt.
    ASSERT_EQ(480u, stats.reclaimable_bytes);
  }
}

TEST_F(KvsErrorHandling, Init_CorruptEntry_CorrectlyAccountsForSectorSize) {
  InitFlashTo(AsBytes(kEntry1, kEntry2, kEntry3, kEntry4));

  // Corrupt the first and third entries.
  flash_.buffer()[9] = byte(0xef);
  flash_.buffer()[67] = byte(0xef);

  ASSERT_EQ(Status::DATA_LOSS, kvs_.Init());

  EXPECT_EQ(2u, kvs_.size());

  byte buffer[64];
  EXPECT_EQ(Status::NOT_FOUND, kvs_.Get("key1", buffer).status());
  EXPECT_EQ(Status::OK, kvs_.Get("k2", buffer).status());
  EXPECT_EQ(Status::NOT_FOUND, kvs_.Get("k3y", buffer).status());
  EXPECT_EQ(Status::OK, kvs_.Get("4k", buffer).status());

  auto stats = kvs_.GetStorageStats();
  ASSERT_EQ(64u, stats.in_use_bytes);
  ASSERT_EQ(448u, stats.reclaimable_bytes);
  ASSERT_EQ(1024u, stats.writable_bytes);
}

TEST_F(KvsErrorHandling, Init_ReadError_IsNotInitialized) {
  InitFlashTo(AsBytes(kEntry1, kEntry2));

  flash_.InjectReadError(
      FlashError::InRange(Status::UNAUTHENTICATED, kEntry1.size()));

  EXPECT_EQ(Status::UNKNOWN, kvs_.Init());
  EXPECT_FALSE(kvs_.initialized());
}

TEST_F(KvsErrorHandling, Init_CorruptSectors_ShouldBeUnwritable) {
  InitFlashTo(AsBytes(kEntry1, kEntry2));

  // Corrupt 3 of the 4 512-byte flash sectors. Corrupt sectors should be
  // unwritable, and the KVS must maintain one empty sector at all times.
  // As GC on write is disabled through KVS options, writes should no longer be
  // possible due to lack of space.
  flash_.buffer()[1] = byte(0xef);
  flash_.buffer()[513] = byte(0xef);
  flash_.buffer()[1025] = byte(0xef);

  ASSERT_EQ(Status::DATA_LOSS, kvs_.Init());
  EXPECT_EQ(Status::RESOURCE_EXHAUSTED, kvs_.Put("hello", ByteStr("world")));
  EXPECT_EQ(Status::RESOURCE_EXHAUSTED, kvs_.Put("a", ByteStr("b")));

  // Existing valid entries should still be readable.
  EXPECT_EQ(1u, kvs_.size());
  byte buffer[64];
  EXPECT_EQ(Status::NOT_FOUND, kvs_.Get("key1", buffer).status());
  EXPECT_EQ(Status::OK, kvs_.Get("k2", buffer).status());

  auto stats = kvs_.GetStorageStats();
  EXPECT_EQ(32u, stats.in_use_bytes);
  EXPECT_EQ(480u + 2 * 512u, stats.reclaimable_bytes);
  EXPECT_EQ(0u, stats.writable_bytes);
}

TEST_F(KvsErrorHandling, Init_CorruptSectors_ShouldRecoverOne) {
  InitFlashTo(AsBytes(kEntry1, kEntry2));

  // Corrupt all of the 4 512-byte flash sectors. Leave the pre-init entries
  // intact. A corrupt sector without entries should be GC'ed on init because
  // the KVS must maintain one empty sector at all times.
  flash_.buffer()[64] = byte(0xef);
  flash_.buffer()[513] = byte(0xef);
  flash_.buffer()[1025] = byte(0xef);
  flash_.buffer()[1537] = byte(0xef);

  ASSERT_EQ(Status::DATA_LOSS, kvs_.Init());

  auto stats = kvs_.GetStorageStats();
  EXPECT_EQ(64u, stats.in_use_bytes);
  EXPECT_EQ(3 * 512u - 64u, stats.reclaimable_bytes);
  EXPECT_EQ(0u, stats.writable_bytes);
}

TEST_F(KvsErrorHandling, Init_CorruptKey_RevertsToPreviousVersion) {
  constexpr auto kVersion7 =
      MakeValidEntry(kMagic, 7, "my_key", ByteStr("version 7"));
  constexpr auto kVersion8 =
      MakeValidEntry(kMagic, 8, "my_key", ByteStr("version 8"));

  InitFlashTo(AsBytes(kVersion7, kVersion8));

  // Corrupt a byte of entry version 8 (addresses 32-63).
  flash_.buffer()[34] = byte(0xef);

  ASSERT_EQ(Status::DATA_LOSS, kvs_.Init());

  char buffer[64] = {};

  EXPECT_EQ(1u, kvs_.size());

  auto result = kvs_.Get("my_key", as_writable_bytes(span(buffer)));
  EXPECT_EQ(Status::OK, result.status());
  EXPECT_EQ(sizeof("version 7") - 1, result.size());
  EXPECT_STREQ("version 7", buffer);

  EXPECT_EQ(32u, kvs_.GetStorageStats().in_use_bytes);
}

TEST_F(KvsErrorHandling, Put_WriteFailure_EntryNotAddedButBytesMarkedWritten) {
  ASSERT_EQ(Status::OK, kvs_.Init());
  flash_.InjectWriteError(FlashError::Unconditional(Status::UNAVAILABLE, 1));

  EXPECT_EQ(Status::UNAVAILABLE, kvs_.Put("key1", ByteStr("value1")));

  EXPECT_EQ(Status::NOT_FOUND, kvs_.Get("key1", span<byte>()).status());
  ASSERT_TRUE(kvs_.empty());

  auto stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, 0u);
  EXPECT_EQ(stats.reclaimable_bytes, 32u);
  EXPECT_EQ(stats.writable_bytes, 512u * 3 - 32);

  // The bytes were marked used, so a new key should not overlap with the bytes
  // from the failed Put.
  EXPECT_EQ(Status::OK, kvs_.Put("key1", ByteStr("value1")));

  stats = kvs_.GetStorageStats();
  EXPECT_EQ(stats.in_use_bytes, 32u);
  EXPECT_EQ(stats.reclaimable_bytes, 32u);
  EXPECT_EQ(stats.writable_bytes, 512u * 3 - 32 * 2);
}

}  // namespace
}  // namespace pw::kvs
