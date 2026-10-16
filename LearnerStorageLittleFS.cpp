/*
Copyright (c) 2026 David Carson (dacarson)
Copyright (c) 2026 Brian Conrey (wsu-runner/navien-manager fork): platform seam

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

// LittleFS implementation of LearnerStorage for the HomeSpan sketch. Blobs
// live at /navien/<name>.bin and are replaced through /navien/<name>.tmp plus
// rename, the same atomic write strategy (and the same file names) the
// BucketStore and NavienLearner used before the storage interface existed, so
// existing devices keep their data.

#include "LearnerPlatform.h"

#ifndef LEARNER_PLATFORM_CUSTOM

#include <LittleFS.h>
#include <stdio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

class LittleFsLearnerStorage : public LearnerStorage {
 public:
  LittleFsLearnerStorage() : lock_(xSemaphoreCreateMutex()) {}

  // save() and load() are reached from both cores (the learner task on Core 0,
  // Telnet's savelearner and the bucket bootstrap on Core 1); one mutex keeps
  // the tmp-write-rename sequences of two callers from interleaving.
  bool load(const char *name, void *buf, size_t len) override {
    Guard g(lock_);
    if (!mount()) return false;
    char path[48];
    makePath(path, sizeof(path), name, "bin");
    if (!LittleFS.exists(path)) return false;

    File f = LittleFS.open(path, "r");
    if (!f) {
      LEARNER_LOG("LearnerStorage: failed to open %s for reading", path);
      return false;
    }
    size_t got = f.read(reinterpret_cast<uint8_t *>(buf), len);
    f.close();
    if (got != len) {
      LEARNER_LOG("LearnerStorage: %s size mismatch (got %u, expected %u)", path, (unsigned) got,
                  (unsigned) len);
      return false;
    }
    return true;
  }

  bool save(const char *name, const void *buf, size_t len) override {
    Guard g(lock_);
    if (!mount()) return false;
    char path[48], tmp[48];
    makePath(path, sizeof(path), name, "bin");
    makePath(tmp, sizeof(tmp), name, "tmp");

    File f = LittleFS.open(tmp, "w");
    if (!f) {
      LEARNER_LOG("LearnerStorage: failed to open %s for writing", tmp);
      return false;
    }
    size_t written = f.write(reinterpret_cast<const uint8_t *>(buf), len);
    f.close();
    if (written != len) {
      LEARNER_LOG("LearnerStorage: short write to %s (%u/%u bytes)", tmp, (unsigned) written,
                  (unsigned) len);
      LittleFS.remove(tmp);
      return false;
    }
    // Atomically replace the live file.
    if (!LittleFS.rename(tmp, path)) {
      LEARNER_LOG("LearnerStorage: rename %s -> %s failed", tmp, path);
      LittleFS.remove(tmp);
      return false;
    }
    return true;
  }

 private:
  struct Guard {
    SemaphoreHandle_t m;
    explicit Guard(SemaphoreHandle_t mutex) : m(mutex) {
      if (m) xSemaphoreTake(m, portMAX_DELAY);
    }
    ~Guard() {
      if (m) xSemaphoreGive(m);
    }
  };

  SemaphoreHandle_t lock_;
  bool mounted_ = false;

  // LittleFS.begin() is a no-op if already mounted (FakeGatoHistoryService
  // mounts it with format-on-fail=true during its constructor). Calling it
  // again is safe and ensures we have a mount before any file access.
  bool mount() {
    if (mounted_) return true;
    if (!LittleFS.begin(true)) {
      LEARNER_LOG("LearnerStorage: LittleFS mount failed");
      return false;
    }
    if (!LittleFS.exists("/navien")) {
      if (!LittleFS.mkdir("/navien")) {
        LEARNER_LOG("LearnerStorage: failed to create /navien directory");
        return false;
      }
      LEARNER_LOG("LearnerStorage: created /navien directory");
    }
    mounted_ = true;
    return true;
  }

  static void makePath(char *out, size_t cap, const char *name, const char *ext) {
    snprintf(out, cap, "/navien/%s.%s", name, ext);
  }
};

}  // namespace

LearnerStorage &learnerStorage() {
  static LittleFsLearnerStorage storage;
  return storage;
}

#endif  // !LEARNER_PLATFORM_CUSTOM
