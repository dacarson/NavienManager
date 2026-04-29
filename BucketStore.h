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

#pragma once

#include <stdint.h>

// File paths on LittleFS
#define BUCKET_FILE     "/navien/buckets.bin"
#define BUCKET_TMP_FILE "/navien/buckets.tmp"

// Magic number: "NAVI" in little-endian ASCII bytes
#define BUCKET_MAGIC          0x4E415649u
#define BUCKET_SCHEMA_VERSION 2

// 7 days x 288 five-minute buckets (1440 min / 5)
#define BUCKET_DAYS    7
#define BUCKET_PER_DAY 288

// BucketFile is the on-disk and in-RAM representation.
// Total size: 8 + 7*288*6 = 12,104 bytes
// IMPORTANT: always declare as a class member (heap), never as a local variable (stack overflow).
struct BucketFile {
    uint32_t magic;           // 0x4E415649 ("NAVI") — detects corruption
    uint16_t schema_version;  // bump if struct layout changes
    uint16_t current_year;    // year buckets[] was last written for

    struct Bucket {
        uint16_t raw_count;       // unweighted cold-start hits
        float    weighted_score;  // sum of recency-weighted scores
    } buckets[BUCKET_DAYS][BUCKET_PER_DAY]; // [dow][bucket_index], dow: 0=Sun
};

// BucketStore manages the lifecycle of the BucketFile:
//   - loads from LittleFS on begin() (creates empty file if absent)
//   - writes back atomically via a .tmp + rename strategy
//   - provides helpers to update individual buckets and zero all data
//
// All public methods are safe to call from a single task (Core 0).
// The caller is responsible for any cross-core synchronisation.
class BucketStore {
public:
    BucketStore();

    // Initialise: mount check, create /navien directory, load or create
    // buckets.bin.  Returns true on success.
    bool begin();

    // Atomically write the in-RAM _buckets to LittleFS.
    // Returns true on success.
    bool save();

    // Increment a single bucket in RAM and persist to LittleFS.
    // dow: 0=Sunday .. 6=Saturday
    // bucket_index: 0-287
    // Returns true on success.
    bool updateBucket(int dow, int bucket_index,
                      uint16_t raw_delta, float score_delta);

    // Zero every bucket in the in-RAM struct and persist to LittleFS.
    // current_year is written into the header after zeroing.
    // Returns true on success.
    bool zeroBuckets(uint16_t current_year);

    // Direct access to the in-RAM data (for peak-finding and efficiency
    // calculations that run entirely on Core 0 without touching flash).
    BucketFile &data() { return _buckets; }
    const BucketFile &data() const { return _buckets; }

    // Return the number of buckets whose raw_count > 0 across all days.
    int nonZeroCount() const;

private:
    // Load buckets.bin from LittleFS into _buckets.
    // Returns true on success; false if the file is absent, wrong size,
    // or has a bad magic/version (caller should create a fresh file).
    bool load();

    // Write _buckets to BUCKET_TMP_FILE then atomically rename to BUCKET_FILE.
    bool writeAtomic();

    // Initialise _buckets to a valid empty state for the given year.
    void initEmpty(uint16_t current_year);

    // The primary in-RAM working copy.  Must live on the heap as a class
    // member — 12,104 bytes is too large for any task stack.
    BucketFile _buckets;
};
