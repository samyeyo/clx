// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  table.cpp · Table Library                  │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <vector>

namespace clx {

//------------------ get_array_len: returns the length (#) of the array part of a table
static size_t get_array_len(LState *L, LTable *t) {
    if (LTable *mt = tbl_metatable(t)) {
        LValue mm = mt->gettable(L->str_len);
        if (mm.type != Nil) {
            LValue res = len(L, LValue(t));
            //------------------ a negative __len is an empty range for every caller (stock's
            //------------------ aux_getn hands the raw integer to loops that then do not run),
            //------------------ so it must not be converted to a huge size_t
            auto clamp_len = [](int64_t v) { return v < 0 ? size_t(0) : static_cast<size_t>(v); };
            if (res.type == Int64)
                return clamp_len(res.as_integer());
            if (res.type == Double && res.val.payload.f64 == static_cast<double>(res.as_integer()))
                return clamp_len(res.as_integer());
            //------------------ stock's aux_getn (luaL_len) insists on an integer length
            throw_runtime_error(file_line_prefix(L) + "object length is not an integer");
        }
    }
    size_t n = table_array_prefix_len(t);
    if (n < t->array_size)
        return n;
    int64_t lo = static_cast<int64_t>(t->array_size);
    int64_t hi = static_cast<int64_t>(t->array_size) + 1;
    while (true) {
        LValue p = t->gettable(LValue(hi));
        if (p.type == Nil)
            break;
        lo = hi;
        hi = (hi < 8) ? 8 : hi * 2;
    }
    while (hi - lo > 1) {
        int64_t mid = lo + (hi - lo) / 2;
        LValue p = t->gettable(LValue(mid));
        if (p.type != Nil)
            lo = mid;
        else
            hi = mid;
    }
    return static_cast<size_t>(lo);
}

//------------------ get_elem: gets array element by 1-based index (hot helper)
static LValue get_elem(LTable *t, size_t idx) {
    if (static_cast<uint64_t>(idx - 1) < t->array_size)
        return LValue(t->array[idx - 1], t->array_types[idx - 1]);
    LValue p = t->gettable(LValue(static_cast<int64_t>(idx)));
    return p.type != Nil ? p : LValue();
}

//------------------ set_elem: sets array element by 1-based index (hot helper)
static void set_elem(LTable *t, size_t idx, const LValue &val) {
    t->settable(LValue(static_cast<int64_t>(idx)), val);
}

//------------------ table_concat: concatenates array elements into a string
MultiValue table_concat(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        throw_runtime_error(bad_arg_msg(L, 1, "table.concat", "table expected, got no value"));
    LTable *list = check_table(L, args[0]);
    const char *sep = "";
    size_t sep_len = 0;
    if (count >= 2 && args[1].type != Nil) {
        sep = check_string(L, args[1]);
        //------------------ a separator may contain NUL bytes: only a real string carries its length,
        //------------------ a converted number was materialised NUL-terminated by check_string
        sep_len = (args[1].type == String) ? args[1].string_len() : std::strlen(sep);
    }
    //------------------ the range stays signed: stock walks it as lua_Integer, so an out-of-range
    //------------------ index reports the element at that index instead of becoming a huge size_t
    int64_t i = 1;
    if (count >= 3 && args[2].type != Nil)
        i = opt_integer(L, args[2], 1);
    int64_t j = static_cast<int64_t>(get_array_len(L, list));
    if (count >= 4 && args[3].type != Nil)
        j = opt_integer(L, args[3], 0);

    if (i > j)
        return MultiValue(LValue(L->intern_string("", 0)));

    //------------------ stock reads every element with lua_geti, so a metatabled table resolves through
    //------------------ __index; the raw array fast paths below only apply without a metatable
    if (tbl_metatable(list)) {
        StringBuilder mmsb;
        int64_t k = i;
        for (; k < j; ++k) {
            if (k > i)
                mmsb.append(sep, sep_len);
            LValue v = list->get_value(L, LValue(k));
            if (v.type != String && v.type != Int64 && v.type != Double) {
                char cbuf[160];
                std::snprintf(cbuf, sizeof(cbuf), "invalid value (%s) at index %lld in table for 'concat'",
                    stock_type_name(v), static_cast<long long>(k - i + 1));
                throw_runtime_error(file_line_prefix(L) + cbuf);
            }
            mmsb.append(L, v);
        }
        if (k > i)
            mmsb.append(sep, sep_len);
        LValue v = list->get_value(L, LValue(k));
        if (v.type != String && v.type != Int64 && v.type != Double) {
            char cbuf[160];
            std::snprintf(cbuf, sizeof(cbuf), "invalid value (%s) at index %lld in table for 'concat'",
                stock_type_name(v), static_cast<long long>(k - i + 1));
            throw_runtime_error(file_line_prefix(L) + cbuf);
        }
        mmsb.append(L, v);
        return MultiValue(mmsb.to_lvalue(L));
    }

    {
        bool in_array_part = i >= 1 && static_cast<uint64_t>(j) <= list->array_size;
        if (in_array_part
            && !clx_validate_types_range(reinterpret_cast<const uint8_t *>(list->array_types),
                static_cast<size_t>(i - 1), static_cast<size_t>(j - i + 1), 2, 4)) {
            int64_t k = i;
            for (; k < j; ++k) {
                LValue v = get_elem(list, static_cast<size_t>(k));
                if (v.type != String && v.type != Int64 && v.type != Double) {
                    char cbuf[160];
                    std::snprintf(cbuf, sizeof(cbuf), "invalid value (%s) at index %lld in table for 'concat'",
                        stock_type_name(v), static_cast<long long>(k));
                    throw_runtime_error(file_line_prefix(L) + cbuf);
                }
            }
            LValue v = get_elem(list, static_cast<size_t>(k));
            if (v.type != String && v.type != Int64 && v.type != Double) {
                char cbuf[160];
                std::snprintf(cbuf, sizeof(cbuf), "invalid value (%s) at index %lld in table for 'concat'",
                    stock_type_name(v), static_cast<long long>(k));
                throw_runtime_error(file_line_prefix(L) + cbuf);
            }
        }
    }

    StringBuilder sb;
    auto append_field = [&](int64_t k) {
        LValue v = get_elem(list, static_cast<size_t>(k));
        if (v.type != String && v.type != Int64 && v.type != Double) {
            char cbuf[160];
            std::snprintf(cbuf, sizeof(cbuf), "invalid value (%s) at index %lld in table for 'concat'",
                stock_type_name(v), static_cast<long long>(k));
            throw_runtime_error(file_line_prefix(L) + cbuf);
        }
        sb.append(L, v);
    };
    int64_t k = i;
    for (; k < j; ++k) {
        if (k > i)
            sb.append(sep, sep_len);
        append_field(k);
    }
    if (k > i)
        sb.append(sep, sep_len);
    append_field(k);
    return MultiValue(sb.to_lvalue(L));
}

//------------------ table_insert: inserts element at a position in the array
MultiValue table_insert(LState *L, const LValue *args, size_t count) {
    if (count != 2 && count != 3)
        throw_runtime_error(file_line_prefix(L) + "wrong number of arguments to 'insert'");
    LTable *list = check_table(L, args[0]);
    size_t len = get_array_len(L, list);
    //------------------ a metatabled table is indexed through lua_geti/lua_seti: __newindex and __index
    //------------------ take part, and stock's position arithmetic wraps instead of saturating
    if (tbl_metatable(list)) {
        int64_t mlen = static_cast<int64_t>(len);
        int64_t e = static_cast<int64_t>(static_cast<uint64_t>(mlen) + 1u);
        int64_t pos;
        LValue val;
        if (count == 2) {
            pos = e;
            val = args[1];
        } else {
            pos = check_integer(L, args[1]);
            if (static_cast<uint64_t>(pos) - 1u >= static_cast<uint64_t>(e))
                throw_runtime_error(bad_arg_msg(L, 2, "table.insert", "position out of bounds"));
            val = args[2];
            for (int64_t k = e; k > pos; --k)
                list->set_value(L, LValue(k), list->get_value(L, LValue(k - 1)));
        }
        list->set_value(L, LValue(pos), val);
        return MultiValue();
    }
    size_t pos;
    LValue val;
    if (count == 2) {
        pos = len + 1;
        val = args[1];
    } else {
        int64_t p = check_integer(L, args[1]);
        if (p < 1)
            throw_runtime_error(bad_arg_msg(L, 2, "table.insert", "position out of bounds"));
        pos = static_cast<size_t>(p);
        val = args[2];
    }
    if (pos < 1 || pos > len + 1)
        throw_runtime_error(bad_arg_msg(L, 2, "table.insert", "position out of bounds"));
    if (pos <= len) {
        if (pos <= list->array_size && len <= list->array_size) {
            if (list->array_cap <= len)
                list->settable(LValue(static_cast<int64_t>(len + 1)), LValue());
            size_t move_n = len - pos + 1;
            std::memmove(&list->array[pos], &list->array[pos - 1], move_n * sizeof(TValue));
            std::memmove(&list->array_types[pos], &list->array_types[pos - 1], move_n * sizeof(ValueType));
            gc_barrier_table(L, list, val);
            list->array[pos - 1] = val.val;
            list->array_types[pos - 1] = val.type;
            table_hint_on_array_write(list, pos - 1, val.type);
            if (list->array_size <= len)
                list->array_size = len + 1;
        } else {
            for (size_t k = len; k >= pos; --k) {
                set_elem(list, k + 1, get_elem(list, k));
                if (k == 0)
                    break;
            }
            set_elem(list, pos, val);
        }
    } else {
        set_elem(list, pos, val);
    }
    return MultiValue();
}

//------------------ table_remove: removes element from array, shifts subsequent elements
MultiValue table_remove(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        throw_runtime_error(bad_arg_msg(L, 1, "table.remove", "table expected, got no value"));
    LTable *list = check_table(L, args[0]);
    size_t len = get_array_len(L, list);
    if (tbl_metatable(list)) {
        int64_t size = static_cast<int64_t>(len);
        int64_t mpos = (count >= 2 && args[1].type != Nil) ? check_integer(L, args[1]) : size;
        if (mpos != size && static_cast<uint64_t>(mpos) - 1u > static_cast<uint64_t>(size))
            clx::error(L, bad_arg_msg(L, 2, "table.remove", "position out of bounds").c_str());
        LValue res = list->get_value(L, LValue(mpos));
        for (; mpos < size; ++mpos)
            list->set_value(L, LValue(mpos), list->get_value(L, LValue(mpos + 1)));
        list->set_value(L, LValue(mpos), LValue());
        return MultiValue(res);
    }
    size_t pos = len;
    //------------------ stock validates a given position only when it differs from the length, so
    //------------------ removing from an empty table (position 0) is legal and removes key 0
    if (count >= 2 && args[1].type != Nil) {
        const int64_t given = check_integer(L, args[1]);
        if (given != static_cast<int64_t>(len)
            && (given < 1 || given > static_cast<int64_t>(len) + 1))
            clx::error(L, bad_arg_msg(L, 2, "table.remove", "position out of bounds").c_str());
        pos = static_cast<size_t>(given);
    }
    if (pos == 0) {
        LValue first = get_elem(list, 0);
        set_elem(list, 0, LValue());
        return MultiValue(first);
    }
    if (pos > len)
        return MultiValue(LValue());
    LValue res;
    if (pos <= list->array_size && len <= list->array_size) {
        res = LValue(list->array[pos - 1], list->array_types[pos - 1]);
        size_t move_n = len - pos;
        std::memmove(&list->array[pos - 1], &list->array[pos], move_n * sizeof(TValue));
        std::memmove(&list->array_types[pos - 1], &list->array_types[pos], move_n * sizeof(ValueType));
        list->array[len - 1] = TValue();
        list->array_types[len - 1] = Nil;
        //------------------ left-shift may pull an unknown slot under the hint; lowering is always sound
        if (pos - 1 < list->array_lim)
            list->array_lim = pos - 1;
    } else {
        res = get_elem(list, pos);
        for (size_t k = pos; k < len; ++k)
            set_elem(list, k, get_elem(list, k + 1));
        set_elem(list, len, LValue());
    }
    return MultiValue(res);
}

//------------------ table_sort: sorts array elements in-place
MultiValue table_sort(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        throw_runtime_error(bad_arg_msg(L, 1, "table.sort", "table expected, got no value"));
    LTable *list = check_table(L, args[0]);
    size_t len = get_array_len(L, list);
    if (len == 0)
        return MultiValue();
    //------------------ stock refuses the interval before sizing anything: a huge __len cannot be
    //------------------ turned into a vector here without running out of memory
    if (len > static_cast<size_t>(INT_MAX))
        throw_runtime_error(bad_arg_msg(L, 1, "table.sort", "array too big"));

    if ((count < 2 || args[1].type == Nil) && len <= list->array_size) {
        if (clx_validate_types_range(reinterpret_cast<const uint8_t *>(list->array_types), 0, len, 2, 2)) {
            std::sort(list->array, list->array + len,
                [](const TValue &a, const TValue &b) { return a.payload.i64 < b.payload.i64; });
            return MultiValue();
        }
        if (clx_validate_types_range(reinterpret_cast<const uint8_t *>(list->array_types), 0, len, 3, 3)) {
            std::sort(list->array, list->array + len,
                [](const TValue &a, const TValue &b) { return a.payload.f64 < b.payload.f64; });
            return MultiValue();
        }
    }

    //------------------ stock's sort reads and writes through lua_geti/lua_seti, so a metatabled table
    //------------------ takes its elements from __index and stores them through __newindex
    bool mm_sort = tbl_metatable(list) != nullptr;
    auto extract_elems = [&](LTable *t, size_t n) {
        std::vector<LValue> elems;
        elems.reserve(n);
        for (size_t k = 1; k <= n; ++k)
            elems.push_back(mm_sort ? t->get_value(L, LValue(static_cast<int64_t>(k))) : get_elem(t, k));
        return elems;
    };
    auto write_elems = [&](LTable *t, const std::vector<LValue> &v) {
        for (size_t k = 0; k < v.size(); ++k) {
            if (mm_sort)
                t->set_value(L, LValue(static_cast<int64_t>(k + 1)), v[k]);
            else
                set_elem(t, k + 1, v[k]);
        }
    };

    if (count >= 2 && args[1].type != Nil) {
        LValue comp_func = args[1];
        auto cmp = [L, comp_func](const LValue &a, const LValue &b) mutable {
            LValue call_args[2] = { a, b };
            static constexpr CallSite sort_site = { "table.sort", 0, false, false };
            MultiValue mr = call_function_rooted(L, comp_func, call_args, 2, &sort_site);
            return mr.count >= 1 && mr[0].as_bool();
        };
        auto elems = extract_elems(list, len);
        //------------------ the comparator runs under the C sort: a yield there has no continuation
        struct YieldBlock {
            LState *L;
            YieldBlock(LState *l)
                : L(l) { ++L->c_yield_blocked; }
            ~YieldBlock() { --L->c_yield_blocked; }
        } yb(L);
        std::sort(elems.begin(), elems.end(), cmp);
        //------------------ stock's partition aborts with "invalid order function for sorting" when the
        //------------------ comparator contradicts itself; checking the result catches the same class of
        //------------------ bug (a comparator that never reports "not less") before the table is written
        for (size_t k = 1; k < elems.size(); ++k) {
            if (cmp(elems[k], elems[k - 1]))
                throw_runtime_error(file_line_prefix(L) + "invalid order function for sorting");
        }
        write_elems(list, elems);
    } else {
        auto cmp = [L](const LValue &a, const LValue &b) {
            LValue cmp = lt(L, a, b);
            return cmp.as_bool();
        };
        auto elems = extract_elems(list, len);
        std::sort(elems.begin(), elems.end(), cmp);
        write_elems(list, elems);
    }
    return MultiValue();
}

//------------------ table_pack: packs variable arguments into a table (with .n field)
MultiValue table_pack(LState *L, const LValue *args, size_t count) {
    LValue t = L->create_table(count);
    LTable *tbl = static_cast<LTable *>(t.as_pointer());
    for (size_t k = 0; k < count; ++k)
        tbl->settable(LValue(static_cast<int64_t>(k + 1)), args[k]);
    clx::set_field(L, t, "n", LValue(static_cast<int64_t>(count)));
    return MultiValue(t);
}

//------------------ table_unpack: forward declaration for vararg_expand below
MultiValue table_unpack(LState *, const LValue *, size_t);

//------------------ vararg_expand: expands the named vararg table of "function f(...t)" for `...`
MultiValue vararg_expand(LState *L, const LValue &tbl) {
    if (tbl.type != Table)
        throw_index_error(L, tbl);
    LTable *list = static_cast<LTable *>(tbl.as_pointer());
    LValue nfield = list->gettable(LValue(L->intern_string("n")));
    //------------------ stock requires a non-negative integer field 'n' (a float, even 2^40, is rejected)
    if (nfield.type != Int64 || nfield.val.payload.i64 < 0 || nfield.val.payload.i64 > INT_MAX)
        throw_runtime_error(file_line_prefix(L) + "vararg table has no proper 'n'");
    int64_t n = nfield.val.payload.i64;
    if (n == 0)
        return MultiValue();
    LValue args[3] = { tbl, LValue(static_cast<int64_t>(1)), LValue(n) };
    return table_unpack(L, args, 3);
}

//------------------ table_unpack: unpacks array elements into multiple return values
MultiValue table_unpack(LState *L, const LValue *args, size_t count) {
    if (count == 0)
        throw_runtime_error(bad_arg_msg(L, 1, "table.unpack", "table expected, got no value"));
    LTable *list = check_table(L, args[0]);
    int64_t len = static_cast<int64_t>(get_array_len(L, list));
    int64_t i = 1;
    if (count >= 2 && args[1].type != Nil)
        i = check_integer(L, args[1]);
    int64_t j = len;
    if (count >= 3 && args[2].type != Nil)
        j = check_integer(L, args[2]);

    if (i > j)
        return MultiValue();
    //------------------ stock bails out on a range it cannot push ("too many results"), it never clamps
    //------------------ the end of the range to the sequence length
    uint64_t span = static_cast<uint64_t>(j) - static_cast<uint64_t>(i);
    //------------------ stock's lua_checkstack refuses a range that cannot fit LUAI_MAXSTACK slots
    if (span >= static_cast<uint64_t>(INT_MAX) || span + 1 >= 1000000)
        throw_runtime_error(file_line_prefix(L) + "too many results to unpack");

    size_t n = static_cast<size_t>(span) + 1;
    //------------------ stock reads the range with lua_geti, so a metatabled table resolves through __index
    auto unpack_elem = [&](size_t k) -> LValue {
        int64_t key = i + static_cast<int64_t>(k);
        if (tbl_metatable(list))
            return list->get_value(L, LValue(key));
        return get_elem(list, static_cast<size_t>(key));
    };
    if (n <= 8) {
        LValue vals[8];
        for (size_t k = 0; k < n; ++k)
            vals[k] = unpack_elem(k);
        return MultiValue(vals, n, L);
    }
    LValue *vals = new LValue[n];
    for (size_t k = 0; k < n; ++k)
        vals[k] = unpack_elem(k);
    MultiValue res(vals, n, L);
    delete[] vals;
    return res;
}

//------------------ table_move: moves elements from one range to another (within or between tables)
MultiValue table_move(LState *L, const LValue *args, size_t count) {
    if (count < 3)
        throw_runtime_error(bad_arg_msg(L, 1, "table.move", "table expected, got no value"));
    LTable *src = check_table(L, args[0]);
    int64_t f = check_integer(L, args[1]);
    int64_t e = check_integer(L, args[2]);
    int64_t t = check_integer(L, args[3]);
    LTable *dst = src;
    if (count >= 5 && args[4].type != Nil)
        dst = check_table(L, args[4]);

    //------------------ stock's range guards, before any element is touched
    if (e >= f) {
        //------------------ INT64_MAX + f wraps in stock's lua_Integer arithmetic, and the tests rely on that
        //------------------ sum (minI + 1 with maxI gives 0, so a range ending at -1 is accepted): compute it
        //------------------ through an unsigned add so the wrap is defined here too
        const int64_t max_plus_f
            = static_cast<int64_t>(static_cast<uint64_t>(INT64_MAX) + static_cast<uint64_t>(f));
        if (!(f > 0 || e < max_plus_f))
            throw_runtime_error(file_line_prefix(L) + "too many elements to move");
        int64_t n_move = e - f + 1;
        if (t > INT64_MAX - n_move + 1)
            throw_runtime_error(file_line_prefix(L) + "destination wrap around");
    }

    if (src == dst && f >= 1 && e >= f && t >= 1 && static_cast<size_t>(e) <= src->array_size
        && static_cast<size_t>(t + (e - f)) <= src->array_size) {
        size_t n = static_cast<size_t>(e - f + 1);
        size_t src_off = static_cast<size_t>(f - 1);
        size_t dst_off = static_cast<size_t>(t - 1);
        std::memmove(&dst->array[dst_off], &src->array[src_off], n * sizeof(TValue));
        std::memmove(&dst->array_types[dst_off], &src->array_types[src_off], n * sizeof(ValueType));
        //------------------ slots below min(src, dst) are untouched, so the hint survives up to there
        size_t keep = src_off < dst_off ? src_off : dst_off;
        if (keep < dst->array_lim)
            dst->array_lim = keep;
        return MultiValue(LValue(dst));
    }

    //------------------ stock's direction rule (tmove): copy forward unless the destination starts
    //------------------ inside the source range, in which case copying backwards preserves the data.
    //------------------ The rule ignores metatables: they only change how a slot is read or written.
    LTable *src_mt = tbl_metatable(src);
    LTable *dst_mt = tbl_metatable(dst);
    bool meta_read = src_mt && src_mt->gettable(L->str_index).type != Nil;
    bool meta_write = dst_mt && dst_mt->gettable(L->str_newindex).type != Nil;
    //------------------ both loops count iterations the way stock's tmove does: stepping the key itself past
    //------------------ an INT64_MAX end would wrap around and never terminate
    int64_t last = e - f;
    auto move_one = [&](int64_t k, int64_t i) {
        LValue v = meta_read ? table_get(L, LValue(src), LValue(k)) : get_elem(src, static_cast<size_t>(k));
        if (meta_write)
            table_set(L, LValue(dst), LValue(t + i), v);
        else {
            gc_barrier_table(L, dst, v);
            set_elem(dst, static_cast<size_t>(t + i), v);
        }
    };
    if (t > e || t <= f || (dst != src))
        for (int64_t i = 0; i <= last; ++i)
            move_one(f + i, i);
    else
        for (int64_t i = last; i >= 0; --i)
            move_one(f + i, i);
    return MultiValue(LValue(dst));
}

MultiValue table_create(LState *L, const LValue *args, size_t count) {
    int64_t narray = check_integer(L, args[0]);
    int64_t nhash = (count >= 2 && args[1].type != Nil) ? check_integer(L, args[1]) : 0;
    if (narray < 0 || narray > INT_MAX)
        throw_runtime_error(bad_arg_msg(L, 1, "table.create", "out of range"));
    if (nhash < 0 || nhash > INT_MAX)
        throw_runtime_error(bad_arg_msg(L, 2, "table.create", "out of range"));
    if (nhash > static_cast<int64_t>(1u << 30))
        throw_runtime_error("table overflow");
    LValue t = L->create_table(static_cast<size_t>(narray), static_cast<size_t>(nhash));
    return MultiValue(t);
}

//------------------ luastd_table: registers the table library into the global state
void luastd_table(LState *L) {
    LValue t = L->create_table();
    LTable *tbl = static_cast<LTable *>(t.as_pointer());
    static constexpr clx::LazyReg table_funcs[]
        = { { "concat", table_concat }, { "insert", table_insert }, { "remove", table_remove }, { "sort", table_sort },
              { "pack", table_pack }, { "unpack", table_unpack }, { "move", table_move }, { "create", table_create } };
    clx::set_lazy_funcs(L, t, table_funcs, std::size(table_funcs));
    set_global(L, "table", t);
}

}
