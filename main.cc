#include <iostream>
#include <chrono>
#include <tuple>
#include <algorithm>
#include <random>
#include <unordered_map>
#include <sstream>
#include <optional>
#include <gflags/gflags.h>

#include "thread.h"
#include "position.h"
#include "movegen.h"
#include "uci.h"

DEFINE_bool(cache, false, "Enable cache for negamax");
DEFINE_bool(killers, false, "Enable killer opt for negamax");
DEFINE_int64(cache_size, 1<<20, "Set cache size for negamax");
DEFINE_bool(idfs, true, "Enable iterative depth first search");
DEFINE_int32(order_buckets, 3, "Number of buckets for fast ordering");
DEFINE_bool(print_move, true, "Dump the moves played");
DEFINE_bool(print_user_move, false, "Echo the moves played by the user");
DEFINE_bool(print_time, false, "Show the time used per move");
DEFINE_bool(print_nps, false, "Display the nodes tried per second");
DEFINE_bool(print_board, false, "Dump the board every move");
DEFINE_bool(print_fen, false, "Dump the FEN every move");
DEFINE_bool(print_depth, false, "Dump the depth achieved every move");
DEFINE_int32(depth, 10, "Maximum depth to search per move");
DEFINE_double(max_time, 1.0, "Maximum time to search per move");
DEFINE_double(scale_time, 1.0, "Scale time provided to white");
DEFINE_string(user, "", "User color");
DEFINE_string(fen, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -", "Initial FEN");


std::mt19937& getRandDevice() {
	static std::random_device rd;
	static std::mt19937 g(rd());
	return g;
}


float val(const Piece& p) {
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
float center_control(const Position& p, Color c) {
  float sum = 0;
	sum += popcount(p.attackers_to(SQ_D5) & p.pieces(c));
	sum += popcount(p.attackers_to(SQ_E5) & p.pieces(c));
	sum += popcount(p.attackers_to(SQ_D4) & p.pieces(c));
	sum += popcount(p.attackers_to(SQ_E4) & p.pieces(c));
	return sum * 0.2;
}

float king_safety(const Position& p, Color c) {
  Square ksq = p.square<KING>(c);
  float sum = popcount(p.attackers_to(ksq) & p.pieces(~c));
  auto cr = p.castling_rights(p.side_to_move());
  sum += float(cr & KING_SIDE + cr & QUEEN_SIDE) / 2;
  return sum * 0.2;
}

float pawn_structure(const Position& p, Color c) {
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

inline float eval(const Position& p, Color c) {
  float sum = 0;
  sum += p.count<PAWN>(c);
  sum += 3 * p.count<KNIGHT>(c);
  sum += 3 * p.count<BISHOP>(c);
  sum += 5 * p.count<ROOK>(c);
  sum += 9 * p.count<QUEEN>(c);
  //sum += 3.5 * p.count<KING>(c);
  //sum += center_control(p, c);
  //sum += pawn_structure(p, c);
  //sum += king_safety(p, c);
  return sum;
}


// returns 0 on equal value
float normalized_eval(const Position& p) {
	return eval(p, p.side_to_move());// - eval(p, ~p.side_to_move());
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

std::vector<Entry>& getCache() {
	static std::vector<Entry> cache(FLAGS_cache_size);
  return cache;
}

Entry lookup(Position& p) {
  const auto& cache = getCache();
  const auto hash = p.key();
  const auto idx = hash % cache.size();
  auto entry = cache[idx];
  entry.valid = (entry.hash == hash);
  return entry;
}

void set(Position& p, Entry& e) {
  auto& cache = getCache();
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
#define KILLERS 64
static size_t killer_offset = 0;
static Move killers[KILLERS];

inline float move_val(const Position& p, const Move& m, const Move& killer) {
  if (killer == m) {
    return 15;
	}
  switch (type_of(m)) {
		case PROMOTION:
			return 14;
    case CASTLING:
    case ENPASSANT:
			return 13;
    case NORMAL:
    {
      int offset = 0;
			if (p.capture(m)) {
        offset = 6;
      }
			switch (type_of(p.moved_piece(m))) {
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
			return 1;
		}
		default:
			return 1;
	}
	return 1;
}

#define PRIME 439
struct Ordered {
  inline const Move* begin() const { return ordered; }
  inline const Move* end() const { return last; }
  size_t size() const { return last - ordered; }
	Move ordered[MAX_MOVES], *last;
  void clear() { last = ordered; }
  void insert(Move m) { *last = m; last++; }
};

static Ordered ordered;
static float vals[MAX_MOVES];

inline std::vector<Move> ordered_moves(const Position& p) {
	MoveList<LEGAL> list(p);
  auto killer = Move();
	if (FLAGS_killers) {
		const auto ply = p.game_ply();
		if (ply >= (killer_offset + KILLERS)) {
			killer_offset = ply;
			std::fill_n(killers, KILLERS, Move());
		}
		killer = killers[p.game_ply() - killer_offset];
	}
  const auto& move_ptr = list.begin();
	const auto N = list.size();
  memset(vals, 0, N);
  float largest_value = 0;
  float largest_idx = 0;
  for (auto i = 0; i < N; ++i) {
		auto v = move_val(p, move_ptr[i], killer);
    vals[i] = v;
    if (v > largest_value) {
			largest_value = v;
			largest_idx = i;
		}
	}
  std::vector<Move> out;
	out.reserve(N);

  // we want to iterate through the list 3 times assigning values
  const float target = largest_value / FLAGS_order_buckets;
  for (auto k = FLAGS_order_buckets - 1; k >= 0; --k) {
    for (auto i = 0; i < N; ++i) {
			auto idx = (PRIME * i + 1) % N;
			const float v = vals[idx];
			if (v > (k * target) && v <= ((k + 1) * target)) {
				out.emplace_back(move_ptr[idx]);
			}
		}
	}
  return out;
}

inline std::vector<Move> ordered_moves_slow(const Position& p) {
	MoveList<LEGAL> list(p);

  std::vector<Move> out;
  std::vector<std::pair<Move, float>> valued;
  out.reserve(list.size());
  valued.reserve(list.size());
  for (const auto& m : list) {
		valued.emplace_back(std::make_pair(m, move_val(p, m, Move())));
	}
  std::shuffle(valued.begin(), valued.end(), getRandDevice());
  std::stable_sort(valued.begin(), valued.end(), 
		[](const std::pair<Move, float>& a, const std::pair<Move, float>& b) {
			return a.second > b.second;
		});
  for (const auto& m : valued) {
    out.emplace_back(m.first);
	}
  return out;
}

// returns value + nodes scanned
std::pair<float, size_t> negamax(Position& p, int depth, float alpha, float beta, const std::chrono::time_point<std::chrono::steady_clock>& start, double max_time) {
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

  for (const auto& m : moves) {
    StateInfo si;
    p.do_move(m, si);
		const auto r = negamax(p, depth-1, -beta, -alpha, start, max_time);
		val = std::max(val, -r.first);
		nodes += r.second;
	  p.undo_move(m);
    alpha = std::max(alpha, val);
    if (alpha >= beta) {
			if (FLAGS_killers) {
				killers[p.game_ply() - killer_offset] = m;
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
std::pair<Move, size_t> best_move(Position& p, double max_time) {
	auto start = std::chrono::steady_clock::now();
  auto moves = ordered_moves(p);
  std::vector<Move> best_calc;
  auto init = FLAGS_idfs ? 0 : FLAGS_depth - 1;
  size_t nodes = 0;
	for (auto d = init; d < FLAGS_depth; ++d) {
		Move best;
		float best_v = -1e9;
    bool completed = true;
		for (const Move& m : moves) {
			std::chrono::duration<double> diff = std::chrono::steady_clock::now() - start;
			if (diff.count() > max_time) {
				completed = false;
				break;
			}
			StateInfo si;
			p.do_move(m, si);
      const auto r = negamax(p, d+1, -ALPHA, -BETA, start, max_time);
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
	return std::make_pair(best_calc.back(), nodes);
}

int main(int argc, char** argv) {
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
      for (const auto& _m : moves) {
				if (m == _m) {
          legal = true;
				}
			}
      if (!p.legal(m) || !is_ok(m) || !legal) {
        std::cerr << "illegal move: " << move << "\n";
				continue;
			}
			p.do_move(m, si);
      if (!p.pos_is_ok()) {
				p.undo_move(m);
        std::cerr << "illegal move: " << move << "\n";
        continue;
			}
		} else if (p.side_to_move() == WHITE) {
      MoveList<LEGAL> ms(p);
      if (ms.size() == 0) {
				if (p.checkers()) {
					std::cout << "black wins\n";
				} else {
					std::cout << "stalemate\n";
				}
				break;
			}
			std::tie(m, nodes) = best_move(p, FLAGS_scale_time * FLAGS_max_time);
      p.do_move(m, si);
      assert(p.pos_is_ok());
		} else if (p.side_to_move() == BLACK) {
      MoveList<LEGAL> ms(p);
      if (ms.size() == 0) {
				if (p.checkers()) {
					std::cout << "white wins\n";
				} else {
					std::cout << "stalemate\n";
				}
				break;
			}
			std::tie(m, nodes) = best_move(p, FLAGS_max_time);
      p.do_move(m, si);
      assert(p.pos_is_ok());
		} else {
			assert(0);
		}
    auto end = std::chrono::steady_clock::now();
		std::chrono::duration<double> elapsed_seconds = end-start;
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
	}
}
