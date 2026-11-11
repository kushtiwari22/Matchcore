# Results

Machine: 4-core Intel Xeon 2.1 GHz cloud VM, Ubuntu 24.04, g++ 13.3 -O2, PostgreSQL 16 on the same machine.
Settings: 1 matching thread, limit orders, 1 ticker.

## Summary

- Matching time per order: 0.2 us median, about 20 us at p99
- End-to-end latency, one order at a time: 25 us median, 60 us at p99
- Max throughput: about 385,000 - 415,000 orders per second
- Database writes: about 2,500 - 2,800 trades per second
- FIFO check: 0 orders out of order (1 million orders)

## Latency under load (1 million orders per test)

    orders/sec   median    p99       p99.9
    10,000       27 us     185 us    2.0 ms
    50,000       24 us     576 us    10.6 ms
    100,000      22 us     3.0 ms    19.1 ms
    200,000      23 us     6.4 ms    23.9 ms

The median stays around 25 us at every rate. The p99 rises once the rate passes about
50,000 orders/sec.

Almost all of the 25 us is two thread handoffs (about 12 us each: intake to validation,
then validation to matching). Matching itself takes about 0.2 us.

## How these numbers were measured

- bench/patch.py makes a copy of matching_engine.cpp that records a timestamp at four points
  for every order: submitted, validated, matching started, matching finished. Timestamps go
  into arrays in memory; nothing is printed while orders are running. The engine logic is
  not changed.
- Test orders are random but repeatable (fixed seed): 100 users, buy or sell 50/50, volume
  1-100, prices near 10,000. About 1 in 3 orders trades. All orders are created before the
  timer starts.
- Latency = matching finished - submitted. For the "under load" table, orders are sent at a
  fixed rate, and latency is measured from when each order was supposed to be sent.
- Throughput = orders processed / (time of last order finished - time of first order submitted),
  sending 1 million orders as fast as possible.
- Database speed = trades saved / time until the last trade was saved.
- FIFO check = the order in which orders reached the book is compared with the order they arrived.
- Run bench/run.sh to repeat all of these tests.

## Why the old numbers were wrong

The old file reported a 33 ms average latency and 2,700 orders/sec. The latency was printed
with cout while holding the order book lock, so every order waited for the previous order's
print. Those numbers measured the console, not the engine.
