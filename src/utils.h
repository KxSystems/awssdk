#pragma once

#include <string>

#include "k.h"

// Outcome of reading an option out of a q dictionary.
enum class DictLookup {
  Absent,    // the key is not in the dictionary
  Found,     // the value was read into the out parameter
  WrongType  // the key is present, but its value has an unusable type
};

// Index of `key` in a q dictionary with symbol keys, or -1 if `dict` is not
// such a dictionary or the key is absent.
J dict_find_idx(K dict, const char * key);

// Reads `key` from `dict` as a string, accepting char vectors ("INFO") and
// symbols (`INFO) from both mixed and typed value lists.
DictLookup dict_find_str(K dict, const char * key, std::string & out);

// Reads `key` from `dict` as a long, accepting boolean/short/int/long atoms
// from both mixed and typed value lists. q writes an integer literal as a long,
// so the narrower types are accepted for the caller's convenience.
DictLookup dict_find_long(K dict, const char * key, J & out);

// Reads `key` from `dict` as a flag. Any nonzero integral value is true, which
// is what lets a q caller write `([noSignRequest: 1])`.
DictLookup dict_find_bool(K dict, const char * key, bool & out);

// The first key of `dict` that is not one of the `n` names in `allowed`, or
// nullptr if every key is allowed. Guards against a mis-spelled option being
// silently ignored, which for something like `region` would quietly send
// requests somewhere else.
const char * dict_unknown_key(K dict, const char * const * allowed, size_t n);
