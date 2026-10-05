-- weather: current weather for your location.
--   ./weather            geolocates you by public IP (ip-api.com), then asks
--                        open-meteo.com for the current conditions there
--   ./weather LAT LON    skips geolocation and uses the given coordinates
--   ./weather LAT LON "Paris"
--
-- Networking goes through luasocket (socket.http) compiled against clx's
-- Lua C API; JSON is decoded by dkjson. Both endpoints speak plain HTTP,
-- which is what luasocket supports (no TLS).

local http = require("socket.http")
local dkjson = require("dkjson")

-- WMO 4677 weather interpretation codes used by open-meteo
local WMO = {
    [0] = "Clear sky",
    [1] = "Mainly clear",
    [2] = "Partly cloudy",
    [3] = "Overcast",
    [45] = "Fog",
    [48] = "Depositing rime fog",
    [51] = "Light drizzle",
    [53] = "Moderate drizzle",
    [55] = "Dense drizzle",
    [56] = "Light freezing drizzle",
    [57] = "Dense freezing drizzle",
    [61] = "Slight rain",
    [63] = "Moderate rain",
    [65] = "Heavy rain",
    [66] = "Light freezing rain",
    [67] = "Heavy freezing rain",
    [71] = "Slight snow fall",
    [73] = "Moderate snow fall",
    [75] = "Heavy snow fall",
    [77] = "Snow grains",
    [80] = "Slight rain showers",
    [81] = "Moderate rain showers",
    [82] = "Violent rain showers",
    [85] = "Slight snow showers",
    [86] = "Heavy snow showers",
    [95] = "Thunderstorm",
    [96] = "Thunderstorm with slight hail",
    [99] = "Thunderstorm with heavy hail",
}

local function die(msg)
    io.stderr:write("weather: " .. msg .. "\n")
    os.exit(1)
end

local function fetch(url)
    http.TIMEOUT = 15
    local body, code = http.request(url)
    if not body then
        die(("request failed: %s\n  (%s)"):format(tostring(code), url))
    end
    if code ~= 200 then
        die(("HTTP %s from %s"):format(tostring(code), url))
    end
    return body
end

local function fetch_json(url)
    local data, _, reason = dkjson.decode(fetch(url))
    if type(data) ~= "table" then
        die("response is not valid JSON: " .. tostring(reason))
    end
    return data
end

-- Coordinates from the command line when given, otherwise from the public IP
local function locate()
    local a1, a2, a3 = arg[1], arg[2], arg[3]
    if a1 or a2 then
        local lat, lon = tonumber(a1), tonumber(a2)
        if not lat or not lon then
            die("usage: weather [latitude longitude [name]]")
        end
        return { lat = lat, lon = lon, label = a3 or "custom coordinates" }
    end

    local geo = fetch_json("http://ip-api.com/json")
    if geo.status ~= "success" then
        die("geolocation failed: " .. tostring(geo.message or geo.status))
    end
    local label = geo.city
    if geo.regionName and geo.regionName ~= geo.city then
        label = label .. ", " .. geo.regionName
    end
    if geo.country then
        label = label .. ", " .. geo.country
    end
    return { lat = geo.lat, lon = geo.lon, label = label }
end

local function current_weather(lat, lon)
    local url = ("http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        .. "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,wind_speed_10m"
        .. "&timezone=auto"):format(lat, lon)
    local data = fetch_json(url)
    if type(data.current) ~= "table" then
        die("unexpected forecast response (no 'current' field)")
    end
    return data.current, data.current_units or {}, data.timezone
end

local function main()
    local where = locate()
    local cur, units, timezone = current_weather(where.lat, where.lon)

    local desc = WMO[cur.weather_code] or ("code " .. tostring(cur.weather_code))
    local day = cur.is_day == 1 and "day" or "night"

    local function unit(name, fallback)
        return units[name] or fallback
    end

    print("Location   : " .. where.label)
    print("Coordinates: " .. ("%.4f, %.4f"):format(where.lat, where.lon))
    print("Time       : " .. tostring(cur.time) .. " (" .. tostring(timezone or "auto") .. ", " .. day .. ")")
    print("Weather    : " .. desc)
    print("Temperature: " .. tostring(cur.temperature_2m) .. " " .. unit("temperature_2m", "°C")
        .. " (feels like " .. tostring(cur.apparent_temperature) .. " " .. unit("apparent_temperature", "°C") .. ")")
    print("Humidity   : " .. tostring(cur.relative_humidity_2m) .. " " .. unit("relative_humidity_2m", "%"))
    print("Wind       : " .. tostring(cur.wind_speed_10m) .. " " .. unit("wind_speed_10m", "km/h"))
end

main()
