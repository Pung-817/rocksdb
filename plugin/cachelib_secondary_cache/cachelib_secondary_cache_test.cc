#include "plugin/cachelib_secondary_cache/cachelib_secondary_cache.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <folly/io/IOBuf.h>
#include <folly/io/RecordIO.h>

#include "cachelib/navy/common/Device.h"
#include "cachelib/navy/serialization/RecordIO.h"
#include "cache/secondary_cache_adapter.h"
#include "port/stack_trace.h"
#include "rocksdb/cache.h"
#include "rocksdb/convenience.h"
#include "test_util/secondary_cache_test_util.h"
#include "test_util/testharness.h"
#include "util/cast_util.h"

namespace ROCKSDB_NAMESPACE {
namespace {

using secondary_cache_test_util::TestCreateContext;
using secondary_cache_test_util::WithCacheType;

class CacheLibSecondaryCacheTest : public testing::Test {
 public:
  CacheLibSecondaryCacheTest()
      : cache_file_(
            test::PerThreadDBPath("cachelib-secondary-cache-test.navy")) {
    Env::Default()->DeleteFile(cache_file_).PermitUncheckedError();
  }

  ~CacheLibSecondaryCacheTest() override {
    cache_.reset();
    Env::Default()->DeleteFile(cache_file_).PermitUncheckedError();
  }

 protected:
  std::shared_ptr<SecondaryCache> Open(bool truncate) {
    std::shared_ptr<SecondaryCache> cache;
    std::string uri =
        std::string("{id=") + CacheLibSecondaryCache::kClassName() +
        ";cache_file=" + cache_file_ +
        ";capacity=67108864;region_size=16777216;reader_threads=1;"
        "writer_threads=1;request_ordering_shard_power=4;truncate=" +
        (truncate ? "true}" : "false}");
    EXPECT_OK(
        SecondaryCache::CreateFromString(ConfigOptions(), uri, &cache));
    EXPECT_NE(cache, nullptr);
    return cache;
  }

  std::string cache_file_;
  std::shared_ptr<SecondaryCache> cache_;
  TestCreateContext create_context_;
};

TEST_F(CacheLibSecondaryCacheTest, FactoryRequiresCacheFile) {
  std::shared_ptr<SecondaryCache> cache;
  Status status = SecondaryCache::CreateFromString(
      ConfigOptions(),
      std::string("{id=") + CacheLibSecondaryCache::kClassName() +
          ";capacity=67108864}",
      &cache);
  ASSERT_TRUE(status.IsInvalidArgument());
  ASSERT_EQ(cache, nullptr);
}

TEST_F(CacheLibSecondaryCacheTest, PersistRecordEndingAtMetadataBoundary) {
  namespace navy = facebook::cachelib::navy;

  constexpr uint32_t kBlockSize = 4096;
  constexpr size_t kPayloadSize =
      kBlockSize - folly::recordio_helpers::headerSize();
  auto device = navy::createFileDevice(
      {cache_file_}, 4 * kBlockSize, true, kBlockSize, 0, 0,
      navy::IoEngine::Sync, 0, false, nullptr, false);
  ASSERT_NE(device, nullptr);

  {
    auto payload = folly::IOBuf::create(kPayloadSize);
    payload->append(kPayloadSize);
    std::memset(payload->writableData(), 'x', kPayloadSize);
    auto writer = navy::createMetadataRecordWriter(*device, kBlockSize);
    writer->writeRecord(std::move(payload));
  }

  auto reader = navy::createMetadataRecordReader(*device, kBlockSize);
  auto restored = reader->readRecord();
  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->length(), kPayloadSize);
  for (size_t i = 0; i < kPayloadSize; ++i) {
    ASSERT_EQ(restored->data()[i], 'x');
  }
}

TEST_F(CacheLibSecondaryCacheTest, ConfigureAsLruSecondaryCache) {
  std::shared_ptr<Cache> cache;
  std::string options =
      std::string("capacity=1048576;num_shard_bits=0;secondary_cache={id=") +
      CacheLibSecondaryCache::kClassName() + ";cache_file=" + cache_file_ +
      ";capacity=67108864;metadata_size=8388608;region_size=16777216;"
      "reader_threads=1;"
      "writer_threads=1;request_ordering_shard_power=4;truncate=true}";
  ASSERT_OK(Cache::CreateFromString(ConfigOptions(), options, &cache));
  auto* adapter =
      static_cast_with_check<CacheWithSecondaryAdapter>(cache.get());
  ASSERT_NE(adapter, nullptr);
  ASSERT_STREQ(adapter->TEST_GetSecondaryCache()->Name(),
               CacheLibSecondaryCache::kClassName());
  ASSERT_NE(adapter->TEST_GetSecondaryCache()
                ->GetPrintableOptions()
                .find("metadata_size=8388608"),
            std::string::npos);
}

TEST_F(CacheLibSecondaryCacheTest, InsertRecoverAndLookup) {
  const std::string key = "cache-key";
  const std::string value = "cache-value";
  auto helper = WithCacheType::GetHelper();

  cache_ = Open(true);
  auto* item = new WithCacheType::TestItem(value.data(), value.size());
  ASSERT_OK(cache_->Insert(key, item, helper, true));
  delete item;
  cache_.reset();

  cache_ = Open(false);
  bool kept_in_secondary = false;
  auto result = cache_->Lookup(key, helper, &create_context_, true, false,
                               nullptr, kept_in_secondary);
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(result->IsReady());
  ASSERT_TRUE(kept_in_secondary);
  auto* restored = static_cast<WithCacheType::TestItem*>(result->Value());
  ASSERT_EQ(restored->ToString(), value);
  helper->del_cb(restored, nullptr);
}

TEST_F(CacheLibSecondaryCacheTest, AsyncLookupAndAdviseErase) {
  const std::string key = "async-cache-key";
  const std::string value = "async-cache-value";
  auto helper = WithCacheType::GetHelper();

  cache_ = Open(true);
  auto* item = new WithCacheType::TestItem(value.data(), value.size());
  ASSERT_OK(cache_->Insert(key, item, helper, true));
  delete item;

  bool kept_in_secondary = true;
  auto result = cache_->Lookup(key, helper, &create_context_, false, true,
                               nullptr, kept_in_secondary);
  ASSERT_NE(result, nullptr);
  ASSERT_FALSE(kept_in_secondary);
  cache_->WaitAll({result.get()});
  ASSERT_TRUE(result->IsReady());
  auto* restored = static_cast<WithCacheType::TestItem*>(result->Value());
  ASSERT_EQ(restored->ToString(), value);
  helper->del_cb(restored, nullptr);

  result.reset();
  cache_.reset();
  cache_ = Open(false);
  kept_in_secondary = true;
  ASSERT_EQ(cache_->Lookup(key, helper, &create_context_, true, false, nullptr,
                          kept_in_secondary),
            nullptr);
}

}  // namespace
}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
