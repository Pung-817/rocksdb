# CacheLib Secondary Cache

This optional plugin connects RocksDB's `SecondaryCache` interface directly to
CacheLib Navy. It does not add another DRAM cache between RocksDB's block cache
and Navy.

The integration baseline is CacheLib `v2024.12.30.00`, commit
`5cfae53826ec3ea98242f8e10ccd0a6c2453e3a3`. The version is intentionally fixed
instead of following CacheLib's weekly tags.

## Scope

The plugin provides the control implementation for later cache-policy
research:

- RocksDB objects are serialized with their compression type and source tier.
- Navy insert, lookup, and erase operations use its asynchronous API.
- CacheLib exceptions are contained at the plugin boundary.
- Cache state can be persisted and recovered with the Navy cache file.
- A small BigHash partition handles objects up to 1 KiB, while the remaining
  objects use Navy BlockCache.

This integration is not itself the research contribution. The proposed
mechanism is LSM-aware admission: combine SST lifetime signals and cache-entry
roles to avoid writing blocks that are likely to be invalidated by compaction
or accessed only once. The hypothesis is that this improves useful secondary
cache hit rate under the same device-write budget.

## Build

CacheLib is pinned as the `third-party/cachelib` Git submodule. The default
RocksDB build does not include this plugin.

The dependency build must use `FOLLY_NO_EXCEPTION_TRACER=ON`. CacheLib's
production `cachelib_common` target must not link Folly benchmark or exception
tracer libraries, and `cachelib_navy` must not compile `testing/MockDevice.cpp`
or link GTest/GMock. RocksDB and the plugin must use RTTI consistently because
CacheLib's Folly headers instantiate RTTI-backed singleton state.

Build CacheLib and all non-system dependencies into `.cachelib/`:

```bash
./build_cachelib.sh
source .cachelib/config.env
```

`build_cachelib.sh` verifies the pinned CacheLib commit, initializes its pinned
nested submodules, applies the RocksDB compatibility patch in an isolated
source copy, and performs a rootless local build. Change install and runtime
paths in `build_cachelib.conf` or with `CACHELIB_*` environment variables.
If a complete Boost installation is already available, set
`CACHELIB_BOOST_ROOT` to its prefix to avoid downloading and rebuilding the
pinned Boost archive. Without that override, the script fetches Boost 1.83.0
with a fixed SHA-256 checksum.

For repeated builds on a machine that already has the complete CacheLib
dependency stack, `CACHELIB_BOOTSTRAP_PREFIX` may point at that prefix. The
script still checks out the pinned submodule, applies the repository patch,
and rebuilds CacheLib Common/Navy into `.cachelib/installed`; only third-party
dependencies are reused. Without this override, all dependencies are fetched
and built under `.cachelib/`.

With Make:

```bash
make -j8 db_bench \
  USE_RTTI=1 \
  ROCKSDB_PLUGINS=cachelib_secondary_cache
```

With CMake:

```bash
cmake -S . -B build \
  -DUSE_RTTI=ON \
  -DROCKSDB_PLUGINS=cachelib_secondary_cache \
  -DCACHELIB_ROOT="${CACHELIB_ROOT}" \
  -DCACHELIB_DEPENDENCY_INCLUDE_DIRS="${CACHELIB_DEPENDENCY_INCLUDE_DIRS}" \
  -DCACHELIB_DEPENDENCY_LIBRARY_DIRS="${CACHELIB_DEPENDENCY_LIBRARY_DIRS}"
cmake --build build --target db_bench
```

## Configuration

`CACHELIB_ROOT` is a build-time dependency path. Navy's cache file, capacity,
layout, and worker counts are runtime RocksDB options.

Generate an OPTIONS file from `build_cachelib.conf`:

```bash
./build_cachelib.sh options
./db_bench --options_file=.cachelib/cachelib.options.ini ...
```

The generated block cache option has this form:

```text
block_cache={capacity=1G;num_shard_bits=4;secondary_cache={id=CacheLibSecondaryCache;cache_file=/tmp/rocksdb-cachelib.navy;capacity=64G;metadata_size=0;region_size=16M;reader_threads=4;writer_threads=4;request_ordering_shard_power=12;truncate=false}}
```

`metadata_size=0` selects the plugin default: 1% of device capacity with a
4 MiB minimum, aligned to `block_size`. Set an explicit larger value for
workloads with unusually many cached blocks. The pinned CacheLib patch also
fixes a RecordIO boundary bug that previously dropped a final metadata block
when serialized metadata ended exactly at the partition boundary.

When `truncate=false`, a missing Navy file or one whose size differs from the
configured capacity is initialized automatically. A matching file is
recovered; if its
metadata cannot be recovered, it is reset. Set
`CACHELIB_NAVY_TRUNCATE=true` only when the existing cache contents should be
discarded unconditionally.

The older `db_bench --secondary_cache_uri=...` flag remains supported, but the
standard RocksDB OPTIONS path is preferred for local deployment.

## Evaluation

The existing fixed 10 GiB `fillrandom` plus `overwrite` workload contains no
read phase, so it cannot demonstrate a secondary-cache benefit. It remains a
non-regression gate for the write path and must continue to use
`enable_blob_files=false`.

Cache-policy evaluation needs a separate immutable read workload with:

- a database larger than the primary block cache;
- repeatable skew and a controlled working-set shift;
- cold, warm, and restart-recovery phases;
- primary-cache, built-in compressed-secondary-cache, and CacheLib/Navy
  controls;
- secondary-cache hit rate, storage-read bytes, p50/p99 latency, Navy bytes
  written, and cache write amplification.

The LSM-aware policy should only be implemented after the control integration
passes correctness tests and its device-write counters are reproducible.
