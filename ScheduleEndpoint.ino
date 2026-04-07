/*
Copyright (c) 2025 David Carson (dacarson)

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

// HTTP endpoint for receiving pushed schedule/bucket data from Pi scripts.
// Listens on port 8080 (HomeSpan owns port 80).
//
// Uses a raw WiFiServer (already part of <WiFi.h>) instead of the WebServer
// library to avoid ~30-50 KB of extra flash.
//
// POST /schedule
//   Content-Type: application/json
//   Body: {"schedule":[{"slots":[{"startHour":H,"startMinute":M,"endHour":H,"endMinute":M},...]},...]}
//   Array index 0=Sunday .. 6=Saturday.
//   Returns 200 OK on success, 400 Bad Request on error.
//
// POST /buckets  (Phase 9 — bootstrap bucket ingest)
//   Content-Type: application/json
//   Body: {"schema_version":1,"current_year":2025,"replace":false,
//          "days":[{"dow":0,"buckets":[{"b":72,"raw":5,"score":12.0},...]},...]}
//   Returns 200 OK with JSON body on success, 400 Bad Request on error.

#include "FakeGatoScheduler.h"
#include "NavienLearner.h"

extern FakeGatoScheduler *scheduler;
extern NavienLearner     *learner;

// Schedule body buffer: 7 days × 4 slots × ~60 chars/slot ≈ 1700 bytes; 2 KB is ample.
#define SCHEDULE_BODY_MAX 2048
static char scheduleBodyBuf[SCHEDULE_BODY_MAX + 1];

// Bucket body buffer: one day of sparse data at a time (see navien_bucket_export.py).
// Largest single-day payload is ~12 KB; 14 KB gives headroom.
#define BUCKETS_BODY_MAX (14 * 1024)
static char bucketsBodyBuf[BUCKETS_BODY_MAX + 1];

static WiFiServer scheduleServer(8080);

// ---------------------------------------------------------------------------
// readHttpHeaders() — read HTTP request line and headers from the client.
// Extracts the request path (e.g. "/schedule") into pathBuf and the
// Content-Length into *contentLen.  Returns true if headers were read
// successfully with a positive Content-Length.
// ---------------------------------------------------------------------------
static bool readHttpHeaders(WiFiClient &client,
                            char *pathBuf, int pathBufSize,
                            int *contentLen) {
  const unsigned long deadline = millis() + 1000;
  char line[80];
  int  lineLen   = 0;
  bool firstLine = true;
  bool pastHeaders = false;
  *contentLen = -1;
  if (pathBufSize > 0) pathBuf[0] = '\0';

  while (!pastHeaders && millis() < deadline) {
    while (client.available()) {
      char c = (char)client.read();
      if (c == '\n') {
        if (lineLen > 0 && line[lineLen - 1] == '\r') lineLen--;
        line[lineLen] = '\0';
        if (lineLen == 0) { pastHeaders = true; break; }
        if (firstLine) {
          firstLine = false;
          // "POST /path HTTP/1.1" — extract path (second space-delimited token)
          char *sp1 = strchr(line, ' ');
          if (sp1) {
            char *sp2 = strchr(sp1 + 1, ' ');
            int plen  = sp2 ? (int)(sp2 - sp1 - 1) : (int)strlen(sp1 + 1);
            if (plen > 0 && plen < pathBufSize - 1) {
              memcpy(pathBuf, sp1 + 1, plen);
              pathBuf[plen] = '\0';
            }
          }
        } else if (strncasecmp(line, "content-length:", 15) == 0) {
          *contentLen = atoi(line + 15);
        }
        lineLen = 0;
      } else if (lineLen < (int)sizeof(line) - 1) {
        line[lineLen++] = c;
      }
    }
    if (!pastHeaders) delay(1);
  }

  return pastHeaders && *contentLen > 0;
}

// ---------------------------------------------------------------------------
// readHttpBody() — read exactly contentLen bytes into buf (NUL-terminated).
// Returns true if the full body was received within the timeout.
// ---------------------------------------------------------------------------
static bool readHttpBody(WiFiClient &client,
                         char *buf, int bufMax,
                         int contentLen, int *bodyLen) {
  const unsigned long deadline = millis() + 1000;
  *bodyLen = 0;
  if (contentLen > bufMax) return false;

  while (*bodyLen < contentLen && millis() < deadline) {
    while (client.available() && *bodyLen < contentLen)
      buf[(*bodyLen)++] = (char)client.read();
    if (*bodyLen < contentLen) delay(1);
  }
  buf[*bodyLen] = '\0';
  return *bodyLen == contentLen;
}

void setupScheduleEndpoint() {
  scheduleServer.begin();
  Serial.println(F("Schedule endpoint listening on navien.local:8080"));
}

void loopScheduleEndpoint() {
  WiFiClient client = scheduleServer.accept();
  if (!client) return;

  char path[24];
  int  contentLen = -1;

  if (!readHttpHeaders(client, path, sizeof(path), &contentLen)) {
    client.print(F("HTTP/1.1 400 Bad Request\r\nContent-Length: 15\r\nConnection: close\r\n\r\nBad HTTP request"));
    client.stop();
    return;
  }

  if (strcmp(path, "/schedule") == 0) {
    // POST /schedule — accept a finished schedule from navien_bootstrap.py
    int  bodyLen = 0;
    bool ok = readHttpBody(client, scheduleBodyBuf, SCHEDULE_BODY_MAX, contentLen, &bodyLen)
              && scheduler->setWeekScheduleFromJSON(scheduleBodyBuf);
    if (ok) {
      Serial.println(F("Schedule updated from HTTP POST"));
      client.print(F("HTTP/1.1 200 OK\r\nContent-Length: 18\r\n\r\nSchedule accepted\n"));
    }
    else
      client.print(F("HTTP/1.1 400 Bad Request\r\nContent-Length: 22\r\nConnection: close\r\n\r\nInvalid schedule JSON\n"));

  } else if (strcmp(path, "/buckets") == 0) {
    // POST /buckets — ingest sparse bucket data from navien_bucket_export.py
    if (!learner || learner->isDisabled()) {
      client.print(F("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 20\r\nConnection: close\r\n\r\nLearner unavailable\n"));
      client.stop();
      return;
    }
    int  bodyLen = 0;
    bool replaced = false;
    int  written  = -1;
    if (readHttpBody(client, bucketsBodyBuf, BUCKETS_BODY_MAX, contentLen, &bodyLen))
      written = learner->ingestBucketPayload(bucketsBodyBuf, replaced);

    if (written < 0) {
      client.print(F("HTTP/1.1 400 Bad Request\r\nContent-Length: 20\r\nConnection: close\r\n\r\nInvalid bucket JSON\n"));
    } else {
      char resp[80];
      int rlen = snprintf(resp, sizeof(resp),
                          "{\"status\":\"ok\",\"buckets_written\":%d,\"replaced\":%s}",
                          written, replaced ? "true" : "false");
      char hdr[80];
      snprintf(hdr, sizeof(hdr),
               "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n",
               rlen);
      client.print(hdr);
      client.print(resp);
    }

  } else {
    client.print(F("HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot Found"));
  }

  client.stop();
}
