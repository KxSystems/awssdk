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
//
// No lock: only q's main thread ever reaches this list. Every caller is a module
// entry point, a handle's destructor, shutDown, or work posted to the main
// thread; the helper thread and the SDK's threads never touch it. That holds
// as long as the module is only called from q's main thread -- see "Threads" in
// docs/reference.md.
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

// Set once anything has had to be kept outside the list (keep_forever), where
// nothing can tell when the SDK is done with it. From then on the SDK can
// never be shown idle, so it is never shut down.
std::atomic<bool> untracked(false);

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
    // Thread creation can fail (std::system_error) when the process is out of
    // threads or memory. This can be reached from a destructor, where an
    // escaping exception would terminate q, so the failure is absorbed and
    // cleanup falls back to the next call into the module.
    try {
        std::thread worker([generation]() {
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
        });
        try {
            worker.detach();
        } catch (...) {
            // A joinable std::thread must not be destroyed, so it is stopped and
            // joined instead: it leaves its loop within one sleep, at most a
            // second.
            ticking = false;
            worker.join();
            throw;
        }
    } catch (const std::exception &) {
        ticking = false;
        std::fprintf(stderr, "awssdk: could not start the cleanup thread; cleanup waits for the next call\n");
    }
}

// use_count is exact when it reads 1: no other owner exists, so none can make
// another copy. A larger value only means "not yet".
bool is_idle(const Retired & entry) {
    return entry.object.use_count() == 1 && (!entry.idle || entry.idle());
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
    if (entries.empty()) { ticking = false; } else { start_ticker(); }
}

void keep_forever(std::shared_ptr<void> object) noexcept {
    // First, before the allocation below, which may fail too.
    if (!untracked.exchange(true)) {
        std::fprintf(stderr, "awssdk: lost track of an SDK object; the SDK will not be shut down\n");
    }
    try {
        new std::shared_ptr<void>(std::move(object));
    } catch (...) {
        // Nothing left to try: the object is released with `object`.
    }
}

void retire(std::shared_ptr<void> object, std::function<bool()> idle,
        std::function<void()> after) noexcept {
    try {
        if (!object) {
            if (after) { after(); }
            return;
        }
        Retired entry;
        entry.object = object;
        entry.idle = std::move(idle);
        entry.after = std::move(after);
        retired().push_back(std::move(entry));
        object.reset();

        // Most often the SDK has let go already, and it goes at once.
        prune_retired();
    } catch (...) {
        // Out of memory, most likely. If it was never stored, `object` still
        // holds it: kept rather than released, since releasing it here could
        // destroy it while an SDK thread is still using it.
        if (object) { keep_forever(std::move(object)); }
    }
}

bool drain_retired(std::chrono::milliseconds limit) {
    if (untracked) { return false; }
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
