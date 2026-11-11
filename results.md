# Results

Tested on a 4-core 2.1 GHz Linux machine, g++ -O2, 1 million limit orders.

## Numbers

- Matching time per order: 0.2 us (median)
- Order latency (submit to matched): 25 us (median), 60 us (p99)
- Throughput: about 400,000 orders per second
- Database writes: about 2,600 trades per second

## Latency at different order rates

    orders/sec   median   p99
    10,000       27 us    185 us
    50,000       24 us    576 us
    100,000      22 us    3 ms

## How we measured

- We recorded the time when each order was submitted and when it finished matching.
  Latency = finish time - submit time.
- Times were stored in memory and printed only at the end, so printing did not slow the test.
- Throughput = number of orders / total time to process all of them.
- Test orders were random buys and sells near the same price, so about 1 in 3 orders traded.
