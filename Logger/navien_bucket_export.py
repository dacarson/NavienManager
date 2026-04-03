#!/usr/bin/python3
"""
navien_bucket_export.py

Step 2 of bootstrap: extract raw bucket data from InfluxDB history and
POST it to the ESP32 POST /buckets endpoint to seed buckets.bin.

Always run navien_bootstrap.py (Step 1) first.
After this step the Pi cron job can be disconnected.

Usage:
    # Dry run — print the JSON payload that would be sent
    python3 navien_bucket_export.py

    # Seed buckets.bin from full history
    python3 navien_bucket_export.py --push

    # Reseed cleanly from scratch (zeroes existing buckets first)
    python3 navien_bucket_export.py --push --replace

Timing constraint:
    Run only when the device is quiet — no active recompute (avoid the
    00:00–00:05 window) and no RS-485 activity. In practice, mid-morning
    on a weekday when the heater has been idle for at least 10 minutes.
"""

import argparse
import json
import sys
from datetime import date as _date

import config
import navien_schedule_learner as nsl


def build_bucket_payload(args, local_tz, replace=False):
    """
    Fetch cold-start events from InfluxDB, convert them to 5-minute buckets,
    and build the sparse JSON payload for POST /buckets.

    Returns a dict ready for json.dumps().
    """
    events = nsl.fetch_consumption_events(args, local_tz)
    if not events:
        print("No events found. Check InfluxDB connection.")
        sys.exit(1)

    raw_counts, weighted_scores = nsl.events_to_minutes(events, verbose=args.verbose)

    days = []
    for dow in range(7):
        day_raw      = raw_counts.get(dow, {})
        day_weighted = weighted_scores.get(dow, {})

        buckets = []
        for b in sorted(day_raw):
            if day_raw[b] > 0 or day_weighted.get(b, 0.0) > 0:
                buckets.append({
                    "b":     b // 5,   # minute offset → 5-min bucket index (0–287)
                    "raw":   int(day_raw[b]),
                    "score": round(float(day_weighted.get(b, 0.0)), 4),
                })
        if buckets:
            days.append({"dow": dow, "buckets": buckets})

    return {
        "schema_version": 1,
        "current_year":   _date.today().year,
        "replace":        replace,
        "days":           days,
    }


def push_buckets(payload, args):
    """POST the bucket payload to the ESP32 /buckets endpoint one day at a time.

    Sends 7 separate requests (one per day-of-week) to stay within the ESP32's
    14 KB receive buffer.  The 'replace' flag is forwarded only on the first
    chunk.  'finalize' is False for all chunks except the last, suppressing a
    premature recompute on partial data.
    """
    import requests

    url  = f"http://{args.esp32_host}:{args.esp32_port}/buckets"
    days = payload["days"]
    total_written = 0

    for i, day in enumerate(days):
        is_last = (i == len(days) - 1)
        chunk = {
            "schema_version": payload["schema_version"],
            "current_year":   payload["current_year"],
            "replace":        payload["replace"] if i == 0 else False,
            "finalize":       is_last,
            "days":           [day],
        }
        body = json.dumps(chunk)
        print(f"[push] POST {url}  day={day['dow']}  ({len(body)} bytes)"
              f"{'  [finalize]' if is_last else ''}")

        try:
            resp = requests.post(url, data=body,
                                 headers={"Content-Type": "application/json"},
                                 timeout=30)
        except requests.exceptions.RequestException as e:
            print(f"[push] Connection error on day {day['dow']}: {e}")
            sys.exit(1)

        if resp.status_code == 200:
            result = resp.json()
            total_written += result.get("buckets_written", 0)
            print(f"[push]   -> {result}")
        else:
            print(f"[push] Error {resp.status_code} on day {day['dow']}: {resp.text}")
            sys.exit(1)

    print(f"[push] All days sent. Total buckets written: {total_written}")
    print("Bootstrap complete. Pi cron job can now be disabled.")


def main():
    parser = argparse.ArgumentParser(
        description="Seed ESP32 buckets.bin from InfluxDB history (bootstrap step 2)",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )

    # InfluxDB connection
    parser.add_argument("--influxdb_host",        default=config.INFLUX_HOST)
    parser.add_argument("--influxdb_port",        default=config.INFLUX_PORT, type=int)
    parser.add_argument("--influxdb_user",        default=None)
    parser.add_argument("--influxdb_pass",        default=None)
    parser.add_argument("--influxdb_db",          default=config.INFLUX_DB)

    # Learning parameters
    parser.add_argument("--recency_weights",      default=nsl.DEFAULT_RECENCY_WEIGHTS,
                        type=int, nargs="+", metavar="W",
                        help="Per-year recency multipliers, most-recent first "
                             "(e.g. --recency_weights 3 2 1 for 3 years of data)")
    parser.add_argument("--cold_gap_minutes",     default=nsl.DEFAULT_COLD_GAP, type=int,
                        help="Inactivity gap in minutes after which pipes are considered cold")
    parser.add_argument("--min_duration_genuine", default=nsl.DEFAULT_MIN_DURATION_GENUINE,
                        type=int,
                        help="Min tap-on buckets (cold pipes) to count as genuine demand "
                             "[10s resolution: 6 buckets = 1 minute]")
    parser.add_argument("--min_duration_recirc",  default=nsl.DEFAULT_MIN_DURATION_RECIRC,
                        type=int,
                        help="Min tap-on buckets (recirc running) to count as genuine demand "
                             "[10s resolution: 3 buckets = 30 seconds]")

    # ESP32 target
    parser.add_argument("--esp32_host",           default="navien.local",
                        help="Hostname or IP of the ESP32")
    parser.add_argument("--esp32_port",           default=8080, type=int)

    # Actions
    parser.add_argument("--replace", action="store_true",
                        help="Zero all buckets on device before seeding "
                             "(safe to re-run if first upload was incorrect)")
    parser.add_argument("--push",    action="store_true",
                        help="Send payload to ESP32 (dry-run by default)")
    parser.add_argument("-v", "--verbose", action="store_true")

    args = parser.parse_args()

    # Full-year window, matching navien_bootstrap.py
    args.window_weeks    = 52
    args.preheat_minutes = nsl.DEFAULT_PREHEAT_MINUTES
    args.gap_minutes     = nsl.DEFAULT_GAP_MINUTES

    today = _date.today()
    years = [today.year - i for i in range(len(args.recency_weights))]
    print(f"Bootstrap mode: window_weeks=52 (full year per recency entry)")
    print(f"Years: {years}  Weights: {args.recency_weights}")

    local_tz, tz_name = nsl.detect_local_timezone()
    print(f"Timezone: {tz_name}")

    print(f"Querying InfluxDB ({args.influxdb_host}:{args.influxdb_port}/{args.influxdb_db}) "
          f"for full-history cold-start events...")

    payload = build_bucket_payload(args, local_tz, replace=args.replace)

    total = sum(len(d["buckets"]) for d in payload["days"])
    payload_json = json.dumps(payload)
    print(f"Built payload: {len(payload['days'])} days, "
          f"{total} non-zero buckets, "
          f"~{len(payload_json) // 1024} KB")

    if args.push:
        push_buckets(payload, args)
    else:
        print("[dry-run] Not pushing. Pass --push to send to ESP32.")
        print(json.dumps(payload, indent=2))


if __name__ == "__main__":
    main()
