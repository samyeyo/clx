local sokol = require("sokol_clx")

-- Window size (displayed to the user)
local WIN_W, WIN_H = 600, 450

-- Internal render resolution
local W, H = 400, 300
local MAX_ITER = 128
local LOG2 = 0.6931471805599453

local buf = {}
-- Initialize buffer
for i = 1, W * H * 4 do
  buf[i] = 0
end

-- Center of the zoom (a point deep inside the set that stays interesting)
local center_r = -0.743643887037151
local center_i = 0.13182590420533

-- zoom-space: half-width of the view in fractal units
-- (fractal extent spans about [-2.2, 1.2], i.e. ~3.5 units wide)
local half_w = 1.75
local zr_speed = 0.12          -- exponential zoom speed per second

local function compute_row(row)
  local vrange = half_w * 2 * (H / W)
  local py = center_i + (row - H / 2) / H * vrange
  local idx = row * W * 4 + 1
  for x = 0, W - 1 do
    local px = center_r + (x - W / 2) / W * (half_w * 2)
    local zx, zy, iter = 0, 0, 0
    while zx * zx + zy * zy <= 4 and iter < MAX_ITER do
      local tmp = zx * zx - zy * zy + px
      zy = 2 * zx * zy + py
      zx = tmp
      iter = iter + 1
    end
    if iter >= MAX_ITER then
      buf[idx] = 0; buf[idx + 1] = 0; buf[idx + 2] = 0; buf[idx + 3] = 255
    else
      -- Smooth (fractional) iteration count for nicer color bands
      local lzn = math.log(zx * zx + zy * zy) / 2
      local nu = math.log(lzn / LOG2) / LOG2
      local t = (iter + 1 - nu) / MAX_ITER
      local r = math.min(1, t * 4)
      local g = math.min(1, (t - 0.25) * 4)
      local b = math.min(1, (t - 0.5) * 2)
      buf[idx] = math.floor(math.max(0, r) * 255 + 0.5)
      buf[idx + 1] = math.floor(math.max(0, g) * 255 + 0.5)
      buf[idx + 2] = math.floor(math.max(0, b) * 255 + 0.5)
      buf[idx + 3] = 255
    end
    idx = idx + 4
  end
end

sokol.run {
  title = "Mandelbrot Zoom",
  width = WIN_W,
  height = WIN_H,

  frame = function(_dt)
    -- Exponential zoom: each frame magnifies by a constant factor per unit time.
    -- Time-based so the zoom speed is frame-rate independent.
    half_w = half_w * math.exp(-zr_speed * (_dt or 1 / 60))

    for row = 0, H - 1 do
      compute_row(row)
    end
    sokol.pixels(W, H, buf)
  end,

  event = function(ev)
    if ev.type == "key_down" and ev.key == "escape" then os.exit(0) end
  end,
}