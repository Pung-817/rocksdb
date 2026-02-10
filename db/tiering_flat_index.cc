#include "db/tiering_flat_index.h"

#include <cstring>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "table/internal_iterator.h"
#include "rocksdb/iterator.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"

namespace ROCKSDB_NAMESPACE {

// table class
Status BuildTieringFlatIndex(InternalIterator* iter, TieringFlatIndex* index) {
  if (iter == nullptr || index == nullptr) {
    return Status::InvalidArgument("iter or index is null");
  }

  // Temporary storage to avoid multiple passes
  std::vector<std::string> keys;
  std::vector<std::string> values;
  size_t total_key_len = 0;
  size_t total_value_len = 0;

  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    Slice key = iter->key();
    Slice value = iter->value();

    // reserve?
    keys.emplace_back(key.data(), key.size());
    values.emplace_back(value.data(), value.size());

    total_key_len += key.size();
    total_value_len += value.size();
  }

  if (!iter->status().ok()) {
    return iter->status();
  }

  size_t count = keys.size();

  // Clean up old data to avoid leaks if reusing index object
  if (index->key_buff) delete[] index->key_buff;
  if (index->value_buff) delete[] index->value_buff;
  if (index->key_offset) delete[] index->key_offset;
  if (index->value_offset) delete[] index->value_offset;

  index->key_nums = count;
  index->key_len = total_key_len;
  index->value_len = total_value_len;

  if (count == 0) {
    index->key_buff = nullptr;
    index->value_buff = nullptr;
    index->key_offset = nullptr;
    index->value_offset = nullptr;
    return Status::OK();
  }

  index->key_buff = new char[total_key_len];
  index->value_buff = new char[total_value_len];
  index->key_offset = new size_t[count + 1];
  index->value_offset = new size_t[count + 1];

  size_t current_key_offset = 0;
  size_t current_value_offset = 0;

  for (size_t i = 0; i < count; ++i) {
    index->key_offset[i] = current_key_offset;
    index->value_offset[i] = current_value_offset;

    memcpy(index->key_buff + current_key_offset, keys[i].data(),
           keys[i].size());
    memcpy(index->value_buff + current_value_offset, values[i].data(),
           values[i].size());

    current_key_offset += keys[i].size();
    current_value_offset += values[i].size();
  }

  index->key_offset[count] = current_key_offset;
  index->value_offset[count] = current_value_offset;

  return Status::OK();
}

}  // namespace ROCKSDB_NAMESPACE
