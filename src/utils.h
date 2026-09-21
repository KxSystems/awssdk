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
//
// The type gate is what lets callers pass a generic null (101h) as "no
// options": it is not a dictionary, so every key reads as absent and no
// dereference of its contents is attempted. dict_unknown_key gates the same
// way, and every dict_find_* routes through here first.
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

// Throws std::invalid_argument(key) if the lookup found the key but its value
// was unusable, reporting the option's own name as the error text -- the
// convention initialize set for a bad `loglevel`. An absent key is *not* an
// error: the caller's default stands.
void dict_require_type(DictLookup lookup, const char * key);

// Reads a q string argument -- a char vector ("bucket") or a symbol (`bucket).
// False if `arg` is neither. The dict_find_* functions only read dictionaries;
// this is for plain arguments.
bool k_to_str(K arg, std::string & out);

// krr() for a message built at run time. krr does not copy, so the text has to
// outlive the return; ss() would do that but interns permanently, and messages
// carrying bucket or object names are unbounded in variety. The returned error
// borrows thread-local storage that stays valid until this thread raises its
// next error, which is after q has consumed this one.
K krr_text(const std::string & message);
