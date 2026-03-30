# =============================================================================
# Navien Recirculation Efficiency Scorer — Configuration
# =============================================================================

# --- InfluxDB Connection ---
INFLUX_HOST = "localhost"
INFLUX_PORT = 8086
INFLUX_DB   = "navien"

# --- Cost Rates ---
# Gas: $0.41601/therm, 1 therm = 29,308 kcal
GAS_RATE_USD_PER_KCAL = 0.41601 / 29308          # ~$0.00001420/kcal

# Water: $11.40/CCF, 1 CCF = 748 gallons, 1 gallon = 3.785 L
WATER_RATE_USD_PER_L  = 11.40 / (748 * 3.785)    # ~$0.004024/L

# --- Timing Parameters ---
COLD_PIPE_DRAIN_MINUTES = 3.0    # assumed drain time when recirc hasn't recently run
RECIRC_WINDOW_MINUTES   = 15.0   # how long after a recirc cycle pipes stay hot
SAMPLE_INTERVAL_SECONDS = 5      # InfluxDB polling interval

# --- Output Measurement (written back to InfluxDB) ---
OUTPUT_MEASUREMENT = "navien_efficiency"
OUTPUT_TAG_DEVICE  = "navien"
