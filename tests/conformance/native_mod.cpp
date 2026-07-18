#include <clx.h>

CLX_API clx::LValue luaopen_native_mod(clx::LState* L) {
    clx::LValue t = L->create_table();
    clx::LTable* mod = static_cast<clx::LTable*>(t.as_pointer());

    clx::LValue captured_G(clx::ValueType::Table, L->_G);

    mod->bind(L, "add", [](clx::LState* L, const clx::LValue* args, size_t n) -> clx::MultiValue {
        double sum = 0;
        for (size_t i = 0; i < n; ++i) sum += clx::check_number(L, args[i]);
        return clx::MultiValue(clx::LValue(sum));
    });

    mod->bind(L, "greet", [](clx::LState* L, const clx::LValue* args, size_t n) -> clx::MultiValue {
        const char* name = (n >= 1) ? clx::check_string(L, args[0]) : "world";
        std::string msg = std::string("hello ") + name;
        return clx::MultiValue(clx::LValue(L->intern_string(msg)));
    });

    mod->bind(L, "get_table", [](clx::LState* L, const clx::LValue* args, size_t n) -> clx::MultiValue {
        clx::LValue t2 = L->create_table();
        clx::raw_set(L, t2, "x", clx::LValue(10.0));
        clx::raw_set(L, t2, "y", clx::LValue(20.0));
        clx::raw_set(L, t2, "label", clx::LValue(L->intern_string("point")));
        return clx::MultiValue(t2);
    });

    mod->bind(L, "test_api", [](clx::LState* L, const clx::LValue* args, size_t n) -> clx::MultiValue {

        clx::LValue t2 = L->create_table();
        clx::raw_set(L, t2, "a", clx::LValue(1.0));
        clx::raw_set(L, t2, "b", clx::LValue(2.0));
        double a = clx::check_field_number(L, clx::raw_get(L, t2, "a"), "a");
        double b = clx::check_field_number(L, clx::raw_get(L, t2, "b"), "b");
        return clx::MultiValue(clx::LValue(a + b));
    });

    mod->bind(L, "get_env", [captured_G](clx::LState* L, const clx::LValue* args, size_t n) -> clx::MultiValue {
        return clx::MultiValue(captured_G);
    });

    return t;
}