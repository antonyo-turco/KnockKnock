#!/usr/bin/env python3

import argparse
import csv
import os
import sqlite3
import sys

ALLOWED_TABLES = ['measurements', 'aggregates', 'features']


def export_table(db_path, out_dir, table_name):
    if table_name not in ALLOWED_TABLES:
        raise ValueError(f'Unsupported table: {table_name}')

    out_path = os.path.join(out_dir, f'{table_name}.csv')
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    cursor = conn.cursor()
    cursor.execute(f'SELECT * FROM {table_name}')
    rows = cursor.fetchall()
    columns = [col[0] for col in cursor.description]

    os.makedirs(out_dir, exist_ok=True)
    with open(out_path, 'w', newline='', encoding='utf-8') as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(columns)
        for row in rows:
            writer.writerow([row[col] for col in columns])

    conn.close()
    print(f'Exported {len(rows)} rows from {table_name} to {out_path}')


def parse_args():
    parser = argparse.ArgumentParser(
        description='Export SQLite accelerometer data to CSV files.'
    )
    parser.add_argument(
        '--db-path',
        required=True,
        help='Path to the SQLite database file (e.g. ../accelerometer.db)'
    )
    parser.add_argument(
        '--out-dir',
        default='exports',
        help='Output directory for CSV files'
    )
    parser.add_argument(
        '--tables',
        default=','.join(ALLOWED_TABLES),
        help='Comma-separated table names to export (default: measurements,aggregates,features)'
    )
    return parser.parse_args()


def main():
    args = parse_args()
    db_path = os.path.expanduser(args.db_path)
    out_dir = os.path.expanduser(args.out_dir)

    if not os.path.exists(db_path):
        print(f'Error: database file not found: {db_path}', file=sys.stderr)
        sys.exit(1)

    tables = [table.strip() for table in args.tables.split(',') if table.strip()]
    for table in tables:
        try:
            export_table(db_path, out_dir, table)
        except Exception as exc:
            print(f'Failed to export {table}: {exc}', file=sys.stderr)
            sys.exit(1)

    print('Done.')


if __name__ == '__main__':
    main()
