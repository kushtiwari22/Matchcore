#include <iostream>
#include <vector>
#include <unordered_map>
#include <map>
#include <queue>
#include <algorithm>
#include <cstdint>
#include <utility>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <pqxx/pqxx>

using namespace std;

atomic<bool> is_running(true);

mutex initial_queue_mutex;
mutex pending_orders_mutex;
mutex trade_q_mtx;
mutex db_mtx;
mutex orderbook_map_mtx; 

condition_variable validate_cv;
condition_variable match_order_cv;
condition_variable trade_settle_cv;
condition_variable db_cv;

uint64_t currTime() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

uint64_t getcurrent(const string& ticker) {
    string api_key = "WRITE YOUR API KEY HERE"; 
    string cmd = "curl -s \"https://www.alphavantage.co/query?function=GLOBAL_QUOTE&symbol=" + ticker + "&apikey=" + api_key + "\"";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return 100; 

    string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
    pclose(pipe);

    size_t pos = result.find("\"05. price\"");
    if (pos == string::npos) return 100; 

    pos = result.find(':', pos);
    pos = result.find('"', pos + 1);
    size_t end = result.find('"', pos + 1);
    if (pos == string::npos || end == string::npos) return 100;

    double price = stod(result.substr(pos + 1, end - pos - 1));
    return static_cast<uint64_t>(price);
}

class User;
class Order;
class Trade;
class User_list;
class Order_list;
class OrderBook;
class Exchange;

const int POOL_SIZE = 10000;
vector<Order> order_pool(POOL_SIZE);
vector<Trade> trade_pool(POOL_SIZE);

queue<int> available_orders;
queue<int> available_trades;

mutex order_pool_mtx;
mutex trade_pool_mtx;

class Trade {
public:
    int pool_index;
    string ticker;
    User* buy_user;
    User* sell_user;
    uint64_t final_price;
    uint64_t volume;

    static queue<Trade*> pending_trades;
    static queue<Trade*> db_save_queue;

    Trade() = default;
    static void settle();
    static void display(const Trade* trade);
};

class Order {
public:
    int pool_index;
    uint64_t timestamp;
    string ticker;
    int order_id;
    int user_id;
    uint64_t order_size;
    int order_type;
    uint64_t order_price;
    uint64_t remaining;
    bool is_market;
    bool is_cancelled;
    uint64_t locked_price;                        
    chrono::steady_clock::time_point submit_time;  

    Order() = default;
    void execute();
    void display() const;
};

Order* get_new_order(string ticker, int id, int user, uint64_t volume, int type, uint64_t price, bool is_market) {
    lock_guard<mutex> lock(order_pool_mtx);
    if (available_orders.empty()) return nullptr;

    int idx = available_orders.front();
    available_orders.pop();

    Order* o = &order_pool[idx];
    o->pool_index = idx;
    o->ticker = ticker;
    o->order_id = id;
    o->user_id = user;
    o->order_size = volume;
    o->remaining = volume;
    o->order_type = type;
    o->order_price = price;
    o->is_market = is_market;
    o->is_cancelled = false;
    o->timestamp = currTime();
    o->submit_time = chrono::steady_clock::now(); 

    return o;
}

void free_order(Order* o) {
    lock_guard<mutex> lock(order_pool_mtx);
    available_orders.push(o->pool_index);
}

Trade* get_new_trade(string ticker, User* buy_user, User* sell_user, uint64_t final_price, uint64_t volume) {
    lock_guard<mutex> lock(trade_pool_mtx);
    if (available_trades.empty()) return nullptr;

    int idx = available_trades.front();
    available_trades.pop();

    Trade* t = &trade_pool[idx];
    t->pool_index = idx;
    t->ticker = ticker;
    t->buy_user = buy_user;
    t->sell_user = sell_user;
    t->final_price = final_price;
    t->volume = volume;

    return t;
}

void free_trade(Trade* t) {
    lock_guard<mutex> lock(trade_pool_mtx);
    available_trades.push(t->pool_index);
}

class User {
public:
    mutex wallet_lock;
    int id;
    unordered_map<string, uint64_t> portfolio;
    unordered_map<string, uint64_t> available_shares;
    int64_t total_amount;
    int64_t current_amount;
    unordered_map<Order*, uint64_t> under_execution;

    User(int id, int64_t amount);
    void add_demat();
    void display() const;
};

class OrderBook {
public:
    map<uint64_t, queue<Order*>, greater<uint64_t>> buy_orders;
    map<uint64_t, queue<Order*>> sell_orders;
    mutex book_mutex; 

    void match_order(User* user, Order* order);
    void insert_order(Order* order);
};

class User_list {
public:
    unordered_map<int, User*> id_to_user;
    void adduser(User* user);
    void delete_user(User* user);
    User* getuser(int id) const;
};

class Order_list {
public:
    mutex order_list_mutex;
    unordered_map<int, Order*> id_to_order;
    void validate_and_lock(User* user, Order* order);
    void delete_order(int order_id);
};

class Exchange {
public:
    unordered_map<string, OrderBook> orderbook;
    User_list userbook;
    Order_list order_list;

    queue<Order*> initial_order_queue;
    queue<pair<User*, Order*>> pending_orders_queue;

    void process_initial_orders();
    void match();
};

Exchange trading_exchange;
queue<Trade*> Trade::pending_trades;
queue<Trade*> Trade::db_save_queue;

void Exchange::process_initial_orders() {
    while (true) {
        Order* order = nullptr;
        {
            unique_lock<mutex> lock(initial_queue_mutex);
            validate_cv.wait(lock, []{ return !trading_exchange.initial_order_queue.empty() || !is_running; });
            if (trading_exchange.initial_order_queue.empty() && !is_running) break;
            order = initial_order_queue.front();
            initial_order_queue.pop();
        }
        if (order->is_cancelled){
            free_order(order);
            continue;
        }
        User* user = userbook.getuser(order->user_id);
        if (user != nullptr) {
            order_list.validate_and_lock(user, order);
        }
    }
}

void Exchange::match() {
    while (true) {
        pair<User*, Order*> pending_task;
        {
            unique_lock<mutex> lock(pending_orders_mutex);
            match_order_cv.wait(lock, []{ return !trading_exchange.pending_orders_queue.empty() || !is_running; });

            if (trading_exchange.pending_orders_queue.empty() && !is_running) break;

            pending_task = pending_orders_queue.front();
            pending_orders_queue.pop();
        }

        User* user = pending_task.first;
        Order* order = pending_task.second;

        if (order->is_cancelled){
            free_order(order);
            continue;
        }

        string tick = order->ticker;

        OrderBook* book;
        {
            lock_guard<mutex> lock(orderbook_map_mtx);
            book = &orderbook[tick]; 
        }
        book->match_order(user, order);
    }
}

void Trade::settle() {
    while (true) {
        Trade* trade = nullptr;

        {
            unique_lock<mutex> lock(trade_q_mtx);
            trade_settle_cv.wait(lock, []{ return !pending_trades.empty() || !is_running; });

            if (pending_trades.empty() && !is_running) break;

            trade = pending_trades.front();
            pending_trades.pop();
        }

        std::lock(trade->buy_user->wallet_lock, trade->sell_user->wallet_lock);
        lock_guard<mutex> lock1(trade->buy_user->wallet_lock, std::adopt_lock);
        lock_guard<mutex> lock2(trade->sell_user->wallet_lock, std::adopt_lock);

        string tick = trade->ticker;

        trade->buy_user->portfolio[tick] += trade->volume;
        trade->buy_user->available_shares[tick] += trade->volume;
        trade->buy_user->total_amount -= trade->final_price * trade->volume;

        trade->sell_user->portfolio[tick] -= trade->volume;
        trade->sell_user->total_amount += trade->final_price * trade->volume;
        trade->sell_user->current_amount += trade->final_price * trade->volume;

        {
            lock_guard<mutex> db_lock(db_mtx);
            db_save_queue.push(trade);
        }
        db_cv.notify_one();
    }
}

void save_trades_to_db(const string& connection_string) {
    try {
        pqxx::connection conn(connection_string);
        pqxx::work setup_txn(conn);
        setup_txn.exec(
            "CREATE TABLE IF NOT EXISTS trade_history ("
            "id SERIAL PRIMARY KEY, "
            "ticker VARCHAR(10), "
            "buyer_id INT, "
            "seller_id INT, "
            "price BIGINT, "
            "volume BIGINT, "
            "timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP)"
        );
        setup_txn.commit();

        while (true) {
            Trade* trade = nullptr;

            {
                unique_lock<mutex> lock(db_mtx);
                db_cv.wait(lock, []{ return !Trade::db_save_queue.empty() || !is_running; });

                if (Trade::db_save_queue.empty() && !is_running) break;

                trade = Trade::db_save_queue.front();
                Trade::db_save_queue.pop();
            }

            pqxx::work txn(conn);
            txn.exec_params(
                "INSERT INTO trade_history (ticker, buyer_id, seller_id, price, volume) "
                "VALUES ($1, $2, $3, $4, $5)",
                trade->ticker, trade->buy_user->id, trade->sell_user->id, trade->final_price, trade->volume
            );
            txn.commit();

            free_trade(trade);
        }
    } catch (const exception& e) {
        cerr << e.what() << "\n";
    }
}

User::User(int id, int64_t amount) {
    this->id = id;
    this->total_amount = amount;
    this->current_amount = amount;
}

void User::add_demat() { trading_exchange.userbook.adduser(this); }

void User::display() const {
    cout << "user id :" << id << endl;
    cout << "total amount :" << total_amount << endl;
    cout << "current amount :" << current_amount << endl;

    cout << "Portfolio Holdings:\n";
    if (portfolio.empty()) {
        cout << "  No stocks currently held.\n";
    } else {
        for (const auto& [tick, qty] : portfolio) {
            uint64_t avail = 0;
            if (available_shares.find(tick) != available_shares.end()) {
                avail = available_shares.at(tick);
            }
            cout << "  " << tick << " | Total: " << qty << " | Available: " << avail << "\n";
        }
    }
    cout << "current orders under execution : " << under_execution.size() << endl;
}

void User_list::adduser(User* user) { id_to_user[user->id] = user; }
void User_list::delete_user(User* user) { id_to_user.erase(user->id); }
User* User_list::getuser(int id) const {
    auto it = id_to_user.find(id);
    if (it != id_to_user.end()) return it->second;
    return nullptr;
}

void Trade::display(const Trade* trade) {
    cout << "Ticker: " << trade->ticker << " | Buyer: " << trade->buy_user->id
         << " | Seller: " << trade->sell_user->id
         << " | Price: " << trade->final_price
         << " | Volume: " << trade->volume << '\n';
}

void Order::execute() {
    {
        lock_guard<mutex> lock(initial_queue_mutex);
        trading_exchange.initial_order_queue.push(this);
    }
    validate_cv.notify_one();
}

void Order::display() const {
    cout << "timestamp " << timestamp << " | Ticker: " << ticker
         << " | order type : " << order_type
         << " | Price: " << order_price
         << " | Volume: " << order_size << '\n';
}

void Order_list::validate_and_lock(User* user, Order* order) {
    {
        lock_guard<mutex> lock(user->wallet_lock);
        if (order->order_size <= 0) {
            free_order(order);
            return;
        }
        string tick = order->ticker;
        if (order->order_type == 1) {
            uint64_t required_price = order->is_market ? (getcurrent(tick) * 11) / 10 : order->order_price;
            order->locked_price = required_price; 
            if (user->current_amount < static_cast<int64_t>(order->order_size * required_price)) {
                free_order(order);
                return;
            }
            user->current_amount -= order->order_size * required_price;
        } else {
            if (user->available_shares[tick] < order->order_size) {
                free_order(order);
                return;
            }
            user->available_shares[tick] -= order->order_size;
        }
        user->under_execution[order] = order->remaining;
    }

    {
        lock_guard<mutex> ol_lock(order_list_mutex);
        id_to_order[order->order_id] = order;
    }
    {
        lock_guard<mutex> lock(pending_orders_mutex);
        trading_exchange.pending_orders_queue.push({user, order});
    }
    match_order_cv.notify_one();
}

void OrderBook::match_order(User* user, Order* order) {
    lock_guard<mutex> book_lock(book_mutex); 
    string tick = order->ticker;

    if (order->order_type == 1) {
        while (order->remaining > 0 && !sell_orders.empty()) {
            auto sell_it = sell_orders.begin();
            Order* sell = sell_it->second.front();
            User* sell_user = trading_exchange.userbook.id_to_user[sell->user_id]; 

            bool sell_cancelled;
            {
                lock_guard<mutex> owner_lock(sell_user->wallet_lock);
                sell_cancelled = sell->is_cancelled; 
            }
            if (sell_cancelled) {
                sell_it->second.pop();
                if (sell_it->second.empty()) sell_orders.erase(sell_it);
                free_order(sell);
                continue;
            }

            if (!order->is_market && order->order_price < sell->order_price) break;

            uint64_t match_price = sell->order_price;
            uint64_t qty = min(order->remaining, sell->remaining);

            {
                std::unique_lock<std::mutex> lock1(user->wallet_lock, std::defer_lock);
                std::unique_lock<std::mutex> lock2(sell_user->wallet_lock, std::defer_lock);
                if (user == sell_user) {
                    lock1.lock(); 
                } else {
                    std::lock(lock1, lock2);
                }

                if (!order->is_market) {
                    user->current_amount += qty * (order->order_price - match_price);
                } else {
                    uint64_t locked_price = order->locked_price; 
                    user->current_amount += qty * (locked_price - match_price);
                }

                order->remaining -= qty;
                sell->remaining -= qty;

                user->under_execution[order] = order->remaining;
                sell_user->under_execution[sell] = sell->remaining;
            }

            {
                std::lock_guard<std::mutex> lock(trade_q_mtx);
                Trade::pending_trades.push(get_new_trade(tick, user, sell_user, match_price, qty));
            }
            trade_settle_cv.notify_one();

            if (sell->remaining == 0) {
                sell_it->second.pop();
                if (sell_it->second.empty()) sell_orders.erase(sell_it);

                {
                    lock_guard<mutex> ol_lock(trading_exchange.order_list.order_list_mutex);
                    trading_exchange.order_list.id_to_order.erase(sell->order_id);
                }

                lock_guard<mutex> slock(sell_user->wallet_lock);
                sell_user->under_execution.erase(sell);
                free_order(sell);
            }
        }
    } else {
        while (order->remaining > 0 && !buy_orders.empty()) {
            auto buy_it = buy_orders.begin();
            Order* buy = buy_it->second.front();
            User* buy_user = trading_exchange.userbook.id_to_user[buy->user_id]; 

            bool buy_cancelled;
            {
                lock_guard<mutex> owner_lock(buy_user->wallet_lock);
                buy_cancelled = buy->is_cancelled; 
            }
            if (buy_cancelled) {
                buy_it->second.pop();
                if (buy_it->second.empty()) buy_orders.erase(buy_it);
                free_order(buy);
                continue;
            }

            if (!order->is_market && order->order_price > buy->order_price) break;

            uint64_t match_price = buy->order_price;
            uint64_t qty = min(order->remaining, buy->remaining);

            {
                std::unique_lock<std::mutex> lock1(user->wallet_lock, std::defer_lock);
                std::unique_lock<std::mutex> lock2(buy_user->wallet_lock, std::defer_lock);
                if (user == buy_user) {
                    lock1.lock(); 
                } else {
                    std::lock(lock1, lock2);
                }

                order->remaining -= qty;
                buy->remaining -= qty;

                user->under_execution[order] = order->remaining;
                buy_user->under_execution[buy] = buy->remaining;
            }

            {
                std::lock_guard<std::mutex> t_lock(trade_q_mtx);
                Trade::pending_trades.push(get_new_trade(tick, buy_user, user, match_price, qty));
            }
            trade_settle_cv.notify_one();

            if (buy->remaining == 0) {
                buy_it->second.pop();
                if (buy_it->second.empty()) buy_orders.erase(buy_it);

                {
                    lock_guard<mutex> ol_lock(trading_exchange.order_list.order_list_mutex);
                    trading_exchange.order_list.id_to_order.erase(buy->order_id);
                }

                lock_guard<mutex> block(buy_user->wallet_lock);
                buy_user->under_execution.erase(buy);
                free_order(buy);
            }
        }
    }

    auto latency_us = chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now() - order->submit_time).count();
    cout << "Order " << order->order_id << " latency: " << latency_us << " us\n";

    if (order->remaining > 0) {
        if (order->is_market) {
            lock_guard<mutex> lock(user->wallet_lock);
            if (order->order_type == 1) {
                uint64_t locked_price = order->locked_price; 
                user->current_amount += order->remaining * locked_price;
            } else {
                user->available_shares[tick] += order->remaining;
            }

            {
                lock_guard<mutex> ol_lock(trading_exchange.order_list.order_list_mutex);
                trading_exchange.order_list.id_to_order.erase(order->order_id);
            }
            user->under_execution.erase(order);
            free_order(order);
        } else {
            insert_order(order);
        }
    } else {
        {
            lock_guard<mutex> ol_lock(trading_exchange.order_list.order_list_mutex);
            trading_exchange.order_list.id_to_order.erase(order->order_id);
        }
        lock_guard<mutex> lock(user->wallet_lock);
        user->under_execution.erase(order);
        free_order(order);
    }
}

void OrderBook::insert_order(Order* order) {
    if (order->order_type == 1) {
        buy_orders[order->order_price].push(order);
    } else {
        sell_orders[order->order_price].push(order);
    }
}

void Order_list::delete_order(int order_id) {
    Order* order = nullptr;

    {
        lock_guard<mutex> ol_lock(order_list_mutex);
        auto it = id_to_order.find(order_id);
        if (it == id_to_order.end()) return;
        order = it->second;
        id_to_order.erase(order_id);
    }

    User* user = trading_exchange.userbook.id_to_user[order->user_id];
    string tick = order->ticker;

    lock_guard<mutex> lock(user->wallet_lock);
    order->is_cancelled = true;

    if (order->order_type == 1) {
        user->current_amount += order->remaining * order->order_price;
    } else {
        user->available_shares[tick] += order->remaining;
    }
    user->under_execution.erase(order);
}



int main() {
    for (int i = 0; i < POOL_SIZE; i++) {
        available_orders.push(i);
        available_trades.push(i);
    }

    string db_credentials = "dbname=exchange_db user=postgres password=secret hostaddr=127.0.0.1 port=5432";

    thread validation_thread(&Exchange::process_initial_orders, &trading_exchange);

    unsigned int num_matching_threads = thread::hardware_concurrency();
    if (num_matching_threads == 0) num_matching_threads = 2;
    vector<thread> matching_threads;
    for (unsigned int i = 0; i < num_matching_threads; i++) {
        matching_threads.emplace_back(&Exchange::match, &trading_exchange);
    }

    thread settlement_thread(Trade::settle);
    thread db_thread(save_trades_to_db, db_credentials);

    vector<User*> active_users;
    
    ifstream infile("instructions.txt");
    string line;
    while (getline(infile, line)) {
        if (line.empty()) continue;

        istringstream iss(line);
        string command;
        iss >> command;

        if (command == "USER") {
            int user_id;
            int64_t initial_amount;
            iss >> user_id >> initial_amount;
            User* new_user = new User(user_id, initial_amount);
            new_user->available_shares["AAPL"] = 500; 
            new_user->add_demat();
            active_users.push_back(new_user);
        } 
        else if (command == "ORDER") {
            string ticker;
            int order_id, user_id, volume, type, is_market_int;
            uint64_t price;
            iss >> ticker >> order_id >> user_id >> volume >> type >> price >> is_market_int;
            bool is_market = (is_market_int != 0);
            Order* new_order = get_new_order(ticker, order_id, user_id, volume, type, price, is_market);
            if (new_order) {
                new_order->execute();
            }
        }
    }
    infile.close();

    this_thread::sleep_for(chrono::seconds(2));

    is_running = false;
    validate_cv.notify_all();
    match_order_cv.notify_all();
    trade_settle_cv.notify_all();
    db_cv.notify_all();

    validation_thread.join();
    for (auto& t : matching_threads) t.join(); 
    settlement_thread.join();
    db_thread.join();

    for (User* u : active_users) {
        delete u;
    }
    
    return 0;
}