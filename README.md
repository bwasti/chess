# chess

Little chess engine I wrote over the holidays to compete with my brother's implementation.
It uses the stockfish board representation/movegen but has the entire engine implementation in main.cc (alpha-beta pruned negamax with transitition table + killer heuristic).


Originally tried to make it play like me (thus the name "brmbot"), but I'm not actually very good so it's just a normal bot now.

### build

```
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```

### usage

#### self play

```
./brmbot --print_board
```

#### user play

in this case you're playing as black (specified by the `b`). type your moves in UCI format (e.g. "e7e5" or "a7a8q")
```
./brmbot --user b --max_time 0.01
```

#### other options

example here: https://asciinema.org/a/o5fGOGhHC69hiViK6A9ZIlMwQ

```
./brmbot --help
```
