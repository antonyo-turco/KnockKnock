# Data Analysis Export Tools

This folder contains utilities to export accelerometer data from the Dockerized Flask server and the SQLite database used by `server/app.py`.

## What is included

- `export_from_sqlite.py` — export raw SQLite tables into CSV files.
- `export_from_api.py` — export server API endpoints into CSV files.
- `README.md` — instructions for copying the Dockerized database and exporting data.

## Current server details

- Server URL: `http://localhost:5010`
- Docker Compose service: `accelerometer-server`
- Database path inside the container: `/data/accelerometer.db`
- Local Docker volume: `data`

## Recommended workflow

### 1. Copy the SQLite database from Docker

If the server is running as a Docker Compose service, use one of these commands:

```bash
cd /Users/alessandrococcia/Desktop/IoT/KnockKnock/server
# Inspect the running container name
docker compose ps

# Copy the database file from the container to the workspace
docker compose exec accelerometer-server cat /data/accelerometer.db > ../data_analysis/accelerometer.db
```

If `docker compose exec` does not work, identify the container name and use `docker cp`:

```bash
docker ps --filter "name=accelerometer-server" --format "{{.Names}}"
# Example output: server_accelerometer-server_1

docker cp server_accelerometer-server_1:/data/accelerometer.db /Users/alessandrococcia/Desktop/IoT/KnockKnock/accelerometer.db
```

> Note: the `server/README.md` still describes the general server setup, but the current compose mapping exposes the application on port `5010`.

### 2. Export data from SQLite

Use `export_from_sqlite.py` to convert the local DB into CSV files.

```bash
cd /Users/alessandrococcia/Desktop/IoT/KnockKnock/data_analysis
python3 export_from_sqlite.py --db-path ../accelerometer.db --out-dir exports
```

This creates:

- `exports/measurements.csv`
- `exports/aggregates.csv`
- `exports/features.csv`

### 3. Export data from the live API

If the Flask server is running, use `export_from_api.py`:

```bash
cd /Users/alessandrococcia/Desktop/IoT/KnockKnock/data_analysis
python3 export_from_api.py --server-url http://localhost:5010 --out-dir exports_api --limit 500
```

This creates:

- `exports_api/data.csv`
- `exports_api/aggregates.csv`
- `exports_api/features.csv`

## Table meanings

- `measurements` — raw accelerometer readings saved per sample.
- `aggregates` — batch-level min/max/average summaries for each 500-sample window.
- `features` — extracted feature values used for classification or event detection.

## TinyML guidance for ESP32

### Why decision trees / random forests?

- `Decision trees` are simple to evaluate with a few comparisons and arithmetic operations.
- `Random forests` are ensembles of many trees; they can improve accuracy, but cost more CPU and flash.
- Both are easier to deploy on ESP32 than deep neural networks when your dataset is small and latency must be low.

### What to do next

1. Export labeled CSV data using the scripts in this folder.
2. Build feature vectors from `features.csv` to feed a model trainer.
3. Prefer a small feature set for embedded inference, e.g. energy, dominant frequency, jerk, and spectral band power.
4. Keep the model size modest:
   - a single decision tree can be converted to nested `if` statements in C/C++,
   - a small random forest is still feasible if each tree is shallow.

### Deployment options for ESP32

- Use a generated C implementation of a tree or forest.
- Store tree thresholds and feature weights in flash or constants.
- Compute features on the ESP32 from raw accelerometer windows, then pass them to the tree logic.

### Practical recommendation

- Start with one decision tree using a small number of features.
- Export `features.csv`, explore which features separate your events, and then convert the final model manually or with a tiny code generator.
- If you need higher accuracy, move to a very small random forest, but test memory usage carefully.

## Notes

- The server currently validates fixed batches of 500 samples in `server/app.py`.
- If you want a combined dataset, you can merge the CSV files later by `timestamp` or `batch_number`.
- These scripts are intentionally lightweight and use only Python standard libraries (with `requests` fallback for API export).
