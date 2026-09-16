#include <cstring>

#include "utils.h"

J dict_find_idx(K dict, const char * key) {
    if (dict == nullptr || dict->t != XD) { return -1; }
    K keys = kK(dict)[0];
    // kS is only valid for a symbol vector; on any other type it would
    // reinterpret non-pointer data as char*.
    if (keys->t != KS) { return -1; }
    for (J i = 0; i < keys->n; ++i) {
        if (0 == std::strcmp(key, kS(keys)[i])) { return i; }
    }
    return -1;
}

DictLookup dict_find_str(K dict, const char * key, std::string & out) {
    const J i = dict_find_idx(dict, key);
    if (i < 0) { return DictLookup::Absent; }

    K vals = kK(dict)[1];
    if (vals->t == KS) {  // typed value list, e.g. `loglevel`region!`INFO`eu-west-1
        out.assign(kS(vals)[i]);
        return DictLookup::Found;
    }
    if (vals->t != 0) { return DictLookup::WrongType; }

    K val = kK(vals)[i];  // mixed value list, one element per key
    if (val->t == KC) {
        out.assign((S)kC(val), val->n);
        return DictLookup::Found;
    }
    if (val->t == -KS) {
        out.assign(val->s);
        return DictLookup::Found;
    }
    return DictLookup::WrongType;
}

DictLookup dict_find_long(K dict, const char * key, J & out) {
    const J i = dict_find_idx(dict, key);
    if (i < 0) { return DictLookup::Absent; }

    K vals = kK(dict)[1];
    switch (vals->t) {  // typed value list, e.g. `connectTimeout`maxConnections!1000 25
        case KB: out = kG(vals)[i]; return DictLookup::Found;
        case KH: out = kH(vals)[i]; return DictLookup::Found;
        case KI: out = kI(vals)[i]; return DictLookup::Found;
        case KJ: out = kJ(vals)[i]; return DictLookup::Found;
        case 0: break;  // mixed value list, handled below
        default: return DictLookup::WrongType;
    }

    K val = kK(vals)[i];  // mixed value list, one element per key
    switch (val->t) {
        case -KB: out = val->g; return DictLookup::Found;
        case -KH: out = val->h; return DictLookup::Found;
        case -KI: out = val->i; return DictLookup::Found;
        case -KJ: out = val->j; return DictLookup::Found;
        default: return DictLookup::WrongType;
    }
}

DictLookup dict_find_bool(K dict, const char * key, bool & out) {
    J value = 0;
    const DictLookup lookup = dict_find_long(dict, key, value);
    if (lookup == DictLookup::Found) { out = (value != 0); }
    return lookup;
}

const char * dict_unknown_key(K dict, const char * const * allowed, size_t n) {
    if (dict == nullptr || dict->t != XD) { return nullptr; }
    K keys = kK(dict)[0];
    if (keys->t != KS) { return nullptr; }

    for (J i = 0; i < keys->n; ++i) {
        bool ok = false;
        for (size_t j = 0; j < n && !ok; ++j) {
            ok = 0 == std::strcmp(allowed[j], kS(keys)[i]);
        }
        if (!ok) { return kS(keys)[i]; }
    }
    return nullptr;
}
