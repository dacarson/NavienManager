#!/usr/bin/python3
"""
navien_bootstrap.py

Step 1 of bootstrap: compute a schedule from full InfluxDB history and
push it to the ESP32 via POST /schedule.

Uses navien_schedule_learner with window_weeks=52 (full year per recency
entry) instead of the default rolling ±4-week seasonal window.

Run once after first flash or whenever buckets.bin is wiped.
Always run navien_bucket_export.py immediately after.

Usage:
    # Dry run — review the historically-learned schedule before pushing
    python3 navien_bootstrap.py

    # Push the finished schedule to the ESP32
    python3 navien_bootstrap.py --push

    # Control years of history and their weights
    python3 navien_bootstrap.py --recency_weights 3 2 1 --push
"""

import argparse
import json
import sys
from datetime import datetime as _datetime, timezone as _timezone

import config
import navien_schedule_learner as nsl


def main():
    parser = argparse.ArgumentParser(
        description="Bootstrap ESP32 schedule from full InfluxDB history",
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
    parser.add_argument("--preheat_minutes",      default=nsl.DEFAULT_PREHEAT_MINUTES, type=int,
                        help="Minutes before first predicted demand to start recirc")
    parser.add_argument("--gap_minutes",          default=nsl.DEFAULT_GAP_MINUTES, type=int,
                        help="Merge demand events closer than this into one window")
    parser.add_argument("--min_occurrences",      default=nsl.DEFAULT_MIN_OCCURRENCES, type=int,
                        help="Minimum times a window must appear to be scheduled")
    parser.add_argument("--min_weighted_score",   default=nsl.DEFAULT_MIN_WEIGHTED_SCORE,
                        type=float,
                        help="Starting score threshold for peak qualification")
    parser.add_argument("--min_score_floor",      default=nsl.DEFAULT_MIN_SCORE_FLOOR,
                        type=float,
                        help="Lowest score the adaptive threshold will relax to")
    parser.add_argument("--peak_half_width",      default=30, type=int,
                        help="Half-width in minutes of the window built around each peak")
    parser.add_argument("--min_peak_separation",  default=45, type=int,
                        help="Minimum minutes between two accepted peaks")

    # ESP32 target
    parser.add_argument("--esp32_host",           default="navien.local",
                        help="Hostname or IP of the ESP32")
    parser.add_argument("--esp32_port",           default=8080, type=int)

    # Actions
    parser.add_argument("--push",    action="store_true",
                        help="Push the schedule to the ESP32 (dry-run by default)")
    parser.add_argument("--dry_run", action="store_true",
                        help="Print schedule but do not push (default behaviour)")
    parser.add_argument("-v", "--verbose", action="store_true")

    args = parser.parse_args()

    # Bootstrap always uses a full-year window so all available history is included
    args.window_weeks = 52

    today = _datetime.now(_timezone.utc).date()
    years = [today.year - i for i in range(len(args.recency_weights))]
    print(f"Bootstrap mode: window_weeks=52 (full year per recency entry)")
    print(f"Years: {years}  Weights: {args.recency_weights}")

    print(f"Querying InfluxDB ({args.influxdb_host}:{args.influxdb_port}/{args.influxdb_db}) "
          f"for full-history cold-start events...")
    events = nsl.fetch_consumption_events(args)
    if not events:
        print("No events found. Check InfluxDB connection.")
        sys.exit(1)
    print(f"Found {len(events)} cold-start events across full history.")

    raw_counts, weighted_scores = nsl.events_to_minutes(events, verbose=args.verbose)

    week = nsl.buckets_to_windows(
        raw_counts, weighted_scores,
        gap_minutes=args.gap_minutes,
        min_occurrences=args.min_occurrences,
        preheat_minutes=args.preheat_minutes,
        peak_half_width=args.peak_half_width,
        min_peak_separation=args.min_peak_separation,
        min_weighted_score=args.min_weighted_score,
        min_score_floor=args.min_score_floor,
        verbose=args.verbose,
    )

    nsl.print_schedule(week, raw_counts=raw_counts, weighted_scores=weighted_scores,
                       verbose=args.verbose, args=args)

    if args.push and not args.dry_run:
        nsl.push_schedule(week, args)
        print("Step 1 complete. Now run: python3 navien_bucket_export.py --push")
    else:
        print("[dry-run] Not pushing. Pass --push to send to ESP32.")
        print(json.dumps({"schedule": week}, indent=2))


if __name__ == "__main__":
    main()
