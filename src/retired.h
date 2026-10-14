#pragma once

#include <chrono>
#include <functional>
#include <memory>


namespace awssdk {

// SDK objects the module has finished with, kept until no SDK thread can still
// be using them, then destroyed on q's main thread.
//
// The SDK's request threads hold their own references to the objects they
// work for: a transfer manager's tasks keep the manager alive, and the manager
// keeps its client. Dropping the module's reference therefore does not
// destroy the object; the last reference can go on an SDK thread, at any time
// -- including during Aws::ShutdownAPI, which the objects must not outlive.
// Handing the reference over here instead means it is always the module that
// lets go last, on the main thread, and that shutDown can wait for it.
//
// Main thread only, like everything that calls it.

// Keeps `object` until it is idle: until this holds its only reference, and
// `idle`, if given, also returns true. It is then destroyed, and `after` runs:
// at once if it is idle already, otherwise at a later prune_retired.
//
// Never throws, as it is called from destructors: if it cannot keep track of
// `object`, the object is kept for the rest of the process instead.
void retire(std::shared_ptr<void> object, std::function<bool()> idle = nullptr,
    std::function<void()> after = nullptr) noexcept;

// Holds `object` for the rest of the process. For when it can be neither
// destroyed safely nor retired. Nothing can then tell when the SDK is done with
// it, so from then on drain_retired always returns false: the SDK is never
// shut down.
void keep_forever(std::shared_ptr<void> object) noexcept;

// Destroys whatever has become idle. Called by the module's entry points; while
// anything remains, it is also scheduled on q's main thread by itself, so that
// nothing waits on another call into the module.
void prune_retired();

// Before Aws::ShutdownAPI: waits up to `limit` for everything retired to be
// destroyed. False if an SDK thread still holds anything, which means SDK work
// is still running and ShutdownAPI must not be called yet; what remains stays
// retired, and is destroyed once it is idle.
bool drain_retired(std::chrono::milliseconds limit);

}
