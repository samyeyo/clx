local sokol = require("sokol_clx")

local W, H = 800, 600
local MAX_ITER = 64

local buf = {}
local next_row = 0
local done = false

-- Initialize buffer to black
for i = 1, W * H * 4 do
  buf[i] = 0
end

local function compute_row(row)
  local py = (row - H / 2) * 4.0 / W
  local idx = row * W * 4 + 1
  for x = 0, W - 1 do
    local px = (x - W / 2) * 4.0 / W
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
      local r = math.min(1, iter / MAX_ITER * 4)
      local g = math.min(1, (iter / MAX_ITER - 0.25) * 4)
      local b = math.min(1, (iter / MAX_ITER - 0.5) * 2)
      buf[idx] = math.floor(math.max(0, r) * 255 + 0.5)
      buf[idx + 1] = math.floor(math.max(0, g) * 255 + 0.5)
      buf[idx + 2] = math.floor(math.max(0, b) * 255 + 0.5)
      buf[idx + 3] = 255
    end
    idx = idx + 4
  end
end

sokol.run {
  title = "Mandelbrot",
  width = W,
  height = H,

  frame = function()
    if not done then
      local rows = 0
      while next_row < H and rows < 100 do
        compute_row(next_row)
        next_row = next_row + 1
        rows = rows + 1
      end
      if next_row >= H then done = true end
    end
    sokol.pixels(W, H, buf)
  end,

  event = function(ev)
    if ev.type == "key_down" and ev.key == "escape" then os.exit(0) end
  end,
}
