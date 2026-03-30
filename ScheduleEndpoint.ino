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

// HTTP endpoint for receiving a pushed schedule from navien_schedule_learner.py.
// Listens on port 8080 (HomeSpan owns port 80).
//
// Uses a raw WiFiServer (already part of <WiFi.h>) instead of the WebServer
// library to avoid ~30-50 KB of extra flash.
//
// POST /schedule
//   Content-Type: application/json
//   Body: {"schedule":[{"slots":[{"startHour":H,"startMinute":M,"endHour":H,"endMinute":M},...]},...]}
//   Array index 0=Sunday .. 6=Saturday.
//
// Returns 200 OK on success, 400 Bad Request on error.

#include "FakeGatoScheduler.h"

extern FakeGatoScheduler *scheduler;

// Maximum JSON body size: 7 days × 4 slots × ~60 chars/slot ≈ 1700 bytes; 2 KB is ample.
#define SCHEDULE_BODY_MAX 2048

static WiFiServer scheduleServer(8080);
static char scheduleBodyBuf[SCHEDULE_BODY_MAX + 1];

// Read HTTP headers line by line, extract Content-Length, then read the body.
// Returns true if a complete body of the advertised length was received within
// the timeout. On success, body points into scheduleBodyBuf (NUL-terminated).
static bool readHttpRequest(WiFiClient &client, char **bodyOut, int *bodyLen) {
  const unsigned long deadline = millis() + 1000;  // 1 s is plenty on LAN
  char line[80];
  int lineLen     = 0;
  int contentLen  = -1;
  bool pastHeaders = false;

  // Read headers one character at a time until the blank separator line.
  while (!pastHeaders && millis() < deadline) {
    while (client.available()) {
      char c = (char)client.read();
      if (c == '\n') {
        if (lineLen > 0 && line[lineLen - 1] == '\r') lineLen--;
        line[lineLen] = '\0';
        if (lineLen == 0) { pastHeaders = true; break; }
        if (strncasecmp(line, "content-length:", 15) == 0)
          contentLen = atoi(line + 15);
        lineLen = 0;
      } else if (lineLen < (int)sizeof(line) - 1) {
        line[lineLen++] = c;
      }
    }
    if (!pastHeaders) delay(1);
  }

  if (!pastHeaders || contentLen <= 0 || contentLen > SCHEDULE_BODY_MAX)
    return false;

  // Read body into static buffer.
  *bodyLen = 0;
  while (*bodyLen < contentLen && millis() < deadline) {
    while (client.available() && *bodyLen < contentLen)
      scheduleBodyBuf[(*bodyLen)++] = (char)client.read();
    if (*bodyLen < contentLen) delay(1);
  }
  scheduleBodyBuf[*bodyLen] = '\0';
  *bodyOut = scheduleBodyBuf;
  return *bodyLen == contentLen;
}

void setupScheduleEndpoint() {
  scheduleServer.begin();
  Serial.println(F("Schedule endpoint listening on navien.local:8080"));
}

void loopScheduleEndpoint() {
  WiFiClient client = scheduleServer.accept();
  if (!client) return;

  char *body   = nullptr;
  int  bodyLen = 0;
  bool ok      = readHttpRequest(client, &body, &bodyLen);

  if (ok)
    ok = scheduler->setWeekScheduleFromJSON(body);

  if (ok)
    client.print(F("HTTP/1.1 200 OK\r\nContent-Length: 18\r\n\r\nSchedule accepted\n"));
  else
    client.print(F("HTTP/1.1 400 Bad Request\r\nContent-Length: 22\r\nConnection: close\r\n\r\nInvalid schedule JSON\n"));

  client.stop();
}
