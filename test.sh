#!/bin/bash

function board_test() {
  build/brmbot --print_eval --uci=false --depth 8 --killers=false --order_buckets=4 --cache=false --max_time=3 --print_time --print_depth --move_limit 1 --fen \""$1"\"
	echo "should be $2"
}

cd build; ninja; cd ..

# mate in one
board_test "1n2k3/2p2pp1/2p2n2/1q6/5K2/3r4/8/8 b - - 7 33" "g7g5"
# mate in two
board_test "4kn2/2p2pp1/2p2n2/8/5K2/3r4/8/8 b - - 4 3" "f8e6 or f8g6"
# up a piece
board_test "rnbqkb1r/pp1ppppp/2p2n2/4P3/8/8/PPPP1PPP/RNBQKBNR w KQkq - 0 3" "e5f6"
# puzzle
board_test "1n2k1n1/2p2pp1/2p5/3r4/8/1p6/5K2/8 b - - 1 28" "b3b2"
# mate in 4
board_test "8/4pR2/8/2P5/3P3Q/2K1P3/4q1k1/2B5 w - - 1 44" "c1d2 or f7g7"
# piece choice
board_test "r1b1kb1r/ppp2ppp/2n5/4p3/2PqQ3/2N5/PP1P1PPP/R1B1K1NR w KQkq - 3 8" "NOT c3b5"
