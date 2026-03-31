#!/usr/bin/env python3
"""
Navien Recirculation Efficiency Scorer
=======================================
Queries InfluxDB for a given date range, classifies gas and water waste,
computes a daily efficiency percentage, prints a summary table, and writes
results back to InfluxDB.

Usage:
    python navien_efficiency.py                          # yesterday (UTC calendar day)
    python navien_efficiency.py --all                    # every day from first to last datapoint
    python navien_efficiency.py --date 2026-03-29       # specific day
    python navien_efficiency.py --start 2026-03-01 --end 2026-03-29  # range

Cron (daily roll-up for yesterday):
    0 5 * * * cd /path/to/Logger && ./venv/bin/python navien_efficiency.py --quiet

``--quiet`` still prints the table and write status; use with ``--no-write`` to dry-run.
Days are aligned to UTC midnight (see ``query_day``), matching stored Influx timestamps.
"""

import argparse
import sys
from datetime import datetime, timedelta, timezone

import numpy as np
import pandas as pd
from influxdb import InfluxDBClient

import config


# =============================================================================
# InfluxDB helpers
# =============================================================================

def get_client():
    return InfluxDBClient(
        host=config.INFLUX_HOST,
        port=config.INFLUX_PORT,
        database=config.INFLUX_DB,
    )


def _measurement_time_span(client, measurement: str):
    """Return (first_ts, last_ts) as pandas Timestamps, or (None, None) if empty."""
    r_first = client.query(
        f"SELECT * FROM {measurement} ORDER BY time ASC LIMIT 1"
    )
    r_last = client.query(
        f"SELECT * FROM {measurement} ORDER BY time DESC LIMIT 1"
    )
    pts_f = list(r_first.get_points(measurement=measurement))
    pts_l = list(r_last.get_points(measurement=measurement))
    if not pts_f or not pts_l:
        return None, None
    return pd.to_datetime(pts_f[0]["time"]), pd.to_datetime(pts_l[0]["time"])


def get_scorable_day_bounds(client) -> tuple[datetime.date, datetime.date]:
    """
    Earliest and latest UTC calendar dates that may contain gas or water points.
    """
    spans = []
    for m in ("water", "gas"):
        lo, hi = _measurement_time_span(client, m)
        if lo is not None and hi is not None:
            spans.append((lo, hi))
    if not spans:
        raise SystemExit(
            "No data in 'water' or 'gas' measurements — cannot determine --all range."
        )
    first = min(s[0] for s in spans)
    last = max(s[1] for s in spans)
    return first.date(), last.date()


def query_day(client, date: datetime.date) -> pd.DataFrame:
    """
    Pull all required fields for one UTC calendar day (00:00Z to 00:00Z next day)
    and return a single time-indexed DataFrame, forward-filling boolean fields.
    """
    # InfluxDB stores timestamps in UTC; we query a full local day in RFC3339
    start = datetime(date.year, date.month, date.day, 0, 0, 0, tzinfo=timezone.utc)
    end   = start + timedelta(days=1)

    start_s = start.strftime("%Y-%m-%dT%H:%M:%SZ")
    end_s   = end.strftime("%Y-%m-%dT%H:%M:%SZ")

    gas_q = (
        f"SELECT current_gas_usage FROM gas "
        f"WHERE time >= '{start_s}' AND time < '{end_s}'"
    )
    water_q = (
        f"SELECT flow_lpm, consumption_active, recirculation_active, recirculation_running "
        f"FROM water "
        f"WHERE time >= '{start_s}' AND time < '{end_s}'"
    )

    gas_result   = client.query(gas_q)
    water_result = client.query(water_q)

    def to_df(result, measurement):
        points = list(result.get_points(measurement=measurement))
        if not points:
            return pd.DataFrame()
        df = pd.DataFrame(points)
        df["time"] = pd.to_datetime(df["time"])
        df = df.set_index("time").sort_index()
        return df

    gas_df   = to_df(gas_result, "gas")
    water_df = to_df(water_result, "water")

    if gas_df.empty and water_df.empty:
        return pd.DataFrame()

    # Merge on time index, forward-fill boolean/integer fields
    if gas_df.empty:
        combined = water_df
    elif water_df.empty:
        combined = gas_df
    else:
        combined = water_df.join(gas_df, how="outer")

    # Zero out flow_lpm where consumption_active is not explicitly 1 BEFORE forward-filling.
    # This must happen on raw values because ffill would otherwise propagate consumption_active=1
    # into recirc-only rows, making the mask ineffective after the join.
    if "flow_lpm" in combined.columns and "consumption_active" in combined.columns:
        combined.loc[combined["consumption_active"] != 1, "flow_lpm"] = 0.0

    # Forward-fill boolean/state fields; fill remaining NaN with 0
    bool_fields = ["consumption_active", "recirculation_active", "recirculation_running"]
    for col in bool_fields:
        if col in combined.columns:
            combined[col] = combined[col].ffill().fillna(0).astype(int)

    if "flow_lpm" in combined.columns:
        combined["flow_lpm"] = combined["flow_lpm"].fillna(0.0)

    if "current_gas_usage" in combined.columns:
        combined["current_gas_usage"] = combined["current_gas_usage"].fillna(0.0)

    return combined


# =============================================================================
# Analysis
# =============================================================================

def find_rising_edges(series: pd.Series):
    """Return index positions where series transitions from 0 → 1."""
    arr = series.values.astype(int)
    edges = np.where((arr[:-1] == 0) & (arr[1:] == 1))[0] + 1
    return edges


def find_contiguous_runs(series: pd.Series, value=1):
    """
    Return list of (start_idx, end_idx) for contiguous runs of `value`.
    end_idx is inclusive.
    """
    arr = series.values.astype(int)
    runs = []
    in_run = False
    start = 0
    for i, v in enumerate(arr):
        if v == value and not in_run:
            in_run = True
            start = i
        elif v != value and in_run:
            in_run = False
            runs.append((start, i - 1))
    if in_run:
        runs.append((start, len(arr) - 1))
    return runs


def score_day(df: pd.DataFrame, date) -> dict:
    """
    Given a merged DataFrame for one day, compute all efficiency metrics.
    Returns a dict of results.
    """
    sample_sec  = config.SAMPLE_INTERVAL_SECONDS
    sample_min  = sample_sec / 60.0
    window_min  = config.RECIRC_WINDOW_MINUTES
    drain_min   = config.COLD_PIPE_DRAIN_MINUTES

    required = ["consumption_active", "recirculation_running", "flow_lpm", "current_gas_usage"]
    for col in required:
        if col not in df.columns:
            df[col] = 0

    times = df.index

    # ------------------------------------------------------------------
    # Step 1: Classify recirculation cycles
    # ------------------------------------------------------------------
    recirc_runs = find_contiguous_runs(df["recirculation_running"])

    total_gas_cost_usd   = 0.0
    wasted_gas_cost_usd  = 0.0
    wasted_recirc_cycles = 0
    efficient_recirc_cycles = 0

    for (start_i, end_i) in recirc_runs:
        # Gas cost for this cycle
        # current_gas_usage is in kcal/hr; multiply by sample duration in hours
        cycle_kcal = float(df["current_gas_usage"].iloc[start_i:end_i + 1].sum()) * (sample_sec / 3600.0)
        cycle_cost = cycle_kcal * config.GAS_RATE_USD_PER_KCAL
        total_gas_cost_usd += cycle_cost

        # Look forward up to RECIRC_WINDOW_MINUTES after cycle ends
        cycle_end_time = times[end_i]
        window_end_time = cycle_end_time + pd.Timedelta(minutes=window_min)
        future_mask = (times > cycle_end_time) & (times <= window_end_time)

        tap_in_window = df.loc[future_mask, "consumption_active"].any()

        if tap_in_window:
            efficient_recirc_cycles += 1
        else:
            wasted_gas_cost_usd += cycle_cost
            wasted_recirc_cycles += 1

    # ------------------------------------------------------------------
    # Step 2: Classify tap-on events — cold pipe drain waste
    # ------------------------------------------------------------------
    tap_edges = find_rising_edges(df["consumption_active"])

    total_water_cost_usd   = 0.0
    wasted_water_cost_usd  = 0.0
    cold_pipe_events       = 0

    for edge_i in tap_edges:
        tap_time = times[edge_i]

        # Was recirc running or did it finish within the last RECIRC_WINDOW_MINUTES?
        window_start = tap_time - pd.Timedelta(minutes=window_min)
        recent_mask  = (times >= window_start) & (times < tap_time)
        pipes_hot    = df.loc[recent_mask, "recirculation_running"].any()

        # Also treat tap as hot if recirc_active fired recently (mode on, pump may have just stopped)
        if not pipes_hot:
            pipes_hot = df.loc[recent_mask, "recirculation_active"].any()

        # Find the full extent of this tap-on event (until consumption_active drops to 0)
        tap_end_i = edge_i
        while tap_end_i + 1 < len(df) and df["consumption_active"].iloc[tap_end_i + 1] == 1:
            tap_end_i += 1
        actual_tap_samples = tap_end_i - edge_i + 1

        # Total water cost = full tap duration
        full_flow_slice      = df["flow_lpm"].iloc[edge_i:tap_end_i + 1]
        total_volume_L       = float(full_flow_slice.sum()) * sample_min
        event_cost           = total_volume_L * config.WATER_RATE_USD_PER_L
        total_water_cost_usd += event_cost

        if not pipes_hot:
            drain_samples = int((drain_min * 60) / sample_sec)
            if actual_tap_samples > drain_samples:
                # Tap ran longer than drain window — user was waiting for hot water.
                # Only the first drain_min of flow is wasted; after that, hot water arrived.
                waste_flow     = df["flow_lpm"].iloc[edge_i:edge_i + drain_samples]
                waste_volume_L = float(waste_flow.sum()) * sample_min
                waste_cost     = waste_volume_L * config.WATER_RATE_USD_PER_L

                wasted_water_cost_usd += waste_cost
                cold_pipe_events      += 1
            # else: tap ran < drain_min — user didn't expect hot water, no waste charged

    # ------------------------------------------------------------------
    # Step 3: Roll up
    # ------------------------------------------------------------------
    total_cost  = total_gas_cost_usd + total_water_cost_usd
    total_waste = wasted_gas_cost_usd + wasted_water_cost_usd

    if total_cost > 0:
        efficiency_pct = (1.0 - total_waste / total_cost) * 100.0
    else:
        efficiency_pct = None   # no data

    return {
        "date":                    str(date),
        "efficiency_pct":          efficiency_pct,
        "total_cost_usd":          total_cost,
        "total_gas_cost_usd":      total_gas_cost_usd,
        "total_water_cost_usd":    total_water_cost_usd,
        "wasted_gas_cost_usd":     wasted_gas_cost_usd,
        "wasted_water_cost_usd":   wasted_water_cost_usd,
        "wasted_recirc_cycles":    wasted_recirc_cycles,
        "efficient_recirc_cycles": efficient_recirc_cycles,
        "cold_pipe_events":        cold_pipe_events,
    }


# =============================================================================
# InfluxDB write-back
# =============================================================================

def write_results(client, results: list[dict]):
    points = []
    for r in results:
        if r["efficiency_pct"] is None:
            continue
        # Timestamp = midnight UTC of that date
        ts = datetime.strptime(r["date"], "%Y-%m-%d").replace(tzinfo=timezone.utc)
        points.append({
            "measurement": config.OUTPUT_MEASUREMENT,
            "tags": {"device": config.OUTPUT_TAG_DEVICE},
            "time": ts.isoformat(),
            "fields": {
                "efficiency_pct":          round(r["efficiency_pct"], 2),
                "total_cost_usd":          round(r["total_cost_usd"], 6),
                "total_gas_cost_usd":      round(r["total_gas_cost_usd"], 6),
                "total_water_cost_usd":    round(r["total_water_cost_usd"], 6),
                "wasted_gas_cost_usd":     round(r["wasted_gas_cost_usd"], 6),
                "wasted_water_cost_usd":   round(r["wasted_water_cost_usd"], 6),
                "total_waste_usd": round(
                    r["wasted_gas_cost_usd"] + r["wasted_water_cost_usd"], 6
                ),
                "wasted_recirc_cycles":    r["wasted_recirc_cycles"],
                "efficient_recirc_cycles": r["efficient_recirc_cycles"],
                "cold_pipe_events":        r["cold_pipe_events"],
            },
        })
    if points:
        client.write_points(points, time_precision="s")
        print(f"\n✓ Wrote {len(points)} record(s) to InfluxDB → {config.OUTPUT_MEASUREMENT}")
    else:
        print("\n⚠  No data to write.")


# =============================================================================
# Output table
# =============================================================================

HEADER = (
    f"{'Date':<12} {'Effic%':>7} {'Total$':>8} {'Gas$':>8} {'Water$':>8} "
    f"{'WstGas$':>9} {'WstH2O$':>9} {'TotalWst$':>10} {'ColdPipe':>9} {'WstRcirc':>9} {'OkRcirc':>8}"
)
ROW_FMT = (
    "{date:<12} {eff:>7} {total:>8} {gas:>8} {water:>8} "
    "{wgas:>9} {wwater:>9} {twaste:>10} {cold:>9} {wrcirc:>9} {okrcirc:>8}"
)

def fmt_usd(v): return f"${v:.4f}"
def fmt_pct(v): return f"{v:.1f}%" if v is not None else "  N/A"

def print_table(results: list[dict]):
    print()
    print(HEADER)
    print("-" * len(HEADER))
    for r in results:
        total_waste = r["wasted_gas_cost_usd"] + r["wasted_water_cost_usd"]
        print(ROW_FMT.format(
            date    = r["date"],
            eff     = fmt_pct(r["efficiency_pct"]),
            total   = fmt_usd(r["total_cost_usd"]),
            gas     = fmt_usd(r["total_gas_cost_usd"]),
            water   = fmt_usd(r["total_water_cost_usd"]),
            wgas    = fmt_usd(r["wasted_gas_cost_usd"]),
            wwater  = fmt_usd(r["wasted_water_cost_usd"]),
            twaste  = fmt_usd(total_waste),
            cold    = str(r["cold_pipe_events"]),
            wrcirc  = str(r["wasted_recirc_cycles"]),
            okrcirc = str(r["efficient_recirc_cycles"]),
        ))
    # Totals row
    valid = [r for r in results if r["efficiency_pct"] is not None]
    if valid:
        print("-" * len(HEADER))
        print(ROW_FMT.format(
            date    = "TOTAL",
            eff     = fmt_pct(sum(r["efficiency_pct"] for r in valid) / len(valid)),
            total   = fmt_usd(sum(r["total_cost_usd"] for r in valid)),
            gas     = fmt_usd(sum(r["total_gas_cost_usd"] for r in valid)),
            water   = fmt_usd(sum(r["total_water_cost_usd"] for r in valid)),
            wgas    = fmt_usd(sum(r["wasted_gas_cost_usd"] for r in valid)),
            wwater  = fmt_usd(sum(r["wasted_water_cost_usd"] for r in valid)),
            twaste  = fmt_usd(sum(r["wasted_gas_cost_usd"] + r["wasted_water_cost_usd"] for r in valid)),
            cold    = str(sum(r["cold_pipe_events"] for r in valid)),
            wrcirc  = str(sum(r["wasted_recirc_cycles"] for r in valid)),
            okrcirc = str(sum(r["efficient_recirc_cycles"] for r in valid)),
        ))


# =============================================================================
# CLI
# =============================================================================

def parse_args():
    yesterday = (datetime.now(timezone.utc).date() - timedelta(days=1))
    parser = argparse.ArgumentParser(description="Navien recirculation efficiency scorer")
    group = parser.add_mutually_exclusive_group()
    group.add_argument(
        "--all",
        action="store_true",
        help="Score every UTC day from first datapoint to last (water ∪ gas)",
    )
    group.add_argument("--date", type=str, help="Single date YYYY-MM-DD (default: yesterday)")
    group.add_argument("--start", type=str, help="Start date YYYY-MM-DD (use with --end)")
    parser.add_argument("--end", type=str, help="End date YYYY-MM-DD (inclusive, use with --start)")
    parser.add_argument("--no-write", action="store_true", help="Skip writing back to InfluxDB")
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Less console noise when scoring many days (per-day lines omitted)",
    )
    args = parser.parse_args()

    if args.start and not args.end:
        parser.error("--start requires --end")
    if args.end and not args.start:
        parser.error("--end requires --start")

    if args.all:
        dates = None  # resolved in main() after a single DB connection is opened
    elif args.date:
        dates = [datetime.strptime(args.date, "%Y-%m-%d").date()]
    elif args.start:
        start = datetime.strptime(args.start, "%Y-%m-%d").date()
        end = datetime.strptime(args.end, "%Y-%m-%d").date()
        if end < start:
            parser.error("--end must be >= --start")
        dates = [start + timedelta(days=i) for i in range((end - start).days + 1)]
    else:
        dates = [yesterday]

    return dates, args.no_write, args.quiet, bool(args.all)


def main():
    dates, no_write, quiet, all_days = parse_args()
    client = get_client()
    if all_days:
        lo, hi = get_scorable_day_bounds(client)
        dates = [lo + timedelta(days=i) for i in range((hi - lo).days + 1)]

    print(f"Scoring {len(dates)} day(s): {dates[0]} → {dates[-1]}")

    results = []
    for n, date in enumerate(dates, start=1):
        if not quiet:
            print(f"  Querying {date}...", end=" ", flush=True)
        elif len(dates) > 31 and (n == 1 or n == len(dates) or n % 10 == 0):
            print(f"  … {n}/{len(dates)} days ({date})", flush=True)
        df = query_day(client, date)
        if df.empty:
            if not quiet:
                print("no data.")
            results.append({
                "date": str(date),
                "efficiency_pct": None,
                "total_cost_usd": 0.0,
                "total_gas_cost_usd": 0.0,
                "total_water_cost_usd": 0.0,
                "wasted_gas_cost_usd": 0.0,
                "wasted_water_cost_usd": 0.0,
                "wasted_recirc_cycles": 0,
                "efficient_recirc_cycles": 0,
                "cold_pipe_events": 0,
            })
            continue
        r = score_day(df, date)
        results.append(r)
        if not quiet:
            eff = fmt_pct(r["efficiency_pct"])
            print(f"done  →  efficiency: {eff}")

    print_table(results)

    if not no_write:
        write_results(client, results)
    else:
        print("\n(--no-write set, skipping InfluxDB write-back)")

    client.close()


if __name__ == "__main__":
    main()
