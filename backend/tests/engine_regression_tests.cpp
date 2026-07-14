#include "EngineLoop.hpp"
#include "Exporter.hpp"
#include "GraphAlgos.hpp"
#include "HawkesPIN.hpp"
#include "LimitOrderBook.hpp"

using namespace mxray;

int main() {
    static_assert(MARKET_OPEN_NS == 34'200'000'000'000ULL);

    LimitOrderBook book(4);
    Tick order{};
    order.order_id = 1;
    order.price = 15'000;
    order.quantity = 100;
    order.side = Side::BUY;
    order.type = OrderType::ADD;
    book.add_order(order);

    const auto partial_cancel = book.cancel_order(order.order_id, 40);
    if (!partial_cancel.found || partial_cancel.order_removed || partial_cancel.affected_quantity != 40) return 1;
    if (book.get_best_bid() != order.price) return 2;
    if (book.get_best_bid_volume() != 60) return 3;
    const auto full_cancel = book.cancel_order(order.order_id, 60);
    if (!full_cancel.found || !full_cancel.order_removed || full_cancel.affected_quantity != 60) return 4;
    if (book.get_best_bid() != 0) return 5;

    Tick lower_bid = order;
    lower_bid.order_id = 2;
    lower_bid.price = 14'900;
    Tick best_ask = order;
    best_ask.order_id = 3;
    best_ask.price = 15'010;
    best_ask.side = Side::SELL;
    if (!book.add_order(lower_bid) || !book.add_order(best_ask)) return 6;
    if (book.top_bids(1).front().price != 14'900 || book.top_asks(1).front().price != 15'010) return 7;

    HawkesProcess hawkes;
    hawkes.record_event(34'200.0);
    const double at_event = hawkes.compute_intensity(34'200.0);
    const double one_second_later = hawkes.compute_intensity(34'201.0);
    if (at_event <= one_second_later) return 8;
    if (hawkes.probability_next_event_within(34'201.0, 1e9) <= 0.0) return 9;

    SpoofingDetector spoof(500'000'000ULL, 0.9, 100);
    spoof.record_add(42, 15'000, 100, 1'000'000'000ULL);
    spoof.record_execute(42, 40);
    const auto spoof_alert = spoof.check_cancel(42, 15'000, 60, 1'100'000'000ULL);
    if (spoof_alert.is_suspected_spoof || spoof_alert.quantity != 60) return 10;

    EngineSnapshot snapshot{};
    snapshot.pin_level = "LOW";
    snapshot.executions.push_back({1'567'157'400'000'000'000ULL, 15'010, 25, Side::BUY});
    const std::string json = snapshot_to_json(snapshot);
    if (json.find("\"executions\":[{") == std::string::npos) return 11;
    if (json.find("\"side\":\"BUY\"") == std::string::npos) return 12;

    EngineConfig engine_cfg{};
    engine_cfg.max_orders = 8;
    engine_cfg.snapshot_every_n = 3;
    EngineLoop engine(engine_cfg);
    std::string emitted;
    engine.on_snapshot([&emitted](const std::string& payload) { emitted = payload; });

    const uint64_t open = ITCH_DATE_OFFSET_NS + MARKET_OPEN_NS;
    Tick bid{};
    bid.timestamp_ns = open + 1;
    bid.order_id = 10;
    bid.price = 15'000;
    bid.quantity = 100;
    bid.side = Side::BUY;
    bid.type = OrderType::ADD;
    Tick ask = bid;
    ask.timestamp_ns = open + 2;
    ask.order_id = 11;
    ask.price = 15'010;
    ask.side = Side::SELL;
    Tick execution = bid;
    execution.timestamp_ns = open + 3;
    execution.quantity = 40;
    execution.type = OrderType::EXECUTE;

    engine.run_from_benchmark({bid, ask, execution});
    if (emitted.find("\"executions\":[{") == std::string::npos) return 13;
    if (emitted.find("\"quantity\":40") == std::string::npos) return 14;
    // A resting BUY was executed, so the aggressor is a SELL.
    if (emitted.find("\"side\":\"SELL\"") == std::string::npos) return 15;
    return 0;
}
