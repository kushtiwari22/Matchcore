#!/usr/bin/env bash
# Reproduces the numbers in results.md.
# Needs: g++ (C++17), libpqxx-dev, and a local PostgreSQL (only for the DB run).
# bench args: mode N tickers match_threads (rate | market_pct) db(0/1)
set -euo pipefail
cd "$(dirname "$0")"
python3 patch.py ../matching_engine.cpp harness_main.cpp bench.cpp
g++ -std=c++17 -O2 -DBENCH_POOL=1200000 bench.cpp -o bench -lpqxx -lpq -lpthread

./bench file ../Sample_order.txt 1 1                                       # sample file
./bench closed 100000 1 1 0 0                                              # latency, one order at a time
for r in 10000 50000 100000 200000; do ./bench rate 1000000 1 1 $r 0; done  # latency under load
./bench burst 1000000 1 1 0 0                                              # max throughput
./bench burst 200000 1 1 0 1                                               # DB write speed
