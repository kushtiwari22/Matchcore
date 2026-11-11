// ---------------- BENCH HARNESS main() ----------------
struct GenOrder { string ticker; int id, user; uint64_t vol; int type; uint64_t price; bool mkt; };

static void pct_report(const char* name, vector<uint64_t> v) {
    if (v.empty()) return;
    sort(v.begin(), v.end());
    auto p = [&](double q) { return v[min(v.size() - 1, (size_t)(q * v.size()))] / 1000.0; };
    double mean = 0; for (auto x : v) mean += x; mean /= v.size() * 1000.0;
    printf("  %-22s mean %9.2f | p50 %9.2f | p90 %9.2f | p99 %9.2f | p99.9 %9.2f | max %9.2f  (us)\n",
           name, mean, p(.5), p(.9), p(.99), p(.999), v.back() / 1000.0);
}

int main(int argc, char** argv) {
    // usage: bench <mode> [N] [tickers] [match_threads] [rate|mkt_pct] [db 0/1]
    // modes: file <path> <threads> <db> | burst | rate | closed
    string mode = argc > 1 ? argv[1] : "burst";
    for (int i = 0; i < POOL_SIZE; i++) { available_orders.push(i); available_trades.push(i); }

    if (getenv("REAL_PRICE")) STUB_PRICE = false;
    vector<GenOrder> orders;
    vector<User*> active_users;
    int N = 0, tickers = 1; unsigned threads = thread::hardware_concurrency(); double rate = 0; int mkt_pct = 0;

    if (mode == "file") {
        string path = argv[2]; threads = atoi(argv[3]); DB_ENABLED = atoi(argv[4]);
        ifstream in(path); string line;
        while (getline(in, line)) {
            if (line.empty()) continue;
            istringstream iss(line); string cmd; iss >> cmd;
            if (cmd == "USER") {
                int uid; int64_t amt; iss >> uid >> amt;
                User* u = new User(uid, amt); u->available_shares["AAPL"] = 500; u->add_demat(); active_users.push_back(u);
            } else if (cmd == "ORDER") {
                GenOrder g; int t, m; iss >> g.ticker >> g.id >> g.user >> g.vol >> t >> g.price >> m;
                g.type = t; g.mkt = m; orders.push_back(g);
            }
        }
    } else {
        N = atoi(argv[2]); tickers = atoi(argv[3]); threads = atoi(argv[4]);
        if (mode == "rate") rate = atof(argv[5]); else mkt_pct = argc > 5 ? atoi(argv[5]) : 0;
        DB_ENABLED = argc > 6 ? atoi(argv[6]) : 0;
        const int USERS = 100;
        for (int u = 0; u < USERS; u++) {
            User* usr = new User(u, (int64_t)1e14);
            for (int t = 0; t < tickers; t++) usr->available_shares["TK" + to_string(t)] = (uint64_t)1e12;
            usr->add_demat(); active_users.push_back(usr);
        }
        mt19937_64 rng(42);
        for (int i = 0; i < N; i++) {
            GenOrder g; g.id = i; g.user = rng() % USERS; g.ticker = "TK" + to_string(rng() % tickers);
            g.type = (rng() & 1) ? 1 : 2; g.vol = 1 + rng() % 100;
            g.mkt = (int)(rng() % 100) < mkt_pct;
            // buys centred slightly below mid, sells slightly above: ~half cross, book stays populated
            g.price = g.type == 1 ? 10000 - 20 + rng() % 26 : 10000 - 5 + rng() % 26;
            orders.push_back(g);
        }
    }
    N = orders.size();
    for (auto& [tk, b] : trading_exchange.orderbook) (void)b;
    // pre-create books so the map isn't mutated while matching threads read it
    for (auto& g : orders) trading_exchange.orderbook[g.ticker];

    string db = "dbname=exchange_db user=postgres password=secret hostaddr=127.0.0.1 port=5432";
    thread validation_thread(&Exchange::process_initial_orders, &trading_exchange);
    vector<thread> mts; for (unsigned i = 0; i < threads; i++) mts.emplace_back(&Exchange::match, &trading_exchange);
    thread settlement_thread(Trade::settle);
    thread db_thread; if (DB_ENABLED) db_thread = thread(save_trades_to_db, db);
    this_thread::sleep_for(chrono::milliseconds(300)); // let threads park, DB connect

    uint64_t start = now_ns();
    for (int i = 0; i < N; i++) {
        auto& g = orders[i];
        if (mode == "rate") {
            uint64_t intended = start + (uint64_t)(i * 1e9 / rate);
            while (now_ns() < intended) {}
            t_submit[g.id] = intended;              // latency measured from intended send time (no coordinated omission)
        } else {
            t_submit[g.id] = now_ns();
        }
        Order* o = get_new_order(g.ticker, g.id, g.user, g.vol, g.type, g.price, g.mkt);
        if (!o) { fprintf(stderr, "ORDER POOL EXHAUSTED at %d\n", i); return 2; }
        o->execute();
        if (mode == "closed") while (n_done + n_rejected < i + 1) {}
    }
    uint64_t submit_end = now_ns();
    uint64_t deadline = now_ns() + 120e9;
    while (n_done + n_rejected < N && now_ns() < deadline) this_thread::yield();
    uint64_t all_done = 0, first = UINT64_MAX;
    vector<uint64_t> e2e, q_valid, q_book, core;
    vector<pair<uint64_t,int>> by_valid;
    for (auto& g : orders) {
        int id = g.id; if (!t_done[id]) continue;
        all_done = max(all_done, t_done[id]); first = min(first, t_submit[id]);
        e2e.push_back(t_done[id] - t_submit[id]);
        q_valid.push_back(t_valid[id] - t_submit[id]);
        q_book.push_back(t_lock[id] - t_valid[id]);
        core.push_back(t_done[id] - t_lock[id]);
        by_valid.push_back({t_valid[id], id});
    }
    uint64_t db_done = 0;
    if (DB_ENABLED) {
        while (n_db_written < n_trades && now_ns() < deadline) this_thread::sleep_for(chrono::microseconds(200));
        db_done = now_ns();
    }
    // FIFO check: validation is single-threaded, so validation order == arrival order.
    sort(by_valid.begin(), by_valid.end());
    // An order is "overtaken" if some order on the SAME ticker that arrived later got the book lock first.
    unordered_map<string, vector<int>> per_tk;
    unordered_map<int, string> tk_of; for (auto& g : orders) tk_of[g.id] = g.ticker;
    for (auto& [t, id] : by_valid) per_tk[tk_of[id]].push_back(book_seq[id]);
    long displaced = 0, maxdisp = 0; size_t nfifo = 0;
    for (auto& [tk, seqs] : per_tk) {
        nfifo += seqs.size(); int sufmin = INT32_MAX; long late = 0;
        for (long i = (long)seqs.size() - 1; i >= 0; i--) {
            if (sufmin < seqs[i]) displaced++;
            sufmin = min(sufmin, seqs[i]);
        }
        // max displacement: how many earlier-arrived orders a single order jumped ahead of
        vector<int> sorted = seqs; sort(sorted.begin(), sorted.end());
        unordered_map<int,long> rk; for (size_t i = 0; i < sorted.size(); i++) rk[sorted[i]] = i;
        for (size_t i = 0; i < seqs.size(); i++) maxdisp = max(maxdisp, (long)i - rk[seqs[i]]);
        (void)late;
    }
    printf("mode=%s orders=%d accepted=%zu rejected=%d trades=%ld tickers=%d match_threads=%u db=%d",
           mode.c_str(), N, e2e.size(), n_rejected.load(), n_trades.load(), tickers, threads, DB_ENABLED);
    if (mode == "rate") printf(" target_rate=%.0f/s", rate);
    printf("\n");
    double span = (all_done - first) / 1e9;
    printf("  throughput: %.0f orders/s  (accepted orders / (last completion - first submit) = %.4f s); submit loop %.4f s\n",
           e2e.size() / span, span, (submit_end - start) / 1e9);
    pct_report("end-to-end", e2e);
    pct_report("submit->validated", q_valid);
    pct_report("validated->book lock", q_book);
    pct_report("matching (in lock)", core);
    if (!STUB_PRICE) printf("  price fetch (popen curl) total %.3f ms\n", price_fetch_ns / 1e6);
    printf("  FIFO: %ld of %zu orders (%.2f%%) were overtaken by a later-arriving order on the same ticker; worst order jumped %ld places\n",
           displaced, nfifo, 100.0 * displaced / max<size_t>(1, nfifo), maxdisp);
    if (DB_ENABLED) printf("  DB: %ld trades persisted, last write %.3f s after first submit => %.0f trades/s\n",
                           n_db_written.load(), (db_done - first) / 1e9, n_db_written / ((db_done - first) / 1e9));
    fflush(stdout);
    validate_running = false; validate_cv.notify_all(); validation_thread.join();
    match_running = false; match_order_cv.notify_all(); for (auto& t : mts) t.join();
    settle_running = false; trade_settle_cv.notify_all(); settlement_thread.join();
    db_running = false; db_cv.notify_all(); if (DB_ENABLED) db_thread.join();
    return 0;
}
