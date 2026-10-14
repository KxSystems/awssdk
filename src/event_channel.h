#pragma once

// Needs no AWS types, so it can be included from any translation unit, but it
// does reach k.h: include it after every AWS header, like the other project
// headers.
#include <functional>

#include "k.h"


namespace awssdk {

// Runs work on q's main thread, for code running on other threads -- the SDK's
// transfer threads -- which must never touch q objects themselves.

// Queues `fn` to run on q's main thread the next time q's event loop is idle.
// Safe to call from any thread, and never blocks on q.
void post_to_main(std::function<void()> fn);

// Registers the channel with q's event loop. Main thread only, as sd1 is, and
// a no-op once done; the channel then lasts for the whole process. Throws
// std::runtime_error("eventChannel") if it cannot be created.
void attach_event_channel();

}
