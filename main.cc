#include <algorithm>
#include <chrono>
#include <gflags/gflags.h>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <tuple>
#include <unordered_map>

#include "movegen.h"
#include "position.h"
#include "thread.h"
#include "uci.h"

DEFINE_bool(cache, true, "Enable cache for negamax");
DEFINE_bool(killers, true, "Enable killer opt for negamax");
DEFINE_int64(cache_size, 1 << 20, "Set cache size for negamax");
DEFINE_bool(idfs, true, "Enable iterative depth first search");
DEFINE_int32(order_buckets, 5, "Number of buckets for fast ordering");
DEFINE_bool(print_move, true, "Dump the moves played");
DEFINE_bool(print_user_move, false, "Echo the moves played by the user");
DEFINE_bool(print_time, false, "Show the time used per move");
DEFINE_bool(print_nps, false, "Display the nodes tried per second");
DEFINE_bool(print_board, false, "Dump the board every move");
DEFINE_bool(print_fen, false, "Dump the FEN every move");
DEFINE_bool(print_depth, false, "Dump the depth achieved every move");
DEFINE_int32(depth, 20, "Maximum depth to search per move");
DEFINE_double(max_time, 1.0, "Maximum time to search per move");
DEFINE_double(scale_time, 1.0, "Scale time provided to white");
DEFINE_string(user, "", "User color");
DEFINE_string(fen, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -",
              "Initial FEN");

std::mt19937 &getRandDevice() {
  static std::random_device rd;
  static std::mt19937 g(rd());
  return g;
}

float val(const Piece &p) {
  switch (type_of(p)) {
  case PAWN:
    return 1;
  case KNIGHT:
    return 3;
  case BISHOP:
    return 3;
  case ROOK:
    return 5;
  case QUEEN:
    return 9;
  case KING:
    return 3.5;
  default:
    return 0;
  }
  return 0;
}

// this is expensive
float center_control(const Position &p, Color c) {
  auto ps = p.pieces(c);
  float sum = popcount(
			(p.attackers_to(SQ_D4) |
			p.attackers_to(SQ_E4) |
			p.attackers_to(SQ_D5) |
			p.attackers_to(SQ_E5))
			& ps);
  return sum * 0.25;
}

float king_safety(const Position &p, Color c) {
  Square ksq = p.square<KING>(c);
  float sum = -popcount(p.attackers_to(ksq) & p.pieces(~c));
  return sum * 0.2;
}

float pawn_structure(const Position &p, Color c) {
  float sum = 0;
  auto pawns = p.pieces(c, PAWN);
  for (auto sq = 0; sq < SQUARE_NB; ++sq) {
    auto s = Square(sq);
    if (s & pawns) {
      sum += popcount(p.attackers_to(s) & pawns);
    }
  }

  return sum * 0.1;
}

inline float eval(const Position &p, Color c) {
  float sum = 0;
  sum += p.count<PAWN>(c);
  //if (sum >= 7) {
	//	sum += center_control(p, c);
	//	sum += king_safety(p, c);
	//}
  sum += 3 * popcount(p.pieces(c, KNIGHT, BISHOP));
  sum += 5 * p.count<ROOK>(c);
  sum += 9 * p.count<QUEEN>(c);
  //sum += pawn_structure(p, c);
  return sum;
}

// returns 0 on equal value
float normalized_eval(const Position &p) {
  return eval(p, p.side_to_move()) - eval(p, ~p.side_to_move());
}

typedef enum { EXACT, UPPERBOUND, LOWERBOUND } entry_flag;
struct Entry {
  Entry() = default;
  int64_t hash;
  bool valid = 0;
  int depth;
  float value;
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

inline float move_val(const Position &p, const Move &m,
                      const Move (&killer)[KILLERS_PER_PLY]) {
  for (auto i = 0; i < KILLERS_PER_PLY; ++i) {
    if (m == killer[i]) {
      return 15;
    }
  }
  switch (type_of(m)) {
  case PROMOTION:
    return 14;
  case CASTLING:
  case ENPASSANT:
    return 13;
  default:
    break;
  }
  auto t = type_of(p.moved_piece(m));
  if (p.capture(m)) {
    constexpr int offset = 6;
    switch (t) {
    case PAWN:
      return offset + 6;
    case BISHOP:
    case KNIGHT:
      return offset + 5;
    case ROOK:
      return offset + 4;
    case QUEEN:
      return offset + 3;
    case KING:
      return offset + 2;
    default:
      return offset + 1;
    }
  }
  return 1;
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

static float g_vals[MAX_MOVES];

inline Ordered ordered_moves(const Position &p) {
  MoveList<LEGAL> list(p);
  Move killer[KILLERS_PER_PLY] = {Move()};
  if (FLAGS_killers) {
    const auto idx = p.game_ply() % KILLERS;
    memcpy(killer, killers[idx], sizeof(Move) * KILLERS_PER_PLY);
  }
  const auto &move_ptr = list.begin();
  const auto N = list.size();
  float largest_value = 0;
  float largest_idx = 0;
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
  const float target = largest_value / FLAGS_order_buckets;
  for (auto k = FLAGS_order_buckets - 1; k >= 0; --k) {
    for (auto i = 0; i < N; ++i) {
      const auto idx = i;//(PRIME * i + 1) % N;
      const float v = g_vals[idx];
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
  std::vector<std::pair<Move, float>> valued;
  out.reserve(list.size());
  valued.reserve(list.size());
  Move killer[KILLERS_PER_PLY] = {Move()};
  for (const auto &m : list) {
    valued.emplace_back(std::make_pair(m, move_val(p, m, killer)));
  }
  std::shuffle(valued.begin(), valued.end(), getRandDevice());
  std::stable_sort(
      valued.begin(), valued.end(),
      [](const std::pair<Move, float> &a, const std::pair<Move, float> &b) {
        return a.second > b.second;
      });
  for (const auto &m : valued) {
    out.emplace_back(m.first);
  }
  return out;
}

// returns value + nodes scanned
std::pair<float, size_t>
negamax(Position &p, int depth, float alpha, float beta,
        const std::chrono::time_point<std::chrono::steady_clock> &start,
        double max_time) {
  std::chrono::duration<double> diff = std::chrono::steady_clock::now() - start;
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

  if (diff.count() > max_time || depth == 0) {
    return std::make_pair(normalized_eval(p), 1);
  }

  auto moves = ordered_moves(p);
  float val = -1e9;
  size_t nodes = 1;

  if (moves.size() == 0) {
    if (p.checkers()) {
      return std::make_pair(-1e9, nodes);
    }
    return std::make_pair(0, nodes);
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
        if (!set) {
          killers[idx][0] = m;
        }
      }
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
  return std::make_pair(0.9 * val, nodes);
}

#define ALPHA 1e9
#define BETA (-ALPHA)

// returns best move and nodes scanned
std::pair<Move, size_t> best_move(Position &p, double max_time) {
  auto start = std::chrono::steady_clock::now();
  auto moves = ordered_moves(p);
  std::vector<Move> best_calc;
  auto init = FLAGS_idfs ? 0 : FLAGS_depth - 1;
  size_t nodes = 0;
  for (auto d = init; d < FLAGS_depth; ++d) {
    Move best = MOVE_NONE;
    float best_v = -1e9;
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
      const auto r = negamax(p, d + 1, -ALPHA, -BETA, start, max_time);
      float val = -r.first;
      nodes += r.second;
      p.undo_move(m);
      if (val > best_v) {
        best = m;
        best_v = val;
      }
    }
    if (completed || best_calc.size() == 0) {
      best_calc.emplace_back(best);
    }
  }
  if (FLAGS_print_depth) {
    std::cout << "depth:\t" << best_calc.size() << "\n";
  }
  // clear a new killers spot
  //memset(killers[(p.game_ply() + 1) % KILLERS], 0, KILLERS_PER_PLY);
  return std::make_pair(best_calc.back(), nodes);
}

int main(int argc, char **argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  UCI::init(Options);
  Bitboards::init();
  Position::init();
  Bitbases::init();
  Threads.set(8);
  Position p;
  StateInfo si;
  p.set(FLAGS_fen, false, &si, Threads.main());
  auto user = 1337;
  if (FLAGS_user == "w" || FLAGS_user == "white") {
    user = WHITE;
  } else if (FLAGS_user == "b" || FLAGS_user == "black") {
    user = BLACK;
  }
  while (true) {
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
