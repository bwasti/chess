#include <algorithm>
#include <chrono>
#include <gflags/gflags.h>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <tuple>
#include <unordered_map>

#include "bitboard.h"
#include "movegen.h"
#include "position.h"
#include "thread.h"
#include "uci.h"

#define BETA (1 << 13)
#define ALPHA (-BETA)
#define START_POS "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -"

DEFINE_bool(cache, true, "Enable cache for negamax");
DEFINE_bool(killers, true, "Enable killer opt for negamax");
DEFINE_int64(cache_size, 1 << 24, "Set cache size for negamax");
DEFINE_int64(move_limit, ((int64_t)1) << 60, "Move limit");
DEFINE_bool(idfs, true, "Enable iterative depth first search");
DEFINE_int32(order_buckets, 5, "Number of buckets for fast ordering");
DEFINE_bool(print_move, true, "Dump the moves played");
DEFINE_bool(print_user_move, false, "Echo the moves played by the user");
DEFINE_bool(print_time, false, "Show the time used per move");
DEFINE_bool(print_nps, false, "Display the nodes tried per second");
DEFINE_bool(print_board, false, "Dump the board every move");
DEFINE_bool(print_fen, false, "Dump the FEN every move");
DEFINE_bool(print_depth, false, "Dump the depth achieved every move");
DEFINE_bool(print_eval, false, "Dump the evaluation for every move");
DEFINE_bool(uci, true, "Run in UCI mode");
DEFINE_bool(debug_uci, true, "If running in UCI mode, dump to stderr");
DEFINE_int32(depth, 20, "Maximum depth to search per move");
DEFINE_double(max_time, 1.0, "Maximum time to search per move");
DEFINE_double(scale_time, 1.0, "Scale time provided to white");
DEFINE_string(user, "", "User color");
DEFINE_string(fen, START_POS, "Initial FEN");

std::mt19937 &getRandDevice() {
  static std::random_device rd;
  static std::mt19937 g(rd());
  return g;
}

int val(const Piece &p) {
  switch (type_of(p)) {
  case PAWN:
    return 100;
  case KNIGHT:
    return 300;
  case BISHOP:
    return 300;
  case ROOK:
    return 500;
  case QUEEN:
    return 900;
  case KING:
    return 350;
  default:
    return 0;
  }
  return 0;
}

// this is expensive
int center_control(const Position &p, Color c) {
  auto ps = p.pieces(c);
  int sum = popcount((p.attackers_to(SQ_D4) | p.attackers_to(SQ_E4) |
                      p.attackers_to(SQ_D5) | p.attackers_to(SQ_E5)) &
                     ps);
  return sum;
}

int king_safety(const Position &p, Color c) {
  Square ksq = p.square<KING>(c);
  int sum = -popcount(p.attackers_to(ksq) & p.pieces(~c));
  return sum;
}

int pawn_structure(const Position &p, Color c) {
  int sum = 0;
  auto pawns = p.pieces(c, PAWN);
  if (c == WHITE) {
    sum = popcount(pawn_attacks_bb<WHITE>(pawns));
  } else {
    sum = popcount(pawn_attacks_bb<BLACK>(pawns));
  }

  return sum;
}

inline int activity(const Position &p, Color c) {
  auto ps = p.pieces(c, KNIGHT, BISHOP);
  if (c == WHITE) {
    return -popcount(ps & Rank1BB);
  } else {
    return -popcount(ps & Rank8BB);
  }
}

inline int eval(const Position &p, Color c) {
  int sum = 0;
  sum += 100 * p.count<PAWN>(c);
  if (sum >= 700) {
    sum += 10 * center_control(p, c);
    sum += 10 * activity(p, c);
    sum += 10 * pawn_structure(p, c);
  }
  sum += 300 * popcount(p.pieces(c, KNIGHT, BISHOP));
  sum += 500 * p.count<ROOK>(c);
  sum += 900 * p.count<QUEEN>(c);
  sum += 10 * king_safety(p, c);
  return sum;
}

// returns 0 on equal value
int normalized_eval(const Position &p) {
  return eval(p, p.side_to_move()) - eval(p, ~p.side_to_move());
}

typedef enum { EXACT, UPPERBOUND, LOWERBOUND } entry_flag;
struct Entry {
  Entry() = default;
  int64_t hash;
  bool valid = 0;
  int depth;
  int value;
  entry_flag flag;
};

std::vector<Entry> &getCache() {
  static std::vector<Entry> cache(FLAGS_cache_size);
  return cache;
}

Entry lookup(Position &p) {
  const auto &cache = getCache();
  const auto hash = p.key();
  const auto idx = hash % cache.size();
  auto entry = cache[idx];
  entry.valid = (entry.hash == hash);
  return entry;
}

void set(Position &p, Entry &e) {
  auto &cache = getCache();
  const auto hash = p.key();
  const auto idx = hash % cache.size();
  e.hash = hash;
  e.valid = true;
  cache[idx] = e;
}

std::string print_square(Square s) {
  std::stringstream ss;
  ss << char(file_of(s) + 'a') << char(rank_of(s) + '1');
  return ss.str();
}

// ply -> move
#define KILLERS 128
#define KILLERS_PER_PLY 3
static Move killers[KILLERS][KILLERS_PER_PLY];

inline int move_val(const Position &p, const Move &m,
                    const Move (&killer)[KILLERS_PER_PLY]) {
  if (type_of(m) == PROMOTION) {
    return 2500;
  }
  if (p.gives_check(m)) {
    return 1500;
  }
  if (p.capture(m)) {
    return 2000;
  }
  return 1000;
}

inline int move_val_old(const Position &p, const Move &m,
                        const Move (&killer)[KILLERS_PER_PLY]) {
  for (auto i = 0; i < KILLERS_PER_PLY; ++i) {
    if (m == killer[i]) {
      return 2000;
    }
  }
  if (p.gives_check(m)) {
    return 1800;
  }
  switch (type_of(m)) {
  case PROMOTION:
    return 1400;
  case CASTLING:
  case ENPASSANT:
    return 1300;
  default:
    break;
  }
  auto t = type_of(p.moved_piece(m));
  constexpr int offset = 500;
  switch (t) {
  case PAWN:
    return (p.capture(m) ? offset : 0) + 600;
  case BISHOP:
  case KNIGHT:
    return (p.capture(m) ? offset : 0) + 500;
  case ROOK:
    return (p.capture(m) ? offset : 0) + 400;
  case QUEEN:
    return (p.capture(m) ? offset : 0) + 300;
  case KING:
    return (p.capture(m) ? offset : 0) + 200;
  default:
    return (p.capture(m) ? offset : 0) + 100;
  }
  return 100;
}

#define PRIME 439
// This is surprisingly important
struct Ordered {
  Ordered() { clear(); }
  Ordered(const Ordered &o) {
    memcpy(ordered_, o.ordered_, o.size());
    last_ = ordered_ + o.size();
    assert(size() == o.size());
  }
  inline const Move *begin() const { return ordered_; }
  inline const Move *end() const { return last_; }
  size_t size() const { return (size_t)(last_ - ordered_); }
  void clear() { last_ = ordered_; }
  void insert(Move m) {
    *last_ = m;
    ++last_;
  }

private:
  Move ordered_[MAX_MOVES], *last_;
};

static int g_vals[MAX_MOVES];

std::vector<Move> ordered_moves(const Position &p) {
  MoveList<LEGAL> list(p);
  static std::vector<Move> checks;
  checks.clear();
  static std::vector<Move> captures;
  captures.clear();
  static std::vector<Move> rest;
  rest.clear();
  for (const auto &m : list) {
    if (p.gives_check(m)) {
      checks.emplace_back(m);
    } else if (p.capture_or_promotion(m)) {
      captures.emplace_back(m);
    } else {
      rest.emplace_back(m);
    }
  }
  static std::vector<Move> out;
  out.clear();
  out.reserve(checks.size() + captures.size() + rest.size());
  out.insert(out.end(), checks.begin(), checks.end());
  out.insert(out.end(), captures.begin(), captures.end());
  out.insert(out.end(), rest.begin(), rest.end());
  return out;
}

Ordered ordered_moves_fast(const Position &p) {
  MoveList<LEGAL> list(p);

  Move killer[KILLERS_PER_PLY] = {Move()};
  if (FLAGS_killers) {
    const auto idx = p.game_ply() % KILLERS;
    memcpy(killer, killers[idx], sizeof(Move) * KILLERS_PER_PLY);
  }
  const auto &move_ptr = list.begin();
  const auto N = list.size();
  int largest_value = 0;
  int largest_idx = 0;
  for (auto i = 0; i < N; ++i) {
    auto v = move_val(p, move_ptr[i], killer);
    g_vals[i] = v;
    if (v > largest_value) {
      largest_value = v;
      largest_idx = i;
    }
  }

  Ordered ordered;

  // we want to iterate through the list 3 times assigning values
  const int target = largest_value / FLAGS_order_buckets;
  for (auto k = FLAGS_order_buckets - 1; k >= 0; --k) {
    for (auto i = 0; i < N; ++i) {
      const auto idx = (PRIME * i + 1) % N;
      const int v = g_vals[idx];
      if (v > (k * target) && v <= ((k + 1) * target)) {
        auto m = move_ptr[idx];
        ordered.insert(m);
      }
    }
  }
  return ordered;
}

inline std::vector<Move> ordered_moves_slow(const Position &p) {
  MoveList<LEGAL> list(p);

  std::vector<Move> out;
  std::vector<std::pair<Move, int>> valued;
  out.reserve(list.size());
  valued.reserve(list.size());
  Move killer[KILLERS_PER_PLY] = {Move()};
  for (const auto &m : list) {
    valued.emplace_back(std::make_pair(m, move_val(p, m, killer)));
  }
  std::shuffle(valued.begin(), valued.end(), getRandDevice());
  std::stable_sort(
      valued.begin(), valued.end(),
      [](const std::pair<Move, int> &a, const std::pair<Move, int> &b) {
        return a.second > b.second;
      });
  for (const auto &m : valued) {
    out.emplace_back(m.first);
  }
  return out;
}

inline void set_killer(const Position &p, const Move &m) {
  if (FLAGS_killers) {
    bool set = false;
    const auto idx = p.game_ply() % KILLERS;
    for (auto i = 0; i < KILLERS_PER_PLY; ++i) {
      if (killers[idx][i]) {
        continue;
      }
      killers[idx][i] = m;
      set = true;
      break;
    }
    // no idea why this is better
    if (!set) {
      // killers[idx][m % KILLERS_PER_PLY] = m;
      killers[idx][0] = m;
    }
  }
}

// returns value + nodes scanned
std::pair<int, size_t>
negamax(Position &p, int depth, int alpha, int beta,
        const std::chrono::time_point<std::chrono::steady_clock> &start,
        double max_time) {

  std::chrono::duration<double> diff = std::chrono::steady_clock::now() - start;
  if (diff.count() > max_time) {
    return std::make_pair(ALPHA, 0);
  }

  auto orig_alpha = alpha;

  if (FLAGS_cache) {
    auto entry = lookup(p);
    if (entry.valid && entry.depth >= depth) {
      switch (entry.flag) {
      case EXACT:
        return std::make_pair(entry.value, 1);
      case LOWERBOUND:
        alpha = std::max(alpha, entry.value);
      case UPPERBOUND:
        beta = std::min(beta, entry.value);
      }
      if (alpha > beta) {
        return std::make_pair(entry.value, 1);
      }
    }
  }

  auto moves = ordered_moves(p);
  int val = ALPHA;
  size_t nodes = 1;

  // first, check for mates
  if (moves.size() == 0) {
    if (popcount(p.checkers())) {
      // checkmate!
      return std::make_pair(ALPHA, nodes);
    }
    // stalemate :/
    return std::make_pair(0, nodes);
  }

  if (depth == 0) {
    return std::make_pair(normalized_eval(p), 1);
  }

  for (const auto &m : moves) {
    StateInfo si;
    p.do_move(m, si);
    const auto r = negamax(p, depth - 1, -beta, -alpha, start, max_time);
    val = std::max(val, -r.first);
    nodes += r.second;
    p.undo_move(m);
    alpha = std::max(alpha, val);
    if (alpha >= beta) {
      set_killer(p, m);
      break;
    }
  }

  if (FLAGS_cache) {
    Entry entry;
    entry.value = val;
    if (val < orig_alpha) {
      entry.flag = UPPERBOUND;
    } else if (val > beta) {
      entry.flag = LOWERBOUND;
    } else {
      entry.flag = EXACT;
    }
    entry.depth = depth;
    set(p, entry);
  }

  if (diff.count() > max_time) {
    return std::make_pair(ALPHA, 0);
  }

  return std::make_pair((val * 99) / 100, nodes);
}

// returns best move and nodes scanned
std::pair<Move, size_t> best_move(Position &p, double max_time,
                                  int32_t depth = -1) {
  auto start = std::chrono::steady_clock::now();
  auto moves = ordered_moves(p);
  std::vector<Move> best_calc;
  int best_eval = 0;
  if (depth == -1) {
    depth = FLAGS_depth;
  }
  auto init = FLAGS_idfs ? 0 : depth - 1;
  size_t nodes = 0;
  for (auto d = init; d < depth; ++d) {
    Move best = MOVE_NONE;
    int best_v = ALPHA;
    int alpha = ALPHA;
    bool completed = true;
    for (const Move &m : moves) {
      std::chrono::duration<double> diff =
          std::chrono::steady_clock::now() - start;
      if (diff.count() > max_time) {
        completed = false;
        break;
      }
      StateInfo si;
      p.do_move(m, si);
      const auto r = negamax(p, d, alpha, BETA, start, max_time);
      int val = -r.first;
      // this negamax did not complete!
      if (r.second == 0) {
        val = ALPHA;
      }
      // alpha = std::max(alpha, val);
      nodes += r.second;
      p.undo_move(m);
      // std::cerr << "considering " << UCI::move(m, false) << ":" << val <<
      // "\n";
      if (val > best_v) {
        best = m;
        best_v = val;
      }
    }
    if (completed || best_calc.size() == 0) {
      best_calc.emplace_back(best);
      best_eval = best_v;
    }
  }
  if (FLAGS_print_depth) {
    std::cout << "depth:\t" << best_calc.size() << "\n";
  }
  if (FLAGS_print_eval) {
    std::cout << "eval:\t" << best_eval * (p.side_to_move() == BLACK ? -1 : 1)
              << "\n";
  }
  // clear a new killers spot
  // memset(killers[(p.game_ply() + 1) % KILLERS], 0, KILLERS_PER_PLY);
  return std::make_pair(best_calc.back(), nodes);
}

void init() {
  UCI::init(Options);
  Bitboards::init();
  Position::init();
  Bitbases::init();
  Threads.set(1);
}

float manage_time(size_t time_left_, size_t increment) {
  float time_left = (1.0 / 1000) * time_left_;
  float target = 1.0f;
  if (increment) {
    float target = time_left / 38 + ((float)increment / 1000);
  }
  target = std::max(target, 1.0f);
  if (time_left < target) {
    target = time_left / 2;
  }
  return target;
}

// for UCI bot play
void uci_loop() {
  std::cerr << "Launching in UCI mode...\n";
  Position p;
  StateInfo si;

  enum State {
    READ,
    OPTION,
    OPTION_NAME,
    OPTION_VALUE,
    POSITION,
    MOVE,
    WTIME,
    BTIME,
    WINC,
    BINC,
  };

  std::unordered_map<std::string, std::string> options;
  std::string option_name;
  std::string option_value;

  int state = READ;
  auto reset_state = [&]() {
    if (state == OPTION_VALUE) {
      options[option_name] = option_value;
      option_name = "";
      option_value = "";
      if (FLAGS_debug_uci) {
        std::cerr << "options:\n";
        for (const auto &option : options) {
          std::cerr << "  " << option.first << ": " << option.second << "\n";
        }
      }
    }
    state = READ;
  };
  Color side = COLOR_NB;
  size_t black_time = 0;
  size_t white_time = 0;
  size_t black_inc = 0;
  size_t white_inc = 0;

  // setup commands
  while (true) {
    std::string cmd;
    std::cin >> cmd;
    if (FLAGS_debug_uci) {
      if (cmd.size()) {
        std::cerr << "IN: " << cmd << "\n";
      }
    }

    if (cmd == "uci") {
      reset_state();
      std::cout << "id author bwasti\n";
      std::cout << "uciok\n";
    } else if (cmd == "quit") {
      break;
    } else if (cmd == "setoption") {
      reset_state();
      state = OPTION;
      option_name = "";
      option_value = "";
    } else if (cmd == "isready") {
      reset_state();
      std::cout << "readyok\n";
    } else if (cmd == "position") {
      reset_state();
      state = POSITION;
    } else if (cmd == "moves") {
      reset_state();
      state = MOVE;
    } else if (cmd == "go") {
      reset_state();
      Move m;
      size_t nodes;
      if (side != COLOR_NB) {
        if (side == WHITE) {
          std::tie(m, nodes) = best_move(p, manage_time(white_time, white_inc));
        } else {
          std::tie(m, nodes) = best_move(p, manage_time(black_time, black_inc));
        }
      } else {
        std::tie(m, nodes) = best_move(p, FLAGS_max_time);
      }
      side = p.side_to_move();
      StateInfo si;
      p.do_move(m, si);
      std::cout << "bestmove " << UCI::move(m, false) << "\n";
    } else if (cmd == "name") {
      if (state == OPTION) {
        state = OPTION_NAME;
      }
    } else if (cmd == "value") {
      if (state == OPTION_NAME) {
        state = OPTION_VALUE;
      }
    } else if (cmd == "wtime") {
      reset_state();
      state = WTIME;
    } else if (cmd == "btime") {
      reset_state();
      state = BTIME;
    } else if (cmd == "winc") {
      reset_state();
      state = WINC;
    } else if (cmd == "binc") {
      reset_state();
      state = BINC;
    } else { // non-keywords
      if (state == OPTION_NAME) {
        if (option_name.size()) {
          option_name += " ";
        }
        option_name += cmd;
      } else if (state == OPTION_VALUE) {
        if (option_value.size()) {
          option_value += " ";
        }
        option_value += cmd;
      } else if (state == POSITION) {
        if (cmd == "startpos") {
          reset_state();
          p.set(START_POS, false, &si, Threads.main());
        } else {
          // I assume FEN?
          std::cerr << "ERROR unknown position " << cmd << "\n";
          return;
        }
      } else if (state == MOVE) {
        auto m = UCI::to_move(p, cmd);
        StateInfo si;
        p.do_move(m, si);
      } else if (state == WTIME) {
        white_time = std::stoi(cmd);
      } else if (state == BTIME) {
        black_time = std::stoi(cmd);
      } else if (state == WINC) {
        white_inc = std::stoi(cmd);
      } else if (state == BINC) {
        black_inc = std::stoi(cmd);
      }
    }
  }
}

int main(int argc, char **argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  init();
  if (FLAGS_uci) {
    uci_loop();
    return 0;
  }

  Position p;
  StateInfo si;
  p.set(FLAGS_fen, false, &si, Threads.main());
  auto limit = p.game_ply() + FLAGS_move_limit;
  auto user = 1337;
  if (FLAGS_user == "w" || FLAGS_user == "white") {
    user = WHITE;
  } else if (FLAGS_user == "b" || FLAGS_user == "black") {
    user = BLACK;
  }
  if (FLAGS_print_board) {
    std::cout << p << "\n";
  }
  while (p.game_ply() < limit) {
    auto start = std::chrono::steady_clock::now();
    Move m;
    size_t nodes = 0;
    if (p.side_to_move() == user) {
      std::string move;
      std::cin.clear();
      fflush(stdin);
      std::cin >> move;
      if (move.size() == 0) {
        continue;
      }
      if (!(move.size() == 4 || move.size() == 5)) {
        std::cerr << "illegal move: " << move << "\n";
        continue;
      }
      if (move[0] > 'h' || move[0] < 'a') {
        std::cerr << "illegal move: " << move << "\n";
        continue;
      }
      if (move[2] > 'h' || move[2] < 'a') {
        std::cerr << "illegal move: " << move << "\n";
        continue;
      }
      if (move[1] > '8' || move[1] < '1') {
        std::cerr << "illegal move: " << move << "\n";
        continue;
      }
      if (move[3] > '8' || move[3] < '1') {
        std::cerr << "illegal move: " << move << "\n";
        continue;
      }

      m = UCI::to_move(p, move);
      auto moves = ordered_moves(p);
      bool legal = false;
      for (const auto &_m : moves) {
        if (m == _m) {
          legal = true;
        }
      }
      if (!p.legal(m) || !is_ok(m) || !legal) {
        std::cerr << "illegal move: " << move << "\n";
        continue;
      }
      StateInfo si;
      p.do_move(m, si);
      if (!p.pos_is_ok()) {
        p.undo_move(m);
        std::cerr << "illegal move: " << move << "\n";
        continue;
      }
    } else if (p.side_to_move() == WHITE) {
      std::tie(m, nodes) = best_move(p, FLAGS_scale_time * FLAGS_max_time);
      if (m == MOVE_NONE) {
        if (p.checkers()) {
          std::cout << "black wins\n";
        } else {
          std::cout << "stalemate\n";
        }
        break;
      }
      StateInfo si;
      p.do_move(m, si);
      assert(p.pos_is_ok());
    } else if (p.side_to_move() == BLACK) {
      std::tie(m, nodes) = best_move(p, FLAGS_max_time);
      if (m == MOVE_NONE) {
        if (p.checkers()) {
          std::cout << "white wins\n";
        } else {
          std::cout << "stalemate\n";
        }
        break;
      }
      StateInfo si;
      p.do_move(m, si);
      assert(p.pos_is_ok());
    } else {
      assert(0);
    }
    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed_seconds = end - start;
    if (FLAGS_print_time) {
      std::cout << "time:\t" << elapsed_seconds.count() << "\n";
    }
    if (FLAGS_print_nps) {
      std::cout << "node/s:\t" << nodes / elapsed_seconds.count() << "\n";
    }
    if (FLAGS_print_move) {
      // we moved once, so this check is reversed
      if (p.side_to_move() == user || FLAGS_print_user_move || user == 1337) {
        std::cout << UCI::move(m, false) << "\n";
      }
    }
    if (FLAGS_print_fen) {
      std::cout << "fen:\t" << p.fen() << "\n";
    }
    if (FLAGS_print_board) {
      std::cout << p << "\n";
    }
    fflush(stdout);
  }
}
