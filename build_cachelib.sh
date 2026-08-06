#!/usr/bin/env bash

set -euo pipefail

readonly ROCKSDB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly CACHELIB_SUBMODULE="${ROCKSDB_ROOT}/third-party/cachelib"
readonly CACHELIB_COMMIT="5cfae53826ec3ea98242f8e10ccd0a6c2453e3a3"
readonly CACHELIB_PATCH="${ROCKSDB_ROOT}/plugin/cachelib_secondary_cache/patches/cachelib-v2024.12.30.00-rocksdb.patch"
readonly CACHELIB_MANIFESTS="${ROCKSDB_ROOT}/plugin/cachelib_secondary_cache/getdeps"
readonly CACHELIB_CONFIG="${CACHELIB_CONFIG:-${ROCKSDB_ROOT}/build_cachelib.conf}"

if [[ -f "${CACHELIB_CONFIG}" ]]; then
  # shellcheck source=build_cachelib.conf
  source "${CACHELIB_CONFIG}"
fi

: "${CACHELIB_STATE_ROOT:=${ROCKSDB_ROOT}/.cachelib}"
: "${CACHELIB_PREFIX:=${CACHELIB_STATE_ROOT}/installed}"
: "${CACHELIB_JOBS:=8}"
: "${CACHELIB_BUILD_TYPE:=RelWithDebInfo}"
: "${CACHELIB_BOOTSTRAP_PREFIX:=}"
: "${CACHELIB_BOOST_ROOT:=${BOOST_ROOT:-}}"
: "${CACHELIB_OPTIONS_FILE:=${CACHELIB_STATE_ROOT}/cachelib.options.ini}"
: "${CACHELIB_PRIMARY_CACHE_CAPACITY:=1073741824}"
: "${CACHELIB_PRIMARY_CACHE_SHARD_BITS:=4}"
: "${CACHELIB_NAVY_CACHE_FILE:=/tmp/rocksdb-cachelib.navy}"
: "${CACHELIB_NAVY_CAPACITY:=68719476736}"
: "${CACHELIB_NAVY_METADATA_SIZE:=0}"
: "${CACHELIB_NAVY_REGION_SIZE:=16777216}"
: "${CACHELIB_NAVY_READER_THREADS:=4}"
: "${CACHELIB_NAVY_WRITER_THREADS:=4}"
: "${CACHELIB_NAVY_ORDERING_SHARD_POWER:=12}"
: "${CACHELIB_NAVY_TRUNCATE:=false}"

readonly CACHELIB_SOURCE_COPY="${CACHELIB_STATE_ROOT}/src"
readonly CACHELIB_GETDEPS_ROOT="${CACHELIB_STATE_ROOT}/getdeps"
readonly CACHELIB_SCRATCH="${CACHELIB_STATE_ROOT}/scratch"
readonly CACHELIB_DEPS="${CACHELIB_STATE_ROOT}/deps"
readonly CACHELIB_BOOTSTRAP_BUILD="${CACHELIB_STATE_ROOT}/bootstrap-build"
readonly CACHELIB_BOOTSTRAP_INCLUDES="${CACHELIB_STATE_ROOT}/bootstrap-includes"
readonly CACHELIB_GFLAGS_PREFIX="${CACHELIB_STATE_ROOT}/gflags"
readonly CACHELIB_ENV_FILE="${CACHELIB_STATE_ROOT}/config.env"
readonly CACHELIB_STAMP="${CACHELIB_PREFIX}/.rocksdb-cachelib-build"

usage() {
  cat <<EOF
Usage: $0 [command]

Commands:
  build         initialize, fetch, patch, and build CacheLib (default)
  init          initialize the pinned CacheLib submodule tree
  fetch         fetch sources without compiling
  options       generate a RocksDB OPTIONS file using CacheLib Navy
  env           print shell exports for building RocksDB
  print-config  print the resolved local build and runtime configuration
  clean         remove local CacheLib build state under CACHELIB_STATE_ROOT

Configuration:
  Edit ${CACHELIB_CONFIG}, or override any CACHELIB_* variable in the
  environment. No system directories are modified and sudo is never used.
EOF
}

die() {
  echo "build_cachelib.sh: $*" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

check_integer() {
  local name="$1"
  local value="$2"
  [[ "${value}" =~ ^[0-9]+$ ]] || die "${name} must be a non-negative integer"
}

check_boolean() {
  local name="$1"
  local value="$2"
  [[ "${value}" == "true" || "${value}" == "false" ]] ||
    die "${name} must be true or false"
}

check_config() {
  check_integer CACHELIB_JOBS "${CACHELIB_JOBS}"
  check_integer CACHELIB_PRIMARY_CACHE_CAPACITY \
    "${CACHELIB_PRIMARY_CACHE_CAPACITY}"
  check_integer CACHELIB_PRIMARY_CACHE_SHARD_BITS \
    "${CACHELIB_PRIMARY_CACHE_SHARD_BITS}"
  check_integer CACHELIB_NAVY_CAPACITY "${CACHELIB_NAVY_CAPACITY}"
  check_integer CACHELIB_NAVY_METADATA_SIZE "${CACHELIB_NAVY_METADATA_SIZE}"
  check_integer CACHELIB_NAVY_REGION_SIZE "${CACHELIB_NAVY_REGION_SIZE}"
  check_integer CACHELIB_NAVY_READER_THREADS \
    "${CACHELIB_NAVY_READER_THREADS}"
  check_integer CACHELIB_NAVY_WRITER_THREADS \
    "${CACHELIB_NAVY_WRITER_THREADS}"
  check_integer CACHELIB_NAVY_ORDERING_SHARD_POWER \
    "${CACHELIB_NAVY_ORDERING_SHARD_POWER}"
  check_boolean CACHELIB_NAVY_TRUNCATE "${CACHELIB_NAVY_TRUNCATE}"
  [[ "${CACHELIB_JOBS}" -gt 0 ]] || die "CACHELIB_JOBS must be positive"
  [[ "${CACHELIB_NAVY_CAPACITY}" -gt 0 ]] ||
    die "CACHELIB_NAVY_CAPACITY must be positive"
  if [[ -n "${CACHELIB_BOOTSTRAP_PREFIX}" ]]; then
    [[ -f "${CACHELIB_BOOTSTRAP_PREFIX}/lib/libfolly.so" ]] ||
      die "CACHELIB_BOOTSTRAP_PREFIX does not contain Folly"
    [[ -f "${CACHELIB_BOOTSTRAP_PREFIX}/lib/libthriftprotocol.so" ]] ||
      die "CACHELIB_BOOTSTRAP_PREFIX does not contain FBThrift"
    [[ -x "${CACHELIB_BOOTSTRAP_PREFIX}/bin/thrift1" ]] ||
      die "CACHELIB_BOOTSTRAP_PREFIX does not contain thrift1"
    if [[ -z "${CACHELIB_BOOST_ROOT}" &&
          -f "${CACHELIB_BOOTSTRAP_PREFIX}/usr/include/boost/version.hpp" ]]; then
      CACHELIB_BOOST_ROOT="${CACHELIB_BOOTSTRAP_PREFIX}/usr"
    fi
  fi
  if [[ -n "${CACHELIB_BOOST_ROOT}" ]]; then
    [[ -f "${CACHELIB_BOOST_ROOT}/include/boost/version.hpp" ]] ||
      die "CACHELIB_BOOST_ROOT does not contain Boost headers"
    export BOOST_ROOT="${CACHELIB_BOOST_ROOT}"
  fi
}

init_submodules() {
  require_command git
  if ! git -C "${CACHELIB_SUBMODULE}" rev-parse --is-inside-work-tree \
      >/dev/null 2>&1; then
    git -C "${ROCKSDB_ROOT}" submodule update --init --checkout \
      third-party/cachelib
  fi
  local actual_commit
  actual_commit="$(git -C "${CACHELIB_SUBMODULE}" rev-parse HEAD)"
  if [[ "${actual_commit}" != "${CACHELIB_COMMIT}" ]]; then
    git -C "${ROCKSDB_ROOT}" submodule update --init --checkout \
      third-party/cachelib
    actual_commit="$(git -C "${CACHELIB_SUBMODULE}" rev-parse HEAD)"
  fi
  [[ "${actual_commit}" == "${CACHELIB_COMMIT}" ]] ||
    die "CacheLib submodule is ${actual_commit}, expected ${CACHELIB_COMMIT}"

  local nested_status
  nested_status="$(
    git -C "${CACHELIB_SUBMODULE}" submodule status --recursive 2>/dev/null ||
      true
  )"
  if [[ -z "${nested_status}" ]] ||
      grep -Eq '^[+-U]' <<<"${nested_status}"; then
    git -C "${CACHELIB_SUBMODULE}" submodule update --init --checkout \
      --recursive
  fi
}

prepare_source_copy() {
  require_command git
  mkdir -p "${CACHELIB_STATE_ROOT}"
  rm -rf "${CACHELIB_SOURCE_COPY}" "${CACHELIB_GETDEPS_ROOT}"
  git clone --quiet --no-hardlinks "${CACHELIB_SUBMODULE}" \
    "${CACHELIB_SOURCE_COPY}"
  git -C "${CACHELIB_SOURCE_COPY}" checkout --quiet "${CACHELIB_COMMIT}"
  git -C "${CACHELIB_SOURCE_COPY}" apply "${CACHELIB_PATCH}"

  cp -a \
    "${CACHELIB_SUBMODULE}/cachelib/external/folly/build/fbcode_builder" \
    "${CACHELIB_GETDEPS_ROOT}"
  local manifest
  for manifest in cachelib folly fizz wangle mvfst fbthrift openssl boost \
      xxhash; do
    install -m 0644 "${CACHELIB_MANIFESTS}/${manifest}" \
      "${CACHELIB_GETDEPS_ROOT}/manifests/${manifest}"
  done
}

getdeps_command() {
  local command="$1"
  shift
  local getdeps="${CACHELIB_GETDEPS_ROOT}/getdeps.py"
  [[ -x "${getdeps}" ]] || die "getdeps.py is missing; run '$0 init'"

  local args=(
    --scratch-path "${CACHELIB_SCRATCH}" \
    --install-prefix "${CACHELIB_DEPS}" \
    --num-jobs "${CACHELIB_JOBS}" \
    --allow-system-packages \
    --no-tests \
    --src-dir="cachelib:${CACHELIB_SOURCE_COPY}" \
    --src-dir="folly:${CACHELIB_SUBMODULE}/cachelib/external/folly" \
    --src-dir="fizz:${CACHELIB_SUBMODULE}/cachelib/external/fizz" \
    --src-dir="wangle:${CACHELIB_SUBMODULE}/cachelib/external/wangle" \
    --src-dir="mvfst:${CACHELIB_SUBMODULE}/cachelib/external/mvfst" \
    --src-dir="fbthrift:${CACHELIB_SUBMODULE}/cachelib/external/fbthrift" \
    --install-dir="cachelib:${CACHELIB_PREFIX}" \
  )
  if [[ "${command}" == "build" ]]; then
    args+=(--build-type "${CACHELIB_BUILD_TYPE}")
  fi
  python3 "${getdeps}" "${command}" "${args[@]}" "$@" \
    "${CACHELIB_MANIFESTS}/cachelib"
}

fetch_sources() {
  init_submodules
  prepare_source_copy
  if [[ -z "${CACHELIB_BOOTSTRAP_PREFIX}" ]]; then
    getdeps_command fetch --recursive
  else
    echo "Using dependency bootstrap ${CACHELIB_BOOTSTRAP_PREFIX}"
  fi
}

dependency_prefixes() {
  if [[ -n "${CACHELIB_BOOTSTRAP_PREFIX}" ]]; then
    printf '%s\n' "${CACHELIB_BOOTSTRAP_PREFIX}"
    if [[ -n "${CACHELIB_BOOST_ROOT}" &&
          "${CACHELIB_BOOST_ROOT}" != "${CACHELIB_BOOTSTRAP_PREFIX}/usr" ]]; then
      printf '%s\n' "${CACHELIB_BOOST_ROOT}"
    fi
    return
  fi
  local getdeps="${CACHELIB_GETDEPS_ROOT}/getdeps.py"
  python3 "${getdeps}" show-inst-dir \
    --scratch-path "${CACHELIB_SCRATCH}" \
    --install-prefix "${CACHELIB_DEPS}" \
    --allow-system-packages \
    --recursive \
    --src-dir="cachelib:${CACHELIB_SOURCE_COPY}" \
    --src-dir="folly:${CACHELIB_SUBMODULE}/cachelib/external/folly" \
    --src-dir="fizz:${CACHELIB_SUBMODULE}/cachelib/external/fizz" \
    --src-dir="wangle:${CACHELIB_SUBMODULE}/cachelib/external/wangle" \
    --src-dir="mvfst:${CACHELIB_SUBMODULE}/cachelib/external/mvfst" \
    --src-dir="fbthrift:${CACHELIB_SUBMODULE}/cachelib/external/fbthrift" \
    --install-dir="cachelib:${CACHELIB_PREFIX}" \
    "${CACHELIB_MANIFESTS}/cachelib" |
    while IFS= read -r prefix; do
      [[ "${prefix}" == "${CACHELIB_PREFIX}" ]] || printf '%s\n' "${prefix}"
    done
  if [[ -n "${CACHELIB_BOOST_ROOT}" ]]; then
    printf '%s\n' "${CACHELIB_BOOST_ROOT}"
  fi
}

dependency_include_paths() {
  printf '%s:%s' "${CACHELIB_BOOTSTRAP_INCLUDES}/include" \
    "${CACHELIB_BOOTSTRAP_INCLUDES}/usr/include"
}

prepare_dependency_includes() {
  local prefixes="$1"
  rm -rf "${CACHELIB_BOOTSTRAP_INCLUDES}"
  mkdir -p "${CACHELIB_BOOTSTRAP_INCLUDES}/include" \
    "${CACHELIB_BOOTSTRAP_INCLUDES}/usr/include"

  local prefix
  while IFS= read -r prefix; do
    local relative
    for relative in include usr/include; do
      local source="${prefix}/${relative}"
      local target="${CACHELIB_BOOTSTRAP_INCLUDES}/${relative}"
      [[ -d "${source}" ]] || continue
      while IFS= read -r -d '' path; do
        local name
        name="$(basename "${path}")"
        [[ "${name}" == "gtest" || "${name}" == "gmock" ]] && continue
        [[ -e "${target}/${name}" || -L "${target}/${name}" ]] && continue
        ln -s "${path}" "${target}/${name}"
      done < <(find "${source}" -mindepth 1 -maxdepth 1 -print0)
    done
  done <<<"${prefixes}"
}

join_library_paths() {
  local result=""
  local prefix
  while IFS= read -r prefix; do
    local path
    for path in "${prefix}/lib" "${prefix}/lib64" "${prefix}/usr/lib" \
        "${prefix}/usr/lib64" "${prefix}"/usr/lib/*-linux-gnu; do
      [[ -d "${path}" ]] || continue
      [[ ":${result}:" == *":${path}:"* ]] && continue
      if [[ -n "${result}" ]]; then
        result+=":"
      fi
      result+="${path}"
    done
  done
  printf '%s' "${result}"
}

prepare_gflags_prefix() {
  local prefixes="$1"
  rm -rf "${CACHELIB_GFLAGS_PREFIX}"

  local gflags_prefix=""
  local prefix
  while IFS= read -r prefix; do
    if [[ -d "${prefix}/include/gflags" &&
          -f "${prefix}/lib/cmake/gflags/gflags-config.cmake" ]]; then
      gflags_prefix="${prefix}"
      break
    fi
  done <<<"${prefixes}"
  [[ -n "${gflags_prefix}" ]] || return

  mkdir -p "${CACHELIB_GFLAGS_PREFIX}/include" \
    "${CACHELIB_GFLAGS_PREFIX}/lib/cmake"
  ln -s "${gflags_prefix}/include/gflags" \
    "${CACHELIB_GFLAGS_PREFIX}/include/gflags"
  local path
  for path in "${gflags_prefix}"/lib/libgflags*.so*; do
    [[ -e "${path}" ]] || continue
    ln -s "${path}" "${CACHELIB_GFLAGS_PREFIX}/lib/$(basename "${path}")"
  done
  ln -s "${gflags_prefix}/lib/cmake/gflags" \
    "${CACHELIB_GFLAGS_PREFIX}/lib/cmake/gflags"
}

write_env_file() {
  local prefixes
  prefixes="$(dependency_prefixes)"
  prepare_dependency_includes "${prefixes}"
  prepare_gflags_prefix "${prefixes}"
  local include_path
  local library_path
  include_path="$(
    dependency_include_paths
  )"
  library_path="$(
    printf '%s\n' "${prefixes}" | join_library_paths
  )"

  mkdir -p "${CACHELIB_STATE_ROOT}"
  cat >"${CACHELIB_ENV_FILE}" <<EOF
export CACHELIB_ROOT="${CACHELIB_PREFIX}"
export CACHELIB_INCLUDE_DIR="${CACHELIB_PREFIX}/include"
export CACHELIB_LIBRARY_DIR="${CACHELIB_PREFIX}/lib"
export CACHELIB_DEPENDENCY_INCLUDE_DIRS="${include_path//:/;}"
export CACHELIB_DEPENDENCY_LIBRARY_DIRS="${library_path//:/;}"
export CPATH="${CACHELIB_PREFIX}/include${include_path:+:${include_path}}\${CPATH:+:\${CPATH}}"
export LIBRARY_PATH="${CACHELIB_PREFIX}/lib${library_path:+:${library_path}}\${LIBRARY_PATH:+:\${LIBRARY_PATH}}"
export LD_LIBRARY_PATH="${CACHELIB_PREFIX}/lib${library_path:+:${library_path}}\${LD_LIBRARY_PATH:+:\${LD_LIBRARY_PATH}}"
export CMAKE_INCLUDE_PATH="${CACHELIB_PREFIX}/include${include_path:+:${include_path}}\${CMAKE_INCLUDE_PATH:+:\${CMAKE_INCLUDE_PATH}}"
export CMAKE_LIBRARY_PATH="${CACHELIB_PREFIX}/lib${library_path:+:${library_path}}\${CMAKE_LIBRARY_PATH:+:\${CMAKE_LIBRARY_PATH}}"
export CMAKE_PREFIX_PATH="${CACHELIB_PREFIX}$([[ -d "${CACHELIB_GFLAGS_PREFIX}" ]] && printf ':%s' "${CACHELIB_GFLAGS_PREFIX}")\${CMAKE_PREFIX_PATH:+:\${CMAKE_PREFIX_PATH}}"
EOF
}

build_with_bootstrap() {
  require_command cmake
  rm -rf "${CACHELIB_BOOTSTRAP_BUILD}"
  if [[ "${CACHELIB_PREFIX}" == "${CACHELIB_STATE_ROOT}/"* ]]; then
    rm -rf "${CACHELIB_PREFIX}"
  fi
  mkdir -p "${CACHELIB_PREFIX}"

  local prefix_path="${CACHELIB_BOOTSTRAP_PREFIX}"
  if [[ -d "${CACHELIB_BOOTSTRAP_PREFIX}/usr" ]]; then
    prefix_path+=";${CACHELIB_BOOTSTRAP_PREFIX}/usr"
  fi

  local path="${CACHELIB_BOOTSTRAP_PREFIX}/bin:${PATH}"
  local library_path
  library_path="$(
    printf '%s\n' "${CACHELIB_BOOTSTRAP_PREFIX}" | join_library_paths
  )"
  local pkg_config_path="${CACHELIB_BOOTSTRAP_PREFIX}/lib/pkgconfig"

  PATH="${path}" \
  LD_LIBRARY_PATH="${library_path}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
  PKG_CONFIG_PATH="${pkg_config_path}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}" \
    cmake -S "${CACHELIB_SOURCE_COPY}/cachelib" \
      -B "${CACHELIB_BOOTSTRAP_BUILD}" \
      -DCMAKE_BUILD_TYPE="${CACHELIB_BUILD_TYPE}" \
      -DCMAKE_INSTALL_PREFIX="${CACHELIB_PREFIX}" \
      -DCMAKE_PREFIX_PATH="${prefix_path}" \
      -DBOOST_ROOT="${CACHELIB_BOOST_ROOT}" \
      -DBUILD_SHARED_LIBS=ON \
      -DBUILD_TESTS=OFF \
      -DCACHELIB_BUILD_NAVY_ONLY=ON
  PATH="${path}" \
  LD_LIBRARY_PATH="${library_path}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    cmake --build "${CACHELIB_BOOTSTRAP_BUILD}" --target install \
      --parallel "${CACHELIB_JOBS}"
}

generate_options() {
  mkdir -p "$(dirname "${CACHELIB_OPTIONS_FILE}")"
  cat >"${CACHELIB_OPTIONS_FILE}" <<EOF
[Version]
  rocksdb_version=9.1.1
  options_file_version=1.1

[DBOptions]
  create_if_missing=true

[CFOptions "default"]
  table_factory=BlockBasedTable
  enable_blob_files=false

[TableOptions/BlockBasedTable "default"]
  block_cache={capacity=${CACHELIB_PRIMARY_CACHE_CAPACITY};num_shard_bits=${CACHELIB_PRIMARY_CACHE_SHARD_BITS};secondary_cache={id=CacheLibSecondaryCache;cache_file=${CACHELIB_NAVY_CACHE_FILE};capacity=${CACHELIB_NAVY_CAPACITY};metadata_size=${CACHELIB_NAVY_METADATA_SIZE};region_size=${CACHELIB_NAVY_REGION_SIZE};reader_threads=${CACHELIB_NAVY_READER_THREADS};writer_threads=${CACHELIB_NAVY_WRITER_THREADS};request_ordering_shard_power=${CACHELIB_NAVY_ORDERING_SHARD_POWER};truncate=${CACHELIB_NAVY_TRUNCATE}}}
EOF
  echo "Generated ${CACHELIB_OPTIONS_FILE}"
}

build_cachelib() {
  fetch_sources
  require_command python3
  mkdir -p "${CACHELIB_PREFIX}"
  if [[ -n "${CACHELIB_BOOTSTRAP_PREFIX}" ]]; then
    build_with_bootstrap
  else
    getdeps_command build
  fi

  write_env_file
  generate_options
  cat >"${CACHELIB_STAMP}" <<EOF
CACHELIB_COMMIT=${CACHELIB_COMMIT}
CACHELIB_PATCH_SHA256=$(sha256sum "${CACHELIB_PATCH}" | awk '{print $1}')
CACHELIB_BUILD_TYPE=${CACHELIB_BUILD_TYPE}
CACHELIB_BOOTSTRAP_PREFIX=${CACHELIB_BOOTSTRAP_PREFIX}
EOF

  echo "CacheLib installed in ${CACHELIB_PREFIX}"
  echo "Run: source ${CACHELIB_ENV_FILE}"
}

print_env() {
  [[ -f "${CACHELIB_ENV_FILE}" ]] ||
    die "${CACHELIB_ENV_FILE} does not exist; run '$0 build'"
  cat "${CACHELIB_ENV_FILE}"
}

print_config() {
  cat <<EOF
CACHELIB_COMMIT=${CACHELIB_COMMIT}
CACHELIB_SUBMODULE=${CACHELIB_SUBMODULE}
CACHELIB_STATE_ROOT=${CACHELIB_STATE_ROOT}
CACHELIB_PREFIX=${CACHELIB_PREFIX}
CACHELIB_JOBS=${CACHELIB_JOBS}
CACHELIB_BUILD_TYPE=${CACHELIB_BUILD_TYPE}
CACHELIB_BOOTSTRAP_PREFIX=${CACHELIB_BOOTSTRAP_PREFIX}
CACHELIB_BOOST_ROOT=${CACHELIB_BOOST_ROOT}
CACHELIB_ENV_FILE=${CACHELIB_ENV_FILE}
CACHELIB_OPTIONS_FILE=${CACHELIB_OPTIONS_FILE}
CACHELIB_NAVY_CACHE_FILE=${CACHELIB_NAVY_CACHE_FILE}
CACHELIB_NAVY_CAPACITY=${CACHELIB_NAVY_CAPACITY}
CACHELIB_NAVY_METADATA_SIZE=${CACHELIB_NAVY_METADATA_SIZE}
CACHELIB_NAVY_TRUNCATE=${CACHELIB_NAVY_TRUNCATE}
EOF
}

clean_state() {
  [[ "${CACHELIB_STATE_ROOT}" == "${ROCKSDB_ROOT}/"* ]] ||
    die "refusing to remove CACHELIB_STATE_ROOT outside the repository"
  rm -rf "${CACHELIB_STATE_ROOT}"
}

check_config

case "${1:-build}" in
  build) build_cachelib ;;
  init) init_submodules ;;
  fetch) fetch_sources ;;
  options) generate_options ;;
  env) print_env ;;
  print-config) print_config ;;
  clean) clean_state ;;
  -h | --help | help) usage ;;
  *) usage >&2; exit 1 ;;
esac
