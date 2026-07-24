# MatchCore — Multi-threaded Limit & Market Order Matching Engine

A C++ exchange simulator implementing price-time (FIFO) priority matching for both **limit** and **market** orders, with per-user balance/portfolio tracking, a concurrent multi-stage processing pipeline, and async trade persistence to PostgreSQL.

## Features

- **Limit and market orders**, both buy and sell side
- **Price-time (FIFO) priority** matching within each price level
- **Per-user account state**: cash balance, locked funds, share portfolio, and available (unlocked) shares
- **Multi-threaded pipeline**: a dedicated validation thread, a pool of concurrent matching worker threads (scales to `hardware_concurrency()`), a dedicated settlement thread, and an async database-writer thread
- **Fixed-size memory pools** for `Order` and `Trade` objects — orders and trades are recycled from pre-allocated pools instead of being heap-allocated per request
- **Live market pricing**: market buy orders fetch a live quote (Alpha Vantage) and lock funds at 10% above it as a safety buffer against price movement between validation and execution
- **Async trade persistence** to PostgreSQL via `libpqxx`, decoupled from the matching path

## Architecture

Orders flow through five stages, each handed off via a thread-safe queue and condition variable:

```
Order intake
     │
     ▼
Validation thread (single)
  — checks funds/shares, locks required amount, price for market orders
     │
     ▼
Pending order queue
     │
     ▼
Matching worker threads (N, concurrent)
  — match against the shared, mutex-protected order book per ticker
     │
     ▼
Settlement thread
  — updates both parties' portfolios and balances
     │
     ▼
DB writer thread
  — persists each trade to PostgreSQL asynchronously
```

Matching against the book itself is protected by a per-ticker `book_mutex`, so multiple tickers can be matched fully in parallel; only orders for the *same* ticker serialize against each other during matching.

## Order types

**Limit orders** rest in the book at their specified price if not immediately fillable, and only match at that price or better.

**Market orders** execute immediately against the best available opposite-side price:
- A market **buy** locks funds at `1.1 × <live price>` (a 10% buffer) at validation time. Once the order fills (fully or partially), any unused locked funds are refunded back to the user's available cash.
- A market **sell** requires the shares to already be available in the user's portfolio; no price lookup is needed since it fills at whatever the book offers.
- If a market order can't be fully filled (book runs out of liquidity on the other side), the remaining quantity is **cancelled outright** rather than resting in the book — this matches real exchange behavior for market orders.

## Build & run

**Dependencies:**
- A C++17 (or newer) compiler
- [`libpqxx`](https://github.com/jtv/libpqxx) and `libpq` (PostgreSQL client library)
- A running PostgreSQL instance (the engine creates its own `trade_history` table on startup)
- `curl` available on `PATH` (used to fetch live prices for market orders)

**Build:**
```bash
g++ -std=c++17 -O2 matching_engine.cpp -o matching_engine -lpqxx -lpq -lpthread
```

**Configure the database connection** — set these before running instead of using the hardcoded default:
```bash
export EXCHANGE_DB_CONN="dbname=exchange_db user=postgres password=<your_password> hostaddr=127.0.0.1 port=5432"
```

**Run:**
```bash
./matching_engine
```
The engine reads commands from Sample_testcase.txt` in the working directory (see format below), processes them, then shuts down cleanly after a short drain period.

## Input format

` Sample_testcase.txt` is a plain-text command file:

```
USER <user_id> <initial_cash>
ORDER <ticker> <order_id> <user_id> <volume> <type> <price> <is_market>
```

- `type`: `1` = buy, `2` = sell
- `price`: limit price (ignored for market orders, but still required as a field)
- `is_market`: `0` = limit order, `1` = market order

Example:
```
USER 1 1000000
USER 2 1000000

ORDER AAPL 101 1 50 2 150 0
ORDER AAPL 102 2 50 1 150 0
ORDER AAPL 103 3 10 1 0 1
```

## Persistence

On startup, the engine creates (if not present):

```sql
CREATE TABLE trade_history (
  id SERIAL PRIMARY KEY,
  ticker VARCHAR(10),
  buyer_id INT,
  seller_id INT,
  price BIGINT,
  volume BIGINT,
  timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

Every settled trade is written here asynchronously by a dedicated DB thread, so persistence never blocks matching.

## Benchmarks

Measured so far, **limit orders only** (180 orders submitted in a burst):

| Metric | Value |
|---|---|
| Mean latency | ~32.8 ms |
| Median latency | ~33.6 ms |
| Min / Max latency | 345 µs / 67.3 ms |
| Sustained throughput | ~2,700 orders/sec |

Latency grows almost perfectly linearly with queue position (~375 µs added per order), which points to a **single-threaded validation stage** as the throughput ceiling — not the matching logic itself, which resolves in the hundreds-of-microseconds range once an order reaches the book.

**Market orders have not yet been benchmarked cleanly.** Each market buy order calls a live, synchronous HTTP price lookup (via `curl`) from inside that same single validation thread. In early testing this added anywhere from ~700ms to several seconds per market order — and because validation is single-threaded, every order queued behind it (market or limit) inherits that delay. A full market-order benchmark first needs the fix described below.

## Known limitations / future work

- **Single-threaded validation is the throughput ceiling.** Matching scales across threads; validation currently does not.
- **Synchronous live price fetch blocks the whole pipeline.** The fix: refresh a cached last-price map on a separate background thread, and have market-order validation read from that cache instead of making a network call inline.
- **In-memory state only.** There's no crash-recovery or restart-from-DB path — order books and balances exist only for the lifetime of the process.
- **DB credentials are hardcoded** — move to environment-based configuration before deploying or publishing.

## Tech stack

C++17/20 • Multithreading (`std::thread`, `std::mutex`, `std::condition_variable`) • Concurrent producer-consumer queues • Custom memory pooling • PostgreSQL • `libpqxx` • Event-driven architecture
