// ┌─────────────────────────────────────────────┐
// │  clx — Lua to C++ Native Compiler           │
// │  Copyright (c) 2026 Tine Samir. MIT License.│
// ├─────────────────────────────────────────────┤
// │  table.cpp · Table Library                  │
// └─────────────────────────────────────────────┘

#include "clx.h"
#include <cstring>
#include <algorithm>
#include <vector>
#include <cmath>

namespace clx {

//------------------ get_array_len: returns the length (#) of the array part of a table
static size_t get_array_len(LState* L, LTable* t) {
    if (t->metatable) {
        LValue mm = t->metatable->gettable(LValue(L->intern_string("__len", 5)));
        if (mm.type != Nil) {
            LValue res = len(L, LValue(t));
            if (res.type == Int64) return static_cast<size_t>(res.as_integer());
            return 0;
        }
    }
    // SIMD: find first nil gap in the array part
    size_t n = clx_find_first_nil(reinterpret_cast<const uint8_t*>(t->array_types), t->array_size);
    if (n < t->array_size) return n;
    // No nil found in array part — fall back to hash-probing binary search
    int64_t lo = static_cast<int64_t>(t->array_size);
    int64_t hi = static_cast<int64_t>(t->array_size) + 1;
    while (true) {
        LValue p = t->gettable(LValue(hi));
        if (p.type == Nil) break;
        lo = hi;
        hi = (hi < 8) ? 8 : hi * 2;
    }
    while (hi - lo > 1) {
        int64_t mid = lo + (hi - lo) / 2;
        LValue p = t->gettable(LValue(mid));
        if (p.type != Nil) lo = mid; else hi = mid;
    }
    return static_cast<size_t>(lo);
}

//------------------ get_elem: gets array element by 1-based index (hot helper)
static LValue get_elem(LTable* t, size_t idx) {
    if (static_cast<uint64_t>(idx - 1) < t->array_cap)
        return LValue(t->array[idx - 1], t->array_types[idx - 1]);
    LValue p = t->gettable(LValue(static_cast<int64_t>(idx)));
    return p.type != Nil ? p : LValue();
}

//------------------ set_elem: sets array element by 1-based index (hot helper)
static void set_elem(LTable* t, size_t idx, const LValue& val) {
    t->settable(LValue(static_cast<int64_t>(idx)), val);
}

//------------------ table_concat: concatenates array elements into a string
MultiValue table_concat(LState* L, const LValue* args, size_t count) {
    if (count == 0)
        throw_runtime_error("bad argument #1 to 'concat' (table expected, got no value)");
    LTable* list = check_table(L, args[0]);
    const char* sep = "";
    size_t sep_len = 0;
    if (count >= 2 && args[1].type != Nil) {
        sep = check_string(L, args[1]);
        sep_len = std::strlen(sep);
    }
    size_t i = 1;
    if (count >= 3 && args[2].type != Nil)
        i = static_cast<size_t>(opt_integer(L, args[2], 1));
    size_t j = get_array_len(L, list);
    if (count >= 4 && args[3].type != Nil)
        j = static_cast<size_t>(opt_integer(L, args[3], 0));

    if (i > j) return MultiValue(LValue(L->intern_string("", 0)));

    // SIMD: validate that all elements in [i..j] are String/Double/Int64
    {
        if (!clx_validate_types_range(reinterpret_cast<const uint8_t*>(list->array_types), i - 1, j - i + 1, 2, 4))
            throw_runtime_error("bad argument #1 to 'concat' (table contains non-string/number value)");
    }

    StringBuilder sb;
    for (size_t k = i; k <= j; ++k) {
        if (k > i) sb.append(sep, sep_len);
        LValue v = get_elem(list, k);
        sb.append(L, v);
    }
    return MultiValue(sb.to_lvalue(L));
}

//------------------ table_insert: inserts element at a position in the array
MultiValue table_insert(LState* L, const LValue* args, size_t count) {
    if (count < 2)
        throw_runtime_error("bad argument #1 to 'insert' (table expected, got no value)");
    LTable* list = check_table(L, args[0]);
    size_t len = get_array_len(L, list);
    size_t pos;
    LValue val;
    if (count == 2) {
        pos = len + 1;
        val = args[1];
    } else {
        int64_t p = check_integer(L, args[1]);
        if (p < 1)
            throw_runtime_error("bad argument #2 to 'insert' (position out of bounds)");
        pos = static_cast<size_t>(p);
        val = args[2];
    }
    if (pos < 1 || pos > len + 1)
        throw_runtime_error("bad argument #2 to 'insert' (position out of bounds)");
    if (pos <= len) {
        if (pos <= list->array_size && len <= list->array_size) {
            if (list->array_cap <= len)
                list->settable(LValue(static_cast<int64_t>(len + 1)), LValue());
            size_t move_n = len - pos + 1;
            std::memmove(&list->array[pos], &list->array[pos - 1], move_n * sizeof(TValue));
            std::memmove(&list->array_types[pos], &list->array_types[pos - 1], move_n * sizeof(ValueType));
            list->array[pos - 1] = val.val; list->array_types[pos - 1] = val.type;
            if (list->array_size <= len) list->array_size = len + 1;
            list->array_version++;
        } else {
            for (size_t k = len; k >= pos; --k) {
                set_elem(list, k + 1, get_elem(list, k));
                if (k == 0) break;
            }
            set_elem(list, pos, val);
        }
    } else {
        set_elem(list, pos, val);
    }
    return MultiValue();
}

//------------------ table_remove: removes element from array, shifts subsequent elements
MultiValue table_remove(LState* L, const LValue* args, size_t count) {
    if (count == 0)
        throw_runtime_error("bad argument #1 to 'remove' (table expected, got no value)");
    LTable* list = check_table(L, args[0]);
    size_t len = get_array_len(L, list);
    size_t pos = len;
    if (count >= 2 && args[1].type != Nil)
        pos = static_cast<size_t>(check_integer(L, args[1]));
    if (pos < 1 || pos > len)
        return MultiValue(LValue());
    LValue res;
    if (pos <= list->array_size && len <= list->array_size) {
        res = LValue(list->array[pos - 1], list->array_types[pos - 1]);
        size_t move_n = len - pos;
        std::memmove(&list->array[pos - 1], &list->array[pos], move_n * sizeof(TValue));
        std::memmove(&list->array_types[pos - 1], &list->array_types[pos], move_n * sizeof(ValueType));
        list->array[len - 1] = TValue(); list->array_types[len - 1] = Nil;
        list->array_version++;
    } else {
        res = get_elem(list, pos);
        for (size_t k = pos; k < len; ++k)
            set_elem(list, k, get_elem(list, k + 1));
        set_elem(list, len, LValue());
    }
    return MultiValue(res);
}

//------------------ table_sort: sorts array elements in-place
MultiValue table_sort(LState* L, const LValue* args, size_t count) {
    if (count == 0)
        throw_runtime_error("bad argument #1 to 'sort' (table expected, got no value)");
    LTable* list = check_table(L, args[0]);
    size_t len = get_array_len(L, list);
    if (len == 0) return MultiValue();

    // Fast path: no comparator, all elements numeric, all within array part — sort doubles directly
    if ((count < 2 || args[1].type == Nil) && len <= list->array_size) {
        if (clx_validate_types_range(reinterpret_cast<const uint8_t*>(list->array_types), 0, len, 2, 4)) {
            std::vector<double> nums(len);
            for (size_t k = 0; k < len; ++k) {
                if (list->array_types[k] == Double)
                    nums[k] = list->array[k].payload.f64;
                else
                    nums[k] = static_cast<double>(list->array[k].payload.i64);
            }
            std::sort(nums.begin(), nums.end());
            for (size_t k = 0; k < len; ++k) {
                list->array[k].payload.f64 = nums[k];
                list->array_types[k] = Double;
            }
            list->array_version++;
            return MultiValue();
        }
    }

    auto extract_elems = [&](LTable* t, size_t n) {
        std::vector<LValue> elems;
        elems.reserve(n);
        for (size_t k = 1; k <= n; ++k) elems.push_back(get_elem(t, k));
        return elems;
    };
    auto write_elems = [&](LTable* t, const std::vector<LValue>& v) {
        for (size_t k = 0; k < v.size(); ++k) set_elem(t, k + 1, v[k]);
    };

    if (count >= 2 && args[1].type != Nil) {
        LValue comp_func = args[1];
        auto cmp = [L, comp_func](const LValue& a, const LValue& b) mutable {
            LValue call_args[2] = {a, b};
            size_t prev_top = L->shadow_top;
            L->shadow_stack[L->shadow_top++] = TypedSlot(&comp_func.val, &comp_func.type);
            L->shadow_stack[L->shadow_top++] = TypedSlot(&call_args[0].val, &call_args[0].type);
            L->shadow_stack[L->shadow_top++] = TypedSlot(&call_args[1].val, &call_args[1].type);
            MultiValue mr = call_function(L, comp_func, call_args, 2, "table.sort", 0);
            L->shadow_top = prev_top;
            return mr.count >= 1 && mr[0].as_bool();
        };
        auto elems = extract_elems(list, len);
        std::sort(elems.begin(), elems.end(), cmp);
        write_elems(list, elems);
    } else {
        auto cmp = [L](const LValue& a, const LValue& b) {
            LValue cmp = lt(L, a, b);
            return cmp.as_bool();
        };
        auto elems = extract_elems(list, len);
        std::sort(elems.begin(), elems.end(), cmp);
        write_elems(list, elems);
    }
    list->array_version++;
    return MultiValue();
}

//------------------ table_pack: packs variable arguments into a table (with .n field)
MultiValue table_pack(LState* L, const LValue* args, size_t count) {
    LValue t = L->create_table(count);
    LTable* tbl = static_cast<LTable*>(t.as_pointer());
    for (size_t k = 0; k < count; ++k)
        tbl->settable(LValue(static_cast<int64_t>(k + 1)), args[k]);
    clx::set_field(L, t, "n", LValue(static_cast<int64_t>(count)));
    return MultiValue(t);
}

//------------------ table_unpack: unpacks array elements into multiple return values
MultiValue table_unpack(LState* L, const LValue* args, size_t count) {
    if (count == 0)
        throw_runtime_error("bad argument #1 to 'unpack' (table expected, got no value)");
    LTable* list = check_table(L, args[0]);
    size_t len = get_array_len(L, list);
    size_t i = 1;
    if (count >= 2 && args[1].type != Nil)
        i = static_cast<size_t>(check_integer(L, args[1]));
    size_t j = len;
    if (count >= 3 && args[2].type != Nil)
        j = static_cast<size_t>(check_integer(L, args[2]));

    if (i > j || i < 1) return MultiValue();
    if (i > len) return MultiValue();
    if (j > len) j = len;

    size_t n = j - i + 1;
    if (n <= 8) {
        LValue vals[8];
        for (size_t k = 0; k < n; ++k)
            vals[k] = get_elem(list, i + k);
        return MultiValue(vals, n);
    }
    LValue* vals = new LValue[n];
    for (size_t k = 0; k < n; ++k)
        vals[k] = get_elem(list, i + k);
    MultiValue res(vals, n);
    delete[] vals;
    return res;
}

//------------------ table_move: moves elements from one range to another (within or between tables)
MultiValue table_move(LState* L, const LValue* args, size_t count) {
    if (count < 3)
        throw_runtime_error("bad argument #1 to 'move' (table expected, got no value)");
    LTable* src = check_table(L, args[0]);
    int64_t f = check_integer(L, args[1]);
    int64_t e = check_integer(L, args[2]);
    int64_t t = check_integer(L, args[3]);
    LTable* dst = src;
    if (count >= 5 && args[4].type != Nil)
        dst = check_table(L, args[4]);

    // Fast path: same table, both ranges within array part — use memmove
    if (src == dst && f >= 1 && e >= f && t >= 1
        && static_cast<size_t>(e) <= src->array_size
        && static_cast<size_t>(t + (e - f)) <= src->array_size) {
        size_t n = static_cast<size_t>(e - f + 1);
        size_t src_off = static_cast<size_t>(f - 1);
        size_t dst_off = static_cast<size_t>(t - 1);
        std::memmove(&dst->array[dst_off], &src->array[src_off], n * sizeof(TValue));
        std::memmove(&dst->array_types[dst_off], &src->array_types[src_off], n * sizeof(ValueType));
        dst->array_version++;
        return MultiValue(LValue(dst));
    }

    if (t >= f) {
        for (int64_t k = e; k >= f; --k) {
            LValue v = get_elem(src, static_cast<size_t>(k));
            set_elem(dst, static_cast<size_t>(t + (k - f)), v);
        }
    } else {
        for (int64_t k = f; k <= e; ++k) {
            LValue v = get_elem(src, static_cast<size_t>(k));
            set_elem(dst, static_cast<size_t>(t + (k - f)), v);
        }
    }
    return MultiValue(LValue(dst));
}

//------------------ luastd_table: registers the table library into the global state
void luastd_table(LState* L) {
    LValue t = L->create_table();
    LTable* tbl = static_cast<LTable*>(t.as_pointer());
    static constexpr clx::LazyReg table_funcs[] = {
        {"concat", table_concat},
        {"insert", table_insert},
        {"remove", table_remove},
        {"sort", table_sort},
        {"pack", table_pack},
        {"unpack", table_unpack},
        {"move", table_move}
    };
    clx::set_lazy_funcs(L, t, table_funcs, std::size(table_funcs));
    set_global(L, "table", t);
}

}
