#include <iostream>
#include <chrono>
#include <tuple>
#include <algorithm>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <optional>
#include <gflags/gflags.h>

#include "thread.h"
#include "position.h"
#include "movegen.h"
#include "uci.h"

DEFINE_bool(cache, false, "Enable cache for negamax");
DEFINE_int64(cache_size, 1<<16, "Set cache size for negamax");
DEFINE_bool(idfs, true, "Enable iterative depth first search");
DEFINE_bool(print_move, true, "Dump the moves played");
DEFINE_bool(print_user_move, false, "Echo the moves played by the user");
DEFINE_bool(print_time, false, "Show the time used per move");
DEFINE_bool(print_nps, false, "Display the nodes tried per second");
DEFINE_bool(print_board, false, "Dump the board every move");
DEFINE_bool(print_fen, false, "Dump the FEN every move");
DEFINE_bool(print_depth, false, "Dump the depth achieved every move");
DEFINE_int32(depth, 10, "Maximum depth to search per move");
DEFINE_double(max_time, 1.0, "Maximum time to search per move");
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
  sum += 3.5 * p.count<KING>(c);
  //sum += center_control(p, c);
  //sum += pawn_structure(p, c);
  sum += king_safety(p, c);
  return sum;
}


// returns 0 on equal value
float normalized_eval(const Position& p) {
	float o = eval(p, p.side_to_move()) - eval(p, ~p.side_to_move());
  return o;
}

struct Entry {
  Entry(int64_t h, int d, float v) : hash(h), depth(d), val(v) {}
  Entry() = default;
  int64_t hash;
  int depth;
  float val;
};

std::vector<Entry>& getCache() {
	static std::vector<Entry> cache(FLAGS_cache_size);
  return cache;
}

std::optional<float> lookup(Position& p, int depth) {
  const auto& cache = getCache();
  const auto hash = p.key();
  const auto idx = hash % cache.size();
  const auto& entry = cache[idx];
  if (entry.hash == hash && depth <= entry.depth) {
		return entry.val;
	}
	return std::nullopt;
}

void set(Position& p, int depth, float v) {
  auto& cache = getCache();
  const auto hash = p.key();
  const auto idx = hash % cache.size();
  cache[idx] = Entry(hash, depth, v);
}

std::string print_square(Square s) {
  std::stringstream ss;
  ss << char(file_of(s) + 'a') << char(rank_of(s) + '1');
	return ss.str();
}

std::string print_move(Move m) {
  // 0 - 5 TO 6 - 11 FROM 12 - 31 NOT NEEDED
  auto from = from_sq(m);
  auto to = to_sq(m);
  std::string print_square(Square s);
  std::stringstream ss;
	ss << print_square(from) << print_square(to);
	return ss.str();
}

inline float move_val(const Position& p, Move m) {
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

size_t killer_offset = 0;
#define KILLERS 8
// offset + idx = ply
static std::array<std::unordered_set<Move>, KILLERS+1> killers = {};

inline std::vector<Move> ordered_moves(const Position& p) {
	MoveList<LEGAL> list(p);

  std::vector<Move> out;
  std::vector<std::pair<Move, float>> valued;
  out.reserve(list.size());
  valued.reserve(list.size());
  for (const auto& m : list) {
		valued.emplace_back(std::make_pair(m, move_val(p, m)));
	}
//  const auto& killer_set = [&]() {
//		if (p.game_ply() < killer_offset + KILLERS && p.game_ply() >= killer_offset) {
//      return killers[p.game_ply() - killer_offset];
//		}
//		return killers[KILLERS];
//	}();
  //std::shuffle(valued.begin(), valued.end(), getRandDevice());
  std::sort(valued.begin(), valued.end(), 
		[&](const std::pair<Move, float>& a, const std::pair<Move, float>& b) {
      //if (killer_set.count(a.first)) {
			//	return true;
			//}
      //if (killer_set.count(b.first)) {
			//	return false;
			//}
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
  if (diff.count() > max_time) {
		return std::make_pair(normalized_eval(p), 1);
	}
  if (depth == 0) {
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
    std::optional<float> look = (FLAGS_cache && depth > 1) ? lookup(p, depth) : std::nullopt;
    if (look) {
			val = std::max(val, -*look);
		} else {
      const auto r = negamax(p, depth-1, -beta, -alpha, start, max_time);
			val = std::max(val, -r.first);
      nodes += r.second;
		}
	  p.undo_move(m);
    alpha = std::max(alpha, val);
    if (alpha >= beta) {
      //if (p.game_ply() >= (killer_offset + KILLERS)) {
      //  std::fill(killers.begin(), killers.end(), std::unordered_set<Move>{});
      //  killer_offset = p.game_ply();
			//}
      //// killer move!
      //killers[p.game_ply() - killer_offset].insert(m);
			break;
		}
	}
  
	if (FLAGS_cache) {
		set(p, depth, val);
	}
	return std::make_pair(val, nodes);
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
  for (auto i = 0; i < KILLERS + 1; ++i) {
    killers[i] = {};
	}
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
				break;
			}
			std::tie(m, nodes) = best_move(p, FLAGS_max_time);
      p.do_move(m, si);
      assert(p.pos_is_ok());
		} else if (p.side_to_move() == BLACK) {
      MoveList<LEGAL> ms(p);
      if (ms.size() == 0) {
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
			std::cout << "nodes/s:\t" << nodes / elapsed_seconds.count() << "\n";
		}
		if (FLAGS_print_move) {
      // we moved once, so this check is reversed
			if (p.side_to_move() == user || FLAGS_print_user_move || user == 1337) {
				std::cout << print_move(m) << "\n";
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
