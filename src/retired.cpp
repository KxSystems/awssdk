#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "event_channel.h"
#include "retired.h"


namespace awssdk {

namespace {

struct Retired {
    std::shared_ptr<void> object;
    std::function<bool()> idle;
    std::function<void()> after;
};

// Never destroyed, so that nothing still here at exit is destroyed after the
// SDK has shut down.
std::vector<Retired> & retired() {
    static auto * entries = new std::vector<Retired>();
    return *entries;
}

// While anything is retired, a helper thread asks for a prune now and then, so
// that cleanup does not depend on another call into the module: after a last
// failed download, its temporary would otherwise stay until shutDown. The
// thread only sleeps and posts to the main thread, where the pruning happens;
// it touches no SDK or q object. Each start bumps the generation, which ends
// any thread from an earlier one.
std::atomic<unsigned> ticker_generation(0);
std::atomic<bool> ticking(false);
// One prune queued at a time, however long q's main thread is busy.
std::atomic<bool> prune_queued(false);

void stop_ticker() {
    ticking = false;
}

void start_ticker() {
    if (ticking) { return; }
    try {
        attach_event_channel();
    } catch (const std::exception &) {
        // Without the channel, cleanup falls back to the next module call.
        std::fprintf(stderr, "awssdk: no event channel; cleanup waits for the next call\n");
        return;
    }
    ticking = true;
    const unsigned generation = ++ticker_generation;
    std::thread([generation]() {
        std::chrono::milliseconds delay(10);
        while (ticking && ticker_generation == generation) {
            std::this_thread::sleep_for(delay);
            delay = std::min(delay * 2, std::chrono::milliseconds(1000));
            bool expected = false;
            if (prune_queued.compare_exchange_strong(expected, true)) {
                post_to_main([]() {
                    prune_queued = false;
                    prune_retired();
                });
            }
        }
    }).detach();
}

// use_count is exact when it reads 1: no other owner exists, so none can make
// another copy. A larger value only means "not yet".
bool is_idle(const Retired & entry) {
    return entry.object.use_count() == 1 && (!entry.idle || entry.idle());
}

bool is_retired(const void * object) {
    for (const Retired & entry : retired()) {
        if (entry.object.get() == object) { return true; }
    }
    return false;
}

}  // namespace

void prune_retired() {
    std::vector<Retired> & entries = retired();
    // Repeated, because destroying one object can make another idle: a
    // transfer manager holds a reference to its client.
    for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < entries.size();) {
            if (!is_idle(entries[i])) {
                ++i;
                continue;
            }
            Retired done = std::move(entries[i]);
            entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
            done.object.reset();
            if (done.after) { done.after(); }
            progress = true;
        }
    }
    if (entries.empty()) { stop_ticker(); } else { start_ticker(); }
}

void retire(std::shared_ptr<void> object, std::function<bool()> idle,
        std::function<void()> after, std::chrono::milliseconds patience) {
    if (!object) {
        if (after) { after(); }
        return;
    }
    const void * raw = object.get();
    Retired entry;
    entry.object = std::move(object);
    entry.idle = std::move(idle);
    entry.after = std::move(after);
    retired().push_back(std::move(entry));

    prune_retired();
    const auto deadline = std::chrono::steady_clock::now() + patience;
    while (is_retired(raw) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        prune_retired();
    }
}

bool drain_retired(std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    prune_retired();
    while (!retired().empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        prune_retired();
    }
    if (retired().empty()) { return true; }

    std::fprintf(stderr, "awssdk: %zu SDK object(s) still in use by SDK threads; "
        "the SDK is left running\n", retired().size());
    return false;
}

}
