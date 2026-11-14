// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  tables.cpp · LTable core implementation    │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include "clx_runtime.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace clx {

//------------------ next_pow2: rounds n up to the next power of two (hash sizing)
static CLX_INLINE_COLD size_t next_pow2(size_t n) {
    if (n < 8)
        return 8;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

//--------- Slow Path (hash table lookup and metamethods)
LValue table_get_slow(LState *L, const LValue &obj, const LValue &key) {
    LTable *mt = nullptr;
    LValue direct;

    if (obj.type == ValueType::Table) {
        LTable *t = static_cast<LTable *>(obj.as_pointer());
        if (key.type == ValueType::Int64) {
            int64_t idx = key.val.payload.i64;
            if (static_cast<uint64_t>(idx - 1) < t->array_size) {
                direct = LValue(t->array[idx - 1], t->array_types[idx - 1]);
            }
        } else if (key.type == ValueType::Double) {
            double d = key.val.payload.f64;
            int64_t idx = static_cast<int64_t>(d);
            if (d == static_cast<double>(idx) && static_cast<uint64_t>(idx - 1) < t->array_size) {
                direct = LValue(t->array[idx - 1], t->array_types[idx - 1]);
            }
        }
        if (direct.type == ValueType::Nil) {
            LTableExt *ex = t->ext;
            if (ex && ex->ic) {
                uint32_t ic_idx
                    = static_cast<uint32_t>(key.val.payload.u64 ^ (key.val.payload.u64 >> 17)
                          ^ (key.val.payload.u64 >> 33) ^ (key.val.payload.u64 >> 5) ^ (key.val.payload.u64 >> 11))
                    % LTABLE_IC_SIZE;
                LTableInlineCache &_ic = ex->ic[ic_idx];
                if (_ic.key_payload == key.val.payload.u64 && _ic.table_ver == ex->hash_version
                    && _ic.entry_idx < ex->hash_size) {
                    HashEntry &_e = ex->entries[_ic.entry_idx];
                    if (_e.ktype != ValueType::Nil)
                        return LValue(_e.val, _e.vtype);
                }
            }
            direct = t->get_value(L, key);
        }
        if (direct.type != ValueType::Nil)
            return direct;
        mt = tbl_metatable(t);
    } else if (obj.type == ValueType::UserData) {
        LUserdata *ud = static_cast<LUserdata *>(obj.as_pointer());
        mt = ud->metatable;
    } else if (obj.type == ValueType::String) {
        mt = L->string_metatable;
    } else {
        //------------------ numbers, booleans and nil can carry a type metatable (debug.setmetatable)
        mt = L->type_metatables[type_meta_index(obj.type)];
        if (!mt)
            throw_index_error(L, obj);
    }

    //------------------ stock follows a chain of __index tables at most MAXTAGLOOP times and reports
    //------------------ a loop instead of recursing without bound
    LValue cur = obj;
    for (int loop = 0; loop < 2000; ++loop) {
        if (!mt)
            return LValue();
        LValue index = mt->gettable(L->str_index);
        if (index.type == ValueType::Nil)
            return LValue();
        if (index.type == ValueType::Function) {
            LValue args[2] = { cur, key };
            static constexpr CallSite index_site = { "__index", 0, false, false };
            MultiValue mv = call_function_rooted(L, index, args, 2, &index_site);
            return mv.count > 0 ? mv[0] : LValue();
        }
        if (index.type != ValueType::Table)
            return LValue();
        LTable *next = static_cast<LTable *>(index.as_pointer());
        LValue direct2 = next->get_value(L, key);
        if (direct2.type != ValueType::Nil)
            return direct2;
        cur = index;
        mt = tbl_metatable(next);
    }
    throw_runtime_error(file_line_prefix(L) + "'__index' chain too long; possible loop");
}

//------------------ LTable::LTable — table constructor
LTable::LTable()
    : array(nullptr)
    , array_types(nullptr)
    , array_size(0)
    , array_cap(0)
    , array_lim(0)
    , ext(nullptr) {
    type = static_cast<uint8_t>(Table);
    marked = 0;
    next = nullptr;
}

//------------------ LTable::~LTable — table destructor
LTable::~LTable() {
    if (ext) {
        if (ext->ic)
            delete[] ext->ic;
        if (!(flags & LFLAG_ARENA))
            table_free_hash(ext->entries, ext->hash_bitmap);
        delete ext;
        ext = nullptr;
    }
    if (!(flags & LFLAG_ARENA))
        table_free_array(array, array_types, small_array);}

//------------------ LTable::resize_hash — allocate/rehash to new_size (power of 2)
void LTable::resize_hash(size_t new_size) {
    new_size = next_pow2(new_size);

    const size_t hb_before = table_heap_bytes(this);
    LTableExt *ex = tbl_ensure_ext(this);

    HashEntry *new_entries = nullptr;
    uint64_t *new_bitmap = nullptr;
    table_alloc_hash(new_entries, new_bitmap, new_size);

    uint32_t mask = static_cast<uint32_t>(new_size - 1);
    if (ex->entries) {
        const uint64_t *old_bm = ex->hash_bitmap;
        for (size_t i = 0; i < ex->hash_size; ++i) {
            if (old_bm && !(old_bm[i >> 6] & (1ULL << (i & 63))))
                continue;
            if (ex->entries[i].ktype == Nil || ex->entries[i].vtype == Nil)
                continue;
            uint64_t h = lvalue_hash(LValue(ex->entries[i].key, ex->entries[i].ktype)) & mask;
            while (new_bitmap[h >> 6] & (1ULL << (h & 63)))
                h = (h + 1) & mask;
            new_entries[h].key = ex->entries[i].key;
            new_entries[h].ktype = ex->entries[i].ktype;
            new_entries[h].val = ex->entries[i].val;
            new_entries[h].vtype = ex->entries[i].vtype;
            new_bitmap[h / 64] |= (1ULL << (h % 64));
        }
        if (!(flags & LFLAG_ARENA))
            table_free_hash(ex->entries, ex->hash_bitmap);
    }

    flags &= ~LFLAG_ARENA;
    ex->entries = new_entries;
    ex->hash_size = new_size;
    ex->hash_tombs = 0;
    ex->hash_bitmap = new_bitmap;
    //------------------ Sole structural invalidator of the inline cache: rehashing relocates entries.
    //------------------ Plain inserts/deletes never move entries, and key-payload + ktype checks reject
    //------------------ stale slots, so they intentionally leave the version alone (hot reads survive
    //------------------ interleaved cold writes).
    ex->hash_version++;
    if (clx_current_L)
        clx_current_L->account_delta(hb_before, table_heap_bytes(this));
}

//------------------ LTable::presize_hash - pre-allocate hash storage at 1/2 load for `expected` entries.
void LTable::presize_hash(size_t expected) {
    size_t need = next_pow2(expected * 2);
    if (!ext || !ext->entries || ext->hash_size < need)
        resize_hash(need);
}

//------------------ LTable::gettable — get value by key
LValue LTable::gettable(const LValue &key) {
    if (key.type == Int64) {
        int64_t idx = key.as_integer();
        if (static_cast<uint64_t>(idx - 1) < array_size)
            return LValue(array[idx - 1], array_types[idx - 1]);
    } else if (key.type == Double) {
        double d = key.as_number();
        int64_t idx = static_cast<int64_t>(d);
        if (d == static_cast<double>(idx) && static_cast<uint64_t>(idx - 1) < array_size)
            return LValue(array[idx - 1], array_types[idx - 1]);
    }
    LTableExt *ex = ext;
    if (!ex || ex->hash_size == 0)
        return LValue();

    if (!ex->ic) {
        const size_t hb_ic = table_heap_bytes(this);
        ex->ic = new LTableInlineCache[LTABLE_IC_SIZE]();
        if (clx_current_L)
            clx_current_L->account_delta(hb_ic, table_heap_bytes(this));
    }

    uint32_t ic_idx = static_cast<uint32_t>(key.val.payload.u64 ^ (key.val.payload.u64 >> 17)
                          ^ (key.val.payload.u64 >> 33) ^ (key.val.payload.u64 >> 5) ^ (key.val.payload.u64 >> 11))
        % LTABLE_IC_SIZE;
    auto &_ic = ex->ic[ic_idx];
    if (_ic.key_payload == key.val.payload.u64 && _ic.table_ver == ex->hash_version && _ic.entry_idx < ex->hash_size) {
        HashEntry &_e = ex->entries[_ic.entry_idx];
        if (_e.ktype != Nil)
            return LValue(_e.val, _e.vtype);
    }

    uint32_t mask = static_cast<uint32_t>(ex->hash_size - 1);
    uint64_t h = lvalue_hash(key) & mask;
    for (;;) {
        HashEntry &e = ex->entries[h];
        if (e.ktype == Nil) {
            if (e.key.payload.u64 == HASH_EMPTY)
                return LValue();
        } else if (lvalue_eq_fast(LValue(e.key, e.ktype), key)) {
            _ic.key_payload = key.val.payload.u64;
            _ic.entry_idx = static_cast<uint32_t>(h);
            _ic.table_ver = ex->hash_version;
            return LValue(e.val, e.vtype);
        }
        h = (h + 1) & mask;
    }
}

//------------------ store_error_prefix: the caller's file:line prefix for a rejected table key, empty when
//------------------ no position is active
static std::string store_error_prefix() {
    return clx_current_L ? file_line_prefix(clx_current_L) : std::string();
}

//------------------ LTable::settable — set value by key
void LTable::settable(const LValue &key, const LValue &val) {
    //------------------ stock luaV_finishset rejects a NaN key; without this it silently created an
    //------------------ unreachable entry (NaN never equals itself, so the lookup could never find it)
    if (key.type == Double && std::isnan(key.val.payload.f64))
        throw_runtime_error(store_error_prefix() + "table index is NaN");
    //------------------ a nil key is refused the same way stock refuses it
    if (key.type == Nil)
        throw_runtime_error(store_error_prefix() + "table index is nil");
    if (clx_current_L) {
        gc_barrier_table(clx_current_L, this, val);
        if (key.is_gc_obj())
            gc_barrier_table(clx_current_L, this, key);
    }

    if (key.type == Int64) {
        int64_t idx = key.as_integer();
        if (static_cast<uint64_t>(idx - 1) < array_cap) {
            if (static_cast<uint64_t>(idx - 1) > array_size)
                table_fill_gap(this, array_size, static_cast<size_t>(idx) - 1);
            array[idx - 1] = val.val;
            array_types[idx - 1] = val.type;
            if (static_cast<size_t>(idx) > array_size)
                array_size = static_cast<size_t>(idx);
            table_hint_on_array_write(this, static_cast<size_t>(idx) - 1, val.type);
            return;
        }
        if (idx == static_cast<int64_t>(array_size + 1)) {
            size_t new_cap = (array_cap == 0) ? 8 : array_cap * 2;
            const size_t hb_grow = table_heap_bytes(this);
            TValue *new_arr = nullptr;
            ValueType *new_types = nullptr;
            table_alloc_array(new_arr, new_types, new_cap);
            if (array_cap) {
                std::memcpy(new_arr, array, array_cap * sizeof(TValue));
                std::memcpy(new_types, array_types, array_cap * sizeof(ValueType));
            }
            if (!(flags & LFLAG_ARENA))
                table_free_array(array, array_types, small_array);
            flags &= ~LFLAG_ARENA;
            array = new_arr;
            array_types = new_types;
            array[array_size] = val.val;
            array_types[array_size] = val.type;
            array_size++;
            table_hint_on_array_write(this, array_size - 1, val.type);
            array_cap = new_cap;
            if (clx_current_L)
                clx_current_L->account_delta(hb_grow, table_heap_bytes(this));
            if (ext && ext->hash_size > 0) {
                for (size_t i = 0; i < ext->hash_size; ++i) {
                    if (ext->entries[i].ktype == Nil || ext->entries[i].vtype == Nil)
                        continue;
                    LValue kv(ext->entries[i].key, ext->entries[i].ktype);
                    int64_t hidx = -1;
                    if (kv.type == Int64)
                        hidx = kv.as_integer();
                    else if (kv.type == Double) {
                        double d = kv.as_number();
                        int64_t t = static_cast<int64_t>(d);
                        if (d == static_cast<double>(t))
                            hidx = t;
                    }
                    if (hidx <= 0 || static_cast<uint64_t>(hidx - 1) >= new_cap)
                        continue;
                    if (hidx > idx) {
                        for (size_t j = array_size; j + 1 < static_cast<size_t>(hidx); ++j) {
                            array[j] = TValue();
                            array_types[j] = Nil;
                        }
                        array[hidx - 1] = ext->entries[i].val;
                        array_types[hidx - 1] = ext->entries[i].vtype;
                        if (static_cast<size_t>(hidx) > array_size)
                            array_size = static_cast<size_t>(hidx);
                    }
                    ext->entries[i].key.payload.u64 = HASH_TOMBSTONE;
                    ext->entries[i].ktype = Nil;
                    ext->entries[i].val = TValue();
                    ext->entries[i].vtype = Nil;
                    ext->hash_count--;
                    ext->hash_tombs++;
                    if (ext->hash_bitmap)
                        ext->hash_bitmap[i / 64] &= ~(1ULL << (i % 64));
                }
            }
            return;
        }
    } else if (key.type == Double) {
        double d = key.as_number();
        int64_t idx = static_cast<int64_t>(d);
        if (d == static_cast<double>(idx)) {
        if (static_cast<uint64_t>(idx - 1) < array_cap) {
            if (static_cast<uint64_t>(idx - 1) > array_size)
                table_fill_gap(this, array_size, static_cast<size_t>(idx) - 1);
            array[idx - 1] = val.val;
            array_types[idx - 1] = val.type;
            if (static_cast<size_t>(idx) > array_size)
                array_size = static_cast<size_t>(idx);
            table_hint_on_array_write(this, static_cast<size_t>(idx) - 1, val.type);
            return;
        }
        if (idx == static_cast<int64_t>(array_size + 1)) {
                size_t new_cap = (array_cap == 0) ? 8 : array_cap * 2;
                const size_t hb_grow = table_heap_bytes(this);
                TValue *new_arr = nullptr;
                ValueType *new_types = nullptr;
                table_alloc_array(new_arr, new_types, new_cap);
                if (array_cap) {
                    std::memcpy(new_arr, array, array_cap * sizeof(TValue));
                    std::memcpy(new_types, array_types, array_cap * sizeof(ValueType));
                }
                if (!(flags & LFLAG_ARENA))
                    table_free_array(array, array_types, small_array);
                flags &= ~LFLAG_ARENA;
                array = new_arr;
                array_types = new_types;
                array[array_size] = val.val;
                array_types[array_size] = val.type;
                array_size++;
                table_hint_on_array_write(this, array_size - 1, val.type);
                array_cap = new_cap;
                if (clx_current_L)
                    clx_current_L->account_delta(hb_grow, table_heap_bytes(this));
                if (ext && ext->hash_size > 0) {
                    for (size_t i = 0; i < ext->hash_size; ++i) {
                        if (ext->entries[i].ktype == Nil || ext->entries[i].vtype == Nil)
                            continue;
                        LValue kv(ext->entries[i].key, ext->entries[i].ktype);
                        int64_t hidx = -1;
                        if (kv.type == Int64)
                            hidx = kv.as_integer();
                        else if (kv.type == Double) {
                            double dd = kv.as_number();
                            int64_t t = static_cast<int64_t>(dd);
                            if (dd == static_cast<double>(t))
                                hidx = t;
                        }
                        if (hidx <= 0 || static_cast<uint64_t>(hidx - 1) >= new_cap)
                            continue;
                        if (hidx > idx) {
                            for (size_t j = array_size; j + 1 < static_cast<size_t>(hidx); ++j) {
                                array[j] = TValue();
                                array_types[j] = Nil;
                            }
                            array[hidx - 1] = ext->entries[i].val;
                            array_types[hidx - 1] = ext->entries[i].vtype;
                            if (static_cast<size_t>(hidx) > array_size)
                                array_size = static_cast<size_t>(hidx);
                        }
                        ext->entries[i].key.payload.u64 = HASH_TOMBSTONE;
                        ext->entries[i].ktype = Nil;
                        ext->entries[i].val = TValue();
                        ext->entries[i].vtype = Nil;
                        ext->hash_count--;
                        ext->hash_tombs++;
                        if (ext->hash_bitmap)
                            ext->hash_bitmap[i / 64] &= ~(1ULL << (i % 64));
                    }
                }
                return;
            }
        }
    }

    if (val.type == Nil) {
        LTableExt *ex = ext;
        if (!ex || ex->hash_size == 0)
            return;
        uint32_t mask = static_cast<uint32_t>(ex->hash_size - 1);
        uint64_t h = lvalue_hash(key) & mask;
        for (;;) {
            HashEntry &e = ex->entries[h];
            if (e.ktype == Nil) {
                if (e.key.payload.u64 == HASH_EMPTY)
                    return;
            } else if (lvalue_eq_fast(LValue(e.key, e.ktype), key)) {
                if (e.vtype == Nil)
                    return;
                e.val = TValue();
                e.vtype = Nil;
                ex->hash_count--;
                ex->hash_tombs++;
                return;
            }
            h = (h + 1) & mask;
        }
    }

    LTableExt *ex = ext;
    if (!ex || ex->hash_size == 0) {
        resize_hash(8);
        ex = ext;
    } else if ((ex->hash_count + ex->hash_tombs + 1) * 4 >= ex->hash_size * 3) {
        resize_hash(ex->hash_count >= ex->hash_size / 2 ? ex->hash_size * 2 : ex->hash_size);
        ex = ext;
    }

    if (ex->ic) {
        uint32_t ic_idx = static_cast<uint32_t>(key.val.payload.u64 ^ (key.val.payload.u64 >> 17)
                              ^ (key.val.payload.u64 >> 33) ^ (key.val.payload.u64 >> 5) ^ (key.val.payload.u64 >> 11))
            % LTABLE_IC_SIZE;
        LTableInlineCache &_ic = ex->ic[ic_idx];
        if (_ic.key_payload == key.val.payload.u64 && _ic.table_ver == ex->hash_version
            && _ic.entry_idx < ex->hash_size) {
            HashEntry &_e = ex->entries[_ic.entry_idx];
            if (_e.ktype != Nil && _e.vtype != Nil) {
                _e.val = val.val;
                _e.vtype = val.type;
                return;
            }
        }
    }

    uint32_t mask = static_cast<uint32_t>(ex->hash_size - 1);
    uint64_t h = lvalue_hash(key) & mask;
    int32_t tomb = -1;
    for (;;) {
        HashEntry &e = ex->entries[h];
        if (e.ktype == Nil) {
            HashEntry &slot = (tomb != -1) ? ex->entries[tomb] : e;
            slot.key = key.val;
            slot.ktype = key.type;
            slot.val = val.val;
            slot.vtype = val.type;
            if (tomb != -1)
                ex->hash_tombs--;
            ex->hash_count++;
            if (ex->hash_bitmap) {
                size_t bit_idx = (tomb != -1) ? static_cast<size_t>(tomb) : h;
                ex->hash_bitmap[bit_idx / 64] |= (1ULL << (bit_idx % 64));
            }
            return;
        }
        if (e.key.payload.u64 == HASH_TOMBSTONE && e.ktype == Nil) {
            if (tomb == -1)
                tomb = static_cast<int32_t>(h);
        } else if (lvalue_eq_fast(LValue(e.key, e.ktype), key)) {
            if (e.vtype == Nil) {
                ex->hash_count++;
                ex->hash_tombs--;
                if (ex->hash_bitmap)
                    ex->hash_bitmap[h / 64] |= (1ULL << (h % 64));
            }
            e.val = val.val;
            e.vtype = val.type;
            return;
        }
        h = (h + 1) & mask;
    }
}

//------------------ LTable::op_direct - fused single-walk t[k]=t[k]<op>amount with int fidelity; misses run the full generic path.
void LTable::op_direct(LState *L, const LValue &obj, const LValue &key, double amount, int op_kind, bool nil_ok) {
    auto nil_case = [amount, op_kind]() -> double {
        switch (op_kind) {
        case 0:
            return amount;
        case 1:
            return -amount;
        case 2:
            return 0.0;
        default:
            return 0.0 / amount;
        }
    };
    LValue amt = table_op_amount(amount);
    auto generic = [&]() { table_set(L, obj, key, table_op_apply(L, table_get(L, obj, key), amt, op_kind)); };

    if (key.is_gc_obj())
        gc_barrier_table(L, this, key);

    int64_t idx = -1;
    if (key.type == ValueType::Int64) {
        idx = key.as_integer();
    } else if (key.type == ValueType::Double) {
        double d = key.as_number();
        int64_t t64 = static_cast<int64_t>(d);
        if (d == static_cast<double>(t64))
            idx = t64;
    }

    if (idx >= 1 && static_cast<uint64_t>(idx - 1) < array_size) {
        LValue cur(array[idx - 1], array_types[idx - 1]);
        LValue nv;
        if (cur.type == ValueType::Nil) {
            if (!nil_ok) {
                generic();
                return;
            }
            nv = LValue(nil_case());
        } else if (cur.type == ValueType::Double || cur.type == ValueType::Int64) {
            nv = table_op_apply(L, cur, amt, op_kind);
        } else {
            generic();
            return;
        }
        gc_barrier_table(L, this, nv);
        array[idx - 1] = nv.val;
        array_types[idx - 1] = nv.type;
        table_hint_on_array_write(this, static_cast<size_t>(idx) - 1, nv.type);
        return;
    }

    LTableExt *ex = ext;
    if (!ex || ex->hash_size == 0) {
        if (idx >= 1) {
            if (!nil_ok) {
                generic();
                return;
            }
            settable(key, LValue(nil_case()));
            return;
        }
        resize_hash(8);
        ex = ext;
    } else if ((ex->hash_count + ex->hash_tombs + 1) * 4 >= ex->hash_size * 3) {
        resize_hash(ex->hash_count >= ex->hash_size / 2 ? ex->hash_size * 2 : ex->hash_size);
        ex = ext;
    }

    uint32_t mask = static_cast<uint32_t>(ex->hash_size - 1);
    uint64_t h = lvalue_hash(key) & mask;
    int32_t tomb = -1;
    for (;;) {
        HashEntry &e = ex->entries[h];
        if (e.ktype == ValueType::Nil) {
            if (e.key.payload.u64 == HASH_EMPTY) {
                if (idx >= 1) {
                    if (!nil_ok) {
                        generic();
                        return;
                    }
                    settable(key, LValue(nil_case()));
                    return;
                }
                if (!nil_ok) {
                    generic();
                    return;
                }
                HashEntry &slot = (tomb != -1) ? ex->entries[tomb] : e;
                slot.key = key.val;
                slot.ktype = key.type;
                slot.val = LValue(nil_case()).val;
                slot.vtype = ValueType::Double;
                if (tomb != -1)
                    ex->hash_tombs--;
                ex->hash_count++;
                if (ex->hash_bitmap) {
                    size_t bit_idx = (tomb != -1) ? static_cast<size_t>(tomb) : h;
                    ex->hash_bitmap[bit_idx / 64] |= (1ULL << (bit_idx % 64));
                }
                return;
            }
            if (e.key.payload.u64 == HASH_TOMBSTONE && tomb == -1)
                tomb = static_cast<int32_t>(h);
            h = (h + 1) & mask;
            continue;
        }
        if (lvalue_eq_fast(LValue(e.key, e.ktype), key)) {
            LValue cur(e.val, e.vtype);
            if (cur.type == ValueType::Nil) {
                if (idx >= 1) {
                    if (!nil_ok) {
                        generic();
                        return;
                    }
                    settable(key, LValue(nil_case()));
                    return;
                }
                if (!nil_ok) {
                    generic();
                    return;
                }
                ex->hash_count++;
                ex->hash_tombs--;
                if (ex->hash_bitmap)
                    ex->hash_bitmap[h / 64] |= (1ULL << (h % 64));
                LValue nv(nil_case());
                gc_barrier_table(L, this, nv);
                e.val = nv.val;
                e.vtype = nv.type;
                return;
            }
            if (cur.type != ValueType::Double && cur.type != ValueType::Int64) {
                generic();
                return;
            }
            LValue nv = table_op_apply(L, cur, amt, op_kind);
            gc_barrier_table(L, this, nv);
            e.val = nv.val;
            e.vtype = nv.type;
            return;
        }
        h = (h + 1) & mask;
    }
}

//------------------ LTable::get_value — get with metamethod fallback
LValue LTable::get_value(LState *L, const LValue &key) {
    //------------------ the __index chain is walked iteratively; stock bounds it with MAXTAGLOOP and
    //------------------ reports a loop instead of overflowing the C stack (a.__index = a)
    LTable *cur = this;
    for (int loop = 0;; ++loop) {
        if (loop >= 2000)
            throw_runtime_error(file_line_prefix(L) + "'__index' chain too long; possible loop");
        LValue ptr = cur->gettable(key);
        if (ptr.type != Nil)
            return ptr;
        if (!cur->ext || !cur->ext->metatable)
            return LValue();
        LValue index_ptr = cur->ext->metatable->gettable(L->str_index);
        if (index_ptr.type == Table) {
            cur = static_cast<LTable *>(index_ptr.as_pointer());
            continue;
        }
        if (index_ptr.type == Function) {
            LValue args[2];
            args[0] = LValue(cur);
            args[1] = key;

            static constexpr CallSite index_ptr_site = { __FILE__, __LINE__, false, false };
            MultiValue ret = call_function_rooted(L, index_ptr, args, 2, &index_ptr_site);
            return ret.count > 0 ? ret[0] : LValue();
        }
        return LValue();
    }
}

//------------------ LTable::set_value — set with metamethod fallback
void LTable::set_value(LState *L, const LValue &key, const LValue &val) {
    //------------------ a chain of __newindex tables is followed at most MAXTAGLOOP times; stock
    //------------------ reports a loop instead of recursing without bound (a.__newindex = a)
    LTable *cur = this;
    for (int loop = 0;; ++loop) {
        if (loop >= 2000)
            throw_runtime_error(file_line_prefix(L) + "'__newindex' chain too long; possible loop");
        LValue ptr = cur->gettable(key);
        if (ptr.type != Nil) {
            cur->settable(key, val);
            return;
        }

        if (!cur->ext || !cur->ext->metatable)
            break;
        LValue newindex_ptr = cur->ext->metatable->gettable(L->str_newindex);
        if (newindex_ptr.type == Table) {
            cur = static_cast<LTable *>(newindex_ptr.as_pointer());
            continue;
        }
        if (newindex_ptr.type == Function) {
            LValue args[3];
            args[0] = LValue(cur);
            args[1] = key;
            args[2] = val;

            static constexpr CallSite newindex_ptr_site = { __FILE__, __LINE__, false, false };
            call_function_rooted(L, newindex_ptr, args, 3, &newindex_ptr_site);
            return;
        }
        break;
    }
    cur->settable(key, val);
}

//------------------ LTable::bind — bind constant value
void LTable::bind(const char *name, const LValue &val) {
    settable(LValue(name), val);
}

//------------------ LTable::bind — bind C function
void LTable::bind(LState *L, const char *name, CFunctionType func) {
    LCFunction *f = new LCFunction(func);
    f->next = L->allocated_objects;
    L->allocated_objects = f;
    L->gc_recent.push(f);
    LValue key = LValue(L->intern_string(name));
    LValue cfn = LValue(Function, f);
    gc_barrier_table(L, this, cfn);
    if (key.is_gc_obj())
        gc_barrier_table(L, this, key);
    settable(key, cfn);
}

//------------------ LTable::bind_all — bind multiple C functions
void LTable::bind_all(LState *L, std::initializer_list<LReg> funcs) {
    for (const auto &reg : funcs)
        bind(L, reg.name, reg.func);
}

}
