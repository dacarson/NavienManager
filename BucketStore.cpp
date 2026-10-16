/*
Copyright (c) 2026 David Carson (dacarson)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "BucketStore.h"
#include "LearnerPlatform.h"
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

BucketStore::BucketStore() {
    memset(&_buckets, 0, sizeof(_buckets));
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool BucketStore::begin() {
    // Try to load an existing record.
    if (load()) {
        LEARNER_LOG("BucketStore: loaded buckets (year=%u, non-zero=%d)",
                    _buckets.current_year, nonZeroCount());
        return true;
    }

    // Record absent or corrupt — start fresh.
    time_t now = LEARNER_NOW();
    struct tm *tm_info = gmtime(&now);
    uint16_t year = (tm_info && tm_info->tm_year > 100)
                    ? (uint16_t)(tm_info->tm_year + 1900)
                    : 2025;

    initEmpty(year);
    if (!writeAtomic()) {
        LEARNER_LOG("BucketStore: failed to write initial bucket record");
        return false;
    }
    LEARNER_LOG("BucketStore: created empty bucket record (year=%u)", year);
    return true;
}

bool BucketStore::save() {
    return writeAtomic();
}

bool BucketStore::updateBucket(int dow, int bucket_index,
                                uint16_t raw_delta, float score_delta) {
    if (dow < 0 || dow >= BUCKET_DAYS ||
        bucket_index < 0 || bucket_index >= BUCKET_PER_DAY) {
        return false;
    }
    _buckets.buckets[dow][bucket_index].raw_count      += raw_delta;
    _buckets.buckets[dow][bucket_index].weighted_score += score_delta;
    return writeAtomic();
}

bool BucketStore::zeroBuckets(uint16_t current_year) {
    memset(_buckets.buckets, 0, sizeof(_buckets.buckets));
    _buckets.magic          = BUCKET_MAGIC;
    _buckets.schema_version = BUCKET_SCHEMA_VERSION;
    _buckets.current_year   = current_year;
    time_t now = LEARNER_NOW();
    _buckets.accumulation_start_epoch = (now > 0) ? (uint32_t)now : 0;
    return writeAtomic();
}

int BucketStore::nonZeroCount() const {
    int count = 0;
    for (int d = 0; d < BUCKET_DAYS; d++) {
        for (int b = 0; b < BUCKET_PER_DAY; b++) {
            if (_buckets.buckets[d][b].raw_count > 0) {
                count++;
            }
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool BucketStore::load() {
    if (!learnerStorage().load(BUCKET_STORAGE_KEY, &_buckets, sizeof(BucketFile))) {
        return false;
    }

    if (_buckets.magic != BUCKET_MAGIC) {
        LEARNER_LOG("BucketStore: bad magic 0x%08X", _buckets.magic);
        return false;
    }

    if (_buckets.schema_version != BUCKET_SCHEMA_VERSION) {
        LEARNER_LOG("BucketStore: schema version mismatch (%u != %u)",
                    _buckets.schema_version, BUCKET_SCHEMA_VERSION);
        return false;
    }

    return true;
}

bool BucketStore::writeAtomic() {
    return learnerStorage().save(BUCKET_STORAGE_KEY, &_buckets, sizeof(BucketFile));
}

void BucketStore::initEmpty(uint16_t current_year) {
    memset(&_buckets, 0, sizeof(_buckets));
    _buckets.magic          = BUCKET_MAGIC;
    _buckets.schema_version = BUCKET_SCHEMA_VERSION;
    _buckets.current_year   = current_year;
    time_t now = LEARNER_NOW();
    _buckets.accumulation_start_epoch = (now > 0) ? (uint32_t)now : 0;
}
