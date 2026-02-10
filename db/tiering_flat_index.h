#pragma once

#include <assert.h>
#include <stdio.h>

#include <iostream>
#include <vector>
#include <algorithm>

#include "db/dbformat.h"
#include "rocksdb/slice.h"
#include "rocksdb/rocksdb_namespace.h"
#include "util/coding.h"
#include "table/get_context.h"
#include "table/internal_iterator.h"

namespace ROCKSDB_NAMESPACE {

struct TieringFlatIndex {
  explicit TieringFlatIndex(const InternalKeyComparator* _c)
      : key_buff(nullptr),
        value_buff(nullptr),
        key_offset(nullptr),
        value_offset(nullptr),
        key_nums(0),
        key_len(0),
        value_len(0),
        c(_c) {}

  ~TieringFlatIndex() {
    delete[] key_buff;
    delete[] value_buff;
    delete[] key_offset;
    delete[] value_offset;
  };

  // Disable copying
  TieringFlatIndex(const TieringFlatIndex&) = delete;
  TieringFlatIndex& operator=(const TieringFlatIndex&) = delete;

  char* key_buff;
  char* value_buff;
  size_t* key_offset;
  size_t* value_offset;
  size_t key_nums;
  size_t key_len;
  size_t value_len;

  const char* get_key_offset(size_t id) const {
    assert(id < key_nums);
    return key_buff + key_offset[id];
  }
  
  size_t get_key_len(size_t id) const {
    assert(id < key_nums);
    return key_offset[id + 1] - key_offset[id];
  }

  const char* get_value_offset(size_t id) const {
    assert(id < key_nums);
    return value_buff + value_offset[id];
  }

  size_t get_value_len(size_t id) const {
    assert(id < key_nums);
    return value_offset[id + 1] - value_offset[id];
  }

  inline const Slice getKey(size_t id) const {
    return Slice(get_key_offset(id), get_key_len(id));
  }

  inline const Slice getValue(size_t id) const {
    return Slice(get_value_offset(id), get_value_len(id));
  }

  size_t size() const {
    return key_len + value_len + key_nums * 2 * sizeof(size_t) +
           2 * sizeof(size_t);
  }

  // Find the first key >= target (Lower Bound)
  size_t getIdx(const Slice& key) const {
    size_t l = 0, r = key_nums;
    while (l < r) {
      size_t mid = l + (r - l) / 2;
      if (c->Compare(getKey(mid), key) < 0) {
        l = mid + 1;
      } else {
        r = mid;
      }
    }
    return l;
  }

  // Look up the key and save value to GetContext if found.
  // Returns Status::OK if found (and saved), Status::NotFound if not found.
  Status Lookup(const Slice& key, GetContext* get_context) const {
    size_t idx = getIdx(key);
    if (idx >= key_nums) {
      return Status::NotFound();
    }

    // getKey(idx) returns the internal key.
    // The incoming 'key' is also an internal key (from Version::Get).
    Slice entry_key = getKey(idx);
    if (c->Compare(entry_key, key) != 0) {
      return Status::NotFound();
    }

    Slice entry_value = getValue(idx);
    ParsedInternalKey parsed_key;
    if (!ParseInternalKey(entry_key, &parsed_key, false).ok()) {
      return Status::Corruption("Malformed key in L0 index");
    }

    bool matched = false; // Will be set by SaveValue
    Status read_status;
    bool more_keys = get_context->SaveValue(parsed_key, entry_value, &matched, &read_status);
    
    if (!read_status.ok()) {
      return read_status;
    }

    return Status::OK();
  }

  static bool ParseValue(Slice map_input) {
    Slice smallest_key;
    uint64_t link_count;
    std::vector<uint64_t> dependence;
    uint64_t flags;
    if (!GetVarint64(&map_input, &flags) ||
        !GetVarint64(&map_input, &link_count) ||
        !GetLengthPrefixedSlice(&map_input, &smallest_key)) {
      // std::cout << "parse error" << std::endl;
      return false;
    }
    uint64_t file_number;
    for (uint64_t i = 0; i < link_count; ++i) {
      if (!GetVarint64(&map_input, &file_number)) {
        return false;
      }
      dependence.push_back(file_number);
    }
    // Just for validation/debug
    // InternalKey ikey;
    // ikey.DecodeFrom(smallest_key);
    return true;
  }

  void DebugString() {
    InternalKey ikey;
    for (size_t i = 0; i < key_nums; i++) {
      ikey.DecodeFrom(getKey(i));
      std::cout << ikey.DebugString(true) << std::endl;
      Slice v = getValue(i);
      ParseValue(v);
    }
  }
  
  const InternalKeyComparator* c;
};

extern Status BuildTieringFlatIndex(InternalIterator* iter, TieringFlatIndex* index);

}  // namespace ROCKSDB_NAMESPACE
