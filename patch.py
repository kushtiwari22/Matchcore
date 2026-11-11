import sys, re
src = open(sys.argv[1]).read().replace('\r\n', '\n')

def rep(old, new, count=1, optional=False):
    global src
    n = src.count(old)
    if optional and n == 0:
        return
    assert n == count, (old[:60], n)
    src = src.replace(old, new)

# 1. includes + instrumentation globals
rep('#include <pqxx/pqxx>\n', '''#include <pqxx/pqxx>
#include <random>
#include <string>
#include <cstring>
''')
rep('uint64_t currTime() {', '''// ---------------- BENCH INSTRUMENTATION ----------------
static inline uint64_t now_ns() {
    return chrono::duration_cast<chrono::nanoseconds>(chrono::steady_clock::now().time_since_epoch()).count();
}
const int MAX_IDS = 2000000;
vector<uint64_t> t_submit(MAX_IDS), t_valid(MAX_IDS), t_lock(MAX_IDS), t_done(MAX_IDS);
vector<int> book_seq(MAX_IDS, -1);
atomic<int> n_done(0), n_rejected(0);
atomic<long> n_trades(0), n_db_written(0);
bool DB_ENABLED = false;
bool STUB_PRICE = true;
atomic<long> price_fetch_ns(0);
// --------------------------------------------------------

uint64_t currTime() {''')

# 2. market price: stub (Alpha Vantage unreachable from bench box) but keep timing hook
rep('uint64_t getcurrent(const string& ticker) {\n', '''uint64_t getcurrent_real(const string& ticker);
uint64_t getcurrent(const string& ticker) {
    if (STUB_PRICE) return 10000;
    uint64_t t0 = now_ns();
    uint64_t p = getcurrent_real(ticker);
    price_fetch_ns += now_ns() - t0;
    return p;
}
uint64_t getcurrent_real(const string& ticker) {
''')

# 3. pool size configurable
rep('const int POOL_SIZE = 10000;', '''#ifndef BENCH_POOL
#define BENCH_POOL 10000
#endif
const int POOL_SIZE = BENCH_POOL;''')

# 4. count rejections in validation, timestamp validation done
src = src.replace('''            if (user->current_amount < static_cast<int64_t>(order->order_size * required_price)) {
                free_order(order);''', '''            if (user->current_amount < static_cast<int64_t>(order->order_size * required_price)) {
                n_rejected++;
                free_order(order);''')
src = src.replace('''            if (user->available_shares[tick] < order->order_size) {
                free_order(order);''', '''            if (user->available_shares[tick] < order->order_size) {
                n_rejected++;
                free_order(order);''')
rep('''        lock_guard<mutex> lock(pending_orders_mutex);
        trading_exchange.pending_orders_queue.push({user, order});''', '''        lock_guard<mutex> lock(pending_orders_mutex);
        t_valid[order->order_id] = now_ns();
        trading_exchange.pending_orders_queue.push({user, order});''')

# 5. match_order: timestamp lock acquire, record FIFO sequence, remove cout from critical section
rep('''    lock_guard<mutex> book_lock(book_mutex);
    string tick = order->ticker;
''', '''    lock_guard<mutex> book_lock(book_mutex);
    const int oid = order->order_id;
    t_lock[oid] = now_ns();
    static atomic<int> global_book_seq(0);
    book_seq[oid] = global_book_seq++;
    string tick = order->ticker;
''')
rep('''    auto latency_us = chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now() - order->submit_time).count();
    cout << "Order " << order->order_id << " latency: " << latency_us << " us\\n";
''', '', optional=True)
# record completion at function exit (after rest/refund/free) via a scope guard placed right after lock
rep('''    t_lock[oid] = now_ns();''', '''    t_lock[oid] = now_ns();
    struct DoneGuard { int id; ~DoneGuard(){ t_done[id] = now_ns(); n_done++; } } done_guard{oid};''')
# trade counter
src = src.replace('Trade::pending_trades.push(t);', 'n_trades++; Trade::pending_trades.push(t);')

# 6. settle: fix self-trade double-lock (std::lock on same mutex twice spins forever)
rep('''        std::lock(trade->buy_user->wallet_lock, trade->sell_user->wallet_lock);
        lock_guard<mutex> lock1(trade->buy_user->wallet_lock, std::adopt_lock);
        lock_guard<mutex> lock2(trade->sell_user->wallet_lock, std::adopt_lock);
''', '''        std::unique_lock<mutex> lock1(trade->buy_user->wallet_lock, std::defer_lock);
        std::unique_lock<mutex> lock2(trade->sell_user->wallet_lock, std::defer_lock);
        if (trade->buy_user == trade->sell_user) lock1.lock(); else std::lock(lock1, lock2);
''', optional=True)
rep('''        {
            lock_guard<mutex> db_lock(db_mtx);
            db_save_queue.push(trade);
        }
        db_cv.notify_one();''', '''        if (!DB_ENABLED) { free_trade(trade); n_db_written++; continue; }
        {
            lock_guard<mutex> db_lock(db_mtx);
            db_save_queue.push(trade);
        }
        db_cv.notify_one();''')
rep('''            free_trade(trade);
        }
    } catch''', '''            free_trade(trade);
            n_db_written++;
        }
    } catch''')

# 7. replace main
i = src.index('int main() {')
src = src[:i] + open(sys.argv[2]).read()
open(sys.argv[3], 'w').write(src)
print("patched ok")
