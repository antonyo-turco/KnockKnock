#!/usr/bin/env python3

import argparse
import csv
import json
import os
import sys

try:
    import requests
except ImportError:
    requests = None
    import urllib.request
    import urllib.error

DEFAULT_ENDPOINTS = ['data', 'aggregates', 'features']


def fetch_json(url):
    if requests:
        resp = requests.get(url, timeout=30)
        resp.raise_for_status()
        return resp.json()
    else:
        req = urllib.request.Request(url, headers={'Accept': 'application/json'})
        with urllib.request.urlopen(req, timeout=30) as response:
            body = response.read().decode('utf-8')
            return json.loads(body)


def save_csv(out_dir, endpoint_name, rows):
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, f'{endpoint_name}.csv')

    if not rows:
        print(f'No rows returned for {endpoint_name}, creating empty CSV: {out_path}')
        with open(out_path, 'w', newline='', encoding='utf-8') as csvfile:
            csvfile.write('')
        return

    header = list(rows[0].keys())
    with open(out_path, 'w', newline='', encoding='utf-8') as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(header)
        for row in rows:
            writer.writerow([row.get(col, '') for col in header])

    print(f'Exported {len(rows)} rows from {endpoint_name} to {out_path}')


def parse_args():
    parser = argparse.ArgumentParser(
        description='Export accelerometer data from the Flask API to CSV files.'
    )
    parser.add_argument(
        '--server-url',
        default='http://localhost:5010',
        help='Base URL for the server API'
    )
    parser.add_argument(
        '--out-dir',
        default='exports_api',
        help='Output directory for CSV files'
    )
    parser.add_argument(
        '--limit',
        type=int,
        default=500,
        help='Maximum number of records to request per endpoint'
    )
    parser.add_argument(
        '--endpoints',
        default=','.join(DEFAULT_ENDPOINTS),
        help='Comma-separated endpoints to export (default: data,aggregates,features)'
    )
    return parser.parse_args()


def normalize_endpoint(endpoint_name):
    if endpoint_name == 'data':
        return '/api/data'
    return f'/api/{endpoint_name}'


def main():
    args = parse_args()
    server_url = args.server_url.rstrip('/')
    out_dir = os.path.expanduser(args.out_dir)
    endpoints = [ep.strip() for ep in args.endpoints.split(',') if ep.strip()]

    for endpoint in endpoints:
        url = server_url + normalize_endpoint(endpoint)
        query_string = f'?limit={args.limit}'
        if endpoint == 'data':
            url += query_string + '&order=asc'
        else:
            url += query_string

        try:
            result = fetch_json(url)
        except Exception as exc:
            print(f'Failed to fetch {url}: {exc}', file=sys.stderr)
            sys.exit(1)

        rows = result.get('data') if isinstance(result, dict) else None
        if rows is None:
            print(f'Unexpected API response for {url}: missing "data" field', file=sys.stderr)
            sys.exit(1)

        save_csv(out_dir, endpoint, rows)

    print('Done.')


if __name__ == '__main__':
    main()
