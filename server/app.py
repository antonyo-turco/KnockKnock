from flask import Flask, request, jsonify, render_template_string, redirect
import sqlite3
from datetime import datetime, timezone, timedelta
import os
import statistics
import json

from feature_extraction import compute_batch_features

app = Flask(__name__)
DB_PATH = '/data/accelerometer.db'

# Aumenta il limite di dimensione del request (100MB)
app.config['MAX_CONTENT_LENGTH'] = 100 * 1024 * 1024

def init_db():
    os.makedirs('/data', exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    cursor = conn.cursor()
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS measurements (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp DATETIME NOT NULL,
            x REAL NOT NULL,
            y REAL NOT NULL,
            z REAL NOT NULL
        )
    ''')
    cursor.execute('''
        CREATE INDEX IF NOT EXISTS idx_timestamp ON measurements(timestamp)
    ''')
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS aggregates (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp DATETIME NOT NULL,
            batch_number INTEGER NOT NULL,
            min_x REAL NOT NULL,
            max_x REAL NOT NULL,
            avg_x REAL NOT NULL,
            min_y REAL NOT NULL,
            max_y REAL NOT NULL,
            avg_y REAL NOT NULL,
            min_z REAL NOT NULL,
            max_z REAL NOT NULL,
            avg_z REAL NOT NULL,
            count INTEGER NOT NULL
        )
    ''')
    cursor.execute('''
        CREATE INDEX IF NOT EXISTS idx_agg_timestamp ON aggregates(timestamp)
    ''')
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS features (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp DATETIME NOT NULL,
            batch_number INTEGER NOT NULL,
            device_id TEXT NOT NULL,
            n_samples INTEGER NOT NULL,
            sampling_rate_hz REAL NOT NULL,
            window_size INTEGER NOT NULL,
            impact_score REAL NOT NULL,
            event_flag INTEGER NOT NULL,
            x_dom_freq REAL NOT NULL,
            y_dom_freq REAL NOT NULL,
            z_dom_freq REAL NOT NULL,
            m_dom_freq REAL NOT NULL,
            x_p99 REAL NOT NULL,
            y_p99 REAL NOT NULL,
            z_p99 REAL NOT NULL,
            m_p99 REAL NOT NULL,
            x_jerk_max REAL NOT NULL,
            y_jerk_max REAL NOT NULL,
            z_jerk_max REAL NOT NULL,
            m_jerk_max REAL NOT NULL,
            x_band_20_40 REAL NOT NULL,
            y_band_20_40 REAL NOT NULL,
            z_band_20_40 REAL NOT NULL,
            m_band_20_40 REAL NOT NULL,
            m_spectral_flux REAL NOT NULL,
            feature_json TEXT NOT NULL
        )
    ''')
    cursor.execute('''
        CREATE INDEX IF NOT EXISTS idx_features_timestamp ON features(timestamp)
    ''')
    cursor.execute('''
        CREATE INDEX IF NOT EXISTS idx_features_batch ON features(batch_number)
    ''')
    conn.commit()
    conn.close()

@app.route('/api/data/bulk', methods=['POST'])
def receive_bulk_data():
    try:
        print("\n" + "="*60)
        print(f"BULK REQUEST at {datetime.now(timezone.utc)}")
        print("="*60)

        data = request.json
        if data is None:
            print("ERROR: Invalid JSON")
            return jsonify({'status': 'error', 'message': 'Invalid JSON'}), 400

        measurements = data.get('measurements', [])
        print(f"Measurements received: {len(measurements)}")
        if not measurements:
            return jsonify({'status': 'error', 'message': 'No measurements'}), 400

        device_id = str(data.get('device_id', 'esp32-unknown'))
        sampling_rate_hz = float(data.get('sampling_rate_hz', 100))
        if sampling_rate_hz <= 0:
            sampling_rate_hz = 100

        batch_size = int(data.get('batch_size', len(measurements)))
        expected_batch_size = 500
        if len(measurements) != expected_batch_size or batch_size != expected_batch_size:
            return jsonify({
                'status': 'error',
                'message': f'Expected fixed batch of {expected_batch_size} samples',
                'received': len(measurements),
                'batch_size': batch_size,
            }), 400

        server_timestamp = datetime.now(timezone.utc)
        sample_interval_ms = 1000.0 / sampling_rate_hz
        first_timestamp = server_timestamp - timedelta(milliseconds=sample_interval_ms * (len(measurements) - 1))

        conn = sqlite3.connect(DB_PATH)
        cursor = conn.cursor()

        cursor.execute('SELECT COALESCE(MAX(batch_number), 0) + 1 FROM aggregates')
        batch_number = cursor.fetchone()[0]

        x_values = []
        y_values = []
        z_values = []

        for i, measurement in enumerate(measurements):
            x = float(measurement.get('x', 0))
            y = float(measurement.get('y', 0))
            z = float(measurement.get('z', 0))

            x_values.append(x)
            y_values.append(y)
            z_values.append(z)

            measurement_timestamp = first_timestamp + timedelta(milliseconds=sample_interval_ms * i)
            cursor.execute(
                'INSERT INTO measurements (timestamp, x, y, z) VALUES (?, ?, ?, ?)',
                (measurement_timestamp.isoformat(), x, y, z)
            )

        avg_x = statistics.mean(x_values) if x_values else 0
        avg_y = statistics.mean(y_values) if y_values else 0
        avg_z = statistics.mean(z_values) if z_values else 0

        min_x = min(x_values) if x_values else 0
        max_x = max(x_values) if x_values else 0
        min_y = min(y_values) if y_values else 0
        max_y = max(y_values) if y_values else 0
        min_z = min(z_values) if z_values else 0
        max_z = max(z_values) if z_values else 0

        cursor.execute(
            '''INSERT INTO aggregates (timestamp, batch_number, min_x, max_x, avg_x,
                                       min_y, max_y, avg_y, min_z, max_z, avg_z, count)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)''',
            (server_timestamp.isoformat(), batch_number, min_x, max_x, avg_x,
             min_y, max_y, avg_y, min_z, max_z, avg_z, len(measurements))
        )

        features_map, impact_score, event_flag = compute_batch_features(
            x_values, y_values, z_values, sampling_rate_hz=sampling_rate_hz, n_fft=512
        )

        cursor.execute(
            '''INSERT INTO features (
                   timestamp, batch_number, device_id, n_samples, sampling_rate_hz, window_size,
                   impact_score, event_flag,
                   x_dom_freq, y_dom_freq, z_dom_freq, m_dom_freq,
                   x_p99, y_p99, z_p99, m_p99,
                   x_jerk_max, y_jerk_max, z_jerk_max, m_jerk_max,
                   x_band_20_40, y_band_20_40, z_band_20_40, m_band_20_40,
                   m_spectral_flux, feature_json
               )
               VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)''',
            (
                server_timestamp.isoformat(),
                batch_number,
                device_id,
                len(measurements),
                sampling_rate_hz,
                512,
                impact_score,
                event_flag,
                features_map['x']['dominant_freq_1'],
                features_map['y']['dominant_freq_1'],
                features_map['z']['dominant_freq_1'],
                features_map['m']['dominant_freq_1'],
                features_map['x']['p99_abs'],
                features_map['y']['p99_abs'],
                features_map['z']['p99_abs'],
                features_map['m']['p99_abs'],
                features_map['x']['jerk_max'],
                features_map['y']['jerk_max'],
                features_map['z']['jerk_max'],
                features_map['m']['jerk_max'],
                features_map['x']['bandpower_20_40'],
                features_map['y']['bandpower_20_40'],
                features_map['z']['bandpower_20_40'],
                features_map['m']['bandpower_20_40'],
                features_map['m']['spectral_flux'],
                json.dumps(features_map),
            )
        )

        conn.commit()
        conn.close()

        print(f"✓ Inserted {len(measurements)} measurements with progressive timestamps")
        print(f"✓ Fixed batch size: {expected_batch_size}")
        print(f"✓ First timestamp: {first_timestamp.isoformat()}")
        print(f"✓ Last timestamp: {(first_timestamp + timedelta(milliseconds=sample_interval_ms * (len(measurements) - 1))).isoformat()}")
        print(f"✓ Aggregates saved (X:{min_x:.2f}-{max_x:.2f}, Y:{min_y:.2f}-{max_y:.2f}, Z:{min_z:.2f}-{max_z:.2f})")
        print(f"✓ Features saved (impact_score={impact_score:.3f}, event_flag={event_flag})")
        print("="*60 + "\n")

        return jsonify({
            'status': 'ok',
            'count': len(measurements),
            'batch_size': expected_batch_size,
            'server_timestamp': server_timestamp.isoformat(),
            'batch_number': batch_number,
            'impact_score': impact_score,
            'event_flag': event_flag
        }), 201

    except Exception as e:
        print(f"ERROR: {str(e)}")
        import traceback
        traceback.print_exc()
        print("="*60 + "\n")
        return jsonify({'status': 'error', 'message': str(e)}), 400

@app.route('/api/data', methods=['POST'])
def receive_data():
    try:
        data = request.json
        x = float(data.get('x', 0))
        y = float(data.get('y', 0))
        z = float(data.get('z', 0))
        
        timestamp_str = datetime.now(timezone.utc).isoformat()
        
        conn = sqlite3.connect(DB_PATH)
        cursor = conn.cursor()
        cursor.execute(
            'INSERT INTO measurements (timestamp, x, y, z) VALUES (?, ?, ?, ?)',
            (timestamp_str, x, y, z)
        )
        conn.commit()
        conn.close()
        
        return jsonify({'status': 'ok'}), 201
    except Exception as e:
        return jsonify({'status': 'error', 'message': str(e)}), 400

@app.route('/api/data', methods=['GET'])
def get_data():
    try:
        limit = request.args.get('limit', default=100, type=int)
        order = request.args.get('order', default='asc', type=str)
        sliding_window = request.args.get('sliding_window', default='false', type=str).lower() == 'true'
        window_seconds = request.args.get('window_seconds', default=60, type=int)
        
        if order not in ['asc', 'desc']:
            order = 'asc'
        
        conn = sqlite3.connect(DB_PATH)
        conn.row_factory = sqlite3.Row
        cursor = conn.cursor()
        
        if sliding_window:
            # Sliding window: get data from last N seconds
            cutoff_time = datetime.now(timezone.utc) - timedelta(seconds=window_seconds)
            cursor.execute(
                f'''SELECT timestamp, x, y, z FROM measurements 
                   WHERE timestamp > ? 
                   ORDER BY timestamp {order} LIMIT ?''',
                (cutoff_time.isoformat(), limit)
            )
        else:
            # Normal mode: get last N records
            cursor.execute(
                f'SELECT timestamp, x, y, z FROM measurements ORDER BY timestamp {order} LIMIT ?',
                (limit,)
            )
        
        rows = cursor.fetchall()
        conn.close()
        
        data = [dict(row) for row in rows]
        return jsonify({
            'count': len(data),
            'data': data
        }), 200
    except Exception as e:
        return jsonify({'status': 'error', 'message': str(e)}), 400

@app.route('/api/aggregates', methods=['GET'])
def get_aggregates():
    try:
        limit = request.args.get('limit', default=100, type=int)
        
        conn = sqlite3.connect(DB_PATH)
        conn.row_factory = sqlite3.Row
        cursor = conn.cursor()
        cursor.execute(
            '''SELECT timestamp, batch_number, min_x, max_x, avg_x, 
                      min_y, max_y, avg_y, min_z, max_z, avg_z, count 
               FROM aggregates 
               ORDER BY timestamp ASC LIMIT ?''',
            (limit,)
        )
        rows = cursor.fetchall()
        conn.close()
        
        data = [dict(row) for row in rows]
        return jsonify({
            'count': len(data),
            'data': data
        }), 200
    except Exception as e:
        return jsonify({'status': 'error', 'message': str(e)}), 400


@app.route('/api/features', methods=['GET'])
def get_features():
    try:
        limit = request.args.get('limit', default=100, type=int)
        order = request.args.get('order', default='desc', type=str).lower()
        from_ts = request.args.get('from_ts', default=None, type=str)
        to_ts = request.args.get('to_ts', default=None, type=str)

        if order not in ['asc', 'desc']:
            order = 'desc'

        conn = sqlite3.connect(DB_PATH)
        conn.row_factory = sqlite3.Row
        cursor = conn.cursor()

        where_clauses = []
        params = []
        if from_ts:
            where_clauses.append('timestamp >= ?')
            params.append(from_ts)
        if to_ts:
            where_clauses.append('timestamp <= ?')
            params.append(to_ts)

        where_sql = ''
        if where_clauses:
            where_sql = 'WHERE ' + ' AND '.join(where_clauses)

        query = f'''SELECT timestamp, batch_number, device_id, n_samples, sampling_rate_hz,
                           impact_score, event_flag,
                           x_dom_freq, y_dom_freq, z_dom_freq, m_dom_freq,
                           m_p99, m_jerk_max, m_band_20_40, m_spectral_flux
                    FROM features
                    {where_sql}
                    ORDER BY timestamp {order}
                    LIMIT ?'''
        params.append(limit)
        cursor.execute(query, tuple(params))

        rows = cursor.fetchall()
        conn.close()

        return jsonify({'count': len(rows), 'data': [dict(r) for r in rows]}), 200
    except Exception as e:
        return jsonify({'status': 'error', 'message': str(e)}), 400


@app.route('/api/features/latest', methods=['GET'])
def get_features_latest():
    try:
        conn = sqlite3.connect(DB_PATH)
        conn.row_factory = sqlite3.Row
        cursor = conn.cursor()
        cursor.execute(
            '''SELECT timestamp, batch_number, device_id, n_samples, sampling_rate_hz,
                      impact_score, event_flag, feature_json
               FROM features
               ORDER BY timestamp DESC
               LIMIT 1'''
        )
        row = cursor.fetchone()
        conn.close()

        if row is None:
            return jsonify({'status': 'empty', 'data': None}), 200

        result = dict(row)
        result['feature_json'] = json.loads(result['feature_json'])
        return jsonify({'status': 'ok', 'data': result}), 200
    except Exception as e:
        return jsonify({'status': 'error', 'message': str(e)}), 400

@app.route('/api/stats', methods=['GET'])
def get_stats():
    try:
        conn = sqlite3.connect(DB_PATH)
        conn.row_factory = sqlite3.Row
        cursor = conn.cursor()
        
        cursor.execute('SELECT COUNT(*) as count FROM measurements')
        count_row = cursor.fetchone()
        
        cursor.execute('SELECT COUNT(*) as count FROM aggregates')
        agg_count = cursor.fetchone()

        cursor.execute('SELECT COUNT(*) as count FROM features')
        feat_count = cursor.fetchone()
        
        cursor.execute('SELECT MIN(timestamp) as first, MAX(timestamp) as last FROM measurements')
        time_row = cursor.fetchone()
        
        conn.close()

        sampling_rate_hz = 100
        seconds_per_day = 24 * 60 * 60
        estimated_samples_24h = sampling_rate_hz * seconds_per_day
        estimated_batches_24h = estimated_samples_24h // 500
        database_size_bytes = os.path.getsize(DB_PATH) if os.path.exists(DB_PATH) else 0
        estimated_raw_bytes_per_sample = 90
        estimated_raw_24h_bytes = estimated_samples_24h * estimated_raw_bytes_per_sample
        estimated_batch_overhead_bytes = 700
        estimated_aux_24h_bytes = estimated_batches_24h * estimated_batch_overhead_bytes
        estimated_total_24h_bytes = estimated_raw_24h_bytes + estimated_aux_24h_bytes
        
        return jsonify({
            'total_measurements': count_row['count'],
            'total_aggregates': agg_count['count'],
            'total_features': feat_count['count'],
            'first_measurement': time_row['first'],
            'last_measurement': time_row['last'],
            'database_size_bytes': database_size_bytes,
            'estimated_samples_24h': estimated_samples_24h,
            'estimated_batches_24h': estimated_batches_24h,
            'estimated_raw_24h_bytes': estimated_raw_24h_bytes,
            'estimated_aux_24h_bytes': estimated_aux_24h_bytes,
            'estimated_total_24h_bytes': estimated_total_24h_bytes,
        }), 200
    except Exception as e:
        return jsonify({'status': 'error', 'message': str(e)}), 400

@app.route('/api/data/clear', methods=['DELETE'])
def clear_data():
    try:
        conn = sqlite3.connect(DB_PATH)
        cursor = conn.cursor()
        cursor.execute('DELETE FROM measurements')
        cursor.execute('DELETE FROM aggregates')
        cursor.execute('DELETE FROM features')
        count = cursor.rowcount
        conn.commit()
        conn.close()
        
        print(f"\n✓ DELETED {count} measurements and aggregates\n")
        
        return jsonify({
            'status': 'ok',
            'deleted': count
        }), 200
    except Exception as e:
        return jsonify({'status': 'error', 'message': str(e)}), 400

@app.route('/api/data/delete-old', methods=['DELETE'])
def delete_old_data():
    try:
        days = request.args.get('days', default=1, type=int)
        
        cutoff_time = datetime.now(timezone.utc) - timedelta(days=days)
        
        conn = sqlite3.connect(DB_PATH)
        cursor = conn.cursor()
        cursor.execute(
            'DELETE FROM measurements WHERE timestamp < ?',
            (cutoff_time.isoformat(),)
        )
        cursor.execute(
            'DELETE FROM aggregates WHERE timestamp < ?',
            (cutoff_time.isoformat(),)
        )
        cursor.execute(
            'DELETE FROM features WHERE timestamp < ?',
            (cutoff_time.isoformat(),)
        )
        count = cursor.rowcount
        conn.commit()
        conn.close()
        
        print(f"\n✓ DELETED {count} measurements older than {days} day(s)\n")
        
        return jsonify({
            'status': 'ok',
            'deleted': count,
            'cutoff_time': cutoff_time.isoformat()
        }), 200
    except Exception as e:
        return jsonify({'status': 'error', 'message': str(e)}), 400

@app.route('/health', methods=['GET'])
def health():
    return jsonify({'status': 'ok'}), 200

DASHBOARD_HTML = '''
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Accelerometer Dashboard</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: "Segoe UI", Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            padding: 20px;
        }
        .container { max-width: 1400px; margin: 0 auto; }
        h1 {
            color: white;
            text-align: center;
            margin-bottom: 20px;
            font-size: 2.5em;
            text-shadow: 2px 2px 4px rgba(0,0,0,0.3);
        }
        .controls {
            background: white;
            padding: 20px;
            border-radius: 10px;
            margin-bottom: 20px;
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 15px;
            align-items: end;
        }
        .control-group {
            display: flex;
            flex-direction: column;
        }
        .control-group label {
            font-weight: bold;
            margin-bottom: 5px;
            color: #333;
            font-size: 0.9em;
        }
        .control-group input, .control-group select {
            padding: 8px;
            border: 1px solid #ddd;
            border-radius: 5px;
            font-size: 0.9em;
        }
        .control-group button {
            padding: 10px 15px;
            background: #667eea;
            color: white;
            border: none;
            border-radius: 5px;
            cursor: pointer;
            font-weight: bold;
            transition: 0.3s;
        }
        .control-group button:hover {
            background: #764ba2;
        }
        .control-group button.danger {
            background: #ff6b6b;
        }
        .control-group button.danger:hover {
            background: #ff5252;
        }
        .stats-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
            gap: 20px;
            margin-bottom: 30px;
        }
        .stat-card {
            background: white;
            padding: 20px;
            border-radius: 10px;
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
            text-align: center;
        }
        .stat-card h3 {
            color: #667eea;
            font-size: 0.9em;
            margin-bottom: 10px;
            text-transform: uppercase;
        }
        .stat-card .value {
            font-size: 2em;
            color: #333;
            font-weight: bold;
        }
        .stat-card .subtitle {
            color: #999;
            font-size: 0.85em;
            margin-top: 10px;
        }
        .charts-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(400px, 1fr));
            gap: 20px;
            margin-bottom: 30px;
        }
        .chart-container {
            background: white;
            padding: 20px;
            border-radius: 10px;
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
        }
        .chart-container h3 {
            color: #667eea;
            margin-bottom: 15px;
            font-size: 1.1em;
        }
        .chart-wrapper {
            position: relative;
            height: 300px;
        }
        canvas { max-height: 300px; }
        .error {
            background: #ff6b6b;
            color: white;
            padding: 15px;
            border-radius: 5px;
            margin-bottom: 20px;
        }
        .success {
            background: #51cf66;
            color: white;
            padding: 15px;
            border-radius: 5px;
            margin-bottom: 20px;
        }
        .badge {
            display: inline-block;
            background: #51cf66;
            color: white;
            padding: 3px 8px;
            border-radius: 3px;
            font-size: 0.8em;
            margin-left: 10px;
        }
        .badge.warning {
            background: #ffa94d;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>📊 Accelerometer Dashboard <span class="badge" id="modeBadge">Sliding Window: OFF</span></h1>
        
        <div id="error" class="error" style="display: none;"></div>
        <div id="success" class="success" style="display: none;"></div>
        
        <div class="controls">
            <div class="control-group">
                <label>Modalità Visualizzazione:</label>
                <select id="viewMode">
                    <option value="sliding">Ultimi 60 secondi (Live)</option>
                    <option value="normal">Ultimi N dati</option>
                </select>
            </div>
            <div class="control-group">
                <label>Dati da visualizzare:</label>
                <input type="number" id="dataLimit" value="200" min="10" max="1000">
            </div>
            <div class="control-group">
                <label>Y-Axis Max:</label>
                <input type="number" id="yAxisMax" value="1000" step="50">
            </div>
            <div class="control-group">
                <label>Formato asse X:</label>
                <select id="xAxisFormat">
                    <option value="index">Indice</option>
                    <option value="time">Tempo</option>
                </select>
            </div>
            <div class="control-group">
                <button onclick="updateChartSettings()">Aggiorna Grafici</button>
            </div>
            <div class="control-group">
                <button class="danger" onclick="deleteAllData()">Elimina Tutti i Dati</button>
            </div>
            <div class="control-group">
                <button onclick="deleteOldData()">Elimina Dati Vecchi (>24h)</button>
            </div>
        </div>
        
        <div class="stats-grid">
            <div class="stat-card">
                <h3>Total Measurements</h3>
                <div class="value" id="stat-count">-</div>
                <div class="subtitle">readings received</div>
            </div>
            <div class="stat-card">
                <h3>Aggregates</h3>
                <div class="value" id="stat-agg">-</div>
                <div class="subtitle">batches</div>
            </div>
            <div class="stat-card">
                <h3>Last Reading</h3>
                <div class="value" id="stat-last" style="font-size: 1.2em;">-</div>
                <div class="subtitle" id="stat-last-sub"></div>
            </div>
            <div class="stat-card">
                <h3>Server Status</h3>
                <div class="value" id="stat-status" style="font-size: 1.5em;">✓</div>
                <div class="subtitle" id="stat-status-time">online</div>
            </div>
        </div>
        
        <div class="charts-grid">
            <div class="chart-container">
                <h3>X Axis Acceleration</h3>
                <div class="chart-wrapper">
                    <canvas id="chartX"></canvas>
                </div>
            </div>
            <div class="chart-container">
                <h3>Y Axis Acceleration</h3>
                <div class="chart-wrapper">
                    <canvas id="chartY"></canvas>
                </div>
            </div>
            <div class="chart-container">
                <h3>Z Axis Acceleration</h3>
                <div class="chart-wrapper">
                    <canvas id="chartZ"></canvas>
                </div>
            </div>
            <div class="chart-container">
                <h3>3D Acceleration (All Axes)</h3>
                <div class="chart-wrapper">
                    <canvas id="chartAll"></canvas>
                </div>
            </div>
        </div>
    </div>
    
    <script>
        const REFRESH_INTERVAL = 2000;
        let chartX, chartY, chartZ, chartAll;
        let lastData = [];
        
        function getChartOptions(title, color) {
            return {
                responsive: true,
                maintainAspectRatio: false,
                animation: { duration: 0 },
                plugins: {
                    legend: { display: false },
                    title: { display: false }
                },
                scales: {
                    y: {
                        beginAtZero: true,
                        max: parseFloat(document.getElementById('yAxisMax').value),
                        min: -parseFloat(document.getElementById('yAxisMax').value)
                    },
                    x: {
                        display: true,
                        ticks: {
                            maxRotation: 45,
                            minRotation: 0
                        }
                    }
                }
            };
        }
        
        function initCharts() {
            const options = getChartOptions();
            
            chartX = new Chart(document.getElementById("chartX"), {
                type: "line",
                data: {
                    labels: [],
                    datasets: [{
                        label: "X",
                        data: [],
                        borderColor: "#FF6384",
                        backgroundColor: "rgba(255,99,132,0.1)",
                        tension: 0.1,
                        pointRadius: 2,
                        pointHoverRadius: 4
                    }]
                },
                options
            });
            
            chartY = new Chart(document.getElementById("chartY"), {
                type: "line",
                data: {
                    labels: [],
                    datasets: [{
                        label: "Y",
                        data: [],
                        borderColor: "#36A2EB",
                        backgroundColor: "rgba(54,162,235,0.1)",
                        tension: 0.1,
                        pointRadius: 2,
                        pointHoverRadius: 4
                    }]
                },
                options
            });
            
            chartZ = new Chart(document.getElementById("chartZ"), {
                type: "line",
                data: {
                    labels: [],
                    datasets: [{
                        label: "Z",
                        data: [],
                        borderColor: "#FFCE56",
                        backgroundColor: "rgba(255,206,86,0.1)",
                        tension: 0.1,
                        pointRadius: 2,
                        pointHoverRadius: 4
                    }]
                },
                options
            });
            
            chartAll = new Chart(document.getElementById("chartAll"), {
                type: "line",
                data: {
                    labels: [],
                    datasets: [
                        {
                            label: "X",
                            data: [],
                            borderColor: "#FF6384",
                            backgroundColor: "rgba(255,99,132,0.05)",
                            tension: 0.1,
                            pointRadius: 1
                        },
                        {
                            label: "Y",
                            data: [],
                            borderColor: "#36A2EB",
                            backgroundColor: "rgba(54,162,235,0.05)",
                            tension: 0.1,
                            pointRadius: 1
                        },
                        {
                            label: "Z",
                            data: [],
                            borderColor: "#FFCE56",
                            backgroundColor: "rgba(255,206,86,0.05)",
                            tension: 0.1,
                            pointRadius: 1
                        }
                    ]
                },
                options
            });
        }
        
        async function fetchData() {
            try {
                const viewMode = document.getElementById('viewMode').value;
                const limit = document.getElementById('dataLimit').value;
                
                let url;
                if (viewMode === 'sliding') {
                    url = `/api/data?limit=${limit}&order=asc&sliding_window=true&window_seconds=60`;
                    document.getElementById('modeBadge').textContent = 'Sliding Window: ON (60s)';
                    document.getElementById('modeBadge').className = 'badge';
                } else {
                    url = `/api/data?limit=${limit}&order=asc`;
                    document.getElementById('modeBadge').textContent = 'Sliding Window: OFF';
                    document.getElementById('modeBadge').className = 'badge warning';
                }
                
                const response = await fetch(url);
                const result = await response.json();
                
                if (result.data && result.data.length > 0) {
                    lastData = result.data;
                    updateCharts(result.data);
                }
                
                const statsResponse = await fetch("/api/stats");
                const stats = await statsResponse.json();
                updateStats(stats);
                
                document.getElementById("error").style.display = "none";
            } catch (error) {
                console.error("Error fetching data:", error);
                document.getElementById("error").textContent = "Error connecting to server: " + error.message;
                document.getElementById("error").style.display = "block";
            }
        }
        
        function formatTimestamp(timestamp) {
            const date = new Date(timestamp);
            return date.toLocaleTimeString();
        }
        
        function updateCharts(data) {
            const xAxisFormat = document.getElementById('xAxisFormat').value;
            
            let labels;
            if (xAxisFormat === 'time') {
                labels = data.map(d => formatTimestamp(d.timestamp));
            } else {
                labels = data.map((_, i) => i);
            }
            
            const xValues = data.map(d => d.x);
            const yValues = data.map(d => d.y);
            const zValues = data.map(d => d.z);
            
            chartX.data.labels = labels;
            chartX.data.datasets[0].data = xValues;
            chartX.update();
            
            chartY.data.labels = labels;
            chartY.data.datasets[0].data = yValues;
            chartY.update();
            
            chartZ.data.labels = labels;
            chartZ.data.datasets[0].data = zValues;
            chartZ.update();
            
            chartAll.data.labels = labels;
            chartAll.data.datasets[0].data = xValues;
            chartAll.data.datasets[1].data = yValues;
            chartAll.data.datasets[2].data = zValues;
            chartAll.update();
        }
        
        function updateStats(stats) {
            document.getElementById("stat-count").textContent = stats.total_measurements || 0;
            document.getElementById("stat-agg").textContent = stats.total_aggregates || 0;
            
            if (stats.last_measurement) {
                const last = new Date(stats.last_measurement);
                document.getElementById("stat-last").textContent = last.toLocaleTimeString();
                document.getElementById("stat-last-sub").textContent = last.toLocaleDateString();
            }
        }
        
        function updateChartSettings() {
            // Update scale options for all charts
            const yMax = parseFloat(document.getElementById('yAxisMax').value);
            
            chartX.options.scales.y.max = yMax;
            chartX.options.scales.y.min = -yMax;
            
            chartY.options.scales.y.max = yMax;
            chartY.options.scales.y.min = -yMax;
            
            chartZ.options.scales.y.max = yMax;
            chartZ.options.scales.y.min = -yMax;
            
            chartAll.options.scales.y.max = yMax;
            chartAll.options.scales.y.min = -yMax;
            
            // Update data and apply changes
            if (lastData.length > 0) {
                updateCharts(lastData);
            }
            showSuccess("Grafici aggiornati!");
        }
        
        async function deleteAllData() {
            if (confirm("⚠️ Sei sicuro? Verranno eliminati TUTTI i dati!")) {
                try {
                    const response = await fetch("/api/data/clear", { method: "DELETE" });
                    const result = await response.json();
                    showSuccess(`✓ Eliminati ${result.deleted} dati`);
                    fetchData();
                } catch (error) {
                    showError("Errore durante l'eliminazione: " + error.message);
                }
            }
        }
        
        async function deleteOldData() {
            if (confirm("Eliminare i dati più vecchi di 24 ore?")) {
                try {
                    const response = await fetch("/api/data/delete-old?days=1", { method: "DELETE" });
                    const result = await response.json();
                    showSuccess(`✓ Eliminati ${result.deleted} dati vecchi`);
                    fetchData();
                } catch (error) {
                    showError("Errore durante l'eliminazione: " + error.message);
                }
            }
        }
        
        function showSuccess(msg) {
            const elem = document.getElementById("success");
            elem.textContent = msg;
            elem.style.display = "block";
            setTimeout(() => { elem.style.display = "none"; }, 3000);
        }
        
        function showError(msg) {
            const elem = document.getElementById("error");
            elem.textContent = msg;
            elem.style.display = "block";
        }
        
        // Initialization
        window.addEventListener('load', () => {
            initCharts();
            fetchData();
            setInterval(fetchData, REFRESH_INTERVAL);
            
            // Update mode badge on mode change
            document.getElementById('viewMode').addEventListener('change', fetchData);
        });
    </script>
</body>
</html>
'''

@app.route('/dashboard')
def dashboard():
    return render_template_string(DASHBOARD_HTML)

@app.route('/')
def index():
    return redirect('/dashboard')

if __name__ == '__main__':
    init_db()
    app.run(host='0.0.0.0', port=5010, debug=False)
