local argparse = require("argparse")

local parser = argparse("test", "test parser")
parser:flag("-v")

assert(parser)

print("[PASS] argparse")
