#!/usr/bin/python3
"""
Backfill new gas and water fields into InfluxDB 1.x by re-parsing the stored
raw debug hex strings.

Gas fields added (previously unknown, now decoded):
  target_gas_usage        (kcal)     - raw bytes 19-20 (little-endian uint16)
  elapsed_install_days    (days)     - raw bytes 28-29 (little-endian uint16)
  accumulated_water_usage (L x 0.1) - raw bytes 32-35 (little-endian uint32)
  recirculation_enabled   (0 or 1)  - raw byte 46

Water fields added (previously unknown, now decoded):
  system_stage            (raw byte) - raw byte 10
  stage_idle/starting/active/shutting_down  (derived from upper nibble)
  stage_standby/demand/pre_purge/ignition/flame_on/ramp_up
  stage_active_combustion/water_adjustment/flame_off
  stage_post_purge_1/post_purge_2/dhw_wait  (specific sub-states)
  system_active           (0 or 1)  - raw byte 27
  operation_time          (hours)   - raw bytes 28-29 (little-endian uint16)
  internal_recirculation  (0 or 1)  - raw byte 24 bit 0x01
  external_recirculation  (0 or 1)  - raw byte 24 bit 0x02

The script pages through each measurement using time-based cursor pagination
(WHERE time > last_seen LIMIT N), which avoids InfluxDB's max-select-point
scan limit. Each batch is parsed and written back immediately so only N records
are ever in memory at once.

Usage:
  python3 backfill_gas_fields.py [--dry_run] [--influxdb_host HOST] ...
"""

import argparse
import sys
import time

# ---------------------------------------------------------------------------
# Raw packet layout constants (matches Navien.h)
# ---------------------------------------------------------------------------
HDR_SIZE = 6   # sizeof(HEADER)

CMD_TYPE_GAS   = 0x45
CMD_TYPE_WATER = 0x42

# --- Gas struct offsets within GAS_DATA (raw index = HDR_SIZE + offset) ---
GAS_OFF_TARGET_LO   = 13  # target_burner_power_lo  (raw byte 19)
GAS_OFF_TARGET_HI   = 14  # target_burner_power_hi  (raw byte 20)
GAS_OFF_ELAPSED_LO  = 22  # elapsed_install_days_lo (raw byte 28)
GAS_OFF_ELAPSED_HI  = 23  # elapsed_install_days_hi (raw byte 29)
GAS_OFF_WATER_B0    = 26  # cumulative_water_usage_lo  (raw byte 32)
GAS_OFF_WATER_B1    = 27  # cumulative_water_usage_hi  (raw byte 33)
GAS_OFF_WATER_B2    = 28  # cumulative_water_usage_b2  (raw byte 34)
GAS_OFF_WATER_B3    = 29  # cumulative_water_usage_b3  (raw byte 35)
GAS_OFF_RECIRC      = 40  # recirculation_enabled       (raw byte 46)

MIN_GAS_LEN = HDR_SIZE + GAS_OFF_RECIRC + 1   # = 47

# --- Water struct offsets within WATER_DATA (raw index = HDR_SIZE + offset) ---
WATER_OFF_STAGE       = 4   # system_stage      (raw byte 10)
WATER_OFF_STATUS      = 18  # system_status     (raw byte 24)
WATER_OFF_ACTIVE      = 21  # system_active     (raw byte 27)
WATER_OFF_OP_TIME_LO  = 22  # operation_time_lo (raw byte 28)
WATER_OFF_OP_TIME_HI  = 23  # operation_time_hi (raw byte 29)

MIN_WATER_LEN = HDR_SIZE + WATER_OFF_OP_TIME_HI + 1  # = 30


# ---------------------------------------------------------------------------
# Parse helpers
# ---------------------------------------------------------------------------

def _decode_hex(hex_string):
    """Return bytes from a space-separated uppercase hex string, or None."""
    if not hex_string:
        return None
    try:
        return bytes(int(b, 16) for b in hex_string.strip().split())
    except ValueError:
        return None


def parse_gas_debug(hex_string):
    """Return dict of new gas fields, or None on failure."""
    raw = _decode_hex(hex_string)
    if raw is None or len(raw) < MIN_GAS_LEN:
        return None
    if raw[HDR_SIZE] != CMD_TYPE_GAS:
        return None

    target_gas_usage = (raw[HDR_SIZE + GAS_OFF_TARGET_HI] << 8 |
                        raw[HDR_SIZE + GAS_OFF_TARGET_LO])

    elapsed_install_days = (raw[HDR_SIZE + GAS_OFF_ELAPSED_HI] << 8 |
                            raw[HDR_SIZE + GAS_OFF_ELAPSED_LO])

    raw_water = (raw[HDR_SIZE + GAS_OFF_WATER_B3] << 24 |
                 raw[HDR_SIZE + GAS_OFF_WATER_B2] << 16 |
                 raw[HDR_SIZE + GAS_OFF_WATER_B1] << 8  |
                 raw[HDR_SIZE + GAS_OFF_WATER_B0])
    accumulated_water_usage = round(0.1 * raw_water, 1)

    return {
        'target_gas_usage':        target_gas_usage,
        'elapsed_install_days':    elapsed_install_days,
        'accumulated_water_usage': accumulated_water_usage,
        'recirculation_enabled':   1 if raw[HDR_SIZE + GAS_OFF_RECIRC] else 0,
    }


def parse_water_debug(hex_string):
    """Return dict of new water fields, or None on failure."""
    raw = _decode_hex(hex_string)
    if raw is None or len(raw) < MIN_WATER_LEN:
        return None
    if raw[HDR_SIZE] != CMD_TYPE_WATER:
        return None

    stage      = raw[HDR_SIZE + WATER_OFF_STAGE]
    sys_status = raw[HDR_SIZE + WATER_OFF_STATUS]
    op_time    = (raw[HDR_SIZE + WATER_OFF_OP_TIME_HI] << 8 |
                  raw[HDR_SIZE + WATER_OFF_OP_TIME_LO])
    upper      = stage & 0xF0

    return {
        'system_stage':           stage,
        # High-level stage groups
        'stage_idle':             1 if upper == 0x10 else 0,
        'stage_starting':         1 if upper == 0x20 else 0,
        'stage_active':           1 if upper == 0x30 else 0,
        'stage_shutting_down':    1 if upper == 0x40 else 0,
        # Specific sub-states
        'stage_standby':          1 if stage == 0x14 else 0,
        'stage_demand':           1 if stage == 0x20 else 0,
        'stage_pre_purge':        1 if stage == 0x29 else 0,
        'stage_ignition':         1 if stage == 0x2B else 0,
        'stage_flame_on':         1 if stage == 0x2C else 0,
        'stage_ramp_up':          1 if stage == 0x2D else 0,
        'stage_active_combustion':1 if stage == 0x33 else 0,
        'stage_water_adjustment': 1 if stage == 0x34 else 0,
        'stage_flame_off':        1 if stage == 0x3C else 0,
        'stage_post_purge_1':     1 if stage == 0x46 else 0,
        'stage_post_purge_2':     1 if stage == 0x47 else 0,
        'stage_dhw_wait':         1 if stage == 0x49 else 0,
        # System status
        'system_active':          1 if raw[HDR_SIZE + WATER_OFF_ACTIVE] else 0,
        'operation_time':         op_time,
        'internal_recirculation': 1 if sys_status & 0x01 else 0,
        'external_recirculation': 1 if sys_status & 0x02 else 0,
    }


# ---------------------------------------------------------------------------
# Backfill logic
# ---------------------------------------------------------------------------

def backfill_measurement(client, measurement, parse_fn, batch_size, delay, dry_run, verbose):
    written = failed = 0
    last_time = 0  # epoch seconds cursor; advances to last timestamp seen

    while True:
        # Select only the debug field — fetching all fields with SELECT * is
        # slow when records have many fields.  The WHERE clause uses only the
        # time index (no field-value scan) so each query is O(batch_size).
        query = (f'SELECT debug FROM "{measurement}"'
                 f' WHERE time > {last_time}s'
                 f' ORDER BY time ASC LIMIT {batch_size}')
        print(f"  Querying {measurement} after t={last_time}...")
        result = client.query(query, epoch='s')
        points = list(result.get_points())

        if not points:
            break

        to_write = []
        for point in points:
            debug = point.get('debug') or ''
            new_field_values = parse_fn(debug)
            if new_field_values is None:
                failed += 1
                if verbose:
                    print(f"    SKIP (parse fail) t={point['time']}  debug={debug!r:.60}")
                continue

            if verbose:
                print(f"    t={point['time']}  {new_field_values}")

            to_write.append({
                'measurement': measurement,
                'time':        point['time'],
                'fields':      new_field_values,
            })

        # Advance cursor to the last timestamp in this batch
        last_time = points[-1]['time']

        if to_write:
            if not dry_run:
                client.write_points(to_write, time_precision='s')
            written += len(to_write)
            print(f"    Wrote {len(to_write)} points (last t={last_time})...")

        if len(points) < batch_size:
            break

        time.sleep(delay)

    return written, failed


def backfill(client, batch_size, delay, dry_run, verbose):
    totals = {}

    for measurement, parse_fn in [
        ('gas',   parse_gas_debug),
        ('water', parse_water_debug),
    ]:
        print(f"\nBackfilling '{measurement}'...")
        w, f = backfill_measurement(
            client, measurement, parse_fn,
            batch_size, delay, dry_run, verbose,
        )
        totals[measurement] = (w, f)

    print()
    print("Results:")
    for measurement, (w, f) in totals.items():
        print(f"  {measurement}:")
        print(f"    Written:        {w}")
        print(f"    Parse failures: {f}")
    if dry_run:
        print("\n  ** Dry run — nothing was written to InfluxDB **")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='Backfill new gas and water fields into InfluxDB 1.x from stored raw debug hex data.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument('--influxdb_host', default='localhost',  help='InfluxDB hostname (default: localhost)')
    parser.add_argument('--influxdb_port', default=8086, type=int, help='InfluxDB port (default: 8086)')
    parser.add_argument('--influxdb_user', default=None,         help='InfluxDB username')
    parser.add_argument('--influxdb_pass', default=None,         help='InfluxDB password')
    parser.add_argument('--influxdb_db',   default='navien',     help='InfluxDB database (default: navien)')
    parser.add_argument('--batch_size',    default=100, type=int,   help='Records per SELECT page / write batch (default: 100)')
    parser.add_argument('--delay',         default=0.5, type=float, help='Seconds to sleep between batches (default: 0.5)')
    parser.add_argument('--timeout',       default=30,  type=int,   help='InfluxDB query timeout in seconds (default: 30)')
    parser.add_argument('--dry_run',       action='store_true',  help='Parse and report without writing anything')
    parser.add_argument('-v', '--verbose', action='store_true',  help='Print each record being processed')
    args = parser.parse_args()

    try:
        from influxdb import InfluxDBClient
    except ImportError:
        print("Error: influxdb package not installed.  Run: pip install influxdb")
        sys.exit(1)

    client = InfluxDBClient(
        host=args.influxdb_host,
        port=args.influxdb_port,
        username=args.influxdb_user,
        password=args.influxdb_pass,
        database=args.influxdb_db,
        timeout=args.timeout,
    )

    backfill(client, args.batch_size, args.delay, args.dry_run, args.verbose)
