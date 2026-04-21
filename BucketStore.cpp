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
#include <LittleFS.h>
#include <Arduino.h>
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
    // LittleFS.begin() is a no-op if already mounted (FakeGatoHistoryService
    // mounts it with format-on-fail=true during its constructor).  Calling it
    // again is safe and ensures we have a mount before any file access.
    if (!LittleFS.begin(true)) {
        Serial.println("BucketStore: LittleFS mount failed");
        return false;
    }

    // Create the /navien directory if it doesn't exist.
    if (!LittleFS.exists("/navien")) {
        if (!LittleFS.mkdir("/navien")) {
            Serial.println("BucketStore: failed to create /navien directory");
            return false;
        }
        Serial.println("BucketStore: created /navien directory");
    }

    // Try to load an existing file.
    if (load()) {
        Serial.printf("BucketStore: loaded buckets.bin (year=%u, non-zero=%d)\n",
                      _buckets.current_year, nonZeroCount());
        return true;
    }

    // File absent or corrupt — start fresh.
    time_t now = time(nullptr);
    struct tm *tm_info = gmtime(&now);
    uint16_t year = (tm_info && tm_info->tm_year > 100)
                    ? (uint16_t)(tm_info->tm_year + 1900)
                    : 2025;

    initEmpty(year);
    if (!writeAtomic()) {
        Serial.println("BucketStore: failed to write initial buckets.bin");
        return false;
    }
    Serial.printf("BucketStore: created empty buckets.bin (year=%u)\n", year);
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
    if (!LittleFS.exists(BUCKET_FILE)) {
        return false;
    }

    File f = LittleFS.open(BUCKET_FILE, "r");
    if (!f) {
        Serial.println("BucketStore: failed to open buckets.bin for reading");
        return false;
    }

    size_t expected = sizeof(BucketFile);
    size_t got = f.read(reinterpret_cast<uint8_t *>(&_buckets), expected);
    f.close();

    if (got != expected) {
        Serial.printf("BucketStore: size mismatch (got %u, expected %u)\n",
                      (unsigned)got, (unsigned)expected);
        return false;
    }

    if (_buckets.magic != BUCKET_MAGIC) {
        Serial.printf("BucketStore: bad magic 0x%08X\n", _buckets.magic);
        return false;
    }

    if (_buckets.schema_version != BUCKET_SCHEMA_VERSION) {
        Serial.printf("BucketStore: schema version mismatch (%u != %u)\n",
                      _buckets.schema_version, BUCKET_SCHEMA_VERSION);
        return false;
    }

    return true;
}

bool BucketStore::writeAtomic() {
    // Write to .tmp first.
    File f = LittleFS.open(BUCKET_TMP_FILE, "w");
    if (!f) {
        Serial.println("BucketStore: failed to open buckets.tmp for writing");
        return false;
    }

    size_t written = f.write(reinterpret_cast<const uint8_t *>(&_buckets),
                             sizeof(BucketFile));
    f.close();

    if (written != sizeof(BucketFile)) {
        Serial.printf("BucketStore: short write to buckets.tmp (%u/%u bytes)\n",
                      (unsigned)written, (unsigned)sizeof(BucketFile));
        LittleFS.remove(BUCKET_TMP_FILE);
        return false;
    }

    // Atomically replace the live file.
    if (!LittleFS.rename(BUCKET_TMP_FILE, BUCKET_FILE)) {
        Serial.println("BucketStore: rename buckets.tmp -> buckets.bin failed");
        LittleFS.remove(BUCKET_TMP_FILE);
        return false;
    }

    return true;
}

void BucketStore::initEmpty(uint16_t current_year) {
    memset(&_buckets, 0, sizeof(_buckets));
    _buckets.magic          = BUCKET_MAGIC;
    _buckets.schema_version = BUCKET_SCHEMA_VERSION;
    _buckets.current_year   = current_year;
}
