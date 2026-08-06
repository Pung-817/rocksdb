#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rocksdb/secondary_cache.h"

namespace facebook {
namespace cachelib {
namespace navy {
class AbstractCache;
}
}  // namespace cachelib
}  // namespace facebook

namespace ROCKSDB_NAMESPACE {

struct CacheLibSecondaryCacheOptions {
  static const char* kName() { return "CacheLibSecondaryCacheOptions"; }

  std::string cache_file;
  uint64_t capacity = 0;
  uint64_t metadata_size = 0;
  uint32_t block_size = 4096;
  uint32_t region_size = 16 * 1024 * 1024;
  uint32_t big_hash_size_percent = 5;
  uint32_t big_hash_bucket_size = 4096;
  uint32_t small_item_max_size = 1024;
  uint32_t reader_threads = 4;
  uint32_t writer_threads = 4;
  uint32_t request_ordering_shard_power = 12;
  bool truncate = false;
  bool checksum = true;
  bool precise_remove = true;
};

class CacheLibSecondaryCache : public SecondaryCache {
 public:
  static const char* kClassName() { return "CacheLibSecondaryCache"; }

  CacheLibSecondaryCache();
  explicit CacheLibSecondaryCache(
      const CacheLibSecondaryCacheOptions& options);
  ~CacheLibSecondaryCache() override;

  const char* Name() const override { return kClassName(); }

  Status Insert(const Slice& key, Cache::ObjectPtr value,
                const Cache::CacheItemHelper* helper,
                bool force_insert) override;

  Status InsertSaved(const Slice& key, const Slice& saved,
                     CompressionType type, CacheTier source) override;

  std::unique_ptr<SecondaryCacheResultHandle> Lookup(
      const Slice& key, const Cache::CacheItemHelper* helper,
      Cache::CreateContext* create_context, bool wait, bool advise_erase,
      Statistics* stats, bool& kept_in_sec_cache) override;

  bool SupportForceErase() const override { return options_.precise_remove; }

  void Erase(const Slice& key) override;

  void WaitAll(std::vector<SecondaryCacheResultHandle*> handles) override;

  Status GetCapacity(size_t& capacity) override;

  Status PrepareOptions(const ConfigOptions& config_options) override;

  std::string GetPrintableOptions() const override;

 private:
  Status Open();
  Status InsertBytes(const Slice& key, const Slice& value);

  CacheLibSecondaryCacheOptions options_;
  std::unique_ptr<facebook::cachelib::navy::AbstractCache> cache_;
};

}  // namespace ROCKSDB_NAMESPACE
