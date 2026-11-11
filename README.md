# MatchCore

A C++17 order matching engine. It matches limit and market orders by price-time (FIFO)
priority, tracks each user's cash and shares, and saves every trade to PostgreSQL.

## How it works

Each order goes through four stages, and each stage runs on its own thread:

1. Validation - checks the user has enough cash or shares and locks them
2. Matching - matches the order against the order book
3. Settlement - updates the buyer's and seller's cash and shares
4. Database - saves the trade to PostgreSQL

Limit orders match at their price or better. Anything not filled stays in the book.
Market orders fill at the best available price. Anything not filled is cancelled.

## Build and run

Needs: g++ (C++17), libpqxx, PostgreSQL, curl.

    g++ -std=c++17 -O2 matching_engine.cpp -o matching_engine -lpqxx -lpq -lpthread
    export EXCHANGE_DB_CONN="dbname=exchange_db user=postgres password=<password> hostaddr=127.0.0.1 port=5432"
    ./matching_engine

It reads orders from Sample_order.txt and prints each user's final balance.
For market orders, put your Alpha Vantage API key in getcurrent().

## Input format

    USER <user_id> <cash>
    ORDER <ticker> <order_id> <user_id> <volume> <type> <price> <is_market>

type: 1 = buy, 2 = sell
is_market: 0 = limit, 1 = market

## Known limitations

- Matching uses one thread, which keeps orders in FIFO order.
- Market orders fetch a live price, which slows down every order queued behind them.
- Saving to the database is much slower than matching.
- All state is in memory. It is lost if the program crashes.

## Results

See results.md.
