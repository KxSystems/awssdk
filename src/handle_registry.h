#pragma once

// Needs no AWS types, so it can be included from any translation unit, but it
// does reach k.h: include it after every AWS header, like the other project
// headers.
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

#include "k.h"


namespace awssdk {

// The q foreign handles for one kind of shared SDK object, such as an S3 client
// or a transfer manager.
//
// Each object is held in its own heap slot, and a handle points straight at
// that slot. The slot is freed only by the foreign's own destructor, so its
// address can never be recycled while a handle still names it -- which is why
// pointer identity is safe here. sweep() therefore *releases* each object
// without freeing its slot: freeing them would hand those addresses to the next
// object while stale handles still pointed at them, and dropping such a handle
// would then destroy the wrong object.
//
// The set is what makes an arbitrary 112h safe to reject rather than
// dereference.
//
// Guarded because the paths that touch the registry -- wrap, the foreign
// destructor q runs at garbage collection, and the shutDown sweep -- are serial
// only while q calls this module from one thread. Enabling setm(1) for the
// async callbacks lifts that guarantee.
template <class T>
class HandleRegistry {
public:
    using Retire = void (*)(std::shared_ptr<T>);

    // `name` is the error a lookup raises for anything that is not a live
    // handle of this kind. `retire` takes over each object the registry lets
    // go of, rather than it being destroyed here: an SDK thread may still hold
    // a reference, and must not be the one to release it last (retired.h).
    HandleRegistry(const char * name, Retire retire) : name_(name), retire_(retire) {}

    HandleRegistry(const HandleRegistry &) = delete;
    HandleRegistry & operator=(const HandleRegistry &) = delete;

    // Wraps `object` in a new 112h. `destructor` becomes the foreign's
    // destructor, which q calls when the last reference goes; it must call
    // release() with the handle it is given.
    K wrap(std::shared_ptr<T> object, K (*destructor)(K)) {
        // The unique_ptr keeps owning the slot until *both* the registry
        // insertion and the foreign have succeeded; release() comes last, and
        // never as an argument to a call that could itself throw.
        std::unique_ptr<Slot> slot(new Slot(std::move(object)));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            live_.insert(slot.get());
        }

        // knk is variadic, so the destructor is passed through untouched; q
        // reads kK(x)[0] as the destructor and kK(x)[1] as the payload.
        K foreign = knk(2, destructor, slot.get());
        if (foreign == nullptr) {
            std::lock_guard<std::mutex> lock(mutex_);
            live_.erase(slot.get());
            throw std::runtime_error("alloc");
        }
        foreign->t = 112;
        slot.release();
        return foreign;
    }

    // The object named by a handle.
    //
    // Returns the shared_ptr by value on purpose: the copy keeps the object
    // alive for the whole of the caller's operation even if a shutDown sweeps
    // the registry immediately after the lookup.
    //
    // Throws std::invalid_argument(name) for anything that is not a live
    // handle: a wrong type, a foreign from somewhere else, or one that a sweep
    // has emptied.
    std::shared_ptr<T> get(K handle) {
        // n is checked as well as the type: a 112h built elsewhere need not
        // have two slots, and slot 1 must exist before it can be read.
        if (handle == nullptr || handle->t != 112 || handle->n != 2) {
            throw std::invalid_argument(name_);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        Slot * found = slot(handle);
        if (live_.find(found) == live_.end()) { throw std::invalid_argument(name_); }
        // Emptied by a sweep: the handle is still ours, but its object is gone.
        if (!*found) { throw std::invalid_argument(name_); }
        return *found;
    }

    // Called from the foreign's destructor. This is the only place a slot is
    // freed.
    void release(K handle) {
        Slot * found = slot(handle);
        bool owned = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            owned = live_.erase(found) != 0;
        }
        // Outside the lock: retiring can run SDK teardown, which must not
        // happen with the registry held.
        if (owned) {
            if (*found) { retire_(std::move(*found)); }
            delete found;
        }
    }

    // Releases every live object, leaving their slots allocated so that
    // handles which outlive this keep pointing at memory that is still theirs.
    // SDK objects hold SDK resources, so this has to run before
    // Aws::ShutdownAPI.
    void sweep() {
        std::vector<Slot> taken;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            taken.reserve(live_.size());
            for (Slot * each : live_) {
                if (*each) { taken.push_back(*each); }
                each->reset();
            }
        }
        // Retired outside the lock, as in release().
        for (Slot & each : taken) { retire_(std::move(each)); }
    }

private:
    using Slot = std::shared_ptr<T>;

    // A real pointer round-tripping through the foreign's payload slot, so
    // this is a well-defined pointer-to-pointer cast -- static_cast cannot
    // express it, since K is an unrelated object pointer type.
    static Slot * slot(K handle) {
        return reinterpret_cast<Slot *>(kK(handle)[1]);
    }

    const char * name_;
    Retire retire_;
    std::set<Slot *> live_;
    std::mutex mutex_;
};

}
