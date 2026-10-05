# Weather

Current weather for your location — a real networked Lua program compiled to a
native executable by clx.

It fetches geolocation and forecast data over HTTP using **luasocket** (the C
core compiled against clx's Lua C API, the pure-Lua parts compiled by clx) and
decodes the JSON responses with **dkjson**.

## Source

`weather.lua` (see the file for the full listing). The interesting parts:

```lua
local http = require("socket.http")
local dkjson = require("dkjson")

local function fetch(url)
    http.TIMEOUT = 15
    local body, code = http.request(url)
    if not body then die(("request failed: %s\n  (%s)"):format(tostring(code), url)) end
    if code ~= 200 then die(("HTTP %s from %s"):format(tostring(code), url)) end
    return body
end
```

## Build

```bash
./build.sh        # Linux / macOS
```

```bat
:: Windows — run from an "x64 Native Tools Command Prompt"
build.bat
```

Both scripts:

1. Fetch luasocket 3.1.0 (pinned release tarball, cached in `build/`).
2. Compile the C cores (`socket.core`, `mime.core`) against clx's Lua C API
   headers and archive them as `socket.core.a` / `mime.core.a` (`.lib` on
   Windows).
3. Stage the Lua side (`socket.lua`, `socket/http.lua`, `ltn12.lua`, ...) —
   the directory layout gives the dotted `require` names.
4. Compile and link `weather` with clx using a probed smallest-binary flag
   set (`-Oz -finline-functions -flto=auto`, plus
   `-Wl,-exported_symbols_list,/dev/null` on macOS), producing a ~890 KB
   executable.

`build.bat` follows the same steps on Windows: it compiles the `wsocket.c`
Winsock backend instead of `usocket.c`, archives `socket.core.lib` /
`mime.core.lib`, compiles the C cores with MSVC smallest-code flags
(`/O1 /GL /Gw /Oi`), and links with clx passing `/Os /Oi /Zc:inline` — the
MSVC equivalents of build.sh's `-Oz -finline-functions -flto=auto` — plus
`ws2_32.lib` for Winsock.

Environment overrides:

| Variable | Default | Purpose |
| --- | --- | --- |
| `LUASOCKET_VERSION` | `3.1.0` | luasocket release to fetch |
| `CLX_INCLUDE` | `../../include` | path to clx's headers (`lua.h`) |
| `CC` | `cc` / `cl.exe` | C compiler for the luasocket cores (`build.bat` detects `cl.exe`, else `gcc`/`clang`) |
| `AR` | `ar` / `lib` | archiver for the module archives (`.a` on POSIX, `.lib` on Windows) |

## Run

```bash
./build/weather              # geolocates you by public IP
./build/weather 48.85 2.35   # use explicit coordinates
./build/weather 48.85 2.35 "Paris"
```

Output:

```text
Location   : Marseille, Provence-Alpes-Côte d'Azur, France
Coordinates: 43.3178, 5.4125
Time       : 2026-10-05T06:30 (Europe/Paris, night)
Weather    : Overcast
Temperature: 19.6 °C (feels like 20.4 °C)
Humidity   : 81.0 %
Wind       : 9.4 km/h
```

Invalid or out-of-range coordinates exit with status 1 and a message on
stderr.

## Data sources

* `http://ip-api.com/json` — geolocation by public IP (used when no
  coordinates are given).
* `http://api.open-meteo.com/v1/forecast` — current conditions
  (`temperature_2m`, `relative_humidity_2m`, `apparent_temperature`, `is_day`,
  `weather_code`, `wind_speed_10m`, `timezone=auto`).
* `dkjson` — JSON decoding.

Both endpoints speak **plain HTTP**, which is what luasocket supports — it has
no TLS layer, so the example cannot use `https://` URLs.

## Purpose

This example demonstrates:

* clx's Lua C API: a real third-party C module (luasocket) compiled against
  `include/lua.h` and linked into a clx binary
* dotted module names (`socket.http`) resolved from the staged directory
  layout and the `--modules socket.core,mime.core` link flags
* mixing compiled Lua, C modules, and pure-Lua libraries (dkjson)
* a complete CLI workflow: argument parsing, error paths, exit codes

## Next steps

After building this example, explore:

* `examples/hello/`
* `examples/mandelbrot/`
* `examples/pong/`
