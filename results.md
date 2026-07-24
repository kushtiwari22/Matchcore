# Benchmark results — limit orders

Run of 180 limit orders submitted in a single burst (order IDs 101–280). Market orders were excluded from this run to isolate matching-engine performance from external network calls.

## Summary

| Metric | Value |
|---|---|
| Orders processed | 180 |
| Min latency | 345 µs |
| Max latency | 67,271 µs (~67.3 ms) |
| Mean latency | ~32.83 ms |
| Median latency | ~33.64 ms |
| Std dev | ~19.50 ms |
| Sustained throughput | ~2,676 orders/sec |
| Per-order queueing cost (slope) | ~374.7 µs/order |

## What the numbers mean

Latency climbs almost perfectly linearly with an order's position in the processing queue (slope ≈ 374.7 µs/order, near-zero intercept). This means:

- Actual per-order validation + matching cost is small (~370 µs).
- The single-threaded validation stage (`process_initial_orders`) is the throughput ceiling — every order waits behind every order submitted before it, regardless of how simple its own validation is.
- The first order (101) finishes in 345 µs; the 180th order to complete (170) waits ~67.3 ms purely from queueing, not compute.

This confirms the bottleneck is architectural (single validation thread), not algorithmic (the matching logic itself is fast).

## Raw latency data

| Order ID | Latency (µs) |
|---|---|
| 101 | 345 |
| 106 | 765 |
| 110 | 1152 |
| 114 | 1546 |
| 116 | 1801 |
| 117 | 2067 |
| 118 | 2253 |
| 119 | 2502 |
| 120 | 2776 |
| 121 | 3183 |
| 122 | 3580 |
| 123 | 4077 |
| 124 | 4369 |
| 125 | 4634 |
| 126 | 4893 |
| 127 | 5199 |
| 128 | 5429 |
| 129 | 5654 |
| 130 | 5935 |
| 131 | 6194 |
| 132 | 6411 |
| 133 | 6739 |
| 134 | 6959 |
| 135 | 7185 |
| 136 | 7434 |
| 137 | 7667 |
| 105 | 8216 |
| 139 | 8398 |
| 140 | 8736 |
| 141 | 9107 |
| 142 | 9513 |
| 143 | 9858 |
| 144 | 10206 |
| 145 | 10605 |
| 146 | 10910 |
| 147 | 11145 |
| 148 | 11483 |
| 149 | 11831 |
| 150 | 12153 |
| 151 | 12488 |
| 152 | 12811 |
| 153 | 13188 |
| 154 | 13605 |
| 155 | 13991 |
| 156 | 14540 |
| 157 | 14977 |
| 158 | 15374 |
| 159 | 15777 |
| 160 | 16219 |
| 161 | 16659 |
| 162 | 17035 |
| 163 | 17394 |
| 164 | 17726 |
| 165 | 18098 |
| 166 | 18621 |
| 167 | 19108 |
| 113 | 19799 |
| 104 | 20212 |
| 115 | 20695 |
| 171 | 20905 |
| 172 | 21259 |
| 173 | 21758 |
| 174 | 22206 |
| 175 | 22582 |
| 103 | 23289 |
| 111 | 23765 |
| 178 | 23853 |
| 179 | 24232 |
| 180 | 24880 |
| 181 | 25572 |
| 182 | 26289 |
| 183 | 26855 |
| 184 | 27380 |
| 185 | 27709 |
| 186 | 28175 |
| 187 | 28566 |
| 188 | 29006 |
| 189 | 29371 |
| 190 | 29746 |
| 191 | 30122 |
| 192 | 30439 |
| 193 | 30747 |
| 194 | 30930 |
| 195 | 31268 |
| 196 | 31701 |
| 197 | 32076 |
| 198 | 32428 |
| 199 | 32792 |
| 200 | 33180 |
| 201 | 33470 |
| 202 | 33803 |
| 203 | 34287 |
| 204 | 35254 |
| 205 | 35550 |
| 206 | 36908 |
| 207 | 37315 |
| 208 | 37716 |
| 209 | 38039 |
| 210 | 38255 |
| 211 | 38493 |
| 212 | 38720 |
| 213 | 38960 |
| 214 | 39291 |
| 215 | 39471 |
| 216 | 39728 |
| 217 | 39988 |
| 218 | 40334 |
| 219 | 40645 |
| 220 | 40979 |
| 221 | 41267 |
| 222 | 41646 |
| 223 | 41911 |
| 224 | 42356 |
| 225 | 42673 |
| 226 | 42983 |
| 227 | 43271 |
| 228 | 43559 |
| 229 | 43833 |
| 230 | 44122 |
| 231 | 44378 |
| 232 | 44581 |
| 233 | 44701 |
| 234 | 44875 |
| 235 | 45077 |
| 236 | 45344 |
| 237 | 45635 |
| 238 | 45963 |
| 239 | 46190 |
| 240 | 46543 |
| 241 | 46759 |
| 242 | 47102 |
| 243 | 47443 |
| 244 | 47727 |
| 245 | 48012 |
| 246 | 48302 |
| 247 | 48526 |
| 248 | 48776 |
| 249 | 49067 |
| 250 | 49481 |
| 251 | 49839 |
| 252 | 50192 |
| 253 | 50508 |
| 254 | 50850 |
| 255 | 51234 |
| 256 | 51590 |
| 257 | 52077 |
| 258 | 52601 |
| 259 | 53217 |
| 260 | 53609 |
| 261 | 54144 |
| 262 | 54569 |
| 263 | 54869 |
| 264 | 55065 |
| 265 | 55477 |
| 266 | 55748 |
| 267 | 55936 |
| 268 | 56356 |
| 269 | 56918 |
| 270 | 57311 |
| 271 | 58254 |
| 272 | 58634 |
| 273 | 58979 |
| 274 | 59551 |
| 275 | 60153 |
| 276 | 60679 |
| 277 | 61567 |
| 278 | 61974 |
| 279 | 62265 |
| 280 | 62600 |
| 138 | 63599 |
| 102 | 64026 |
| 107 | 64361 |
| 108 | 64681 |
| 109 | 65102 |
| 176 | 65174 |
| 177 | 65479 |
| 112 | 66165 |
| 168 | 66536 |
| 169 | 66861 |
| 170 | 67271 |

## Market orders — not yet benchmarked cleanly

An earlier run mixing in market buy orders showed latencies jumping to 2–3.5 seconds, traced to a synchronous live-price HTTP fetch (`getcurrent()`) running inside the single validation thread — this blocks every order queued behind it, not just the market order itself. See [Known limitations](README.md#known-limitations--future-work) in the README for the planned fix (background-refreshed price cache).
