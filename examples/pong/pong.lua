local sokol = require("sokol_clx")

-- Game state
local ball = { x = 400, y = 300, r = 8, dx = 250, dy = 200 }
local paddle_w, paddle_h = 12, 80
local l_paddle = { x = 30, y = 300 }
local r_paddle = { x = 770, y = 300 }
local l_score, r_score = 0, 0
local keys = {}  -- currently pressed keys

local W, H = 800, 600

-- 7-segment display patterns (segments: A,B,C,D,E,F,G)
local digits = {
  {true,  true,  true,  true,  true,  true,  false},  -- 0
  {false, true,  true,  false, false, false, false},  -- 1
  {true,  true,  false, true,  true,  false, true },  -- 2
  {true,  true,  true,  true,  false, false, true },  -- 3
  {false, true,  true,  false, false, true,  true },  -- 4
  {true,  false, true,  true,  false, true,  true },  -- 5
  {true,  false, true,  true,  true,  true,  true },  -- 6
  {true,  true,  true,  false, false, false, false},  -- 7
  {true,  true,  true,  true,  true,  true,  true },  -- 8
  {true,  true,  true,  true,  false, true,  true },  -- 9
}

local function draw_digit(x, y, d)
  local s = 4  -- segment thickness
  local segs = digits[d]
  if not segs then return end
  -- A: top horizontal
  if segs[1] then sokol.draw_rect(x + s, y, s * 3, s) end
  -- B: top-right vertical
  if segs[2] then sokol.draw_rect(x + s * 4, y + s, s, s * 3) end
  -- C: bottom-right vertical
  if segs[3] then sokol.draw_rect(x + s * 4, y + s * 5, s, s * 3) end
  -- D: bottom horizontal
  if segs[4] then sokol.draw_rect(x + s, y + s * 8, s * 3, s) end
  -- E: bottom-left vertical
  if segs[5] then sokol.draw_rect(x, y + s * 5, s, s * 3) end
  -- F: top-left vertical
  if segs[6] then sokol.draw_rect(x, y + s, s, s * 3) end
  -- G: middle horizontal
  if segs[7] then sokol.draw_rect(x + s, y + s * 4.5, s * 3, s) end
end

local function draw_score(x, y, left, right)
  draw_digit(x,      y, math.floor(left  / 10))
  draw_digit(x + 30, y, left  % 10)
  draw_digit(x + 70, y, math.floor(right / 10))
  draw_digit(x + 100, y, right % 10)
end

sokol.run {
  title = "Pong",
  width = W,
  height = H,

  init = function()
    math.randomseed(os.time())
  end,

  frame = function(dt)
    -- Paddle movement
    if keys["w"] then l_paddle.y = math.max(paddle_h / 2, l_paddle.y - 300 * dt) end
    if keys["s"] then l_paddle.y = math.min(H - paddle_h / 2, l_paddle.y + 300 * dt) end
    if keys["up"]    then r_paddle.y = math.max(paddle_h / 2, r_paddle.y - 300 * dt) end
    if keys["down"]  then r_paddle.y = math.min(H - paddle_h / 2, r_paddle.y + 300 * dt) end

    -- Ball movement
    ball.x = ball.x + ball.dx * dt
    ball.y = ball.y + ball.dy * dt

    -- Wall bounce (top/bottom)
    if ball.y - ball.r < 0 then ball.y = ball.r;  ball.dy = -ball.dy end
    if ball.y + ball.r > H then ball.y = H - ball.r; ball.dy = -ball.dy end

    -- Left paddle collision
    if ball.x - ball.r < l_paddle.x + paddle_w / 2 and
       ball.x + ball.r > l_paddle.x - paddle_w / 2 and
       ball.y + ball.r > l_paddle.y - paddle_h / 2 and
       ball.y - ball.r < l_paddle.y + paddle_h / 2 then
      ball.dx = -ball.dx
      ball.x = l_paddle.x + paddle_w / 2 + ball.r
      ball.dx = ball.dx * 1.05
    end

    -- Right paddle collision
    if ball.x + ball.r > r_paddle.x - paddle_w / 2 and
       ball.x - ball.r < r_paddle.x + paddle_w / 2 and
       ball.y + ball.r > r_paddle.y - paddle_h / 2 and
       ball.y - ball.r < r_paddle.y + paddle_h / 2 then
      ball.dx = -ball.dx
      ball.x = r_paddle.x - paddle_w / 2 - ball.r
      ball.dx = ball.dx * 1.05
    end

    -- Scoring
    if ball.x < -20 then
      r_score = r_score + 1
      ball.x = W / 2; ball.y = H / 2
      ball.dx = 250; ball.dy = 200
      if math.random() > 0.5 then ball.dx = -ball.dx end
      if math.random() > 0.5 then ball.dy = -ball.dy end
    end
    if ball.x > W + 20 then
      l_score = l_score + 1
      ball.x = W / 2; ball.y = H / 2
      ball.dx = -250; ball.dy = 200
      if math.random() > 0.5 then ball.dx = -ball.dx end
      if math.random() > 0.5 then ball.dy = -ball.dy end
    end

    -- Draw
    sokol.clear(0.1, 0.1, 0.12, 1)

    -- Center line (dashed)
    sokol.color(0.3, 0.3, 0.35, 1)
    for i = 0, H, 30 do
      sokol.draw_rect(W / 2 - 1, i, 2, 15)
    end

    -- Paddles
    sokol.color(1, 1, 1, 1)
    sokol.draw_rect(l_paddle.x - paddle_w / 2, l_paddle.y - paddle_h / 2, paddle_w, paddle_h)
    sokol.draw_rect(r_paddle.x - paddle_w / 2, r_paddle.y - paddle_h / 2, paddle_w, paddle_h)

    -- Ball
    sokol.draw_filled_circle(ball.x, ball.y, ball.r, 20)

    -- Score (7-segment digits)
    sokol.color(0.5, 0.5, 0.6, 1)
    draw_score(W / 2 - 70, 20, l_score, r_score)
  end,

  event = function(ev)
    if ev.type == "key_down" then
      keys[ev.key] = true
      if ev.key == "escape" then os.exit(0) end
    elseif ev.type == "key_up" then
      keys[ev.key] = nil
    end
  end
}
