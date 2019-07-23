/*
 * Copyright 2017 MapD Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "StringDictionary.h"
#include "../Shared/sqltypes.h"
#include "../Utils/Regexp.h"
#include "../Utils/StringLike.h"
#include "LeafHostInfo.h"
#include "Shared/Logger.h"
#include "Shared/thread_count.h"
#include "StringDictionaryClient.h"

#include <sys/fcntl.h>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/sort/spreadsort/string_sort.hpp>

#include <future>
#include <thread>

namespace {
const int SYSTEM_PAGE_SIZE = getpagesize();

size_t file_size(const int fd) {
  struct stat buf;
  int err = fstat(fd, &buf);
  CHECK_EQ(0, err);
  return buf.st_size;
}

int checked_open(const char* path, const bool recover) {
  auto fd = open(path, O_RDWR | O_CREAT | (recover ? O_APPEND : O_TRUNC), 0644);
  if (fd > 0) {
    return fd;
  }
  auto err = std::string("Dictionary path ") + std::string(path) +
             std::string(" does not exist.");
  LOG(ERROR) << err;
  throw DictPayloadUnavailable(err);
}

void* checked_mmap(const int fd, const size_t sz) {
  auto ptr = mmap(nullptr, sz, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
  CHECK(ptr != reinterpret_cast<void*>(-1));
#ifdef __linux__
#ifdef MADV_HUGEPAGE
  madvise(ptr, sz, MADV_RANDOM | MADV_WILLNEED | MADV_HUGEPAGE);
#else
  madvise(ptr, sz, MADV_RANDOM | MADV_WILLNEED);
#endif
#endif
  return ptr;
}
void checked_munmap(void* addr, size_t length) {
  CHECK_EQ(0, munmap(addr, length));
}

const uint64_t round_up_p2(const uint64_t num) {
  uint64_t in = num;
  in--;
  in |= in >> 1;
  in |= in >> 2;
  in |= in >> 4;
  in |= in >> 8;
  in |= in >> 16;
  in++;
  // TODO MAT deal with case where filesize has been increased but reality is
  // we are constrained to 2^31.
  // In that situation this calculation will wrap to zero
  if (in == 0 || (in > (UINT32_MAX))) {
    in = UINT32_MAX;
  }
  return in;
}

uint32_t rk_hash(const std::string& str) {
  uint32_t str_hash = 1;
  // rely on fact that unsigned overflow is defined and wraps
  for (size_t i = 0; i < str.size(); ++i) {
    str_hash = str_hash * 997 + str[i];
  }
  return str_hash;
}
}  // namespace

constexpr int32_t StringDictionary::INVALID_STR_ID;
constexpr size_t StringDictionary::MAX_STRLEN;
constexpr size_t StringDictionary::MAX_STRCOUNT;

StringDictionary::StringDictionary(const std::string& folder,
                                   const bool isTemp,
                                   const bool recover,
                                   const bool materializeHashes,
                                   size_t initial_capacity)
    : str_count_(0)
    , str_ids_(initial_capacity, INVALID_STR_ID)
    , rk_hashes_(initial_capacity)
    , isTemp_(isTemp)
    , materialize_hashes_(materializeHashes)
    , payload_fd_(-1)
    , offset_fd_(-1)
    , offset_map_(nullptr)
    , payload_map_(nullptr)
    , offset_file_size_(0)
    , payload_file_size_(0)
    , payload_file_off_(0)
    , strings_cache_(nullptr) {
  if (!isTemp && folder.empty()) {
    return;
  }

  // initial capacity must be a power of two for efficient bucket computation
  CHECK_EQ(size_t(0), (initial_capacity & (initial_capacity - 1)));
  if (!isTemp_) {
    boost::filesystem::path storage_path(folder);
    offsets_path_ = (storage_path / boost::filesystem::path("DictOffsets")).string();
    const auto payload_path =
        (storage_path / boost::filesystem::path("DictPayload")).string();
    payload_fd_ = checked_open(payload_path.c_str(), recover);
    offset_fd_ = checked_open(offsets_path_.c_str(), recover);
    payload_file_size_ = file_size(payload_fd_);
    offset_file_size_ = file_size(offset_fd_);
  }

  if (payload_file_size_ == 0) {
    addPayloadCapacity();
  }
  if (offset_file_size_ == 0) {
    addOffsetCapacity();
  }
  if (!isTemp_) {  // we never mmap or recover temp dictionaries
    payload_map_ = reinterpret_cast<char*>(checked_mmap(payload_fd_, payload_file_size_));
    offset_map_ =
        reinterpret_cast<StringIdxEntry*>(checked_mmap(offset_fd_, offset_file_size_));
    if (recover) {
      const size_t bytes = file_size(offset_fd_);
      if (bytes % sizeof(StringIdxEntry) != 0) {
        LOG(WARNING) << "Offsets " << offsets_path_ << " file is truncated";
      }
      const uint64_t str_count = bytes / sizeof(StringIdxEntry);
      // at this point we know the size of the StringDict we need to load
      // so lets reallocate the vector to the correct size
      const uint64_t max_entries = round_up_p2(str_count * 2 + 1);
      std::vector<int32_t> new_str_ids(max_entries, INVALID_STR_ID);
      str_ids_.swap(new_str_ids);
      if (materialize_hashes_) {
        std::vector<uint32_t> new_rk_hashes(max_entries / 2);
        rk_hashes_.swap(new_rk_hashes);
      }
      unsigned string_id = 0;
      mapd_lock_guard<mapd_shared_mutex> write_lock(rw_mutex_);

      uint32_t thread_inits = 0;
      const auto thread_count = std::thread::hardware_concurrency();
      const uint32_t items_per_thread = std::max<uint32_t>(
          2000, std::min<uint32_t>(200000, (str_count / thread_count) + 1));
      std::vector<std::future<std::vector<std::pair<uint32_t, unsigned int>>>>
          dictionary_futures;
      for (string_id = 0; string_id < str_count; string_id += items_per_thread) {
        dictionary_futures.emplace_back(std::async(
            std::launch::async, [string_id, str_count, items_per_thread, this] {
              std::vector<std::pair<uint32_t, unsigned int>> hashVec;
              for (uint32_t curr_id = string_id;
                   curr_id < string_id + items_per_thread && curr_id < str_count;
                   curr_id++) {
                const auto recovered = getStringFromStorage(curr_id);
                if (recovered.canary) {
                  // hit the canary, recovery finished
                  break;
                } else {
                  std::string temp(recovered.c_str_ptr, recovered.size);
                  hashVec.emplace_back(std::make_pair(rk_hash(temp), temp.size()));
                }
              }
              return hashVec;
            }));
        thread_inits++;
        if (thread_inits % thread_count == 0) {
          processDictionaryFutures(dictionary_futures);
        }
      }
      // gather last few threads
      if (dictionary_futures.size() != 0) {
        processDictionaryFutures(dictionary_futures);
      }
    }
  }
}

void StringDictionary::processDictionaryFutures(
    std::vector<std::future<std::vector<std::pair<uint32_t, unsigned int>>>>&
        dictionary_futures) {
  for (auto& dictionary_future : dictionary_futures) {
    dictionary_future.wait();
    auto hashVec = dictionary_future.get();
    for (auto& hash : hashVec) {
      uint32_t bucket = computeUniqueBucketWithHash(hash.first, str_ids_);
      payload_file_off_ += hash.second;
      str_ids_[bucket] = static_cast<int32_t>(str_count_);
      if (materialize_hashes_) {
        rk_hashes_[str_count_] = hash.first;
      }
      ++str_count_;
    }
  }
  dictionary_futures.clear();
}

StringDictionary::StringDictionary(const LeafHostInfo& host, const DictRef dict_ref)
    : strings_cache_(nullptr)
    , client_(new StringDictionaryClient(host, dict_ref, true))
    , client_no_timeout_(new StringDictionaryClient(host, dict_ref, false)) {}

StringDictionary::~StringDictionary() noexcept {
  if (client_) {
    return;
  }
  if (payload_map_) {
    if (!isTemp_) {
      CHECK(offset_map_);
      checked_munmap(payload_map_, payload_file_size_);
      checked_munmap(offset_map_, offset_file_size_);
      CHECK_GE(payload_fd_, 0);
      close(payload_fd_);
      CHECK_GE(offset_fd_, 0);
      close(offset_fd_);
    } else {
      CHECK(offset_map_);
      free(payload_map_);
      free(offset_map_);
    }
  }
}

int32_t StringDictionary::getOrAdd(const std::string& str) noexcept {
  if (client_) {
    std::vector<int32_t> string_ids;
    client_->get_or_add_bulk(string_ids, {str});
    CHECK_EQ(size_t(1), string_ids.size());
    return string_ids.front();
  }
  return getOrAddImpl(str);
}

namespace {

template <class T>
void log_encoding_error(const std::string& str) {
  LOG(ERROR) << "Could not encode string: " << str
             << ", the encoded value doesn't fit in " << sizeof(T) * 8
             << " bits. Will store NULL instead.";
}

}  // namespace

void StringDictionary::getOrAddBulkArray(
    const std::vector<std::vector<std::string>>& string_array_vec,
    std::vector<std::vector<int32_t>>& ids_array_vec) {
  ids_array_vec.resize(string_array_vec.size());
  for (size_t i = 0; i < string_array_vec.size(); i++) {
    auto& strings = string_array_vec[i];
    auto& ids = ids_array_vec[i];
    ids.resize(strings.size());
    getOrAddBulk(strings, &ids[0]);
  }
}

template <class T>
void StringDictionary::getOrAddBulk(const std::vector<std::string>& string_vec,
                                    T* encoded_vec) {
  if (client_no_timeout_) {
    getOrAddBulkRemote(string_vec, encoded_vec);
    return;
  }
  size_t out_idx{0};
  mapd_lock_guard<mapd_shared_mutex> write_lock(rw_mutex_);

  for (const auto& str : string_vec) {
    if (str.empty()) {
      encoded_vec[out_idx++] = inline_int_null_value<T>();
      continue;
    }
    CHECK(str.size() <= MAX_STRLEN);
    uint32_t bucket;
    const uint32_t hash = rk_hash(str);
    bucket = computeBucket(hash, str, str_ids_, false);
    if (str_ids_[bucket] != INVALID_STR_ID) {
      encoded_vec[out_idx++] = str_ids_[bucket];
      continue;
    }
    // need to add record to dictionary
    // check there is room
    if (str_count_ == static_cast<size_t>(max_valid_int_value<T>())) {
      log_encoding_error<T>(str);
      encoded_vec[out_idx++] = inline_int_null_value<T>();
      continue;
    }
    if (str_ids_[bucket] == INVALID_STR_ID) {
      CHECK_LT(str_count_, MAX_STRCOUNT)
          << "Maximum number (" << str_count_
          << ") of Dictionary encoded Strings reached for this column, offset path "
             "for column is  "
          << offsets_path_;
      if (fillRateIsHigh()) {
        // resize when more than 50% is full
        increaseCapacity();
        bucket = computeBucket(hash, str, str_ids_, false);
      }
      appendToStorage(str);

      str_ids_[bucket] = static_cast<int32_t>(str_count_);
      if (materialize_hashes_) {
        rk_hashes_[str_count_] = hash;
      }
      ++str_count_;
    }
    encoded_vec[out_idx++] = str_ids_[bucket];
  }
  invalidateInvertedIndex();
}
template void StringDictionary::getOrAddBulk(const std::vector<std::string>& string_vec,
                                             uint8_t* encoded_vec);
template void StringDictionary::getOrAddBulk(const std::vector<std::string>& string_vec,
                                             uint16_t* encoded_vec);
template void StringDictionary::getOrAddBulk(const std::vector<std::string>& string_vec,
                                             int32_t* encoded_vec);

template <class T>
void StringDictionary::getOrAddBulkRemote(const std::vector<std::string>& string_vec,
                                          T* encoded_vec) {
  CHECK(client_no_timeout_);
  std::vector<int32_t> string_ids;
  client_no_timeout_->get_or_add_bulk(string_ids, string_vec);
  size_t out_idx{0};
  for (size_t i = 0; i < string_ids.size(); ++i) {
    const auto string_id = string_ids[i];
    const bool invalid = string_id > max_valid_int_value<T>();
    if (invalid || string_id == inline_int_null_value<int32_t>()) {
      if (invalid) {
        log_encoding_error<T>(string_vec[i]);
      }
      encoded_vec[out_idx++] = inline_int_null_value<T>();
      continue;
    }
    encoded_vec[out_idx++] = string_id;
  }
}

template void StringDictionary::getOrAddBulkRemote(
    const std::vector<std::string>& string_vec,
    uint8_t* encoded_vec);
template void StringDictionary::getOrAddBulkRemote(
    const std::vector<std::string>& string_vec,
    uint16_t* encoded_vec);
template void StringDictionary::getOrAddBulkRemote(
    const std::vector<std::string>& string_vec,
    int32_t* encoded_vec);

int32_t StringDictionary::getIdOfString(const std::string& str) const {
  mapd_shared_lock<mapd_shared_mutex> read_lock(rw_mutex_);
  if (client_) {
    return client_->get(str);
  }
  return getUnlocked(str);
}

int32_t StringDictionary::getUnlocked(const std::string& str) const noexcept {
  const uint32_t hash = rk_hash(str);
  auto str_id = str_ids_[computeBucket(hash, str, str_ids_, false)];
  return str_id;
}

std::string StringDictionary::getString(int32_t string_id) const {
  mapd_shared_lock<mapd_shared_mutex> read_lock(rw_mutex_);
  if (client_) {
    std::string ret;
    client_->get_string(ret, string_id);
    return ret;
  }
  return getStringUnlocked(string_id);
}

std::string StringDictionary::getStringUnlocked(int32_t string_id) const noexcept {
  CHECK_LT(string_id, static_cast<int32_t>(str_count_));
  return getStringChecked(string_id);
}

std::pair<char*, size_t> StringDictionary::getStringBytes(int32_t string_id) const
    noexcept {
  mapd_shared_lock<mapd_shared_mutex> read_lock(rw_mutex_);
  CHECK(!client_);
  CHECK_LE(0, string_id);
  CHECK_LT(string_id, static_cast<int32_t>(str_count_));
  return getStringBytesChecked(string_id);
}

size_t StringDictionary::storageEntryCount() const {
  mapd_shared_lock<mapd_shared_mutex> read_lock(rw_mutex_);
  if (client_) {
    return client_->storage_entry_count();
  }
  return str_count_;
}

namespace {

bool is_like(const std::string& str,
             const std::string& pattern,
             const bool icase,
             const bool is_simple,
             const char escape) {
  return icase
             ? (is_simple ? string_ilike_simple(
                                str.c_str(), str.size(), pattern.c_str(), pattern.size())
                          : string_ilike(str.c_str(),
                                         str.size(),
                                         pattern.c_str(),
                                         pattern.size(),
                                         escape))
             : (is_simple ? string_like_simple(
                                str.c_str(), str.size(), pattern.c_str(), pattern.size())
                          : string_like(str.c_str(),
                                        str.size(),
                                        pattern.c_str(),
                                        pattern.size(),
                                        escape));
}

}  // namespace

std::vector<int32_t> StringDictionary::getLike(const std::string& pattern,
                                               const bool icase,
                                               const bool is_simple,
                                               const char escape,
                                               const size_t generation) const {
  mapd_lock_guard<mapd_shared_mutex> write_lock(rw_mutex_);
  if (client_) {
    return client_->get_like(pattern, icase, is_simple, escape, generation);
  }
  const auto cache_key = std::make_tuple(pattern, icase, is_simple, escape);
  const auto it = like_cache_.find(cache_key);
  if (it != like_cache_.end()) {
    return it->second;
  }
  std::vector<int32_t> result;
  std::vector<std::thread> workers;
  int worker_count = cpu_threads();
  CHECK_GT(worker_count, 0);
  std::vector<std::vector<int32_t>> worker_results(worker_count);
  CHECK_LE(generation, str_count_);
  for (int worker_idx = 0; worker_idx < worker_count; ++worker_idx) {
    workers.emplace_back([&worker_results,
                          &pattern,
                          generation,
                          icase,
                          is_simple,
                          escape,
                          worker_idx,
                          worker_count,
                          this]() {
      for (size_t string_id = worker_idx; string_id < generation;
           string_id += worker_count) {
        const auto str = getStringUnlocked(string_id);
        if (is_like(str, pattern, icase, is_simple, escape)) {
          worker_results[worker_idx].push_back(string_id);
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  for (const auto& worker_result : worker_results) {
    result.insert(result.end(), worker_result.begin(), worker_result.end());
  }
  // place result into cache for reuse if similar query
  const auto it_ok = like_cache_.insert(std::make_pair(cache_key, result));

  CHECK(it_ok.second);

  return result;
}

std::vector<int32_t> StringDictionary::getEquals(std::string pattern,
                                                 std::string comp_operator,
                                                 size_t generation) {
  std::vector<int32_t> result;
  auto eq_id_itr = equal_cache_.find(pattern);
  int32_t eq_id = MAX_STRLEN + 1;
  int32_t cur_size = str_count_;
  if (eq_id_itr != equal_cache_.end()) {
    auto eq_id = eq_id_itr->second;
    if (comp_operator == "=") {
      result.push_back(eq_id);
    } else {
      for (int32_t idx = 0; idx <= cur_size; idx++) {
        if (idx == eq_id) {
          continue;
        }
        result.push_back(idx);
      }
    }
  } else {
    std::vector<std::thread> workers;
    int worker_count = cpu_threads();
    CHECK_GT(worker_count, 0);
    std::vector<std::vector<int32_t>> worker_results(worker_count);
    CHECK_LE(generation, str_count_);
    for (int worker_idx = 0; worker_idx < worker_count; ++worker_idx) {
      workers.emplace_back(
          [&worker_results, &pattern, generation, worker_idx, worker_count, this]() {
            for (size_t string_id = worker_idx; string_id < generation;
                 string_id += worker_count) {
              const auto str = getStringUnlocked(string_id);
              if (str == pattern) {
                worker_results[worker_idx].push_back(string_id);
              }
            }
          });
    }
    for (auto& worker : workers) {
      worker.join();
    }
    for (const auto& worker_result : worker_results) {
      result.insert(result.end(), worker_result.begin(), worker_result.end());
    }
    if (result.size() > 0) {
      const auto it_ok = equal_cache_.insert(std::make_pair(pattern, result[0]));
      CHECK(it_ok.second);
      eq_id = result[0];
    }
    if (comp_operator == "<>") {
      for (int32_t idx = 0; idx <= cur_size; idx++) {
        if (idx == eq_id) {
          continue;
        }
        result.push_back(idx);
      }
    }
  }
  return result;
}

std::vector<int32_t> StringDictionary::getCompare(const std::string& pattern,
                                                  const std::string& comp_operator,
                                                  const size_t generation) {
  mapd_lock_guard<mapd_shared_mutex> write_lock(rw_mutex_);
  if (client_) {
    return client_->get_compare(pattern, comp_operator, generation);
  }
  std::vector<int32_t> ret;
  if (str_count_ == 0) {
    return ret;
  }
  if (sorted_cache.size() < str_count_) {
    if (comp_operator == "=" || comp_operator == "<>") {
      return getEquals(pattern, comp_operator, generation);
    }

    buildSortedCache();
  }
  auto cache_index = compare_cache_.get(pattern);

  if (!cache_index) {
    cache_index = std::make_shared<StringDictionary::compare_cache_value_t>();
    const auto cache_itr = std::lower_bound(
        sorted_cache.begin(),
        sorted_cache.end(),
        pattern,
        [this](decltype(sorted_cache)::value_type const& a, decltype(pattern)& b) {
          auto a_str = this->getStringFromStorage(a);
          return string_lt(a_str.c_str_ptr, a_str.size, b.c_str(), b.size());
        });

    if (cache_itr == sorted_cache.end()) {
      cache_index->index = sorted_cache.size() - 1;
      cache_index->diff = 1;
    } else {
      const auto cache_str = getStringFromStorage(*cache_itr);
      if (!string_eq(
              cache_str.c_str_ptr, cache_str.size, pattern.c_str(), pattern.size())) {
        cache_index->index = cache_itr - sorted_cache.begin() - 1;
        cache_index->diff = 1;
      } else {
        cache_index->index = cache_itr - sorted_cache.begin();
        cache_index->diff = 0;
      }
    }

    compare_cache_.put(pattern, cache_index);
  }

  // since we have a cache in form of vector of ints which is sorted according to
  // corresponding strings in the dictionary all we need is the index of the element
  // which equal to the pattern that we are trying to match or the index of “biggest”
  // element smaller than the pattern, to perform all the comparison operators over
  // string. The search function guarantees we have such index so now it is just the
  // matter to include all the elements in the result vector.

  // For < operator if the index that we have points to the element which is equal to
  // the pattern that we are searching for we simply get all the elements less than the
  // index. If the element pointed by the index is not equal to the pattern we are
  // comparing with we also need to include that index in result vector, except when the
  // index points to 0 and the pattern is lesser than the smallest value in the string
  // dictionary.

  if (comp_operator == "<") {
    size_t idx = cache_index->index;
    if (cache_index->diff) {
      idx = cache_index->index + 1;
      if (cache_index->index == 0 && cache_index->diff > 0) {
        idx = cache_index->index;
      }
    }
    for (size_t i = 0; i < idx; i++) {
      ret.push_back(sorted_cache[i]);
    }

    // For <= operator if the index that we have points to the element which is equal to
    // the pattern that we are searching for we want to include the element pointed by
    // the index in the result set. If the element pointed by the index is not equal to
    // the pattern we are comparing with we just want to include all the ids with index
    // less than the index that is cached, except when pattern that we are searching for
    // is smaller than the smallest string in the dictionary.

  } else if (comp_operator == "<=") {
    size_t idx = cache_index->index + 1;
    if (cache_index == 0 && cache_index->diff > 0) {
      idx = cache_index->index;
    }
    for (size_t i = 0; i < idx; i++) {
      ret.push_back(sorted_cache[i]);
    }

    // For > operator we want to get all the elements with index greater than the index
    // that we have except, when the pattern we are searching for is lesser than the
    // smallest string in the dictionary we also want to include the id of the index
    // that we have.

  } else if (comp_operator == ">") {
    size_t idx = cache_index->index + 1;
    if (cache_index->index == 0 && cache_index->diff > 0) {
      idx = cache_index->index;
    }
    for (size_t i = idx; i < sorted_cache.size(); i++) {
      ret.push_back(sorted_cache[i]);
    }

    // For >= operator when the indexed element that we have points to element which is
    // equal to the pattern we are searching for we want to include that in the result
    // vector. If the index that we have does not point to the string which is equal to
    // the pattern we are searching we don’t want to include that id into the result
    // vector except when the index is 0.

  } else if (comp_operator == ">=") {
    size_t idx = cache_index->index;
    if (cache_index->diff) {
      idx = cache_index->index + 1;
      if (cache_index->index == 0 && cache_index->diff > 0) {
        idx = cache_index->index;
      }
    }
    for (size_t i = idx; i < sorted_cache.size(); i++) {
      ret.push_back(sorted_cache[i]);
    }
  } else if (comp_operator == "=") {
    if (!cache_index->diff) {
      ret.push_back(sorted_cache[cache_index->index]);
    }

    // For <> operator it is simple matter of not including id of string which is equal
    // to pattern we are searching for.
  } else if (comp_operator == "<>") {
    if (!cache_index->diff) {
      size_t idx = cache_index->index;
      for (size_t i = 0; i < idx; i++) {
        ret.push_back(sorted_cache[i]);
      }
      ++idx;
      for (size_t i = idx; i < sorted_cache.size(); i++) {
        ret.push_back(sorted_cache[i]);
      }
    } else {
      for (size_t i = 0; i < sorted_cache.size(); i++) {
        ret.insert(ret.begin(), sorted_cache.begin(), sorted_cache.end());
      }
    }

  } else {
    std::runtime_error("Unsupported string comparison operator");
  }
  return ret;
}

namespace {

bool is_regexp_like(const std::string& str,
                    const std::string& pattern,
                    const char escape) {
  return regexp_like(str.c_str(), str.size(), pattern.c_str(), pattern.size(), escape);
}

}  // namespace

std::vector<int32_t> StringDictionary::getRegexpLike(const std::string& pattern,
                                                     const char escape,
                                                     const size_t generation) const {
  mapd_lock_guard<mapd_shared_mutex> write_lock(rw_mutex_);
  if (client_) {
    return client_->get_regexp_like(pattern, escape, generation);
  }
  const auto cache_key = std::make_pair(pattern, escape);
  const auto it = regex_cache_.find(cache_key);
  if (it != regex_cache_.end()) {
    return it->second;
  }
  std::vector<int32_t> result;
  std::vector<std::thread> workers;
  int worker_count = cpu_threads();
  CHECK_GT(worker_count, 0);
  std::vector<std::vector<int32_t>> worker_results(worker_count);
  CHECK_LE(generation, str_count_);
  for (int worker_idx = 0; worker_idx < worker_count; ++worker_idx) {
    workers.emplace_back([&worker_results,
                          &pattern,
                          generation,
                          escape,
                          worker_idx,
                          worker_count,
                          this]() {
      for (size_t string_id = worker_idx; string_id < generation;
           string_id += worker_count) {
        const auto str = getStringUnlocked(string_id);
        if (is_regexp_like(str, pattern, escape)) {
          worker_results[worker_idx].push_back(string_id);
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  for (const auto& worker_result : worker_results) {
    result.insert(result.end(), worker_result.begin(), worker_result.end());
  }
  const auto it_ok = regex_cache_.insert(std::make_pair(cache_key, result));
  CHECK(it_ok.second);

  return result;
}

std::shared_ptr<const std::vector<std::string>> StringDictionary::copyStrings() const {
  mapd_lock_guard<mapd_shared_mutex> write_lock(rw_mutex_);
  if (client_) {
    // TODO(miyu): support remote string dictionary
    throw std::runtime_error(
        "copying dictionaries from remote server is not supported yet.");
  }

  if (strings_cache_) {
    return strings_cache_;
  }

  strings_cache_ = std::make_shared<std::vector<std::string>>();
  strings_cache_->reserve(str_count_);
  const bool multithreaded = str_count_ > 10000;
  const auto worker_count =
      multithreaded ? static_cast<size_t>(cpu_threads()) : size_t(1);
  CHECK_GT(worker_count, 0UL);
  std::vector<std::vector<std::string>> worker_results(worker_count);
  auto copy = [this](std::vector<std::string>& str_list,
                     const size_t start_id,
                     const size_t end_id) {
    CHECK_LE(start_id, end_id);
    str_list.reserve(end_id - start_id);
    for (size_t string_id = start_id; string_id < end_id; ++string_id) {
      str_list.push_back(getStringUnlocked(string_id));
    }
  };
  if (multithreaded) {
    std::vector<std::future<void>> workers;
    const auto stride = (str_count_ + (worker_count - 1)) / worker_count;
    for (size_t worker_idx = 0, start = 0, end = std::min(start + stride, str_count_);
         worker_idx < worker_count && start < str_count_;
         ++worker_idx, start += stride, end = std::min(start + stride, str_count_)) {
      workers.push_back(std::async(
          std::launch::async, copy, std::ref(worker_results[worker_idx]), start, end));
    }
    for (auto& worker : workers) {
      worker.get();
    }
  } else {
    CHECK_EQ(worker_results.size(), size_t(1));
    copy(worker_results[0], 0, str_count_);
  }

  for (const auto& worker_result : worker_results) {
    strings_cache_->insert(
        strings_cache_->end(), worker_result.begin(), worker_result.end());
  }
  return strings_cache_;
}

bool StringDictionary::fillRateIsHigh() const noexcept {
  return str_ids_.size() < str_count_ * 2;
}

void StringDictionary::increaseCapacity() noexcept {
  std::vector<int32_t> new_str_ids(str_ids_.size() * 2, INVALID_STR_ID);

  if (materialize_hashes_) {
    for (size_t i = 0; i < str_ids_.size(); ++i) {
      if (str_ids_[i] != INVALID_STR_ID) {
        const uint32_t hash = rk_hashes_[str_ids_[i]];
        uint32_t bucket = computeUniqueBucketWithHash(hash, new_str_ids);
        new_str_ids[bucket] = str_ids_[i];
      }
    }
    rk_hashes_.resize(rk_hashes_.size() * 2);
  } else {
    for (size_t i = 0; i < str_count_; ++i) {
      const auto str = getStringChecked(i);
      const uint32_t hash = rk_hash(str);
      uint32_t bucket = computeUniqueBucketWithHash(hash, new_str_ids);
      new_str_ids[bucket] = i;
    }
  }
  str_ids_.swap(new_str_ids);
}

int32_t StringDictionary::getOrAddImpl(const std::string& str) noexcept {
  // @TODO(wei) treat empty string as NULL for now
  if (str.size() == 0) {
    return inline_int_null_value<int32_t>();
  }
  CHECK(str.size() <= MAX_STRLEN);
  uint32_t bucket;
  const uint32_t hash = rk_hash(str);
  {
    mapd_shared_lock<mapd_shared_mutex> read_lock(rw_mutex_);
    bucket = computeBucket(hash, str, str_ids_, false);
    if (str_ids_[bucket] != INVALID_STR_ID) {
      return str_ids_[bucket];
    }
  }
  mapd_lock_guard<mapd_shared_mutex> write_lock(rw_mutex_);
  // need to recalculate the bucket in case it changed before
  // we got the lock
  bucket = computeBucket(hash, str, str_ids_, false);
  if (str_ids_[bucket] == INVALID_STR_ID) {
    CHECK_LT(str_count_, MAX_STRCOUNT)
        << "Maximum number (" << str_count_
        << ") of Dictionary encoded Strings reached for this column, offset path "
           "for column is  "
        << offsets_path_;
    if (fillRateIsHigh()) {
      // resize when more than 50% is full
      increaseCapacity();
      bucket = computeBucket(hash, str, str_ids_, false);
    }
    appendToStorage(str);
    str_ids_[bucket] = static_cast<int32_t>(str_count_);
    if (materialize_hashes_) {
      rk_hashes_[str_count_] = hash;
    }
    ++str_count_;
    invalidateInvertedIndex();
  }
  return str_ids_[bucket];
}

std::string StringDictionary::getStringChecked(const int string_id) const noexcept {
  const auto str_canary = getStringFromStorage(string_id);
  CHECK(!str_canary.canary);
  return std::string(str_canary.c_str_ptr, str_canary.size);
}

std::pair<char*, size_t> StringDictionary::getStringBytesChecked(
    const int string_id) const noexcept {
  const auto str_canary = getStringFromStorage(string_id);
  CHECK(!str_canary.canary);
  return std::make_pair(str_canary.c_str_ptr, str_canary.size);
}

uint32_t StringDictionary::computeBucket(const uint32_t hash,
                                         const std::string str,
                                         const std::vector<int32_t>& data,
                                         const bool unique) const noexcept {
  auto bucket = hash & (data.size() - 1);
  while (true) {
    if (data[bucket] ==
        INVALID_STR_ID) {  // In this case it means the slot is available for use
      break;
    }
    // if records are unique I don't need to do this test as I know it will not be the
    // same
    if (!unique) {
      if (materialize_hashes_) {
        if (hash == rk_hashes_[data[bucket]]) {
          // can't be the same string if hash is different
          const auto old_str = getStringFromStorage(data[bucket]);
          if (str.size() == old_str.size &&
              !memcmp(str.c_str(), old_str.c_str_ptr, str.size())) {
            // found the string
            break;
          }
        }
      } else {
        const auto old_str = getStringFromStorage(data[bucket]);
        if (str.size() == old_str.size &&
            !memcmp(str.c_str(), old_str.c_str_ptr, str.size())) {
          // found the string
          break;
        }
      }
    }
    // wrap around
    if (++bucket == data.size()) {
      bucket = 0;
    }
  }
  return bucket;
}

uint32_t StringDictionary::computeUniqueBucketWithHash(
    const uint32_t hash,
    const std::vector<int32_t>& data) const noexcept {
  auto bucket = hash & (data.size() - 1);
  while (true) {
    if (data[bucket] ==
        INVALID_STR_ID) {  // In this case it means the slot is available for use
      break;
    }
    // wrap around
    if (++bucket == data.size()) {
      bucket = 0;
    }
  }
  return bucket;
}

void StringDictionary::appendToStorage(const std::string& str) noexcept {
  if (!isTemp_) {
    CHECK_GE(payload_fd_, 0);
    CHECK_GE(offset_fd_, 0);
  }
  // write the payload
  if (payload_file_off_ + str.size() > payload_file_size_) {
    if (!isTemp_) {
      checked_munmap(payload_map_, payload_file_size_);
      addPayloadCapacity();
      CHECK(payload_file_off_ + str.size() <= payload_file_size_);
      payload_map_ =
          reinterpret_cast<char*>(checked_mmap(payload_fd_, payload_file_size_));
    } else {
      addPayloadCapacity();
    }
  }
  memcpy(payload_map_ + payload_file_off_, str.c_str(), str.size());
  // write the offset and length
  size_t offset_file_off = str_count_ * sizeof(StringIdxEntry);
  StringIdxEntry str_meta{static_cast<uint64_t>(payload_file_off_), str.size()};
  payload_file_off_ += str.size();
  if (offset_file_off + sizeof(str_meta) >= offset_file_size_) {
    if (!isTemp_) {
      checked_munmap(offset_map_, offset_file_size_);
      addOffsetCapacity();
      CHECK(offset_file_off + sizeof(str_meta) <= offset_file_size_);
      offset_map_ =
          reinterpret_cast<StringIdxEntry*>(checked_mmap(offset_fd_, offset_file_size_));
    } else {
      addOffsetCapacity();
    }
  }
  memcpy(offset_map_ + str_count_, &str_meta, sizeof(str_meta));
}

StringDictionary::PayloadString StringDictionary::getStringFromStorage(
    const int string_id) const noexcept {
  if (!isTemp_) {
    CHECK_GE(payload_fd_, 0);
    CHECK_GE(offset_fd_, 0);
  }
  CHECK_GE(string_id, 0);
  const StringIdxEntry* str_meta = offset_map_ + string_id;
  if (str_meta->size == 0xffff) {
    // hit the canary
    return {nullptr, 0, true};
  }
  return {payload_map_ + str_meta->off, str_meta->size, false};
}

void StringDictionary::addPayloadCapacity() noexcept {
  if (!isTemp_) {
    payload_file_size_ += addStorageCapacity(payload_fd_);
  } else {
    payload_map_ =
        static_cast<char*>(addMemoryCapacity(payload_map_, payload_file_size_));
  }
}

void StringDictionary::addOffsetCapacity() noexcept {
  if (!isTemp_) {
    offset_file_size_ += addStorageCapacity(offset_fd_);
  } else {
    offset_map_ =
        static_cast<StringIdxEntry*>(addMemoryCapacity(offset_map_, offset_file_size_));
  }
}

size_t StringDictionary::addStorageCapacity(int fd) noexcept {
  static const ssize_t CANARY_BUFF_SIZE = 1024 * SYSTEM_PAGE_SIZE;
  if (!CANARY_BUFFER) {
    CANARY_BUFFER = static_cast<char*>(malloc(CANARY_BUFF_SIZE));
    CHECK(CANARY_BUFFER);
    memset(CANARY_BUFFER, 0xff, CANARY_BUFF_SIZE);
  }
  CHECK_NE(lseek(fd, 0, SEEK_END), -1);
  CHECK(write(fd, CANARY_BUFFER, CANARY_BUFF_SIZE) == CANARY_BUFF_SIZE);
  return CANARY_BUFF_SIZE;
}

void* StringDictionary::addMemoryCapacity(void* addr, size_t& mem_size) noexcept {
  static const ssize_t CANARY_BUFF_SIZE = 1024 * SYSTEM_PAGE_SIZE;
  if (!CANARY_BUFFER) {
    CANARY_BUFFER = reinterpret_cast<char*>(malloc(CANARY_BUFF_SIZE));
    CHECK(CANARY_BUFFER);
    memset(CANARY_BUFFER, 0xff, CANARY_BUFF_SIZE);
  }
  void* new_addr = realloc(addr, mem_size + CANARY_BUFF_SIZE);
  CHECK(new_addr);
  void* write_addr = reinterpret_cast<void*>(static_cast<char*>(new_addr) + mem_size);
  CHECK(memcpy(write_addr, CANARY_BUFFER, CANARY_BUFF_SIZE));
  mem_size += CANARY_BUFF_SIZE;
  return new_addr;
}

void StringDictionary::invalidateInvertedIndex() noexcept {
  if (!like_cache_.empty()) {
    decltype(like_cache_)().swap(like_cache_);
  }
  if (!regex_cache_.empty()) {
    decltype(regex_cache_)().swap(regex_cache_);
  }
  if (!equal_cache_.empty()) {
    decltype(equal_cache_)().swap(equal_cache_);
  }
  compare_cache_.invalidateInvertedIndex();
}

char* StringDictionary::CANARY_BUFFER{nullptr};

bool StringDictionary::checkpoint() noexcept {
  if (client_) {
    try {
      return client_->checkpoint();
    } catch (...) {
      return false;
    }
  }
  CHECK(!isTemp_);
  bool ret = true;
  ret = ret && (msync((void*)offset_map_, offset_file_size_, MS_SYNC) == 0);
  ret = ret && (msync((void*)payload_map_, payload_file_size_, MS_SYNC) == 0);
  ret = ret && (fsync(offset_fd_) == 0);
  ret = ret && (fsync(payload_fd_) == 0);
  return ret;
}

void StringDictionary::buildSortedCache() {
  // This method is not thread-safe.
  const auto cur_cache_size = sorted_cache.size();
  std::vector<int32_t> temp_sorted_cache;
  for (size_t i = cur_cache_size; i < str_count_; i++) {
    temp_sorted_cache.push_back(i);
  }
  sortCache(temp_sorted_cache);
  mergeSortedCache(temp_sorted_cache);
}

void StringDictionary::sortCache(std::vector<int32_t>& cache) {
  // This method is not thread-safe.

  // this boost sort is creating some problems when we use UTF-8 encoded strings.
  // TODO (vraj): investigate What is wrong with boost sort and try to mitigate it.

  std::sort(cache.begin(), cache.end(), [this](int32_t a, int32_t b) {
    auto a_str = this->getStringFromStorage(a);
    auto b_str = this->getStringFromStorage(b);
    return string_lt(a_str.c_str_ptr, a_str.size, b_str.c_str_ptr, b_str.size);
  });
}

void StringDictionary::mergeSortedCache(std::vector<int32_t>& temp_sorted_cache) {
  // this method is not thread safe
  std::vector<int32_t> updated_cache(temp_sorted_cache.size() + sorted_cache.size());
  size_t t_idx = 0, s_idx = 0, idx = 0;
  for (; t_idx < temp_sorted_cache.size() && s_idx < sorted_cache.size(); idx++) {
    auto t_string = getStringFromStorage(temp_sorted_cache[t_idx]);
    auto s_string = getStringFromStorage(sorted_cache[s_idx]);
    const auto insert_from_temp_cache =
        string_lt(t_string.c_str_ptr, t_string.size, s_string.c_str_ptr, s_string.size);
    if (insert_from_temp_cache) {
      updated_cache[idx] = temp_sorted_cache[t_idx++];
    } else {
      updated_cache[idx] = sorted_cache[s_idx++];
    }
  }
  while (t_idx < temp_sorted_cache.size()) {
    updated_cache[idx++] = temp_sorted_cache[t_idx++];
  }
  while (s_idx < sorted_cache.size()) {
    updated_cache[idx++] = sorted_cache[s_idx++];
  }
  sorted_cache.swap(updated_cache);
}

void StringDictionary::populate_string_ids(std::vector<int32_t>& dest_ids,
                                           StringDictionary* dest_dict,
                                           const std::vector<int32_t>& source_ids,
                                           const StringDictionary* source_dict) {
  std::vector<std::string> strings;

  for (const int32_t source_id : source_ids) {
    if (source_id == std::numeric_limits<int32_t>::min()) {
      strings.emplace_back("");
    } else if (source_id < 0) {
      throw std::runtime_error("Unexpected negative source ID");
    } else {
      strings.push_back(source_dict->getString(source_id));
    }
  }

  dest_ids.resize(strings.size());
  dest_dict->getOrAddBulk(strings, &dest_ids[0]);
}

void StringDictionary::populate_string_array_ids(
    std::vector<std::vector<int32_t>>& dest_array_ids,
    StringDictionary* dest_dict,
    const std::vector<std::vector<int32_t>>& source_array_ids,
    const StringDictionary* source_dict) {
  dest_array_ids.resize(source_array_ids.size());

  std::atomic<size_t> row_idx{0};
  auto processor = [&row_idx, &dest_array_ids, dest_dict, &source_array_ids, source_dict](
                       int thread_id) {
    for (;;) {
      auto row = row_idx.fetch_add(1);

      if (row >= dest_array_ids.size()) {
        return;
      }
      const auto& source_ids = source_array_ids[row];
      auto& dest_ids = dest_array_ids[row];
      populate_string_ids(dest_ids, dest_dict, source_ids, source_dict);
    }
  };

  const int num_worker_threads = std::thread::hardware_concurrency();

  if (source_array_ids.size() / num_worker_threads > 10) {
    std::vector<std::future<void>> worker_threads;
    for (int i = 0; i < num_worker_threads; ++i) {
      worker_threads.push_back(std::async(std::launch::async, processor, i));
    }

    for (auto& child : worker_threads) {
      child.wait();
    }
    for (auto& child : worker_threads) {
      child.get();
    }
  } else {
    processor(0);
  }
}

void translate_string_ids(std::vector<int32_t>& dest_ids,
                          const LeafHostInfo& dict_server_host,
                          const DictRef dest_dict_ref,
                          const std::vector<int32_t>& source_ids,
                          const DictRef source_dict_ref,
                          const int32_t dest_generation) {
  DictRef temp_dict_ref(-1, -1);
  StringDictionaryClient string_client(dict_server_host, temp_dict_ref, false);
  string_client.translate_string_ids(
      dest_ids, dest_dict_ref, source_ids, source_dict_ref, dest_generation);
}
