/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

#include "velox/common/caching/AsyncDataCache.h"
#include "velox/common/caching/FileIds.h"
#include "velox/common/caching/SsdCache.h"

#include <folly/executors/QueuedImmediateExecutor.h>
#include "velox/common/base/SuccinctPrinter.h"
#include "velox/common/caching/FileIds.h"

namespace facebook::velox::cache {

using memory::MachinePageCount;
using memory::MemoryAllocator;

AsyncDataCacheEntry::AsyncDataCacheEntry(CacheShard* shard) : shard_(shard) {
  accessStats_.reset();
}

AsyncDataCacheEntry::~AsyncDataCacheEntry() {
  shard_->cache()->allocator()->freeNonContiguous(data_);
}

void AsyncDataCacheEntry::setExclusiveToShared() {
  VELOX_CHECK(isExclusive());
  numPins_ = 1;
  std::unique_ptr<folly::SharedPromise<bool>> promise;
  {
    std::lock_guard<std::mutex> l(shard_->mutex());
    // Enter the shard's mutex to make sure a promise is not being added during
    // the move.
    promise = std::move(promise_);
  }
  if (promise != nullptr) {
    promise->setValue(true);
  }

  // The entry may now have other readers, It is safe to do read-only ops like
  // integrity and notifying SSD cache of another candidate.
  const auto& hook = shard_->cache()->verifyHook();
  if (hook != nullptr) {
    hook(*this);
  }

  if ((ssdFile_ == nullptr) && (shard_->cache()->ssdCache() != nullptr)) {
    auto* ssdCache = shard_->cache()->ssdCache();
    assert(ssdCache); // for lint only.
    if (ssdCache->groupStats().shouldSaveToSsd(groupId_, trackingId_)) {
      ssdSaveable_ = true;
      shard_->cache()->possibleSsdSave(size_);
    }
  }
}

void AsyncDataCacheEntry::release() {
  VELOX_CHECK_NE(0, numPins_);
  if (numPins_ == kExclusive) {
    // Dereferencing an exclusive entry without converting to shared means that
    // the content could not be shared, e.g. error in loading.
    auto promise = shard_->removeEntry(this);
    // Realize the promise outside of the shard mutex.
    if (promise != nullptr) {
      promise->setValue(true);
    }
    numPins_ = 0;
  } else {
    const auto oldPins = numPins_.fetch_add(-1);
    VELOX_CHECK_LE(1, oldPins, "pin count goes negative");
  }
}

void AsyncDataCacheEntry::addReference() {
  VELOX_CHECK(!isExclusive());
  ++numPins_;
}

memory::MachinePageCount AsyncDataCacheEntry::setPrefetch(bool flag) {
  isPrefetch_ = flag;
  const auto numPages = memory::AllocationTraits::numPages(size_);
  return shard_->cache()->incrementPrefetchPages(flag ? numPages : -numPages);
}

void AsyncDataCacheEntry::initialize(FileCacheKey key) {
  VELOX_CHECK(isExclusive());
  setSsdFile(nullptr, 0);
  key_ = std::move(key);
  auto* cache = shard_->cache();
  ClockTimer t(shard_->allocClocks());
  if (size_ < AsyncDataCacheEntry::kTinyDataSize) {
    tinyData_.resize(size_);
    tinyData_.shrink_to_fit();
  } else {
    tinyData_.clear();
    tinyData_.shrink_to_fit();
    const auto sizePages = memory::AllocationTraits::numPages(size_);
    if (cache->allocator()->allocateNonContiguous(sizePages, data_)) {
      cache->incrementCachedPages(data().numPages());
    } else {
      // No memory to cover 'this'.
      release();
      _VELOX_THROW(
          VeloxRuntimeError,
          error_source::kErrorSourceRuntime.c_str(),
          error_code::kNoCacheSpace.c_str(),
          /* isRetriable */ true,
          "Failed to allocate {} bytes for cache",
          size_);
    }
  }
}

void AsyncDataCacheEntry::makeEvictable() {
  accessStats_.lastUse = 0;
  accessStats_.numUses = 0;
}

std::string AsyncDataCacheEntry::toString() const {
  return fmt::format(
      "<entry key:{}:{} size {} pins {}>",
      key_.fileNum.id(),
      key_.offset,
      size_,
      numPins_);
}

std::unique_ptr<AsyncDataCacheEntry> CacheShard::getFreeEntry() {
  std::unique_ptr<AsyncDataCacheEntry> newEntry;
  if (freeEntries_.empty()) {
    newEntry = std::make_unique<AsyncDataCacheEntry>(this);
  } else {
    newEntry = std::move(freeEntries_.back());
    freeEntries_.pop_back();
  }
  return newEntry;
}

CachePin CacheShard::findOrCreate(
    RawFileCacheKey key,
    uint64_t size,
    folly::SemiFuture<bool>* wait) {
  AsyncDataCacheEntry* entryToInit = nullptr;
  {
    std::lock_guard<std::mutex> l(mutex_);
    ++eventCounter_;
    auto it = entryMap_.find(key);
    if (it != entryMap_.end()) {
      auto* found = it->second;
      if (found->isExclusive()) {
        ++numWaitExclusive_;
        if (wait == nullptr) {
          return CachePin();
        }
        *wait = found->getFuture();
        return CachePin();
      }
      if (found->size() >= size) {
        found->touch();
        // The entry is in a readable state. Add a pin.
        if (found->isPrefetch()) {
          found->isFirstUse_ = true;
          found->setPrefetch(false);
        } else {
          ++numHit_;
          hitBytes_ += found->size();
        }
        ++found->numPins_;
        CachePin pin;
        pin.setEntry(found);
        return pin;
      }

      // TODO: add stats to report or send alert in production.

      // This can happen if different load quanta apply to access via different
      // connectors. This is not an error but still worth logging.
      VELOX_CACHE_LOG_EVERY_MS(WARNING, 1'000)
          << "Requested larger entry. Found size " << found->size()
          << " requested size " << size;
      // The old entry is superseded. Possible readers of the old entry still
      // retain a valid read pin.
      found->key_.fileNum.clear();
    }

    auto newEntry = getFreeEntry();
    // Initialize the members that must be set inside 'mutex_'.
    newEntry->numPins_ = AsyncDataCacheEntry::kExclusive;
    newEntry->promise_ = nullptr;
    entryToInit = newEntry.get();
    entryMap_[key] = newEntry.get();
    if (emptySlots_.empty()) {
      entries_.push_back(std::move(newEntry));
    } else {
      const auto index = emptySlots_.back();
      emptySlots_.pop_back();
      entries_[index] = std::move(newEntry);
    }
    ++numNew_;
    // Inside the shard mutex.
    VELOX_CHECK_EQ(entryToInit->size_, 0);
    entryToInit->size_ = size;
    entryToInit->isFirstUse_ = true;
  }
  return initEntry(key, entryToInit);
}

bool CacheShard::exists(RawFileCacheKey key) const {
  std::lock_guard<std::mutex> l(mutex_);
  auto it = entryMap_.find(key);
  if (it != entryMap_.end()) {
    it->second->touch();
    return true;
  }
  return false;
}

CachePin CacheShard::initEntry(
    RawFileCacheKey key,
    AsyncDataCacheEntry* entry) {
  // The new entry is in the map and is in exclusive mode and is otherwise
  // uninitialized. Other threads may find it and may add a promise or wait for
  // a promise that another one has added. The new entry is otherwise volatile
  // and uninterpretable except for this thread. Non access serializing members
  // can be set outside of 'mutex_'.
  entry->initialize(
      FileCacheKey{StringIdLease(fileIds(), key.fileNum), key.offset});
  cache_->incrementNew(entry->size());
  CachePin pin;
  pin.setEntry(entry);
  return pin;
}

CoalescedLoad::~CoalescedLoad() {
  // Continue possibly waiting threads.
  setEndState(State::kCancelled);
}

bool CoalescedLoad::loadOrFuture(folly::SemiFuture<bool>* wait) {
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (state_ == State::kCancelled || state_ == State::kLoaded) {
      return true;
    }
    if (state_ == State::kLoading) {
      if (wait == nullptr) {
        return false;
      }
      if (promise_ == nullptr) {
        promise_ = std::make_unique<folly::SharedPromise<bool>>();
      }
      *wait = promise_->getSemiFuture();
      return false;
    }

    VELOX_CHECK_EQ(State::kPlanned, state_);
    state_ = State::kLoading;
  }
  // Outside of 'mutex_'.
  try {
    const auto pins = loadData(!wait);
    for (const auto& pin : pins) {
      auto* entry = pin.checkedEntry();
      VELOX_CHECK(entry->key().fileNum.hasValue());
      VELOX_CHECK(entry->isExclusive());
      entry->setExclusiveToShared();
    }
    setEndState(State::kLoaded);
  } catch (std::exception& e) {
    try {
      setEndState(State::kCancelled);
    } catch (std::exception& inner) {
      // May not throw from inside catch.
    }
    throw;
  }
  return true;
}

void CoalescedLoad::setEndState(State endState) {
  std::lock_guard<std::mutex> l(mutex_);
  state_ = endState;
  if (promise_ != nullptr) {
    promise_->setValue(true);
    promise_.reset();
  }
}

std::unique_ptr<folly::SharedPromise<bool>> CacheShard::removeEntry(
    AsyncDataCacheEntry* entry) {
  std::lock_guard<std::mutex> l(mutex_);
  removeEntryLocked(entry);
  // After the entry is removed from the hash table, a promise can no longer
  // be made. It is safe to move the promise and realize it.
  return entry->movePromise();
}

void CacheShard::removeEntryLocked(AsyncDataCacheEntry* entry) {
  if (!entry->key_.fileNum.hasValue()) {
    return;
  }
  const auto it = entryMap_.find(
      RawFileCacheKey{entry->key_.fileNum.id(), entry->key_.offset});
  VELOX_CHECK(it != entryMap_.end());
  entryMap_.erase(it);
  entry->key_.fileNum.clear();
  entry->setSsdFile(nullptr, 0);
  if (entry->isPrefetch()) {
    entry->setPrefetch(false);
  }
  // An entry can have data allocated if we remove it after failing
  // to fill it. Free the data and account for the difference. In
  // eviction, the data of the evicted entries is moved away, so
  // that freeing while holding the shard mutex is exceptional.
  const auto numPages = entry->data().numPages();
  if (numPages > 0) {
    cache_->incrementCachedPages(-numPages);
    cache_->allocator()->freeNonContiguous(entry->data());
  }
}

void CacheShard::evict(
    uint64_t bytesToFree,
    bool evictAllUnpinned,
    int32_t pagesToAcquire,
    memory::Allocation& acquired) {
  int64_t tinyFreed = 0;
  int64_t largeFreed = 0;
  int32_t evictSaveableSkipped = 0;
  auto ssdCache = cache_->ssdCache();
  bool skipSsdSaveable = ssdCache && ssdCache->writeInProgress();
  auto now = accessTime();
  std::vector<memory::Allocation> toFree;
  {
    std::lock_guard<std::mutex> l(mutex_);
    int size = entries_.size();
    if (!size) {
      return;
    }
    int32_t counter = 0;
    int32_t numChecked = 0;
    auto entryIndex = (clockHand_ % size);
    auto iter = entries_.begin() + entryIndex;
    while (++counter <= size) {
      if (++iter == entries_.end()) {
        iter = entries_.begin();
        entryIndex = 0;
      } else {
        ++entryIndex;
      }
      ++numEvictChecks_;
      auto candidate = iter->get();
      if (!candidate) {
        continue;
      }
      ++numChecked;
      ++clockHand_;
      if (evictionThreshold_ == kNoThreshold ||
          eventCounter_ > entries_.size() / 4 ||
          numChecked > entries_.size() / 8) {
        now = accessTime();
        calibrateThreshold();
        numChecked = 0;
        eventCounter_ = 0;
      }
      int32_t score = 0;
      if (candidate->numPins_ == 0 &&
          (!candidate->key_.fileNum.hasValue() || evictAllUnpinned ||
           (score = candidate->score(now)) >= evictionThreshold_)) {
        if (skipSsdSaveable && candidate->ssdSaveable_ && !evictAllUnpinned) {
          ++evictSaveableSkipped;
          continue;
        }
        largeFreed += candidate->data_.byteSize();
        if (pagesToAcquire > 0) {
          auto candidatePages = candidate->data().numPages();
          pagesToAcquire = candidatePages > pagesToAcquire
              ? 0
              : pagesToAcquire - candidatePages;
          acquired.appendMove(candidate->data());
          VELOX_CHECK(candidate->data().empty());
        } else {
          toFree.push_back(std::move(candidate->data()));
        }
        removeEntryLocked(candidate);
        emptySlots_.push_back(entryIndex);
        tinyFreed += candidate->tinyData_.size();
        candidate->tinyData_.clear();
        candidate->tinyData_.shrink_to_fit();
        candidate->size_ = 0;
        tryAddFreeEntry(std::move(*iter));
        ++numEvict_;
        if (score) {
          sumEvictScore_ += score;
        }
        if (largeFreed + tinyFreed > bytesToFree) {
          break;
        }
      }
    }
  }
  ClockTimer t(allocClocks_);
  freeAllocations(toFree);
  cache_->incrementCachedPages(
      -largeFreed / static_cast<int32_t>(memory::AllocationTraits::kPageSize));
  if (evictSaveableSkipped && ssdCache && ssdCache->startWrite()) {
    // Rare. May occur if SSD is unusually slow. Useful for  diagnostics.
    VELOX_SSD_CACHE_LOG(INFO)
        << "Start save for old saveable, skipped " << cache_->numSkippedSaves();
    cache_->numSkippedSaves() = 0;
    cache_->saveToSsd();
  } else if (evictSaveableSkipped) {
    ++cache_->numSkippedSaves();
  }
}

void CacheShard::tryAddFreeEntry(std::unique_ptr<AsyncDataCacheEntry>&& entry) {
  freeEntries_.push_back(std::move(entry));
  // If we have too many free entries, we free up half of them to save space.
  if (freeEntries_.size() >= kMaxFreeEntries) {
    freeEntries_.resize(kMaxFreeEntries >> 1);
  }
}

void CacheShard::freeAllocations(std::vector<memory::Allocation>& allocations) {
  for (auto& allocation : allocations) {
    cache_->allocator()->freeNonContiguous(allocation);
  }
  allocations.clear();
}

void CacheShard::calibrateThreshold() {
  auto numSamples = std::min<int32_t>(10, entries_.size());
  auto now = accessTime();
  auto entryIndex = (clockHand_ % entries_.size());
  auto step = entries_.size() / numSamples;
  auto iter = entries_.begin() + entryIndex;
  evictionThreshold_ = percentile<int32_t>(
      [&]() -> int32_t {
        AsyncDataCacheEntry* element = iter->get();
        int32_t score = element ? element->score(now) : 0;
        if (entryIndex + step >= entries_.size()) {
          entryIndex = (entryIndex + step) % entries_.size();
          iter = entries_.begin() + entryIndex;
        } else {
          entryIndex += step;
          iter += step;
        }
        return score;
      },
      numSamples,
      80);
}

void CacheShard::updateStats(CacheStats& stats) {
  std::lock_guard<std::mutex> l(mutex_);
  for (auto& entry : entries_) {
    if (!entry || !entry->key_.fileNum.hasValue()) {
      ++stats.numEmptyEntries;
      continue;
    } else if (entry->isExclusive()) {
      stats.exclusivePinnedBytes +=
          entry->data().byteSize() + entry->tinyData_.capacity();
      ++stats.numExclusive;
    } else if (entry->isShared()) {
      stats.sharedPinnedBytes +=
          entry->data().byteSize() + entry->tinyData_.capacity();
      ++stats.numShared;
    }
    if (entry->isPrefetch_) {
      ++stats.numPrefetch;
      stats.prefetchBytes += entry->size();
    }
    ++stats.numEntries;
    stats.tinySize += entry->tinyData_.size();
    stats.tinyPadding += entry->tinyData_.capacity() - entry->tinyData_.size();
    stats.largeSize += entry->size_;
    stats.largePadding += entry->data_.byteSize() - entry->size_;
  }
  stats.numHit += numHit_;
  stats.hitBytes += hitBytes_;
  stats.numNew += numNew_;
  stats.numEvict += numEvict_;
  stats.numEvictChecks += numEvictChecks_;
  stats.numWaitExclusive += numWaitExclusive_;
  stats.sumEvictScore += sumEvictScore_;
  stats.allocClocks += allocClocks_;
}

void CacheShard::appendSsdSaveable(std::vector<CachePin>& pins) {
  std::lock_guard<std::mutex> l(mutex_);
  // Do not add more than 70% of entries to a write batch.If SSD save
  // is slower than storage read, we must not have a situation where
  // SSD save pins everything and stops reading.
  const int32_t limit = (entries_.size() * 100) / 70;
  VELOX_CHECK(cache_->ssdCache()->writeInProgress());
  for (auto& entry : entries_) {
    if (entry && !entry->ssdFile_ && !entry->isExclusive() &&
        entry->ssdSaveable_) {
      CachePin pin;
      ++entry->numPins_;
      pin.setEntry(entry.get());
      pins.push_back(std::move(pin));
      if (pins.size() >= limit) {
        VELOX_SSD_CACHE_LOG(INFO)
            << "Limiting SSD save batch to " << limit << " entries";
        break;
      }
    }
  }
}

AsyncDataCache::AsyncDataCache(
    memory::MemoryAllocator* allocator,
    std::unique_ptr<SsdCache> ssdCache)
    : allocator_(allocator), ssdCache_(std::move(ssdCache)), cachedPages_(0) {
  for (auto i = 0; i < kNumShards; ++i) {
    shards_.push_back(std::make_unique<CacheShard>(this));
  }
}

AsyncDataCache::~AsyncDataCache() {}

// static
std::shared_ptr<AsyncDataCache> AsyncDataCache::create(
    memory::MemoryAllocator* allocator,
    std::unique_ptr<SsdCache> ssdCache) {
  auto cache = std::make_shared<AsyncDataCache>(allocator, std::move(ssdCache));
  allocator->registerCache(cache);
  return cache;
}

// static
AsyncDataCache* AsyncDataCache::getInstance() {
  return *getInstancePtr();
}

// static
void AsyncDataCache::setInstance(AsyncDataCache* asyncDataCache) {
  *getInstancePtr() = asyncDataCache;
}

// static
AsyncDataCache** AsyncDataCache::getInstancePtr() {
  static AsyncDataCache* cache_{nullptr};
  return &cache_;
}

void AsyncDataCache::shutdown() {
  for (auto& shard : shards_) {
    shard->shutdown();
  }
}

void CacheShard::shutdown() {
  entries_.clear();
  freeEntries_.clear();
}

CachePin AsyncDataCache::findOrCreate(
    RawFileCacheKey key,
    uint64_t size,
    folly::SemiFuture<bool>* wait) {
  const int shard = std::hash<RawFileCacheKey>()(key) & (kShardMask);
  return shards_[shard]->findOrCreate(key, size, wait);
}

bool AsyncDataCache::exists(RawFileCacheKey key) const {
  int shard = std::hash<RawFileCacheKey>()(key) & (kShardMask);
  return shards_[shard]->exists(key);
}

bool AsyncDataCache::makeSpace(
    MachinePageCount numPages,
    std::function<bool(memory::Allocation& allocation)> allocate) {
  // Try to allocate and if failed, evict the desired amount and
  // retry. This is without synchronization, so that other threads may
  // get what one thread evicted but this will usually work in a
  // couple of iterations. If this does not settle within 8 tries, we
  // start counting the contending threads and doing random backoff to
  // serialize the evicts and allocates. If a new thread enters when
  // thread counting and backoff are in effect, it gets a rank at the
  // end of the queue. The larger the rank, the larger the backoff, so
  // that first comer is likelier to get the memory. We cannot
  // serialize with a mutex because memory arbitration must not be
  // called from inside a global mutex.

  constexpr int32_t kMaxAttempts = kNumShards * 4;
  // Evict at least 1MB even for small allocations to avoid constantly hitting
  // the mutex protected evict loop.
  constexpr int32_t kMinEvictPages = 256;
  // If requesting less than kSmallSizePages try up to 4x more if
  // first try failed.
  constexpr int32_t kSmallSizePages = 2048; // 8MB
  float sizeMultiplier = 1.2;
  // True if this thread is counted in 'numThreadsInAllocate_'.
  bool isCounted = false;
  // If more than half the allowed retries are needed, this is the rank in
  // arrival order of this.
  int32_t rank = 0;
  // Allocation into which evicted pages are moved.
  memory::Allocation acquired;
  // 'acquired' is not managed by a pool. Make sure it is freed on throw.
  // Destruct without pool and non-empty kills the process.
  auto guard = folly::makeGuard([&]() {
    allocator_->freeNonContiguous(acquired);
    if (isCounted) {
      --numThreadsInAllocate_;
    }
  });
  VELOX_CHECK(
      numThreadsInAllocate_ >= 0 && numThreadsInAllocate_ < 10000,
      "Leak in numThreadsInAllocate_: {}",
      numThreadsInAllocate_);
  if (numThreadsInAllocate_) {
    rank = ++numThreadsInAllocate_;
    isCounted = true;
  }
  for (auto nthAttempt = 0; nthAttempt < kMaxAttempts; ++nthAttempt) {
    if (canTryAllocate(numPages, acquired)) {
      if (allocate(acquired)) {
        return true;
      }
    }

    if (nthAttempt > 2 && ssdCache_ && ssdCache_->writeInProgress()) {
      VELOX_SSD_CACHE_LOG(INFO)
          << "Pause 0.5s after failed eviction waiting for SSD cache write to unpin memory";
      std::this_thread::sleep_for(std::chrono::milliseconds(500)); // NOLINT
    }
    if (nthAttempt > kMaxAttempts / 2) {
      if (!isCounted) {
        rank = ++numThreadsInAllocate_;
        isCounted = true;
      }
    }
    if (rank) {
      // Free the grabbed allocation before sleep so the contender can make
      // progress. This is only on heavy contention, after 8 missed tries.
      allocator_->freeNonContiguous(acquired);
      backoff(nthAttempt + rank);
      // If some of the competing threads are done, maybe give this thread a
      // better rank.
      rank = std::min<int32_t>(rank, numThreadsInAllocate_);
    }
    ++shardCounter_;
    int32_t numPagesToAcquire =
        acquired.numPages() < numPages ? numPages - acquired.numPages() : 0;
    // Evict from next shard. If we have gone through all shards once
    // and still have not made the allocation, we go to desperate mode
    // with 'evictAllUnpinned' set to true.
    shards_[shardCounter_ & (kShardMask)]->evict(
        memory::AllocationTraits::pageBytes(
            std::max<int32_t>(kMinEvictPages, numPages) * sizeMultiplier),
        nthAttempt >= kNumShards,
        numPagesToAcquire,
        acquired);
    if (numPages < kSmallSizePages && sizeMultiplier < 4) {
      sizeMultiplier *= 2;
    }
  }
  memory::setCacheFailureMessage(fmt::format(
      "After failing to evict from cache state: {}", toString(false)));
  return false;
}

bool AsyncDataCache::canTryAllocate(
    int32_t numPages,
    const memory::Allocation& acquired) const {
  if (numPages <= acquired.numPages()) {
    return true;
  }
  return numPages - acquired.numPages() <=
      (memory::AllocationTraits::numPages(allocator_->capacity())) -
      allocator_->numAllocated();
}

void AsyncDataCache::backoff(int32_t counter) {
  size_t seed = folly::hasher<uint16_t>()(++backoffCounter_);
  const auto usec = (seed & 0xfff) * (counter & 0x1f);
  VELOX_CACHE_LOG_EVERY_MS(INFO, 1'000)
      << "Backoff in allocation contention for " << succinctMicros(usec);

  std::this_thread::sleep_for(std::chrono::microseconds(usec)); // NOLINT
}

void AsyncDataCache::incrementNew(uint64_t size) {
  newBytes_ += size;
  if (ssdCache_ == nullptr) {
    return;
  }
  if (newBytes_ > nextSsdScoreSize_) {
    // Check next time after replacing half the cache.
    nextSsdScoreSize_ = newBytes_ +
        std::max<int64_t>(memory::AllocationTraits::pageBytes(cachedPages_),
                          1UL << 28);
    ssdCache_->groupStats().updateSsdFilter(ssdCache_->maxBytes() * 0.9);
  }
}

void AsyncDataCache::possibleSsdSave(uint64_t bytes) {
  constexpr int32_t kMinSavePages = 4096; // Save at least 16MB at a time.
  if (ssdCache_ == nullptr) {
    return;
  }

  ssdSaveable_ += bytes;
  if (memory::AllocationTraits::numPages(ssdSaveable_) >
      std::max<int32_t>(kMinSavePages, cachedPages_ / 8)) {
    // Do not start a new save if another one is in progress.
    if (!ssdCache_->startWrite()) {
      return;
    }
    saveToSsd();
  }
}

void AsyncDataCache::saveToSsd() {
  std::vector<CachePin> pins;
  VELOX_CHECK(ssdCache_->writeInProgress());
  ssdSaveable_ = 0;
  for (auto& shard : shards_) {
    shard->appendSsdSaveable(pins);
  }
  ssdCache_->write(std::move(pins));
}

CacheStats AsyncDataCache::refreshStats() const {
  CacheStats stats;
  for (auto& shard : shards_) {
    shard->updateStats(stats);
  }
  if (ssdCache_ != nullptr) {
    stats.ssdStats = std::make_shared<SsdCacheStats>(ssdCache_->stats());
  }
  return stats;
}

void AsyncDataCache::clear() {
  for (auto& shard : shards_) {
    memory::Allocation acquired;
    shard->evict(std::numeric_limits<int32_t>::max(), true, 0, acquired);
    VELOX_CHECK(acquired.empty());
  }
}

std::string AsyncDataCache::toString(bool details) const {
  auto stats = refreshStats();
  std::stringstream out;
  out << "AsyncDataCache:\n"
      << stats.toString() << "\n"
      << "Allocated pages: " << allocator_->numAllocated()
      << " cached pages: " << cachedPages_ << "\n";
  if (details) {
    out << "Backing: " << allocator_->toString();
    if (ssdCache_) {
      out << "\nSSD: " << ssdCache_->toString();
    }
  }
  return out.str();
}

std::string CacheStats::toString() const {
  std::stringstream out;
  // Cache size stats.
  out << "Cache size: "
      << succinctBytes(tinySize + largeSize + tinyPadding + largePadding)
      << " tinySize: " << succinctBytes(tinySize + tinyPadding)
      << " large size: " << succinctBytes(largeSize + largePadding)
      << "\n"
      // Cache entries
      << "Cache entries: " << numEntries << " read pins: " << numShared
      << " write pins: " << numExclusive
      << " pinned shared: " << succinctBytes(sharedPinnedBytes)
      << " pinned exclusive: " << succinctBytes(exclusivePinnedBytes) << "\n"
      << " num write wait: " << numWaitExclusive
      << " empty entries: " << numEmptyEntries
      << "\n"
      // Cache access stats.
      << "Cache access miss: " << numNew << " hit: " << numHit
      << " hit bytes: " << succinctBytes(hitBytes) << " eviction: " << numEvict
      << " eviction checks: " << numEvictChecks
      << "\n"
      // Cache prefetch stats.
      << "Prefetch entries: " << numPrefetch
      << " bytes: " << succinctBytes(prefetchBytes)
      << "\n"
      // Cache timing stats.
      << "Alloc Megaclocks " << (allocClocks >> 20);
  return out.str();
}

CoalesceIoStats readPins(
    const std::vector<CachePin>& pins,
    int32_t maxGap,
    int32_t rangesPerIo,
    std::function<uint64_t(int32_t index)> offsetFunc,
    std::function<void(
        const std::vector<CachePin>& pins,
        int32_t begin,
        int32_t end,
        uint64_t offset,
        const std::vector<folly::Range<char*>>& buffers)> readFunc) {
  return coalesceIo<CachePin, folly::Range<char*>>(
      pins,
      maxGap,
      rangesPerIo,
      std::move(offsetFunc),
      [&](int32_t index) { return pins[index].checkedEntry()->size(); },
      [&](int32_t index) {
        return std::max<int32_t>(
            1, pins[index].checkedEntry()->data().numRuns());
      },
      [&](const CachePin& pin, std::vector<folly::Range<char*>>& ranges) {
        auto* entry = pin.checkedEntry();
        auto& data = entry->data();
        uint64_t offsetInRuns = 0;
        auto size = entry->size();
        if (data.numPages() == 0) {
          ranges.push_back(
              folly::Range<char*>(pin.checkedEntry()->tinyData(), size));
          offsetInRuns = size;
        } else {
          for (int i = 0; i < data.numRuns(); ++i) {
            const auto run = data.runAt(i);
            const uint64_t bytes = run.numBytes();
            const uint64_t readSize = std::min(bytes, size - offsetInRuns);
            ranges.push_back(folly::Range<char*>(run.data<char>(), readSize));
            offsetInRuns += readSize;
          }
        }
        VELOX_CHECK_EQ(offsetInRuns, size);
      },
      [&](int32_t size, std::vector<folly::Range<char*>>& ranges) {
        // This hack allows us to store the size of the gap in the Range,
        // without actually allocating a buffer for it.
        ranges.push_back(folly::Range<char*>(nullptr, (char*)(uint64_t)size));
      },
      std::move(readFunc));
}

} // namespace facebook::velox::cache
