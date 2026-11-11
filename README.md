# MatchCore

A C++17 order matching engine. It matches limit and market orders using price-time (FIFO)
priority, tracks each user's cash and shares, and saves every trade to PostgreSQL.

## How it works

Each order goes through these stages, and each stage runs on its own thread:

1. Validation: checks the user has enough cash or shares and locks them.
2. Matching: matches the order against the order book for its ticker.
3. Settlement: updates the buyer's and seller's cash and shares.
4. Database: writes the trade to PostgreSQL.

The stages pass orders to each other through queues. Order and trade objects come from
pre-allocated pools instead of being allocated one by one.

Limit orders match at their price or better. Anything not filled stays in the book.

Market orders fill at the best available price. Anything not filled is cancelled.
A market buy locks 1.1 x the live price (from Alpha Vantage) and never pays more than that.
If the live price can't be fetched, the order is rejected.

## Build and run

Needs: g++ with C++17, libpqxx, a running PostgreSQL, and curl.

    g++ -std=c++17 -O2 matching_engine.cpp -o matching_engine -lpqxx -lpq -lpthread
    export EXCHANGE_DB_CONN="dbname=exchange_db user=postgres password=<password> hostaddr=127.0.0.1 port=5432"
    ./matching_engine

The engine reads Sample_order.txt, processes every order, writes the trades to the
trade_history table, and prints each user's final balance.

For market orders, put your Alpha Vantage API key in getcurrent().

## Input format

    USER <user_id> <cash>
    ORDER <ticker> <order_id> <user_id> <volume> <type> <price> <is_market>

type: 1 = buy, 2 = sell. is_market: 0 = limit, 1 = market (price is ignored for market orders).
Orders with volume <= 0, a missing limit price, or an unknown type are rejected.

## Performance

See results.md.

## Known limitations

- Matching runs on one thread. With more than one, orders on the same ticker could reach the
  book out of order, which breaks FIFO.
- Shares a user buys can be sold only after the trade settles. Because of this, a sell sent
  right after a buy may be accepted or rejected depending on timing.
- The live price fetch runs on the validation thread, so it delays every order behind it.
- Saving to the database (~2,600 trades/s) is much slower than matching. The database can fall
  behind, but nothing is lost on a normal shutdown.
- All state is in memory. If the process crashes, open orders and balances are lost.
- Order cancellation (delete_order) exists but isn't connected to the input and isn't safe to use yet.
