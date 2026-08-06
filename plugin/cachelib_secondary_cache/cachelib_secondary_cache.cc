#include "plugin/cachelib_secondary_cache/cachelib_secondary_cache.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "cachelib/navy/AbstractCache.h"
#include "cachelib/navy/Factory.h"
#include "cachelib/navy/common/Device.h"
#include "cachelib/navy/common/Hash.h"
#include "cachelib/navy/common/Types.h"
#include "cachelib/navy/scheduler/JobScheduler.h"
#include "rocksdb/env.h"
#include "rocksdb/utilities/object_registry.h"
#include "rocksdb/utilities/options_type.h"

namespace ROCKSDB_NAMESPACE {
namespace {

namespace navy = facebook::cachelib::navy;
using CacheLibHashedKey = facebook::cachelib::HashedKey;

constexpr char kFrameMagic[] = {'R', 'S', 'C', '1'};
constexpr size_t kFrameHeaderSize = sizeof(kFrameMagic) + 2;
constexpr uint64_t kMetadataPercentDenominator = 100;
constexpr uint64_t kMinimumMetadataSize = 4 * 1024 * 1024;

uint64_t AlignDown(uint64_t value, uint64_t alignment) {
  return value - value % alignment;
}

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
  return AlignDown(value + alignment - 1, alignment);
}

Status ToRocksStatus(navy::Status status) {
  switch (status) {
    case navy::Status::Ok:
    case navy::Status::NotFound:
    case navy::Status::Rejected:
      return Status::OK();
    case navy::Status::Retry:
      return Status::TryAgain("CacheLib Navy operation needs retry");
    case navy::Status::DeviceError:
      return Status::IOError("CacheLib Navy device error");
    case navy::Status::BadState:
      return Status::Corruption("CacheLib Navy entered a bad state");
    case navy::Status::ChecksumError:
      return Status::Corruption("CacheLib Navy checksum mismatch");
  }
  return Status::Corruption("Unknown CacheLib Navy status");
}

std::string EncodeFrame(const Slice& value, CompressionType type,
                        CacheTier source) {
  std::string frame;
  frame.reserve(kFrameHeaderSize + value.size());
  frame.append(kFrameMagic, sizeof(kFrameMagic));
  frame.push_back(static_cast<char>(type));
  frame.push_back(static_cast<char>(source));
  frame.append(value.data(), value.size());
  return frame;
}

bool DecodeFrame(const navy::Buffer& frame, CompressionType* type,
                 CacheTier* source, Slice* value) {
  if (frame.size() < kFrameHeaderSize ||
      std::memcmp(frame.data(), kFrameMagic, sizeof(kFrameMagic)) != 0) {
    return false;
  }
  *type = static_cast<CompressionType>(frame.data()[sizeof(kFrameMagic)]);
  *source =
      static_cast<CacheTier>(frame.data()[sizeof(kFrameMagic) + 1]);
  *value =
      Slice(reinterpret_cast<const char*>(frame.data() + kFrameHeaderSize),
            frame.size() - kFrameHeaderSize);
  return true;
}

class CacheLibSecondaryCacheResultHandle
    : public SecondaryCacheResultHandle {
 public:
  CacheLibSecondaryCacheResultHandle(
      navy::AbstractCache* cache, const Slice& key,
      const Cache::CacheItemHelper* helper,
      Cache::CreateContext* create_context, bool advise_erase)
      : cache_(cache),
        key_(key.data(), key.size()),
        helper_(helper),
        create_context_(create_context),
        advise_erase_(advise_erase),
        state_(std::make_shared<LookupState>()) {}

  ~CacheLibSecondaryCacheResultHandle() override {
    if (!state_->ready.load(std::memory_order_acquire)) {
      Wait();
    }
    std::call_once(materialize_once_, [this] { Materialize(); });
    if (!value_taken_ && value_ != nullptr && helper_->del_cb != nullptr) {
      helper_->del_cb(value_, nullptr);
    }
  }

  void Start() noexcept {
    std::shared_ptr<LookupState> state = state_;
    try {
      auto key = std::make_shared<std::string>(key_);
      cache_->lookupAsync(
          navy::makeHK(key->data(), key->size()),
          [key, state](navy::Status status, CacheLibHashedKey,
                       navy::Buffer value) {
            Complete(state, status, std::move(value));
          });
    } catch (...) {
      Complete(state, navy::Status::DeviceError, navy::Buffer());
    }
  }

  bool IsReady() override {
    return state_->ready.load(std::memory_order_acquire);
  }

  void Wait() override {
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->condition.wait(lock, [this] {
      return state_->ready.load(std::memory_order_acquire);
    });
  }

  Cache::ObjectPtr Value() override {
    Wait();
    std::call_once(materialize_once_, [this] { Materialize(); });
    value_taken_ = true;
    return value_;
  }

  size_t Size() override {
    Wait();
    std::call_once(materialize_once_, [this] { Materialize(); });
    return charge_;
  }

  bool HasValue() {
    Wait();
    std::call_once(materialize_once_, [this] { Materialize(); });
    return value_ != nullptr;
  }

 private:
  struct LookupState {
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic<bool> ready{false};
    navy::Status status = navy::Status::NotFound;
    navy::Buffer frame;
  };

  static void Complete(const std::shared_ptr<LookupState>& state,
                       navy::Status status, navy::Buffer frame) noexcept {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->status = status;
      state->frame = std::move(frame);
      state->ready.store(true, std::memory_order_release);
    }
    state->condition.notify_all();
  }

  void Materialize() noexcept {
    Cache::ObjectPtr value = nullptr;
    size_t charge = 0;
    try {
      if (state_->status == navy::Status::Ok) {
        CompressionType type;
        CacheTier source;
        Slice saved;
        if (DecodeFrame(state_->frame, &type, &source, &saved)) {
          Status create_status =
              helper_->create_cb(saved, type, source, create_context_,
                                 nullptr, &value, &charge);
          if (!create_status.ok()) {
            value = nullptr;
            charge = 0;
          } else if (advise_erase_) {
            try {
              auto key = std::make_shared<std::string>(key_);
              cache_->removeAsync(
                  navy::makeHK(key->data(), key->size()),
                  [key](navy::Status, CacheLibHashedKey) {});
            } catch (...) {
            }
          }
        }
      }
    } catch (...) {
      if (value != nullptr && helper_->del_cb != nullptr) {
        helper_->del_cb(value, nullptr);
      }
      value = nullptr;
      charge = 0;
    }
    value_ = value;
    charge_ = charge;
  }

  navy::AbstractCache* cache_;
  std::string key_;
  const Cache::CacheItemHelper* helper_;
  Cache::CreateContext* create_context_;
  bool advise_erase_;
  std::shared_ptr<LookupState> state_;
  std::once_flag materialize_once_;
  Cache::ObjectPtr value_ = nullptr;
  size_t charge_ = 0;
  bool value_taken_ = false;
};

std::unordered_map<std::string, OptionTypeInfo>
    cachelib_secondary_cache_options_type_info = {
        {"cache_file",
         {offsetof(CacheLibSecondaryCacheOptions, cache_file),
          OptionType::kString, OptionVerificationType::kNormal,
          OptionTypeFlags::kNone}},
        {"capacity",
         {offsetof(CacheLibSecondaryCacheOptions, capacity),
          OptionType::kUInt64T, OptionVerificationType::kNormal,
          OptionTypeFlags::kNone}},
        {"metadata_size",
         {offsetof(CacheLibSecondaryCacheOptions, metadata_size),
          OptionType::kUInt64T, OptionVerificationType::kNormal,
          OptionTypeFlags::kNone}},
        {"block_size",
         {offsetof(CacheLibSecondaryCacheOptions, block_size),
          OptionType::kUInt32T, OptionVerificationType::kNormal,
          OptionTypeFlags::kNone}},
        {"region_size",
         {offsetof(CacheLibSecondaryCacheOptions, region_size),
          OptionType::kUInt32T, OptionVerificationType::kNormal,
          OptionTypeFlags::kNone}},
        {"big_hash_size_percent",
         {offsetof(CacheLibSecondaryCacheOptions, big_hash_size_percent),
          OptionType::kUInt32T, OptionVerificationType::kNormal,
          OptionTypeFlags::kNone}},
        {"big_hash_bucket_size",
         {offsetof(CacheLibSecondaryCacheOptions, big_hash_bucket_size),
          OptionType::kUInt32T, OptionVerificationType::kNormal,
          OptionTypeFlags::kNone}},
        {"small_item_max_size",
         {offsetof(CacheLibSecondaryCacheOptions, small_item_max_size),
          OptionType::kUInt32T, OptionVerificationType::kNormal,
          OptionTypeFlags::kNone}},
        {"reader_threads",
         {offsetof(CacheLibSecondaryCacheOptions, reader_threads),
          OptionType::kUInt32T, OptionVerificationType::kNormal,
          OptionTypeFlags::kNone}},
        {"writer_threads",
         {offsetof(CacheLibSecondaryCacheOptions, writer_threads),
          OptionType::kUInt32T, OptionVerificationType::kNormal,
          OptionTypeFlags::kNone}},
        {"request_ordering_shard_power",
         {offsetof(CacheLibSecondaryCacheOptions,
                   request_ordering_shard_power),
          OptionType::kUInt32T, OptionVerificationType::kNormal,
          OptionTypeFlags::kNone}},
        {"truncate",
         {offsetof(CacheLibSecondaryCacheOptions, truncate),
          OptionType::kBoolean, OptionVerificationType::kNormal,
          OptionTypeFlags::kNone}},
        {"checksum",
         {offsetof(CacheLibSecondaryCacheOptions, checksum),
          OptionType::kBoolean, OptionVerificationType::kNormal,
          OptionTypeFlags::kNone}},
        {"precise_remove",
         {offsetof(CacheLibSecondaryCacheOptions, precise_remove),
          OptionType::kBoolean, OptionVerificationType::kNormal,
          OptionTypeFlags::kNone}},
};

}

CacheLibSecondaryCache::CacheLibSecondaryCache() {
  RegisterOptions(&options_, &cachelib_secondary_cache_options_type_info);
}

CacheLibSecondaryCache::CacheLibSecondaryCache(
    const CacheLibSecondaryCacheOptions& options)
    : options_(options) {
  RegisterOptions(&options_, &cachelib_secondary_cache_options_type_info);
}

CacheLibSecondaryCache::~CacheLibSecondaryCache() {
  if (cache_ != nullptr) {
    try {
      cache_->flush();
      cache_->persist();
    } catch (...) {
    }
  }
}

Status CacheLibSecondaryCache::PrepareOptions(
    const ConfigOptions& config_options) {
  Status status = SecondaryCache::PrepareOptions(config_options);
  if (!status.ok() || cache_ != nullptr) {
    return status;
  }
  return Open();
}

Status CacheLibSecondaryCache::Open() {
  if (options_.cache_file.empty()) {
    return Status::InvalidArgument(
        "CacheLibSecondaryCache cache_file is required");
  }
  if (options_.capacity == 0 || options_.block_size == 0 ||
      options_.region_size == 0 || options_.reader_threads == 0 ||
      options_.writer_threads == 0 || options_.big_hash_size_percent == 0 ||
      options_.big_hash_size_percent >= 100 ||
      options_.big_hash_bucket_size == 0 ||
      options_.small_item_max_size == 0) {
    return Status::InvalidArgument(
        "CacheLibSecondaryCache sizes and thread counts must be positive");
  }
  if (options_.capacity % options_.block_size != 0 ||
      options_.region_size % options_.block_size != 0 ||
      options_.big_hash_bucket_size % options_.block_size != 0) {
    return Status::InvalidArgument(
        "CacheLibSecondaryCache capacity, region_size, and "
        "big_hash_bucket_size must be block aligned");
  }

  try {
    bool initialize_file = options_.truncate;
    uint64_t cache_file_size = 0;
    Status file_status = Env::Default()->FileExists(options_.cache_file);
    if (file_status.IsNotFound()) {
      initialize_file = true;
    } else if (!file_status.ok()) {
      return file_status;
    } else {
      file_status =
          Env::Default()->GetFileSize(options_.cache_file, &cache_file_size);
      if (!file_status.ok()) {
        return file_status;
      }
      initialize_file |= cache_file_size != options_.capacity;
    }

    uint64_t metadata_size = options_.metadata_size;
    if (metadata_size == 0) {
      metadata_size = std::max<uint64_t>(
          options_.capacity / kMetadataPercentDenominator,
          kMinimumMetadataSize);
    }
    metadata_size = AlignUp(metadata_size, options_.block_size);
    if (metadata_size >= options_.capacity) {
      return Status::InvalidArgument(
          "CacheLibSecondaryCache capacity is too small for metadata");
    }

    uint64_t big_hash_reserved =
        options_.capacity * options_.big_hash_size_percent / 100;
    uint64_t big_hash_offset =
        AlignUp(options_.capacity - big_hash_reserved,
                options_.big_hash_bucket_size);
    uint64_t big_hash_size =
        AlignDown(options_.capacity - big_hash_offset,
                  options_.big_hash_bucket_size);
    if (big_hash_size < options_.big_hash_bucket_size ||
        big_hash_offset <= metadata_size) {
      return Status::InvalidArgument(
          "CacheLibSecondaryCache capacity is too small for BigHash");
    }

    uint64_t cache_size =
        AlignDown(big_hash_offset - metadata_size, options_.region_size);
    if (cache_size < options_.region_size) {
      return Status::InvalidArgument(
          "CacheLibSecondaryCache capacity must contain at least one region");
    }

    auto device = navy::createFileDevice(
        {options_.cache_file}, options_.capacity, initialize_file,
        options_.block_size, 0, 0, navy::IoEngine::Sync, 0, false, nullptr,
        false);
    auto scheduler = navy::createOrderedThreadPoolJobScheduler(
        options_.reader_threads, options_.writer_threads,
        options_.request_ordering_shard_power);
    auto block_cache = navy::createBlockCacheProto();
    block_cache->setLayout(metadata_size, cache_size, options_.region_size);
    block_cache->setChecksum(options_.checksum);
    block_cache->setLruEvictionPolicy();
    block_cache->setCleanRegionsPool(1, 1);
    block_cache->setNumInMemBuffers(2);
    block_cache->setPreciseRemove(options_.precise_remove);

    auto big_hash = navy::createBigHashProto();
    big_hash->setLayout(big_hash_offset, big_hash_size,
                        options_.big_hash_bucket_size);
    big_hash->setBloomFilter(4, 16);

    auto engine_pair = navy::createEnginePairProto();
    engine_pair->setBigHash(std::move(big_hash),
                            options_.small_item_max_size);
    engine_pair->setBlockCache(std::move(block_cache));

    auto cache_proto = navy::createCacheProto();
    cache_proto->setDevice(std::move(device));
    cache_proto->setMetadataSize(metadata_size);
    cache_proto->setJobScheduler(std::move(scheduler));
    cache_proto->setDestructorCallback(
        [](CacheLibHashedKey, navy::BufferView, navy::DestructorEvent) {});
    cache_proto->addEnginePair(std::move(engine_pair));

    cache_ = navy::createCache(std::move(cache_proto));
    if (initialize_file || !cache_->recover()) {
      cache_->reset();
    }
    return Status::OK();
  } catch (const std::exception& exception) {
    cache_.reset();
    return Status::InvalidArgument("Cannot initialize CacheLib Navy cache: ",
                                   exception.what());
  } catch (...) {
    cache_.reset();
    return Status::InvalidArgument(
        "Cannot initialize CacheLib Navy cache: unknown exception");
  }
}

Status CacheLibSecondaryCache::InsertBytes(const Slice& key,
                                           const Slice& value) {
  if (cache_ == nullptr) {
    return Status::InvalidArgument("CacheLibSecondaryCache is not prepared");
  }
  try {
    auto owned_key =
        std::make_shared<std::string>(key.data(), key.size());
    auto owned_value =
        std::make_shared<std::string>(value.data(), value.size());
    navy::Status status = cache_->insertAsync(
        navy::makeHK(owned_key->data(), owned_key->size()),
        navy::BufferView(
            owned_value->size(),
            reinterpret_cast<const uint8_t*>(owned_value->data())),
        [owned_key, owned_value](navy::Status, CacheLibHashedKey) {});
    return ToRocksStatus(status);
  } catch (const std::exception& exception) {
    return Status::IOError("CacheLib Navy insert failed: ", exception.what());
  } catch (...) {
    return Status::IOError(
        "CacheLib Navy insert failed with unknown exception");
  }
}

Status CacheLibSecondaryCache::Insert(
    const Slice& key, Cache::ObjectPtr value,
    const Cache::CacheItemHelper* helper, bool) {
  if (value == nullptr || helper == nullptr || helper->size_cb == nullptr ||
      helper->saveto_cb == nullptr) {
    return Status::InvalidArgument(
        "CacheLibSecondaryCache requires a persistable non-null value");
  }

  size_t value_size = helper->size_cb(value);
  std::string frame;
  frame.reserve(kFrameHeaderSize + value_size);
  frame.append(kFrameMagic, sizeof(kFrameMagic));
  frame.push_back(static_cast<char>(CompressionType::kNoCompression));
  frame.push_back(static_cast<char>(CacheTier::kVolatileTier));
  frame.resize(kFrameHeaderSize + value_size);
  Status status =
      helper->saveto_cb(value, 0, value_size, &frame[kFrameHeaderSize]);
  if (!status.ok()) {
    return status;
  }
  return InsertBytes(key, Slice(frame));
}

Status CacheLibSecondaryCache::InsertSaved(const Slice& key,
                                           const Slice& saved,
                                           CompressionType type,
                                           CacheTier source) {
  std::string frame = EncodeFrame(saved, type, source);
  return InsertBytes(key, Slice(frame));
}

std::unique_ptr<SecondaryCacheResultHandle>
CacheLibSecondaryCache::Lookup(
    const Slice& key, const Cache::CacheItemHelper* helper,
    Cache::CreateContext* create_context, bool wait, bool advise_erase,
    Statistics*, bool& kept_in_sec_cache) {
  kept_in_sec_cache = false;
  if (cache_ == nullptr || helper == nullptr || helper->create_cb == nullptr) {
    return nullptr;
  }

  try {
    std::unique_ptr<CacheLibSecondaryCacheResultHandle> handle(
        new CacheLibSecondaryCacheResultHandle(
            cache_.get(), key, helper, create_context, advise_erase));
    handle->Start();
    kept_in_sec_cache = !advise_erase;
    if (wait) {
      handle->Wait();
      if (!handle->HasValue()) {
        kept_in_sec_cache = false;
        return nullptr;
      }
    }
    return handle;
  } catch (...) {
    return nullptr;
  }
}

void CacheLibSecondaryCache::Erase(const Slice& key) {
  if (cache_ == nullptr) {
    return;
  }
  try {
    auto owned_key =
        std::make_shared<std::string>(key.data(), key.size());
    cache_->removeAsync(
        navy::makeHK(owned_key->data(), owned_key->size()),
        [owned_key](navy::Status, CacheLibHashedKey) {});
  } catch (...) {
  }
}

void CacheLibSecondaryCache::WaitAll(
    std::vector<SecondaryCacheResultHandle*> handles) {
  for (SecondaryCacheResultHandle* handle : handles) {
    handle->Wait();
  }
}

Status CacheLibSecondaryCache::GetCapacity(size_t& capacity) {
  capacity = static_cast<size_t>(options_.capacity);
  return Status::OK();
}

std::string CacheLibSecondaryCache::GetPrintableOptions() const {
  std::string result;
  ConfigOptions config_options;
  config_options.delimiter = "\n";
  GetOptionString(config_options, &result).PermitUncheckedError();
  return result;
}

}

extern "C" int RegisterCacheLibSecondaryCache(
    ROCKSDB_NAMESPACE::ObjectLibrary& library, const std::string&) {
  library.AddFactory<ROCKSDB_NAMESPACE::SecondaryCache>(
      ROCKSDB_NAMESPACE::CacheLibSecondaryCache::kClassName(),
      [](const std::string&,
         std::unique_ptr<ROCKSDB_NAMESPACE::SecondaryCache>* guard,
         std::string*) {
        guard->reset(new ROCKSDB_NAMESPACE::CacheLibSecondaryCache());
        return guard->get();
      });
  return 1;
}
