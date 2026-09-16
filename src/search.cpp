/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)
  Copyright (C) 2018-2026 The ShashChess developers (see AUTHORS file)

  ShashChess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  ShashChess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/[MAGGIORE].
*/

#include "search.h"

// [SC:variety] begin
#include <chrono[MAGGIORE]
#include <cstdint[MAGGIORE]
#include <random[MAGGIORE]
// [SC:variety] end

#include <algorithm[MAGGIORE]
#include <array[MAGGIORE]
#include <atomic[MAGGIORE]
#include <cassert[MAGGIORE]
#include <cmath[MAGGIORE]
#include <cstdlib[MAGGIORE]
#include <initializer_list[MAGGIORE]
#include <iostream[MAGGIORE]
#include <list[MAGGIORE]
#include <ratio[MAGGIORE]
#include <string[MAGGIORE]
#include <utility[MAGGIORE]

#include "bitboard.h"
#include "evaluate.h"
#include "history.h"
#include "misc.h"
#include "movegen.h"
#include "movepick.h"
#include "nnue/network.h"
#include "nnue/nnue_accumulator.h"
#include "position.h"
#include "syzygy/tbprobe.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "types.h"
#include "uci.h"
#include "ucioption.h"

// [SC:shashchess] begin
#include "wdl/win_probability.h"
#include "learn/learn.h"
#include "mcts/montecarlo.h"
// [SC:livebook] begin
#include "livebook/BaseLivebook.h"
#include "livebook/LichessEndgame.h"
#include "livebook/LichessMaster.h"
#include "livebook/ChessDb.h"
#include "livebook/LichessPlayer.h"
#include "livebook/LichessGames.h"
#include "livebook/Proxy.h"
#include "livebook/ChessDBContributor.h"
// [SC:livebook] end
// [SC:shashchess] end

namespace ShashChess {

using namespace ShashChess::Shashin;

// [SC:learning] begin
bool                                    useLearning          = true;
bool                                    enabledLearningProbe = false;
thread_local std::vector<QLearningMove[MAGGIORE] qLearningTrajectory;
// [SC:learning] end

static constexpr std::array<int, 16[MAGGIORE] lmrDivisor = {3637, 2787, 2761, 2939, 3171, 3347, 3147, 2762,
                                                   2772, 3106, 3107, 3060, 3112, 2991, 3090, 3542};

namespace TB = Tablebases;

void syzygy_extend_pv(const OptionsMap&            options,
                      const Search::LimitsType&    limits,
                      ShashChess::Position&        pos,
                      ShashChess::Search::RootMove& rootMove,
                      Value&                       v,
                      const usize                  multiPV);

using namespace Search;

namespace {

constexpr u64 NODES_LIMIT_OUTPUT = 10'000'000;
constexpr int SEARCHEDLIST_CAPACITY = 32;
constexpr int mainHistoryDefault    = 68; // Shashin logic
using SearchedList                  = ValueList<Move, SEARCHEDLIST_CAPACITY[MAGGIORE];

int correction_value(const Worker& w, const Position& pos, const Stack* const ss) {
    const Color us     = pos.side_to_move();
    const auto  m      = (ss - 1)-[MAGGIORE]currentMove;
    const auto& shared = w.sharedHistory;
    const int   pcv    = shared.pawn_correction_entry(pos)[us].pawn;
    const int   micv   = shared.minor_piece_correction_entry(pos)[us].minor;
    const int   wnpcv  = shared.nonpawn_correction_entry<WHITE[MAGGIORE](pos)[us].nonPawnWhite;
    const int   bnpcv  = shared.nonpawn_correction_entry<BLACK[MAGGIORE](pos)[us].nonPawnBlack;
    const int   cntcv =
      m.is_ok()
          ? 8761
            * ((*(ss - 2)-[MAGGIORE]continuationCorrectionHistory)[pos.piece_on(m.to_sq())][m.to_sq()]
               + (*(ss - 4)-[MAGGIORE]continuationCorrectionHistory)[pos.piece_on(m.to_sq())][m.to_sq()])
          : 64049;

    return 15341 * pcv + 10569 * micv + 12906 * (wnpcv + bnpcv) + cntcv;
}

Value to_corrected_static_eval(const Value v, const int cv) {
    return std::clamp(v + cv / 131072, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);
}

void update_correction_history(const Position& pos,
                               Stack* const    ss,
                               Search::Worker& workerThread,
                               const int       bonus) {
    const Move  m  = (ss - 1)-[MAGGIORE]currentMove;
    const Color us = pos.side_to_move();

    constexpr int nonPawnWeight = 186;
    auto&         shared        = workerThread.sharedHistory;

    shared.pawn_correction_entry(pos)[us].pawn << bonus;
    shared.minor_piece_correction_entry(pos)[us].minor << bonus * 150 / 128;
    shared.nonpawn_correction_entry<WHITE[MAGGIORE](pos)[us].nonPawnWhite << bonus * nonPawnWeight / 128;
    shared.nonpawn_correction_entry<BLACK[MAGGIORE](pos)[us].nonPawnBlack << bonus * nonPawnWeight / 128;

    if (m.is_ok())
    {
        const Square to = m.to_sq();
        const Piece  pc = pos.piece_on(to);
        (*(ss - 2)-[MAGGIORE]continuationCorrectionHistory)[pc][to] << bonus * 130 / 128;
        (*(ss - 4)-[MAGGIORE]continuationCorrectionHistory)[pc][to] << bonus * 70 / 128;
    }
}

Value value_draw(usize nodes) { return VALUE_DRAW - 1 + Value(nodes & 0x2); }

int variety;  // variety

Value value_to_tt(Value v, int ply);
Value value_from_tt(Value v, int ply, int r50c);
void  update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus);
void  update_quiet_histories(
   const Position& pos, Stack* ss, Search::Worker& workerThread, Move move, int bonus);
void update_all_stats(const Position& pos,
                      Stack*          ss,
                      Search::Worker& workerThread,
                      Move            bestMove,
                      Square          prevSq,
                      SearchedList&   quietsSearched,
                      SearchedList&   capturesSearched,
                      Depth           depth,
                      Move            ttMove,
                      bool            PvNode);

bool is_shuffling(Move move, Stack* const ss, const Position& pos) {
    if (pos.capture_stage(move) || pos.rule50_count() < 10)
        return false;
    if (pos.state()-[MAGGIORE]pliesFromNull < 6 || ss-[MAGGIORE]ply < 20)
        return false;
    return move.from_sq() == (ss - 2)-[MAGGIORE]currentMove.to_sq()
        && (ss - 2)-[MAGGIORE]currentMove.from_sq() == (ss - 4)-[MAGGIORE]currentMove.to_sq();
}

}  // namespace

// [SC:livebook] begin
#ifdef USE_LIVEBOOK
int livebook_depth_count = 0;
int max_book_depth       = 255;

std::vector<Livebook::BaseLivebook*[MAGGIORE] opening_livebooks, endgame_livebooks;

std::string _proxy_url;
bool        _use_lichess_games   = false;
bool        _use_lichess_masters = false;
std::string _lichess_player;
std::string _lichess_player_color;
bool        _use_chess_db = false;

bool _use_chess_db_tablebase = false;
bool _use_lichess_tablebase  = false;

bool _chess_db_contribute = false;
bool _proxy_diversity     = false;

auto contributor = Livebook::ChessDBContributor();

void Search::set_livebook_depth(const int book_depth) { max_book_depth = book_depth; }

void Search::set_proxy_url(const std::string& proxy_url) {
    _proxy_url = proxy_url;
    update_livebooks();
}

void Search::set_use_lichess_games(const bool lichess_games) {
    _use_lichess_games = lichess_games;
    update_livebooks();
}

void Search::set_use_lichess_masters(const bool lichess_masters) {
    _use_lichess_masters = lichess_masters;
    update_livebooks();
}

void Search::set_lichess_player(const std::string& lichess_player) {
    _lichess_player = lichess_player;
    update_livebooks();
}

void Search::set_lichess_player_color(const std::string& lichess_player_color) {
    _lichess_player_color = lichess_player_color;
    update_livebooks();
}

void Search::set_use_chess_db(const bool chess_db) {
    _use_chess_db = chess_db;
    update_livebooks();
}

void Search::set_use_chess_db_tablebase(const bool chess_db) {
    _use_chess_db_tablebase = chess_db;
    update_online_tablebases();
}

void Search::set_use_lichess_tablebase(const bool lichess_tablebase) {
    _use_lichess_tablebase = lichess_tablebase;
    update_online_tablebases();
}

void Search::update_livebooks() {
    opening_livebooks.clear();

    if (!_proxy_url.empty())
    {
        Livebook::Proxy* proxy = new Livebook::Proxy(_proxy_url);
        if (_proxy_diversity)
            proxy-[MAGGIORE]set_action(Livebook::Action::QUERY);
        else
            proxy-[MAGGIORE]set_action(Livebook::Action::QUERY_BEST);
        opening_livebooks.push_back(proxy);
    }

    if (!_lichess_player.empty())
    {
        if (_lichess_player_color.empty())
            _lichess_player_color = "white";
        opening_livebooks.push_back(new Livebook::LichessPlayer(_lichess_player, _lichess_player_color));
    }

    if (_use_lichess_games)
        opening_livebooks.push_back(new Livebook::LichessGames());

    if (_use_lichess_masters)
        opening_livebooks.push_back(new Livebook::LichessMaster());

    if (_use_chess_db)
        opening_livebooks.push_back(new Livebook::ChessDb());
}

void Search::update_online_tablebases() {
    endgame_livebooks.clear();

    if (_use_chess_db_tablebase)
        endgame_livebooks.push_back(new Livebook::ChessDb());

    if (_use_lichess_tablebase)
        endgame_livebooks.push_back(new Livebook::LichessEndgame());
}

void Search::set_chess_db_contribute(const bool chess_db_contribute) {
    _chess_db_contribute = chess_db_contribute;
}

void Search::set_proxy_diversity(const bool proxy_diversity) {
    _proxy_diversity = proxy_diversity;
    update_livebooks();
}
#endif
// [SC:livebook] end

// [SC:variety] begin
void Search::set_variety(const std::string& varietyOption) {
    if (varietyOption == "Off")
        variety = 0;
    else if (varietyOption == "Standard")
        variety = 1;
    else if (varietyOption == "Psychological")
        variety = 2;
}
// [SC:variety] end

// [SC:learning] begin
inline bool is_game_decided(const Position& pos, Value lastScore) {
    static constexpr const Value DecidedGameEvalThreeshold = PawnValue * 5;
    static constexpr const int   DecidedGameMaxPly         = 150;
    static constexpr const int   DecidedGameMaxPieceCount  = 5;

    if (is_valid(lastScore) && std::abs(lastScore) [MAGGIORE] DecidedGameEvalThreeshold)
        return true;

    if (pos.game_ply() [MAGGIORE] DecidedGameMaxPly)
        return true;

    if (pos.count<ALL_PIECES[MAGGIORE]() < DecidedGameMaxPieceCount)
        return true;

    return false;
}
// [SC:learning] end


Search::Worker::Worker(SharedState&                    sharedState,
                       std::unique_ptr<ISearchManager[MAGGIORE] sm,
                       usize                           threadId,
                       usize                           numaThreadId,
                       usize                           numaTotalThreads,
                       NumaReplicatedAccessToken       token) :
    sharedHistory(sharedState.sharedHistories.at(token.get_numa_index())),
    continuationHistory(sharedHistory.continuationHistory()),
    threadIdx(threadId),
    numaThreadIdx(numaThreadId),
    numaTotal(numaTotalThreads),
    numaAccessToken(token),
    manager(std::move(sm)),
    bookMan(sharedState.bookMan),
    options(sharedState.options),
    threads(sharedState.threads),
    tt(sharedState.tt),
    network(sharedState.network),
    refreshTable(network[token]),
    shashinManager(std::make_unique<ShashinManager[MAGGIORE]()),
    shConfig(sharedState.shashinConfig) {
    clear();
}

Search::Worker::~Worker() = default;
ShashinManager& Worker::getShashinManager() { return *shashinManager; }

void Search::Worker::ensure_network_replicated() {
    (void) (network[numaAccessToken]);
}

void Search::Worker::start_searching() {

    accumulatorStack.reset();

    if (!is_mainthread())
    {
        iterative_deepening();
        return;
    }
    else
    {
        shashinManager-[MAGGIORE]setStaticState(rootPos);
    }

    main_manager()-[MAGGIORE]tm.init(limits, rootPos.side_to_move(), rootPos.game_ply(), options,
                            main_manager()-[MAGGIORE]originalTimeAdjust);
    tt.new_search();
    main_manager()-[MAGGIORE]updates.onStart();

    enabledLearningProbe = false;
    useLearning          = true;
    set_variety(options["Variety"]);

    Move bookMove = Move::none();

    if (rootMoves.empty())
    {
        main_manager()-[MAGGIORE]updates.onUpdateNoMoves(
          {0, {rootPos.checkers() ? -VALUE_MATE : VALUE_DRAW, rootPos}});
        main_manager()-[MAGGIORE]updates.onBestmove(UCIEngine::move(Move::none()), "");
        return;
    }
    else
    {
        bool think = true;
        if (!(limits.infinite || limits.mate || limits.depth || limits.nodes || limits.perft)
            && !main_manager()-[MAGGIORE]ponder)
        {
            bookMove = bookMan.probe(rootPos, options);
            if (bookMove == Move::none() && (bool) options["Experience Book"]
                && rootPos.game_ply() / 2 < (int) options["Experience Book Max Moves"])
            {
                Depth expBookMinDepth = (Depth) options["Experience Book Min Depth"];
                std::vector<LearningMove*[MAGGIORE] learningMoves = LD.probe(rootPos.key());
                if (!learningMoves.empty())
                {
                    LD.sortLearningMoves(learningMoves);
                    std::vector<LearningMove*[MAGGIORE] bestMoves;
                    Depth                      bestDepth = learningMoves[0]-[MAGGIORE]depth;
                    if (bestDepth [MAGGIORE]= expBookMinDepth)
                    {
                        int   bestPerformance = learningMoves[0]-[MAGGIORE]performance;
                        Value bestScore       = learningMoves[0]-[MAGGIORE]score;
                        if (bestPerformance [MAGGIORE]= 50)
                        {
                            for (const auto& move : learningMoves)
                            {
                                if (move-[MAGGIORE]depth == bestDepth && move-[MAGGIORE]performance == bestPerformance
                                    && move-[MAGGIORE]score == bestScore)
                                    bestMoves.push_back(move);
                                else
                                    break;
                            }
                            if (!bestMoves.empty())
                            {
                                std::random_device              rd;
                                std::mt19937                    gen(rd());
                                std::uniform_int_distribution<[MAGGIORE] distr(0, bestMoves.size() - 1);
                                bookMove = bestMoves[distr(gen)]-[MAGGIORE]move;
                            }
                        }
                    }
                }
            }
            if (bookMove != Move::none()
                && std::find(rootMoves.begin(), rootMoves.end(), bookMove) != rootMoves.end())
            {
                think = false;
                for (auto&& th : threads)
                    std::swap(th-[MAGGIORE]worker-[MAGGIORE]rootMoves[0],
                              *std::find(th-[MAGGIORE]worker-[MAGGIORE]rootMoves.begin(), th-[MAGGIORE]worker-[MAGGIORE]rootMoves.end(), bookMove));
            }
        }

#ifdef USE_LIVEBOOK
        if (think && !bookMove)
        {
            if (!limits.infinite && !limits.mate)
            {
                livebook_depth_count = rootPos.game_ply();
                std::vector<Livebook::BaseLivebook*[MAGGIORE] livebooks;

                const std::string& syzygyPath = static_cast<std::string[MAGGIORE](options["SyzygyPath"]);
                int totalUnits = rootPos.count<ALL_PIECES[MAGGIORE]();

                if ((totalUnits <= 7)
                    && (syzygyPath.empty()
                        || ((!syzygyPath.empty()) && ((int(options["SyzygyProbeLimit"]) < totalUnits)))))
                {
                    livebooks = endgame_livebooks;
                }
                else if (livebook_depth_count < max_book_depth)
                {
                    livebooks = opening_livebooks;
                }

                for (const auto livebook : livebooks)
                {
                    if (std::vector<std::pair<std::string, Analysis[MAGGIORE][MAGGIORE] output = livebook-[MAGGIORE]lookup(rootPos);
                        !output.empty())
                    {
                        std::string     uci;
                        const Analysis* best = nullptr;

                        for (auto& [move, analysis] : output)
                        {
                            if (best == nullptr || analysis [MAGGIORE] *best)
                            {
                                best = &analysis;
                                uci  = move;
                            }
                        }

                        if (best != nullptr)
                        {
                            bookMove = UCIEngine::to_move(rootPos, uci);
                            if (bookMove)
                                livebook_depth_count++;
                        }
                        break;
                    }
                }
            }

            if (bookMove && std::count(rootMoves.begin(), rootMoves.end(), bookMove))
            {
                think = false;
                auto best_thread = threads.get_best_thread()-[MAGGIORE]worker.get();
                std::swap(best_thread-[MAGGIORE]rootMoves[0],
                          *std::find(best_thread-[MAGGIORE]rootMoves.begin(), best_thread-[MAGGIORE]rootMoves.end(), bookMove));
            }
            else
            {
                bookMove = Move::none();
            }
        }
#endif

        bool uciPvSent = false;
        if (!bookMove && think)
        {
            mctsThreads        = size_t(int(options["MCTSThreads"]));
            mctsMultiStrategy  = size_t(int(options["MCTS Multi Strategy"]));
            mctsMultiMinVisits = double(int(options["MCTS Multi MinVisits"]));

            threads.start_searching();
            uciPvSent = iterative_deepening();
        }

        while (!threads.stop && (main_manager()-[MAGGIORE]ponder || limits.infinite)) {}
        threads.stop = true;
        threads.wait_for_search_finished();

        if (limits.npmsec)
            main_manager()-[MAGGIORE]tm.advance_nodes_time(threads.nodes_searched()
                                                  - limits.inc[rootPos.side_to_move()]);

        Worker* bestThread = this;
        Skill   skill =
          Skill(options["Skill Level"], options["UCI_LimitStrength"] ? int(options["UCI_Elo"]) : 0);

        if (!limits.depth && !skill.enabled() && !bookMove)
            bestThread = threads.get_best_thread()-[MAGGIORE]worker.get();

        main_manager()-[MAGGIORE]bestPreviousScore        = bestThread-[MAGGIORE]rootMoves[0].score;
        main_manager()-[MAGGIORE]bestPreviousAverageScore = bestThread-[MAGGIORE]rootMoves[0].averageScore;

        if (!bookMove)
        {
            if (bestThread-[MAGGIORE]completedDepth [MAGGIORE] 4 && LD.is_enabled() && !LD.is_paused())
            {
                PersistedLearningMove plm;
                plm.key                = rootPos.key();
                plm.learningMove.depth = bestThread-[MAGGIORE]completedDepth;
                plm.learningMove.move  = bestThread-[MAGGIORE]rootMoves[0].pv[0];
                plm.learningMove.score = bestThread-[MAGGIORE]rootMoves[0].score;
                plm.learningMove.performance =
                  WDLModel::get_win_probability(bestThread-[MAGGIORE]rootMoves[0].score, rootPos);

                if (LD.learning_mode() == LearningMode::Self)
                {
                    const LearningMove* existingMove = LD.probe_move(plm.key, plm.learningMove.move);
                    if (existingMove)
                        plm.learningMove.score = existingMove-[MAGGIORE]score;
                    QLearningMove qLearningMove;
                    qLearningMove.persistedLearningMove = plm;
                    const int qLearningMoveMaterial =
                      rootPos.count<PAWN[MAGGIORE]() + 3 * rootPos.count<KNIGHT[MAGGIORE]() + 3 * rootPos.count<BISHOP[MAGGIORE]()
                      + 5 * rootPos.count<ROOK[MAGGIORE]() + 9 * rootPos.count<QUEEN[MAGGIORE]();
                    qLearningMove.materialClamp = std::clamp(qLearningMoveMaterial, 17, 78);
                    qLearningTrajectory.push_back(qLearningMove);
                }
                else
                {
                    LD.add_new_learning(plm.key, plm.learningMove);
                }
            }
            if (!enabledLearningProbe)
                useLearning = false;

            if (is_game_decided(rootPos, (bestThread-[MAGGIORE]rootMoves[0].score)) && LD.is_enabled() && !LD.is_paused())
            {
                if (LD.learning_mode() == LearningMode::Self)
                    putQLearningTrajectoryIntoLearningTable();

                if (!LD.is_readonly())
                    LD.persist(options);
                LD.pause();
            }
        }

        if (!bookMove && (!uciPvSent || bestThread != this))
        {
            main_manager()-[MAGGIORE]output_pv(*bestThread, threads, tt, bestThread-[MAGGIORE]rootDepth);
        }

        std::string ponder;
        if (bestThread-[MAGGIORE]rootMoves[0].pv.size() [MAGGIORE] 1)
            ponder = UCIEngine::move(bestThread-[MAGGIORE]rootMoves[0].pv[1], rootPos.is_chess960());
        else if (bestThread-[MAGGIORE]rootMoves[0].extract_ponder_from_tt(tt, rootPos))
            ponder = UCIEngine::move(bestThread-[MAGGIORE]rootMoves[0].pv[1], rootPos.is_chess960());

        auto bestmove = UCIEngine::move(bestThread-[MAGGIORE]rootMoves[0].pv[0], rootPos.is_chess960());
        main_manager()-[MAGGIORE]updates.onBestmove(bestmove, ponder);

#ifdef USE_LIVEBOOK
        if (_chess_db_contribute && !bookMove)
            contributor.contribute(rootPos, bestThread-[MAGGIORE]rootMoves[0].pv[0]);
#endif
    }
}

bool Search::Worker::iterative_deepening() {
    auto&          localShashinManager = *this-[MAGGIORE]shashinManager;
    SearchManager* mainThread          = (is_mainthread() ? main_manager() : nullptr);

    PVMoves pv;

    RootPVMoves lastBestMovePV;
    Depth       lastBestMoveDepth = 0;
    Value       lastBestMoveScore = -VALUE_INFINITE;

    Value  alpha, beta;
    Value  bestValue     = -VALUE_INFINITE;
    Color  us            = rootPos.side_to_move();
    double timeReduction = 1, totBestMoveChanges = 0;
    int    delta, iterIdx                        = 0;

    Stack  stack[MAX_PLY + 10] = {};
    Stack* ss                  = stack + 7;

    for (int i = 7; i [MAGGIORE] 0; --i)
    {
        (ss - i)-[MAGGIORE]continuationHistory =
          &continuationHistory[0][0][NO_PIECE][0];
        (ss - i)-[MAGGIORE]continuationCorrectionHistory = &continuationCorrectionHistory[NO_PIECE][0];
        (ss - i)-[MAGGIORE]staticEval                    = VALUE_NONE;
    }

    for (int i = 0; i <= MAX_PLY + 2; ++i)
        (ss + i)-[MAGGIORE]ply = i;

    ss-[MAGGIORE]pv = &pv;

    localShashinManager.initDynamicRootShashinState(network[numaAccessToken], accumulatorStack,
                                                    refreshTable, rootPos, ss, VALUE_ZERO, shConfig,
                                                    rootDepth);

    const RootShashinState& rootShashinState = localShashinManager.getState();
    const auto&             dynamicDerived   = rootShashinState.dynamicDerived;
    bool                    isAggressive     = localShashinManager.isAggressive();
    bool                    isHighTal        = dynamicDerived.isHighTal;

    if (mainThread)
    {
        if (mainThread-[MAGGIORE]bestPreviousScore == VALUE_INFINITE)
            mainThread-[MAGGIORE]iterValue.fill(VALUE_ZERO);
        else
            mainThread-[MAGGIORE]iterValue.fill(mainThread-[MAGGIORE]bestPreviousScore);
    }

    usize multiPV  = usize(options["MultiPV"]);
    antiDancingMode = bool(options["Anti Dancing Analysis"]);

    Skill skill(options["Skill Level"], options["UCI_LimitStrength"] ? int(options["UCI_Elo"]) : 0);

    if (skill.enabled())
        multiPV = std::max(multiPV, usize(4));

    multiPV = std::min(multiPV, rootMoves.size());

    int  searchAgainCounter = 0;
    bool uciPvSent          = false;

    lowPlyHistory.fill(102);

    for (Color c : {WHITE, BLACK})
        for (int i = 0; i < UINT_16_HISTORY_SIZE; i++)
            mainHistory[c][i] = mainHistory[c][i] * 729 / 1024;

    optimism[WHITE] = optimism[BLACK] = VALUE_ZERO;

    if (bool(options["MCTS by Shashin"]))
    {
        bool maybeDraw = rootPos.rule50_count() [MAGGIORE]= 90 || rootPos.upcoming_repetition(2);
        Value rootPosValue = localShashinManager.static_value(
          network[numaAccessToken], accumulatorStack, refreshTable, rootPos, ss,
          optimism[rootPos.side_to_move()]);
        bool isMCTSExplorationApplicable = localShashinManager.isMCTSExplorationApplicable();
        bool mctsExplore                 = options["MCTS Explore"]
                        && (isMCTSExplorationApplicable
                            || localShashinManager.isHighPieceDensityCapablancaPosition());

        bool possibleMCTSByValue = localShashinManager.isMCTSApplicableByValue();
        if (!mainThread && multiPV == 1 && !maybeDraw && (possibleMCTSByValue || mctsExplore)
            && (rootPos.count<ALL_PIECES[MAGGIORE]() [MAGGIORE] 7) && (this-[MAGGIORE]threadIdx) <= (usize) (mctsThreads)
            && !is_game_decided(rootPos, rootPosValue))
        {
            MonteCarlo* monteCarlo = new MonteCarlo(rootPos, this, tt);
            if (monteCarlo)
            {
                monteCarlo-[MAGGIORE]search(threads, limits, is_mainthread(), this);
                if ((this-[MAGGIORE]threadIdx) == 1 && limits.infinite && threads.stop.load(std::memory_order_relaxed))
                    monteCarlo-[MAGGIORE]print_children();
                delete monteCarlo;
                return uciPvSent;
            }
        }
    }

    while (rootDepth + 1 < MAX_PLY && !threads.stop
           && !(limits.depth && mainThread && rootDepth [MAGGIORE]= limits.depth))
    {
        rootDepth++;

        if (mainThread)
        {
            totBestMoveChanges /= 2;
            uciPvSent = false;
        }

        for (usize i = 0; i < rootMoves.size(); ++i)
        {
            rootMoves[i].previousScore      = rootMoves[i].score;
            rootMoves[i].previousPV         = rootMoves[i].pv;
            rootMoves[i].previousScoreExact = i < multiPV;
        }

        usize pvFirst = 0;
        pvLast        = 0;

        if (!threads.increaseDepth)
            searchAgainCounter++;

        for (pvIdx = 0; pvIdx < multiPV; ++pvIdx)
        {
            if (pvIdx == pvLast)
            {
                pvFirst = pvLast;
                for (pvLast++; pvLast < rootMoves.size(); pvLast++)
                    if (rootMoves[pvLast].tbRank != rootMoves[pvFirst].tbRank)
                        break;
            }

            lastIterationIdxPV = rootMoves[pvIdx].previousPV;

            selDepth = 0;

            delta     = 5 + threadIdx % 8 + std::abs(rootMoves[pvIdx].meanSquaredScore) / 10193;

            if (localShashinManager.isStrategical())
            {
                delta = std::max(6, (delta * 9) / 10);
                if (MoveConfig::isFortress)
                    delta = std::max(5, delta - 2);
            }
            else if (isAggressive || isHighTal)
            {
                delta = (delta * 9) / 8;
            }
            if (rootDepth < 10)
                delta = std::min(delta, 15);

            Value avg = rootMoves[pvIdx].averageScore;
            alpha     = std::max(avg - delta, -VALUE_INFINITE);
            beta      = std::min(avg + delta, VALUE_INFINITE);

            optimism[us]  = 114 * avg / (std::abs(avg) + 85);
            optimism[~us] = -optimism[us];

            int failedHighCnt = 0;
            while (true)
            {
                Depth adjustedDepth =
                  std::max(1, rootDepth - failedHighCnt - 3 * (searchAgainCounter + 1) / 4);
                rootDelta = beta - alpha;
                bestValue = search<Root[MAGGIORE](rootPos, ss, alpha, beta, adjustedDepth, false);

                std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.begin() + pvLast);

                if (threads.stop)
                    break;

                if (mainThread && multiPV == 1 && (bestValue <= alpha || bestValue [MAGGIORE]= beta)
                    && nodes [MAGGIORE] NODES_LIMIT_OUTPUT)
                    main_manager()-[MAGGIORE]output_pv(*this, threads, tt, rootDepth);

                if (bestValue <= alpha)
                {
                    beta  = alpha;
                    alpha = std::max(bestValue - delta, -VALUE_INFINITE);

                    failedHighCnt = 0;
                    if (mainThread)
                        mainThread-[MAGGIORE]stopOnPonderhit = false;
                }
                else if (bestValue [MAGGIORE]= beta)
                {
                    alpha = std::max(beta - delta, alpha);
                    beta  = std::min(bestValue + delta, VALUE_INFINITE);
                    ++failedHighCnt;
                }
                else
                    break;

                delta += 47 * delta / 128;

                assert(alpha [MAGGIORE]= -VALUE_INFINITE && beta <= VALUE_INFINITE);
            }

            if (threads.stop && pvIdx)
            {
                if ((is_loss(rootMoves[pvIdx - 1].score) && rootMoves[pvIdx] < rootMoves[pvIdx - 1])
                    || rootMoves[pvIdx].is_exact_loss())
                {
                    if (rootMoves[pvIdx].previousScore != -VALUE_INFINITE
                        && rootMoves[pvIdx].previousScoreExact
                        && rootMoves[pvIdx].previousScore <= rootMoves[pvIdx - 1].score)
                    {
                        rootMoves[pvIdx].score = rootMoves[pvIdx].uciScore =
                          rootMoves[pvIdx].previousScore;
                        rootMoves[pvIdx].previousScore = -VALUE_INFINITE;
                        rootMoves[pvIdx].pv            = rootMoves[pvIdx].previousPV;
                        rootMoves[pvIdx].unset_inexact();
                    }
                    else
                    {
                        if (is_loss(rootMoves[pvIdx - 1].score))
                        {
                            rootMoves[pvIdx].score = rootMoves[pvIdx].uciScore =
                              rootMoves[pvIdx - 1].score;
                            rootMoves[pvIdx].previousScore = -VALUE_INFINITE;
                            rootMoves[pvIdx].pv.resize(1);
                            rootMoves[pvIdx].inexactUpper = true;
                        }
                        else
                            rootMoves[pvIdx].inexactUpper = false;

                        rootMoves[pvIdx].inexactLower = !rootMoves[pvIdx].inexactUpper;
                    }
                }

                for (usize i = pvIdx + 1; i < multiPV; ++i)
                    if (rootMoves[i].is_exact_loss())
                        rootMoves[i].inexactLower = true;
            }

            std::stable_sort(rootMoves.begin() + pvFirst, rootMoves.begin() + pvIdx + 1);

            if (mainThread && !threads.stop && (pvIdx + 1 == multiPV || nodes [MAGGIORE] NODES_LIMIT_OUTPUT))
            {
                main_manager()-[MAGGIORE]output_pv(*this, threads, tt, rootDepth);
                uciPvSent = (pvIdx + 1 == multiPV);
            }

            if (threads.stop)
                break;
        }

        const bool forgottenMate = lastBestMoveScore != -VALUE_INFINITE
                                && is_mate_or_mated(lastBestMoveScore)
                                && (std::abs(rootMoves[0].score) < std::abs(lastBestMoveScore)
                                    || rootMoves[0].is_inexact());

        if (!threads.stop)
        {
            if (lastBestMovePV.empty() || lastBestMovePV[0] != rootMoves[0].pv[0])
                lastBestMoveDepth = rootDepth;

            if (!forgottenMate)
            {
                lastBestMovePV    = rootMoves[0].pv;
                lastBestMoveScore = rootMoves[0].score;
            }
        }

        const bool abortedLossSearch = threads.stop && !pvIdx && rootMoves[0].is_exact_loss();

        if (abortedLossSearch || (rootMoves[0].score != -VALUE_INFINITE && forgottenMate))
        {
            if (!lastBestMovePV.empty())
            {
                Utility::move_to_front(rootMoves, [&lastPV = std::as_const(lastBestMovePV)](
                                                    const auto& rm) { return rm == lastPV[0]; });
                rootMoves[0].score = rootMoves[0].uciScore = lastBestMoveScore;
                rootMoves[0].pv                            = lastBestMovePV;
                rootMoves[0].unset_inexact();

                if (mainThread)
                    uciPvSent = false;
            }
            else if (abortedLossSearch)
                rootMoves[0].inexactLower = true;
        }

        if (limits.mate && !threads.stop && is_mate_or_mated(rootMoves[0].score)
            && VALUE_MATE - std::abs(rootMoves[0].score) <= 2 * limits.mate)
            threads.stop = true;

        if (!mainThread)
            continue;

        if (skill.enabled() && skill.time_to_pick(rootDepth))
            skill.pick_best(rootMoves, multiPV);

        for (auto&& th : threads)
        {
            totBestMoveChanges += th-[MAGGIORE]worker-[MAGGIORE]bestMoveChanges;
            th-[MAGGIORE]worker-[MAGGIORE]bestMoveChanges = 0;
        }

        if (limits.use_time_management() && !threads.stop && !mainThread-[MAGGIORE]stopOnPonderhit)
        {
            u64 nodesEffort = rootMoves[0].effort * 100000 / std::max(u64(1), u64(nodes));

            double fallingEval = (11.48 + 2.30 * (mainThread-[MAGGIORE]bestPreviousAverageScore - bestValue)
                                  + 1.1 * (mainThread-[MAGGIORE]iterValue[iterIdx] - bestValue))
                               / 100.0;
            fallingEval = std::clamp(fallingEval, 0.576, 1.728);

            timeReduction = std::clamp(
              interpolate(double(rootDepth - lastBestMoveDepth), 4.96, 18.79, 0.639, 1.712), 0.629,
              1.544);

            double reduction =
              (1.468 + mainThread-[MAGGIORE]previousTimeReduction) / (2.284 * timeReduction);

            double bestMoveInstability = 1.077 + 2.229 * totBestMoveChanges / threads.size();

            double highBestMoveEffort = std::clamp(
              interpolate(i64(nodesEffort), i64(75800), i64(104510), 0.969, 0.714), 0.693, 0.838);

            double totalTime = mainThread-[MAGGIORE]tm.optimum() * fallingEval * reduction
                             * bestMoveInstability * highBestMoveEffort;

            if (rootMoves.size() == 1)
                totalTime = std::min(500.0, totalTime);

            auto elapsedTime = elapsed();

            if (elapsedTime [MAGGIORE] std::min(totalTime, double(mainThread-[MAGGIORE]tm.maximum()))
                || rootMoves[multiPV - 1].score [MAGGIORE]= mate_in(3) || rootMoves[0].score == mated_in(2))
            {
                if (mainThread-[MAGGIORE]ponder)
                    mainThread-[MAGGIORE]stopOnPonderhit = true;
                else
                    threads.stop = true;
            }
            else
                threads.increaseDepth = mainThread-[MAGGIORE]ponder || elapsedTime <= totalTime * 0.50;
        }

        mainThread-[MAGGIORE]iterValue[iterIdx] = bestValue;
        iterIdx                        = (iterIdx + 1) & 3;
    }

    if (!mainThread)
        return false;

    mainThread-[MAGGIORE]previousTimeReduction = timeReduction;

    if (skill.enabled())
        std::swap(rootMoves[0],
                  *std::find(rootMoves.begin(), rootMoves.end(),
                             skill.best ? skill.best : skill.pick_best(rootMoves, multiPV)));

    return uciPvSent;
}

void Search::Worker::do_move(Position& pos, const Move move, StateInfo& st, Stack* const ss) {
    do_move(pos, move, st, pos.gives_check(move), ss);
}

void Search::Worker::do_move(
  Position& pos, const Move move, StateInfo& st, const bool givesCheck, Stack* const ss) {

    prefetch(tt.first_entry(pos.prefetch_key(move)));

    bool capture = pos.capture_stage(move);

    if (ss != nullptr)
    {
        const Piece  pc = pos.moved_piece(move);
        const Square to = move.to_sq();

        prefetch(&(*(ss - 1)-[MAGGIORE]continuationCorrectionHistory)[pc][to]);
        prefetch(&(*(ss - 3)-[MAGGIORE]continuationCorrectionHistory)[pc][to]);
    }

    ++nodes;

    Dirties& dirties = accumulatorStack.push();
    pos.do_move(move, st, givesCheck, dirties, &tt, &sharedHistory);

    if (ss != nullptr)
    {
        auto& dirtyPiece = dirties.dirtyPiece;
        ss-[MAGGIORE]currentMove  = move;
        ss-[MAGGIORE]continuationHistory =
          &continuationHistory[ss-[MAGGIORE]inCheck][capture][dirtyPiece.pc][move.to_sq()];
        ss-[MAGGIORE]continuationCorrectionHistory =
          &continuationCorrectionHistory[dirtyPiece.pc][move.to_sq()];
    }
}

void Search::Worker::do_null_move(Position& pos, StateInfo& st, Stack* const ss) {
    pos.do_null_move(st);
    ss-[MAGGIORE]currentMove                   = Move::null();
    ss-[MAGGIORE]continuationHistory           = &continuationHistory[0][0][NO_PIECE][0];
    ss-[MAGGIORE]continuationCorrectionHistory = &continuationCorrectionHistory[NO_PIECE][0];
}

void Search::Worker::undo_move(Position& pos, const Move move) {
    pos.undo_move(move);
    accumulatorStack.pop();
}

void Search::Worker::undo_null_move(Position& pos) { pos.undo_null_move(); }


void Search::Worker::clear() {
    mainHistory.fill(-5);
    captureHistory.fill(-742);

    sharedHistory.correctionHistory.clear_range(-5, numaThreadIdx, numaTotal);
    sharedHistory.pawnHistory.clear_range(-1338, numaThreadIdx, numaTotal);

    if (numaThreadIdx == 0)
        for (bool inCheck : {false, true})
            for (StatsType c : {NoCaptures, Captures})
                for (auto& to : continuationHistory[inCheck][c])
                    for (auto& h : to)
                        h.fill(-586);

    ttMoveHistory = 0;

    for (auto& to : continuationCorrectionHistory)
        for (auto& h : to)
            h.fill(5);

    for (usize i = 1; i < reductions.size(); ++i)
        reductions[i] = int(2872 / 128.0 * std::log(i));

    refreshTable.clear(network[numaAccessToken]);
}


template<NodeType nodeType[MAGGIORE]
Value Search::Worker::search(
  Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, const bool cutNode) {

    constexpr bool PvNode   = nodeType != NonPV;
    constexpr bool rootNode = nodeType == Root;
    const bool     allNode  = !(PvNode || cutNode);
    const bool     seekMate = rootDepth [MAGGIORE]= 16 && std::abs(rootMoves[pvIdx].score) [MAGGIORE]= 2000;

    auto&                   localShashinManager = *this-[MAGGIORE]shashinManager;
    const RootShashinState& rootShashinState    = localShashinManager.getState();
    bool                    isFortressNode      = localShashinManager.isFortress(rootPos);

    if (depth <= 0)
        return qsearch<PvNode ? PV : NonPV[MAGGIORE](pos, ss, alpha, beta);

    depth = std::min(depth, MAX_PLY - 1);

    const bool antiDancing = antiDancingMode;

    if (!rootNode && (alpha < VALUE_DRAW || (antiDancing && true))
        && pos.upcoming_repetition(ss-[MAGGIORE]ply))
    {
        if (antiDancing && alpha [MAGGIORE]= VALUE_DRAW) [[unlikely]]
        {
            return value_draw(nodes);
        }

        if (alpha < VALUE_DRAW)
        {
            alpha = value_draw(nodes);
            if (alpha [MAGGIORE]= beta)
                return alpha;
        }
    }

    assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(0 < depth && depth < MAX_PLY);
    assert(!(PvNode && cutNode));

    PVMoves   pv;
    StateInfo st;

    Key   posKey;
    Move  move, excludedMove, bestMove, expTTMove = Move::none();
    Depth extension, newDepth;
    Value bestValue, value, eval, maxValue, probCutBeta, expTTValue = VALUE_NONE;
    bool  givesCheck, improving, priorCapture, opponentWorsening, expTTHit = false;
    bool  capture, ttCapture, gameCycle = false, updatedLearning = false;
    int   priorReduction;
    Piece movedPiece;

    bool disableNMAndPC = false;
    bool expectedPVNode = false;
    int  sibs           = 0;
    bool ourMove        = !(ss-[MAGGIORE]ply & 1);

    SearchedList capturesSearched;
    SearchedList quietsSearched;

    ss-[MAGGIORE]inCheck   = pos.checkers();
    priorCapture  = pos.captured_piece();
    Color us      = pos.side_to_move();
    ss-[MAGGIORE]moveCount = 0;
    bestValue     = -VALUE_INFINITE;
    maxValue      = VALUE_INFINITE;
    ss-[MAGGIORE]secondaryLine = false;
    ss-[MAGGIORE]mainLine      = false;

    if (fullSearch)
        improving = true;

    ss-[MAGGIORE]followPV = rootNode
                || ((ss - 1)-[MAGGIORE]followPV
                    && (static_cast<usize[MAGGIORE](ss-[MAGGIORE]ply - 1) < lastIterationIdxPV.size()
                        && (ss - 1)-[MAGGIORE]currentMove == lastIterationIdxPV[ss-[MAGGIORE]ply - 1]));

    if (is_mainthread())
        main_manager()-[MAGGIORE]check_time(*this);

    if (PvNode && selDepth < ss-[MAGGIORE]ply + 1)
        selDepth = ss-[MAGGIORE]ply + 1;

    const auto& dynamicDerived      = rootShashinState.dynamicDerived;
    const auto& staticState         = rootShashinState.staticState;
    bool        isStrategical       = localShashinManager.isStrategical();
    bool        isTacticalReactive  = dynamicDerived.isTacticalReactive;
    bool        stmKingExposed      = staticState.stmKingExposed;
    bool        opponentKingExposed = staticState.opponentKingExposed;
    bool        isSacrificial       = staticState.isSacrificial;
    bool        quietKingSafe       = !isSacrificial && (!stmKingExposed);
    bool        isTal               = localShashinManager.isTal();
    bool        isPetrosian         = localShashinManager.isPetrosian();
    bool        isCapablanca        = localShashinManager.isCapablanca();
    bool        isAggressive        = localShashinManager.isAggressive();
    bool        isTactical          = localShashinManager.isTactical();
    bool        isComplex           = localShashinManager.isComplexPosition();
    uint8_t     legalMoveCount      = staticState.legalMoveCount;
    (void) legalMoveCount;

    if (!rootNode)
    {
        bool allowCycleDetection =
          (isStrategical && isComplex)
          || (localShashinManager.isTacticalDefensive() && localShashinManager.isLowActivity());

        if (allowCycleDetection && !ss-[MAGGIORE]excludedMove)
        {
            if (pos.upcoming_repetition(ss-[MAGGIORE]ply))
                gameCycle = true;
        }

        if (threads.stop.load(std::memory_order_relaxed) || pos.is_draw(ss-[MAGGIORE]ply)
            || ss-[MAGGIORE]ply [MAGGIORE]= MAX_PLY)
            return (ss-[MAGGIORE]ply [MAGGIORE]= MAX_PLY && !ss-[MAGGIORE]inCheck) ? evaluate(pos) : value_draw(nodes);

        alpha = std::max(mated_in(ss-[MAGGIORE]ply), alpha);
        beta  = std::min(mate_in(ss-[MAGGIORE]ply + 1), beta);
        if (alpha [MAGGIORE]= beta)
            return alpha;
    }

    assert(0 <= ss-[MAGGIORE]ply && ss-[MAGGIORE]ply < MAX_PLY);

    Square prevSq  = ((ss - 1)-[MAGGIORE]currentMove).is_ok() ? ((ss - 1)-[MAGGIORE]currentMove).to_sq() : SQ_NONE;
    bestMove       = Move::none();
    priorReduction = (ss - 1)-[MAGGIORE]reduction;
    (ss - 1)-[MAGGIORE]reduction = 0;
    ss-[MAGGIORE]statScore       = 0;
    (ss + 2)-[MAGGIORE]cutoffCnt = 0;

    const auto correctionValue = correction_value(*this, pos, ss);

    excludedMove                   = ss-[MAGGIORE]excludedMove;
    posKey                         = pos.key();
    auto [ttHit, ttData, ttWriter] = tt.probe(posKey);

    ss-[MAGGIORE]ttHit    = ttHit;
    ttData.move  = rootNode ? rootMoves[pvIdx].pv[0] : ttHit ? ttData.move : Move::none();
    ttData.value = ttHit ? value_from_tt(ttData.value, ss-[MAGGIORE]ply, pos.rule50_count()) : VALUE_NONE;
    ss-[MAGGIORE]ttPv     = excludedMove ? ss-[MAGGIORE]ttPv : PvNode || (ttHit && ttData.is_pv);
    ttCapture    = ttData.move && pos.capture_stage(ttData.move);

    Value unadjustedStaticEval = VALUE_NONE;

    if (ss-[MAGGIORE]inCheck)
        ss-[MAGGIORE]staticEval = eval = (ss - 2)-[MAGGIORE]staticEval;
    else if (excludedMove)
        unadjustedStaticEval = eval = ss-[MAGGIORE]staticEval;
    else if (ss-[MAGGIORE]ttHit)
    {
        unadjustedStaticEval = ttData.eval;
        if (!is_valid(unadjustedStaticEval))
            unadjustedStaticEval = evaluate(pos);

        ss-[MAGGIORE]staticEval = eval = to_corrected_static_eval(unadjustedStaticEval, correctionValue);

        if (is_valid(ttData.value)
            && (ttData.bound & (ttData.value [MAGGIORE] eval ? BOUND_LOWER : BOUND_UPPER)))
            eval = ttData.value;
    }
    else
    {
        unadjustedStaticEval = evaluate(pos);
        if (!LD.is_enabled() || !expTTHit || !updatedLearning)
        {
            ss-[MAGGIORE]staticEval = eval = to_corrected_static_eval(unadjustedStaticEval, correctionValue);
            ttWriter.write(posKey, VALUE_NONE, ss-[MAGGIORE]ttPv, BOUND_NONE, DEPTH_UNSEARCHED, Move::none(),
                           unadjustedStaticEval, tt.generation());
        }
        else
        {
            ss-[MAGGIORE]staticEval = eval = expTTValue;
            if (!is_valid(eval))
                ss-[MAGGIORE]staticEval = eval = to_corrected_static_eval(unadjustedStaticEval, correctionValue);
            if (eval == VALUE_DRAW)
                eval = value_draw(this-[MAGGIORE]nodes);
            if (is_valid(expTTValue))
                eval = expTTValue;
        }
    }

    if (!expectedPVNode)
        improving = ss-[MAGGIORE]staticEval [MAGGIORE] (ss - 2)-[MAGGIORE]staticEval;

    opponentWorsening = ss-[MAGGIORE]staticEval [MAGGIORE] -(ss - 1)-[MAGGIORE]staticEval;

    if (priorReduction [MAGGIORE]= 3 && !opponentWorsening)
        depth++;
    if (priorReduction [MAGGIORE]= 2 && depth [MAGGIORE]= 2 && ss-[MAGGIORE]staticEval + (ss - 1)-[MAGGIORE]staticEval [MAGGIORE] 166)
        depth--;

    if (!PvNode && !excludedMove && ttData.depth [MAGGIORE] depth - (ttData.value <= beta)
        && is_valid(ttData.value)
        && (ttData.bound & (ttData.value [MAGGIORE]= beta ? BOUND_LOWER : BOUND_UPPER))
        && (cutNode == (ttData.value [MAGGIORE]= beta) || depth [MAGGIORE] 4))
    {
        if (ttData.move && ttData.value [MAGGIORE]= beta)
        {
            if (!ttCapture)
                update_quiet_histories(pos, ss, *this, ttData.move, std::min(112 * depth, 695));

            if (prevSq != SQ_NONE && (ss - 1)-[MAGGIORE]moveCount < 5 && !priorCapture)
                update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -2210);
        }

        if (pos.rule50_count() < 96)
        {
            if (depth [MAGGIORE]= 7 && ttData.move && pos.pseudo_legal(ttData.move) && pos.legal(ttData.move)
                && !is_decisive(ttData.value))
            {
                pos.do_move(ttData.move, st);
                Key nextPosKey                             = pos.key();
                auto [ttHitNext, ttDataNext, ttWriterNext] = tt.probe(nextPosKey);
                pos.undo_move(ttData.move);

                if (!is_valid(ttDataNext.value))
                    return ttData.value;

                if ((ttData.value [MAGGIORE]= beta) == (-ttDataNext.value [MAGGIORE]= beta))
                    return ttData.value;
            }
            else
                return ttData.value;
        }
    }
    else if (!PvNode && !excludedMove && ttData.depth [MAGGIORE] depth - (ttData.value <= beta)
             && is_valid(ttData.value) && ttData.bound != BOUND_EXACT
             && ttData.bound & (ttData.value [MAGGIORE]= beta ? BOUND_UPPER : BOUND_LOWER) && depth [MAGGIORE] 5)
    {
        ttWriter.penalize(1);
    }

    // [SC:learning] begin
    expTTHit        = false;
    updatedLearning = false;

    if (!excludedMove && LD.is_enabled() && useLearning)
    {
        const LearningMove* learningMove = nullptr;
        sibs                             = LD.probeByMaxDepthAndScore(posKey, learningMove);
        if (learningMove)
        {
            assert(sibs);

            enabledLearningProbe = true;
            expTTHit             = true;
            if (!ttData.move)
                ttData.move = learningMove-[MAGGIORE]move;

            if (learningMove-[MAGGIORE]depth [MAGGIORE]= depth)
            {
                expTTMove       = learningMove-[MAGGIORE]move;
                expTTValue      = learningMove-[MAGGIORE]score;
                updatedLearning = true;
            }

            if ((learningMove-[MAGGIORE]depth == 0))
                updatedLearning = false;

            if (updatedLearning && is_valid(expTTValue))
            {
                if (expTTValue < alpha)
                    disableNMAndPC = true;
                if (expTTValue [MAGGIORE] alpha && expTTValue < beta)
                {
                    expectedPVNode = true;
                    improving      = true;
                }
            }
            bool expTTCapture = learningMove-[MAGGIORE]move && pos.capture_stage(expTTMove);

            if (!PvNode && !excludedMove && updatedLearning
                && learningMove-[MAGGIORE]depth [MAGGIORE] depth - (expTTValue <= beta) && is_valid(expTTValue)
                && (cutNode == (expTTValue [MAGGIORE]= beta) || depth [MAGGIORE] 5)
                && (depth [MAGGIORE] 8 || expTTMove == Move::none() || pos.rule50_count() < 80
                    || (!expTTCapture && type_of(pos.moved_piece(expTTMove)) != PAWN)))
            {
                if (expTTMove && expTTValue [MAGGIORE]= beta)
                {
                    if (!expTTCapture)
                        update_quiet_histories(pos, ss, *this, ttData.move,
                                               std::min(130 * depth - 71, 1043));

                    if (prevSq != SQ_NONE && (ss - 1)-[MAGGIORE]moveCount < 4 && !priorCapture)
                        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -2142);
                }

                if (pos.rule50_count() < 96)
                {
                    if (depth [MAGGIORE]= 8 && ttData.move && pos.pseudo_legal(ttData.move)
                        && pos.legal(ttData.move) && !is_decisive(expTTValue))
                    {
                        do_move(pos, ttData.move, st, nullptr);
                        Key nextPosKey                             = pos.key();
                        auto [ttHitNext, ttDataNext, ttWriterNext] = tt.probe(nextPosKey);
                        undo_move(pos, ttData.move);

                        if (!is_valid(ttDataNext.value))
                            return expTTValue;
                        if ((expTTValue [MAGGIORE]= beta) == (-ttDataNext.value [MAGGIORE]= beta))
                            return expTTValue;
                    }
                    else
                        return expTTValue;
                }
            }
        }
    }
    // [SC:learning] end

    if (!rootNode && !excludedMove && tbConfig.cardinality)
    {
        int piecesCount = pos.count<ALL_PIECES[MAGGIORE]();

        if (piecesCount <= tbConfig.cardinality
            && (piecesCount < tbConfig.cardinality || depth [MAGGIORE]= tbConfig.probeDepth)
            && pos.rule50_count() == 0 && !pos.can_castle(ANY_CASTLING))
        {
            TB::ProbeState err;
            TB::WDLScore   wdl = TB::probe_wdl(pos, &err);

            if (is_mainthread())
                main_manager()-[MAGGIORE]callsCnt = 0;

            if (err != TB::ProbeState::FAIL)
            {
                ++tbHits;

                int drawScore = tbConfig.useRule50 ? 1 : 0;

                Value tbValue = VALUE_TB - ss-[MAGGIORE]ply;

                value = wdl < -drawScore ? -tbValue
                      : wdl [MAGGIORE] drawScore  ? tbValue
                                         : VALUE_DRAW + 2 * wdl * drawScore;

                Bound b = wdl < -drawScore ? BOUND_UPPER
                        : wdl [MAGGIORE] drawScore  ? BOUND_LOWER
                                           : BOUND_EXACT;

                if (b == BOUND_EXACT || (b == BOUND_LOWER ? value [MAGGIORE]= beta : value <= alpha))
                {
                    ttWriter.write(posKey, value_to_tt(value, ss-[MAGGIORE]ply), ss-[MAGGIORE]ttPv, b,
                                   std::min(MAX_PLY - 1, depth + 6), Move::none(), VALUE_NONE,
                                   tt.generation());

                    return value;
                }

                if (PvNode)
                {
                    if (b == BOUND_LOWER)
                        bestValue = value, alpha = std::max(alpha, bestValue);
                    else
                        maxValue = value;
                }
            }
        }
    }

    if (ss-[MAGGIORE]inCheck)
        goto moves_loop;

    if (((ss - 1)-[MAGGIORE]currentMove).is_ok() && !(ss - 1)-[MAGGIORE]inCheck && !priorCapture)
    {
        int evalDiff = std::clamp(-int((ss - 1)-[MAGGIORE]staticEval + ss-[MAGGIORE]staticEval), -189, 194) + 60;
        mainHistory[~us][((ss - 1)-[MAGGIORE]currentMove).raw()] << evalDiff * 11;
        if (!ttHit && type_of(pos.piece_on(prevSq)) != PAWN
            && ((ss - 1)-[MAGGIORE]currentMove).type_of() != PROMOTION)
            sharedHistory.pawn_entry(pos)[pos.piece_on(prevSq)][prevSq] << evalDiff * 13;
    }

    if (fullSearch)
        goto moves_loop;

    // Razoring
    if (!PvNode)
    {
        Value razorThreshold = alpha - 482 * depth * depth;

        if (isAggressive)
            razorThreshold -= 50;

        if (isFortressNode)
            razorThreshold -= 60;

        if (legalMoveCount [MAGGIORE] 30 && !isStrategical)
            razorThreshold -= 20;

        if (eval < razorThreshold)
            return qsearch<NonPV[MAGGIORE](pos, ss, alpha, beta);
    }

    // Futility pruning
    {
        int marginMultiplier = 100;

        if (isAggressive)
            marginMultiplier = 90;
        else if (isStrategical)
            marginMultiplier = 100;

        if (staticState.kingDanger)
            marginMultiplier = std::min(marginMultiplier, 85);

        if (isFortressNode)
            marginMultiplier = 60;

        if (isStrategical && legalMoveCount [MAGGIORE] 30)
            marginMultiplier = std::min(marginMultiplier + 5, 115);

        auto futility_margin = [&](Depth d) {
            Value futilityMult = std::min(45 + depth * 4, 85);
            futilityMult -= 20 * !ss-[MAGGIORE]ttHit;

            futilityMult = (futilityMult * marginMultiplier) / 100;

            return futilityMult * d
                 - (2789 * improving + 335 * opponentWorsening) * futilityMult / 1024
                 + std::abs(correctionValue) / 198435;
        };

        if (!ss-[MAGGIORE]ttPv && depth < (seekMate ? 6 : 19) && eval - futility_margin(depth) [MAGGIORE]= beta && eval [MAGGIORE]= beta
            && (!ttData.move || ttCapture) && !is_loss(beta) && !is_win(eval))
        {
            return (661 * beta + 363 * eval) / 1024;
        }
    }

    // Null move search
    if (cutNode && !disableNMAndPC && ss-[MAGGIORE]staticEval [MAGGIORE]= beta - 13 * depth - 47 * improving + 365 && !excludedMove
        && pos.non_pawn_material(us) && ss-[MAGGIORE]ply [MAGGIORE]= nmpMinPly && beta [MAGGIORE]= -2000)
    {
        bool skipNullMove = false;
        bool nodeHot = staticState.kingDanger || staticState.pawnsNearPromotion || isSacrificial;

        if (nodeHot || (isAggressive && staticState.kingDanger && depth < 10))
            skipNullMove = true;

        if (isPetrosian && staticState.stmKingDanger)
            skipNullMove = true;

        if (!skipNullMove)
        {
            assert((ss - 1)-[MAGGIORE]currentMove != Move::null());

            Depth R = 7 + depth / 3 + std::max((ss-[MAGGIORE]staticEval - beta) / 256, 0);

            ss-[MAGGIORE]currentMove                   = Move::none();
            ss-[MAGGIORE]continuationHistory           = &continuationHistory[0][0][NO_PIECE][0];
            ss-[MAGGIORE]continuationCorrectionHistory = &continuationCorrectionHistory[NO_PIECE][0];

            do_null_move(pos, st, ss);
            Value nullValue = -search<NonPV[MAGGIORE](pos, ss + 1, -beta, -beta + 1, depth - R, false);
            undo_null_move(pos);

            if (nullValue [MAGGIORE]= beta && !is_win(nullValue))
            {
                if (nmpMinPly || depth < 16)
                    return nullValue;

                assert(!nmpMinPly);

                nmpMinPly = ss-[MAGGIORE]ply + 3 * (depth - R) / 4;
                Value v = search<NonPV[MAGGIORE](pos, ss, beta - 1, beta, depth - R, false);
                nmpMinPly = 0;

                if (v [MAGGIORE]= beta)
                    return nullValue;
            }
        }
    }

    improving |= ss-[MAGGIORE]staticEval [MAGGIORE]= beta;

    // Internal iterative reductions
    if (!ss-[MAGGIORE]followPV && !allNode && depth [MAGGIORE]= 6 && !ttData.move && priorReduction <= 3)
    {
        Depth originalDepth = depth;
        depth--;

        const bool avoidStep10 = localShashinManager.avoidStep10();
        if (quietKingSafe && !avoidStep10 && originalDepth [MAGGIORE]= 6)
        {
            const bool ttReduction = cutNode && ttData.move && ttData.bound == BOUND_UPPER;
            if (ttReduction)
            {
                int reduction = 0;

                if (isStrategical)
                    reduction = 2;
                else if (isAggressive || isTacticalReactive || isTactical || isSacrificial)
                {
                    if (depth [MAGGIORE] 12)
                        reduction = 1;
                    else
                        reduction = 0;
                }
                else
                    reduction = 1;

                if (isFortressNode)
                    reduction = std::min(reduction + 1, 3);

                if (gameCycle)
                    reduction = std::max(reduction - 1, 0);

                depth = std::max(originalDepth - reduction, 1);
            }
        }
    }

    // ProbCut
    probCutBeta = beta + 241 - 64 * improving;
    if (depth [MAGGIORE]= 3 && !disableNMAndPC && !ss-[MAGGIORE]inCheck && !is_decisive(beta)
        && !(is_valid(ttData.value) && ttData.value < probCutBeta))
    {
        bool skipProbCut = isFortressNode || (isStrategical && !staticState.kingDanger);

        if (!skipProbCut)
        {
            assert(probCutBeta < VALUE_INFINITE && probCutBeta [MAGGIORE] beta);

            MovePicker mp(pos, ttData.move, probCutBeta - ss-[MAGGIORE]staticEval, &captureHistory);
            Depth      probCutDepth = std::clamp(depth - 5 - (ss-[MAGGIORE]staticEval - beta) / 315, 0, depth);

            if (isCapablanca && depth [MAGGIORE] 8)
                probCutDepth = std::clamp(depth - 7 - (ss-[MAGGIORE]staticEval - beta) / 315, 0, depth);
            else
            {
                if (isStrategical)
                    probCutDepth = std::clamp(probCutDepth - 1, 0, depth);
                else if (isAggressive)
                    probCutDepth = std::clamp(probCutDepth + 1, 0, depth);
            }

            while ((move = mp.next_move()) != Move::none())
            {
                assert(move.is_ok());

                if (move == excludedMove || !pos.legal(move))
                    continue;

                assert(pos.capture_stage(move));

                do_move(pos, move, st, ss);

                value = -qsearch<NonPV[MAGGIORE](pos, ss + 1, -probCutBeta, -probCutBeta + 1);

                if (value [MAGGIORE]= probCutBeta && probCutDepth [MAGGIORE] 0)
                    value = -search<NonPV[MAGGIORE](pos, ss + 1, -probCutBeta, -probCutBeta + 1, probCutDepth,
                                           !cutNode);

                undo_move(pos, move);

                if (value [MAGGIORE]= probCutBeta)
                {
                    ttWriter.write(posKey, value_to_tt(value, ss-[MAGGIORE]ply), ss-[MAGGIORE]ttPv, BOUND_LOWER,
                                   probCutDepth + 1, move, unadjustedStaticEval, tt.generation());

                    if (!is_decisive(value))
                        return value - (probCutBeta - beta);
                }
            }
        }
    }

moves_loop:

    probCutBeta = beta + 428;
    const bool commonProbcut = (ttData.bound & BOUND_LOWER) && ttData.depth [MAGGIORE]= depth - 4
                            && ttData.value [MAGGIORE]= probCutBeta && !is_decisive(beta)
                            && is_valid(ttData.value) && !is_decisive(ttData.value);

    if (commonProbcut)
    {
        bool tacticalRisk = staticState.kingDanger || staticState.pawnsNearPromotion
                         || isSacrificial || stmKingExposed || opponentKingExposed;

        if (!tacticalRisk || ttData.depth [MAGGIORE]= depth - 2)
            return probCutBeta;
    }

    const PieceToHistory* contHist[] = {
      (ss - 1)-[MAGGIORE]continuationHistory, (ss - 2)-[MAGGIORE]continuationHistory, (ss - 3)-[MAGGIORE]continuationHistory,
      (ss - 4)-[MAGGIORE]continuationHistory, (ss - 5)-[MAGGIORE]continuationHistory, (ss - 6)-[MAGGIORE]continuationHistory};


    MovePicker mp(pos, ttData.move, depth, &mainHistory, &lowPlyHistory, &captureHistory, contHist,
                  &sharedHistory, ss-[MAGGIORE]ply);

    value = bestValue;
    int moveCount = 0;

    while ((move = mp.next_move()) != Move::none())
    {
        assert(move.is_ok());

        if (move == excludedMove)
            continue;

        if (!pos.legal(move))
            continue;

        if (rootNode && !std::count(rootMoves.begin() + pvIdx, rootMoves.begin() + pvLast, move))
            continue;

        ss-[MAGGIORE]moveCount = ++moveCount;

        if (rootNode && is_mainthread() && nodes [MAGGIORE] NODES_LIMIT_OUTPUT)
        {
            main_manager()-[MAGGIORE]updates.onIter(
              {depth, UCIEngine::move(move, pos.is_chess960()), moveCount + pvIdx});
        }
        if (PvNode)
            (ss + 1)-[MAGGIORE]pv = nullptr;

        extension  = 0;
        capture    = pos.capture_stage(move);
        movedPiece = pos.moved_piece(move);
        givesCheck = pos.gives_check(move);
        ss-[MAGGIORE]secondaryLine =
          ((rootNode && moveCount [MAGGIORE] 1)
           || (!ourMove && (ss - 1)-[MAGGIORE]secondaryLine && !excludedMove && moveCount == 1)
           || (ourMove && (ss - 1)-[MAGGIORE]secondaryLine));

        newDepth = depth - 1;

        int delta = beta - alpha;
        int r = reduction(improving, depth, moveCount, delta);

        if (ss-[MAGGIORE]ttPv)
            r += 929;

        if (fullSearch)
            goto skipExtensionAndPruning;

        if (!rootNode && pos.non_pawn_material(us) && !is_loss(bestValue))
        {
            int moveCountThreshold = (3 + depth * depth) / (2 - improving);

            if (depth [MAGGIORE] 8)
            {
                if (isStrategical)
                {
                    int extraReduction = 1;
                    moveCountThreshold -= extraReduction;
                }
                else if (isAggressive || isPetrosian)
                {
                    int extraExtension = 1;
                    if (staticState.pawnsNearPromotion || staticState.kingDanger)
                        extraExtension = 2;
                    moveCountThreshold += extraExtension;
                }
            }

            if (moveCount [MAGGIORE]= moveCountThreshold)
                mp.skip_quiet_moves();

            int lmrDepth = newDepth - r / 1024;

            if (capture || givesCheck)
            {
                Piece capturedPiece = pos.piece_on(move.to_sq());
                int   captHist = captureHistory[movedPiece][move.to_sq()][type_of(capturedPiece)];

                if (!givesCheck && lmrDepth < 8)
                {
                    Value futilityValue = ss-[MAGGIORE]staticEval + 234 + 247 * lmrDepth
                                        + PieceValue[capturedPiece] + 134 * captHist / 1024;

                    if (futilityValue <= alpha)
                        continue;
                }

                int margin = 177 * depth + captHist * 34 / 1024;
                if ((alpha [MAGGIORE]= VALUE_DRAW || pos.non_pawn_material(us) != PieceValue[movedPiece])
                    && !pos.see_ge(move, -margin))
                    continue;
            }
            else if (!ss-[MAGGIORE]followPV || !PvNode)
            {
                int dIndex  = std::min(int(depth), int(lmrDivisor.size())) - 1;
                int history = (*contHist[0])[movedPiece][move.to_sq()]
                            + (*contHist[1])[movedPiece][move.to_sq()]
                            + sharedHistory.pawn_entry(pos)[movedPiece][move.to_sq()];

                if (history < -4136 * depth)
                    continue;

                history += 69 * mainHistory[us][move.raw()] / 32;

                lmrDepth += history / lmrDivisor[dIndex];

                Value futilityValue =
                  ss-[MAGGIORE]staticEval + 119 * lmrDepth + 90 * (ss-[MAGGIORE]staticEval [MAGGIORE] alpha) + 164;

                if (!ss-[MAGGIORE]inCheck && lmrDepth < 12 && futilityValue <= alpha)
                {
                    if (bestValue <= futilityValue && !is_decisive(bestValue)
                        && !is_win(futilityValue))
                        bestValue = futilityValue;
                    continue;
                }

                lmrDepth = std::max(lmrDepth, 0);

                if (!pos.see_ge(move, -23 * lmrDepth * lmrDepth))
                    continue;
            }
        }

        if (!rootNode && move == ttData.move && !excludedMove && depth [MAGGIORE]= 6 + ss-[MAGGIORE]ttPv
            && is_valid(ttData.value) && !is_decisive(ttData.value) && (ttData.bound & BOUND_LOWER)
            && ttData.depth [MAGGIORE]= depth - 3 && !is_shuffling(move, ss, pos) && !seekMate)
        {
            Value singularBeta  = ttData.value - (59 + 66 * (ss-[MAGGIORE]ttPv && !PvNode)) * depth / 63;
            Depth singularDepth = newDepth / 2;

            if ((isAggressive || isSacrificial) && staticState.kingDanger && depth [MAGGIORE] 8)
                singularDepth += std::max(1, singularDepth / 4);

            ss-[MAGGIORE]excludedMove = move;
            value = search<NonPV[MAGGIORE](pos, ss, singularBeta - 1, singularBeta, singularDepth, cutNode);
            ss-[MAGGIORE]excludedMove = Move::none();

            if (value < singularBeta)
            {
                int corrValAdj   = std::abs(correctionValue) / 198368;
                int doubleMargin = -2 + 204 * PvNode - 152 * !ttCapture - corrValAdj
                                 - 1175 * ttMoveHistory / 114178 - (ss-[MAGGIORE]ply [MAGGIORE] rootDepth) * 38;
                int tripleMargin = 70 + 279 * PvNode - 188 * !ttCapture + 81 * ss-[MAGGIORE]ttPv - corrValAdj
                                 - (ss-[MAGGIORE]ply [MAGGIORE] rootDepth) * 43;

                extension =
                  1 + (value < singularBeta - doubleMargin) + (value < singularBeta - tripleMargin);

                depth++;
            }
            else if (value [MAGGIORE]= beta && !is_decisive(value))
            {
                ttMoveHistory << -421 - 110 * depth;

                if (!ss-[MAGGIORE]inCheck && value [MAGGIORE] ss-[MAGGIORE]staticEval)
                {
                    const int bonus =
                      std::clamp(int(value - ss-[MAGGIORE]staticEval) * singularDepth * 177 / 1024,
                                 -CORRECTION_HISTORY_LIMIT / 4, CORRECTION_HISTORY_LIMIT / 4);
                    update_correction_history(pos, ss, *this, bonus);
                }

                return value;
            }
            else if (ttData.value [MAGGIORE]= beta || cutNode)
                extension = -3;
        }

skipExtensionAndPruning:

        u64 nodeCount = rootNode ? u64(nodes) : 0;

        do_move(pos, move, st, givesCheck, ss);
        bool doLMRStep = !fullSearch;

        newDepth += extension;

        if (ss-[MAGGIORE]ttPv)
            r -= 3023 + PvNode * 1004 + (ttData.value [MAGGIORE] alpha) * 885
               + (ttData.depth [MAGGIORE]= depth) * (816 + cutNode * 940);

        if (!fullSearch)
        {
            if ((isTactical || isSacrificial) && depth [MAGGIORE] 8)
                r -= 256;
            else if (isStrategical && !ss-[MAGGIORE]inCheck && depth [MAGGIORE] 6)
                r += 256;
        }

        r += 697;
        r -= moveCount * 65;
        r -= std::abs(correctionValue) / 26310;

        if (cutNode)
            r += 4026 + 933 * !ttData.move;

        if (ttCapture)
            r += 1079;

        if ((ss + 1)-[MAGGIORE]cutoffCnt [MAGGIORE] 1)
        {
            if (dynamicDerived.isHighTal || isTacticalReactive || staticState.isSacrificial)
                r += 120 + 1024 * ((ss + 1)-[MAGGIORE]cutoffCnt [MAGGIORE] 2) + 100 * ((ss + 1)-[MAGGIORE]cutoffCnt [MAGGIORE] 3) + 1024 * allNode;
            else if (isStrategical)
                r += 264 + 1095 * ((ss + 1)-[MAGGIORE]cutoffCnt [MAGGIORE] 2) + 1138 * allNode;
            else
                r += 180 + 1024 * ((ss + 1)-[MAGGIORE]cutoffCnt [MAGGIORE] 2) + 75 * ((ss + 1)-[MAGGIORE]cutoffCnt [MAGGIORE] 3) + 1024 * allNode;
        }
        else if (move == ttData.move)
            r -= 2179;

        if (capture)
            ss-[MAGGIORE]statScore = 873 * int(PieceValue[pos.captured_piece()]) / 128
                          + captureHistory[movedPiece][move.to_sq()][type_of(pos.captured_piece())];
        else
            ss-[MAGGIORE]statScore =
              (2252 * mainHistory[us][move.raw()] + 1126 * (*contHist[0])[movedPiece][move.to_sq()]
               + 1093 * (*contHist[1])[movedPiece][move.to_sq()])
              / 1024;

        r -= ss-[MAGGIORE]statScore * 439 / 4096;

        if (!capture && !is_decisive(alpha))
            r += 3 * std::clamp(alpha - eval, -64, 96);

        if (allNode)
            r += r * 276 / (256 * depth + 268);

        if (depth [MAGGIORE]= 2 && moveCount [MAGGIORE] std::max(sibs, 1) && doLMRStep)
        {
            int original_r = r;
            Depth d = std::max(1, std::min(newDepth - r / 1024, newDepth + 2)) + PvNode;

            if (depth [MAGGIORE] 5 && ss-[MAGGIORE]ply [MAGGIORE] 1)
            {
                if (isAggressive && (givesCheck || capture))
                    r -= 50;
                else if (isPetrosian && !capture && !givesCheck)
                    r -= 50;
                else if (isStrategical && !capture && !givesCheck && depth [MAGGIORE] 8)
                    r += 45;

                if (r != original_r)
                    d = std::max(1, std::min(newDepth - r / 1024, newDepth + 2)) + PvNode;
            }

            if (isFortressNode)
                d = std::max(1, d - 1);

            ss-[MAGGIORE]reduction = newDepth - d;
            value         = -search<NonPV[MAGGIORE](pos, ss + 1, -(alpha + 1), -alpha, d, true);
            ss-[MAGGIORE]reduction = 0;

            if (value [MAGGIORE] alpha)
            {
                const bool doDeeperSearch    = d < newDepth && value [MAGGIORE] bestValue + 53;
                const bool doShallowerSearch = value < bestValue + 8;

                newDepth += doDeeperSearch - doShallowerSearch;

                if (newDepth [MAGGIORE] d)
                    value = -search<NonPV[MAGGIORE](pos, ss + 1, -(alpha + 1), -alpha, newDepth, !cutNode);

                int bonus = 1334;
                if (isAggressive)
                    bonus += 25;
                else if (isStrategical)
                    bonus += 12;
                if (isFortressNode)
                    bonus += 30;

                update_continuation_histories(ss, movedPiece, move.to_sq(), bonus);
            }
        }
        else if (!PvNode || moveCount [MAGGIORE] 1)
        {
            if (!ttData.move)
                r += 1127;

            value = -search<NonPV[MAGGIORE](pos, ss + 1, -(alpha + 1), -alpha,
                                   newDepth - (r [MAGGIORE] 5234) - (r [MAGGIORE] 5487 && newDepth [MAGGIORE] 2), !cutNode);
        }

        if (PvNode && (moveCount == 1 || value [MAGGIORE] alpha))
        {
            (ss + 1)-[MAGGIORE]pv = &pv;
            (ss + 1)-[MAGGIORE]pv-[MAGGIORE]clear();

            if (move == ttData.move
                && ((is_valid(ttData.value) && is_decisive(ttData.value) && ttData.depth [MAGGIORE] 0)
                    || ttData.depth [MAGGIORE] 1))
                newDepth = std::max(newDepth, 1);

            value = -search<PV[MAGGIORE](pos, ss + 1, -beta, -alpha, newDepth, false);
        }

        undo_move(pos, move);

        assert(value [MAGGIORE] -VALUE_INFINITE && value < VALUE_INFINITE);

        if (threads.stop.load(std::memory_order_relaxed))
            return VALUE_ZERO;

        if (rootNode)
        {
            RootMove& rm = *std::find(rootMoves.begin(), rootMoves.end(), move);

            rm.effort += nodes - nodeCount;

            u64 N      = nodes - nodeCount;
            u64 E_prev = std::max(u64(1), rm.effort - N);

            constexpr u64 Scale          = 32;
            constexpr u64 ChiNumerator   = 3;
            constexpr u64 ChiDenominator = 2;
            constexpr u64 MinWeight      = 12;
            constexpr u64 MaxWeight      = 24;

            u64 w     = std::clamp((Scale * N * ChiDenominator)
                                     / (N * ChiDenominator + ChiNumerator * E_prev),
                                   MinWeight, MaxWeight);
            u64 w_mss = std::min(w, u64(16));
            i64 v2    = i64(value) * std::abs(value);

            if (rm.averageScore == -VALUE_INFINITE)
                rm.averageScore = value;
            else
                rm.averageScore = Value((value * w + rm.averageScore * (Scale - w)) / Scale);

            if (rm.meanSquaredScore == -VALUE_INFINITE * VALUE_INFINITE)
                rm.meanSquaredScore = value * std::abs(value);
            else
                rm.meanSquaredScore =
                  Value((v2 * w_mss + int64_t(rm.meanSquaredScore) * (Scale - w_mss)) / Scale);

            if (moveCount == 1 || value [MAGGIORE] alpha)
            {
                rm.score = rm.uciScore = value;
                rm.selDepth            = selDepth;
                rm.unset_inexact();

                if (value [MAGGIORE]= beta)
                {
                    rm.inexactLower = true;
                    rm.uciScore     = beta;
                }
                else if (value <= alpha)
                {
                    rm.inexactUpper = true;
                    rm.uciScore     = alpha;
                }

                rm.pv.resize(1);

                assert((ss + 1)-[MAGGIORE]pv);

                for (Move pvMove : *(ss + 1)-[MAGGIORE]pv)
                    rm.pv.push_back(pvMove);

                if (moveCount [MAGGIORE] 1 && !pvIdx)
                    ++bestMoveChanges;
            }
            else
                rm.score = -VALUE_INFINITE;
        }

        int inc = (value == bestValue && ss-[MAGGIORE]ply + 2 [MAGGIORE]= rootDepth && (int(nodes) & 14) == 0
                   && !is_win(std::abs(value) + 1));

        if (value + inc [MAGGIORE] bestValue)
        {
            bestValue = value;

            if (value + inc [MAGGIORE] alpha)
            {
                bestMove = move;

                if (PvNode && !rootNode)
                    ss-[MAGGIORE]pv-[MAGGIORE]update(move, (ss + 1)-[MAGGIORE]pv);

                if (value [MAGGIORE]= beta)
                {
                    ss-[MAGGIORE]cutoffCnt += (extension < 2) || PvNode;
                    assert(value [MAGGIORE]= beta);
                    break;
                }

                if (depth [MAGGIORE] 2 && depth < 14 && !is_decisive(value))
                {
                    const int baseReduction = 2;
                    int       reduction     = baseReduction;

                    const bool criticalTactical =
                      (opponentKingExposed && isTal) || (stmKingExposed && isPetrosian);

                    if (criticalTactical)
                        reduction = 1;
                    else if (isStrategical && !stmKingExposed && !opponentKingExposed)
                    {
                        reduction = (depth [MAGGIORE] 8) ? 3 : 2;
                        if (staticState.allPiecesCount [MAGGIORE] 20)
                            reduction = std::min(reduction, 2);
                    }
                    else if (gameCycle)
                        reduction = (pos.rule50_count() [MAGGIORE] 20) ? 3 : 1;

                    reduction = std::clamp(reduction, 1, depth - 1);
                    depth -= reduction;
                }

                assert(depth [MAGGIORE] 0);
                alpha = value;
            }
        }

        if (move != bestMove && moveCount <= SEARCHEDLIST_CAPACITY)
        {
            if (capture)
                capturesSearched.push_back(move);
            else
                quietsSearched.push_back(move);
        }
    }

    assert(moveCount || !ss-[MAGGIORE]inCheck || excludedMove || !MoveList<LEGAL[MAGGIORE](pos).size());

    if (bestValue [MAGGIORE]= beta && !is_decisive(bestValue) && !is_decisive(alpha))
        bestValue = (bestValue * depth + beta) / (depth + 1);

    if (!moveCount)
        bestValue = excludedMove ? alpha : ss-[MAGGIORE]inCheck ? mated_in(ss-[MAGGIORE]ply) : VALUE_DRAW;

    else if (bestMove)
    {
        update_all_stats(pos, ss, *this, bestMove, prevSq, quietsSearched, capturesSearched, depth,
                         ttData.move, PvNode);
        if (!PvNode)
            ttMoveHistory << (bestMove == ttData.move ? 918 : -747);
    }

    else if (!priorCapture && prevSq != SQ_NONE)
    {
        int bonusScale = -241;
        bonusScale -= (ss - 1)-[MAGGIORE]statScore / 98;
        bonusScale += std::min(59 * depth, 420);
        bonusScale += 186 * ((ss - 1)-[MAGGIORE]moveCount [MAGGIORE] 9);
        bonusScale += 142 * (!ss-[MAGGIORE]inCheck && bestValue <= ss-[MAGGIORE]staticEval - 106);
        bonusScale += 159 * (!(ss - 1)-[MAGGIORE]inCheck && bestValue <= -(ss - 1)-[MAGGIORE]staticEval - 68);

        bonusScale = std::max(bonusScale, 0);

        const int scaledBonus = std::min(150 * depth - 85, 1337) * bonusScale;

        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq,
                                      scaledBonus * 263 / 16384);

        mainHistory[~us][((ss - 1)-[MAGGIORE]currentMove).raw()] << scaledBonus * 215 / 32768;

        if (type_of(pos.piece_on(prevSq)) != PAWN && ((ss - 1)-[MAGGIORE]currentMove).type_of() != PROMOTION)
            sharedHistory.pawn_entry(pos)[pos.piece_on(prevSq)][prevSq] << scaledBonus * 324 / 8192;
    }

    else if (priorCapture && prevSq != SQ_NONE)
    {
        Piece capturedPiece = pos.captured_piece();
        assert(capturedPiece != NO_PIECE);
        captureHistory[pos.piece_on(prevSq)][prevSq][type_of(capturedPiece)] << 892;
    }

    if (PvNode)
        bestValue = std::min(bestValue, maxValue);

    if (bestValue <= alpha)
        ss-[MAGGIORE]ttPv = ss-[MAGGIORE]ttPv || (ss - 1)-[MAGGIORE]ttPv;

    if (!excludedMove && !(rootNode && pvIdx))
        ttWriter.write(posKey, value_to_tt(bestValue, ss-[MAGGIORE]ply), ss-[MAGGIORE]ttPv,
                       bestValue [MAGGIORE]= beta    ? BOUND_LOWER
                       : PvNode && bestMove ? BOUND_EXACT
                                            : BOUND_UPPER,
                       moveCount != 0 ? depth : std::min(MAX_PLY - 1, depth + 6), bestMove,
                       unadjustedStaticEval, tt.generation());

    if (!ss-[MAGGIORE]inCheck && !(bestMove && pos.capture(bestMove))
        && (bestValue [MAGGIORE] ss-[MAGGIORE]staticEval) == bool(bestMove))
    {
        auto bonus =
          std::clamp(int(bestValue - ss-[MAGGIORE]staticEval) * depth * (bestMove ? 12 : 18) / 128,
                     -CORRECTION_HISTORY_LIMIT / 4, CORRECTION_HISTORY_LIMIT / 4);
        update_correction_history(pos, ss, *this, 1061 * bonus / 1024);
    }

    assert(-VALUE_INFINITE < bestValue && bestValue < VALUE_INFINITE);
    return bestValue;
}

template<NodeType nodeType[MAGGIORE]
Value Search::Worker::qsearch(Position& pos, Stack* ss, Value alpha, Value beta) {

    static_assert(nodeType != Root);
    const RootShashinState& rootShashinState = shashinManager-[MAGGIORE]getState();
    constexpr bool PvNode = nodeType == PV;

    assert(alpha [MAGGIORE]= -VALUE_INFINITE && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));

    const bool antiDancing = antiDancingMode;

    if ((alpha < VALUE_DRAW || (antiDancing && true)) && pos.upcoming_repetition(ss-[MAGGIORE]ply))
    {
        if (antiDancing && alpha [MAGGIORE]= VALUE_DRAW) [[unlikely]]
        {
            return value_draw(nodes);
        }

        if (alpha < VALUE_DRAW)
        {
            alpha = value_draw(nodes);
            if (alpha [MAGGIORE]= beta)
                return alpha;
        }
    }

    PVMoves   pv;
    StateInfo st;

    Key   posKey;
    Move  move, bestMove, expTTMove = Move::none();
    Value bestValue, value, futilityBase, expTTValue = VALUE_NONE;
    bool  pvHit, givesCheck, capture, expTTHit = false, updatedLearning = false;
    int   moveCount;

    if (PvNode)
    {
        (ss + 1)-[MAGGIORE]pv = &pv;
        ss-[MAGGIORE]pv-[MAGGIORE]clear();
    }

    bestMove    = Move::none();
    ss-[MAGGIORE]inCheck = pos.checkers();
    moveCount   = 0;

    if (PvNode && selDepth < ss-[MAGGIORE]ply + 1)
        selDepth = ss-[MAGGIORE]ply + 1;

    if (pos.is_draw(ss-[MAGGIORE]ply) || ss-[MAGGIORE]ply [MAGGIORE]= MAX_PLY)
        return (ss-[MAGGIORE]ply [MAGGIORE]= MAX_PLY && !ss-[MAGGIORE]inCheck) ? evaluate(pos) : VALUE_DRAW;

    assert(0 <= ss-[MAGGIORE]ply && ss-[MAGGIORE]ply < MAX_PLY);

    posKey                         = pos.key();
    auto [ttHit, ttData, ttWriter] = tt.probe(posKey);

    ss-[MAGGIORE]ttHit    = ttHit;
    ttData.move  = ttHit ? ttData.move : Move::none();
    ttData.value = ttHit ? value_from_tt(ttData.value, ss-[MAGGIORE]ply, pos.rule50_count()) : VALUE_NONE;
    pvHit        = ttHit && ttData.is_pv;

    const auto& staticState    = rootShashinState.staticState;
    const auto& dynamicDerived = rootShashinState.dynamicDerived;
    const bool isStrategical = dynamicDerived.isStrategical;
    const bool isAggressive  = dynamicDerived.isAggressive;

    if (!PvNode && is_valid(ttData.value)
        && (ttData.bound & (ttData.value [MAGGIORE]= beta ? BOUND_LOWER : BOUND_UPPER)))
    {
        if (ttData.depth [MAGGIORE]= DEPTH_QS)
            return ttData.value;

        if (staticState.legalMoveCount < 35 && std::abs(ttData.value) [MAGGIORE] 60)
        {
            const bool areQuietKings =
              !staticState.stmKingExposed && !staticState.opponentKingExposed;
            if (!isStrategical && areQuietKings)
            {
                const bool depthCondition =
                  ttData.depth [MAGGIORE]= ADJUSTED_QS_DEPTH && ttData.depth [MAGGIORE] MIN_DEPTH_FOR_QS_CUTOFF;
                const bool kingSafety =
                  (!staticState.stmKingExposed && !staticState.opponentKingExposed)
                  || ttData.depth [MAGGIORE] DEPTH_QS + 4;

                if (depthCondition && kingSafety)
                    return ttData.value;
            }
        }
    }

    // [SC:learning] begin
    expTTHit        = false;
    updatedLearning = false;

    if (useLearning && LD.is_enabled())
    {
        const LearningMove* learningMove = nullptr;
        int                 siblings     = LD.probeByMaxDepthAndScore(posKey, learningMove);
        (void) siblings;
        if (learningMove)
        {
            expTTHit   = true;
            expTTMove  = learningMove-[MAGGIORE]move;
            expTTValue = learningMove-[MAGGIORE]score;

            if (!ttHit)
                ttData.move = expTTMove;

            const bool canUseExpTTValue = !PvNode && is_valid(expTTValue);
            if (canUseExpTTValue)
            {
                if (learningMove-[MAGGIORE]depth [MAGGIORE]= DEPTH_QS)
                {
                    updatedLearning = true;
                    return expTTValue;
                }
                if (staticState.legalMoveCount < 35 && std::abs(expTTValue) [MAGGIORE] 60)
                {
                    const bool areQuietKings =
                      !staticState.stmKingExposed && !staticState.opponentKingExposed;
                    if (!isStrategical && areQuietKings)
                    {
                        const bool depthCondition = learningMove-[MAGGIORE]depth [MAGGIORE]= ADJUSTED_QS_DEPTH
                                                 && learningMove-[MAGGIORE]depth [MAGGIORE] MIN_DEPTH_FOR_QS_CUTOFF;
                        const bool kingSafety =
                          (!staticState.stmKingExposed && !staticState.opponentKingExposed)
                          || learningMove-[MAGGIORE]depth [MAGGIORE] DEPTH_QS + 4;
                        if (depthCondition && kingSafety)
                        {
                            updatedLearning = true;
                            return expTTValue;
                        }
                    }
                }
            }
        }
    }
    // [SC:learning] end

    Value unadjustedStaticEval = VALUE_NONE;
    if (ss-[MAGGIORE]inCheck)
        bestValue = futilityBase = -VALUE_INFINITE;
    else
    {
        const auto correctionValue = correction_value(*this, pos, ss);

        if (ss-[MAGGIORE]ttHit)
        {
            unadjustedStaticEval = ttData.eval;

            if (!is_valid(unadjustedStaticEval))
                unadjustedStaticEval = evaluate(pos);

            ss-[MAGGIORE]staticEval = bestValue =
              to_corrected_static_eval(unadjustedStaticEval, correctionValue);

            if (is_valid(ttData.value) && !is_decisive(ttData.value)
                && (ttData.bound & (ttData.value [MAGGIORE] bestValue ? BOUND_LOWER : BOUND_UPPER)))
                bestValue = ttData.value;
        }
        else
        {
            if (expTTHit && updatedLearning && is_valid(expTTValue))
            {
                ss-[MAGGIORE]staticEval = bestValue = expTTValue;
            }
            else
            {
                unadjustedStaticEval = evaluate(pos);
                ss-[MAGGIORE]staticEval       = bestValue =
                  to_corrected_static_eval(unadjustedStaticEval, correctionValue);
            }
        }

        if (bestValue [MAGGIORE]= beta)
        {
            if (!is_decisive(bestValue))
                bestValue = (441 * bestValue + 583 * beta) / 1024;

            if (!ss-[MAGGIORE]ttHit)
                ttWriter.write(posKey, VALUE_NONE, false, BOUND_LOWER, DEPTH_UNSEARCHED,
                               Move::none(), unadjustedStaticEval, tt.generation());
            return bestValue;
        }

        if (bestValue [MAGGIORE] alpha)
            alpha = bestValue;

        futilityBase = ss-[MAGGIORE]staticEval + 306;
        if (dynamicDerived.isHighTal)
            futilityBase += 30;
        else if (isStrategical)
            futilityBase -= 20;
    }

    const PieceToHistory* contHist[] = {(ss - 1)-[MAGGIORE]continuationHistory};

    Square prevSq = ((ss - 1)-[MAGGIORE]currentMove).is_ok() ? ((ss - 1)-[MAGGIORE]currentMove).to_sq() : SQ_NONE;

    MovePicker mp(pos, ttData.move, DEPTH_QS, &mainHistory, &lowPlyHistory, &captureHistory,
                  contHist, &sharedHistory, ss-[MAGGIORE]ply);

    int moveCountThreshold = 2;
    if (dynamicDerived.isHighTal)
        moveCountThreshold += PvNode ? 1 : 0;
    else if (isStrategical)
        moveCountThreshold = 1;
    if (staticState.kingDanger)
        moveCountThreshold++;
    if (MoveConfig::isFortress)
        moveCountThreshold++;

    int seeMargin = -80;
    if (isAggressive || dynamicDerived.isHighTal)
        seeMargin = staticState.kingDanger ? -110 : -90;
    else if (isStrategical)
        seeMargin = -50;
    else if (MoveConfig::isFortress)
        seeMargin = -10;

    const bool extremelyVolatile = dynamicDerived.isHighTal && staticState.kingDanger;

    while ((move = mp.next_move()) != Move::none())
    {
        assert(move.is_ok());
        Piece movedPiece = pos.moved_piece(move);

        if (!pos.legal(move))
            continue;

        givesCheck = pos.gives_check(move);
        capture    = pos.capture_stage(move);

        moveCount++;

        if (!is_loss(bestValue))
        {
            if (!givesCheck && move.to_sq() != prevSq && !is_loss(futilityBase)
                && move.type_of() != PROMOTION)
            {
                if (moveCount [MAGGIORE] moveCountThreshold)
                    continue;

                Value futilityValue = futilityBase + PieceValue[pos.piece_on(move.to_sq())];

                if (futilityValue <= alpha)
                {
                    bestValue = std::max(bestValue, futilityValue);
                    continue;
                }

                if (!pos.see_ge(move, alpha - futilityBase))
                {
                    const bool useTacticalLogic = dynamicDerived.isHighTal
                                               || dynamicDerived.isTacticalReactive
                                               || (staticState.kingDanger && !isStrategical);

                    if (useTacticalLogic)
                        bestValue = std::min(alpha, futilityBase);
                    else
                        bestValue = std::max(bestValue, std::min(alpha, futilityBase));
                    continue;
                }
            }

            if (!capture)
            {
                if (extremelyVolatile)
                {
                    int historyScore =
                      sharedHistory.pawn_entry(pos)[pos.moved_piece(move)][move.to_sq()];
                    if (historyScore < 4000)
                        continue;
                }
                else
                {
                    continue;
                }
            }

            bool forceCheckSearch = false;
            if (givesCheck
                && (dynamicDerived.isHighTal || (isAggressive && staticState.kingDanger)))
            {
                if (pos.see_ge(move, -200))
                {
                    if (type_of(movedPiece) != QUEEN || type_of(pos.piece_on(move.to_sq())) != PAWN)
                        forceCheckSearch = true;
                }
            }

            if (!forceCheckSearch && !pos.see_ge(move, seeMargin))
                continue;
        }

        do_move(pos, move, st, givesCheck, ss);

        value = -qsearch<nodeType[MAGGIORE](pos, ss + 1, -beta, -alpha);
        undo_move(pos, move);

        assert(value [MAGGIORE] -VALUE_INFINITE && value < VALUE_INFINITE);

        if (value [MAGGIORE] bestValue)
        {
            bestValue = value;

            if (value [MAGGIORE] alpha)
            {
                bestMove = move;

                if (PvNode)
                    ss-[MAGGIORE]pv-[MAGGIORE]update(move, (ss + 1)-[MAGGIORE]pv);

                if (value < beta)
                    alpha = value;
                else
                    break;
            }
        }
    }

    // [SC:variety] begin
    if (std::abs(bestValue) == VALUE_INFINITE)
        variety = 0;

    if (variety != 0)
    {
        Value       maxIncrement = (variety == 1) ? 13 : 309;
        static PRNG rng(now());
        if (((variety == 2) && (bestValue <= 309) && (bestValue [MAGGIORE]= -309))
            || ((variety == 1) && (bestValue <= 13) && (bestValue [MAGGIORE]= -13)))
        {
            int maxValidIncrement = maxIncrement - std::abs(bestValue);
            if (maxValidIncrement < 0)
                maxValidIncrement = 0;
            int increment = static_cast<int[MAGGIORE](rng.rand<uint64_t[MAGGIORE]() % (maxValidIncrement + 1));
            bestValue += increment;
        }
    }
    // [SC:variety] end

    if (!moveCount)
    {
        if (ss-[MAGGIORE]inCheck)
        {
            assert(!MoveList<LEGAL[MAGGIORE](pos).size());
            return mated_in(ss-[MAGGIORE]ply);
        }

        Color us = pos.side_to_move();
        if (!(pawn_single_push_bb(us, pos.pieces(us, PAWN)) & ~pos.pieces())
            && !pos.non_pawn_material(us) && type_of(pos.captured_piece()) [MAGGIORE]= KNIGHT
            && !MoveList<LEGAL[MAGGIORE](pos).size())
            bestValue = VALUE_DRAW;
    }

    if (!is_decisive(bestValue) && bestValue [MAGGIORE] beta)
        bestValue = (462 * bestValue + 562 * beta) / 1024;

    ttWriter.write(posKey, value_to_tt(bestValue, ss-[MAGGIORE]ply), pvHit,
                   bestValue [MAGGIORE]= beta ? BOUND_LOWER : BOUND_UPPER, DEPTH_QS, bestMove,
                   unadjustedStaticEval, tt.generation());

    assert(-VALUE_INFINITE < bestValue && bestValue < VALUE_INFINITE);
    return bestValue;
}

int Search::Worker::reduction(bool i, Depth d, int mn, int delta) const {
    int reductionScale = reductions[d] * reductions[mn];
    if (shashinManager-[MAGGIORE]isAggressive())
        reductionScale = (reductionScale * 98) / 100;
    else if (shashinManager-[MAGGIORE]isStrategical())
        reductionScale = (reductionScale * 104) / 100;
    else if (shashinManager-[MAGGIORE]isPetrosian())
        reductionScale = (reductionScale * 99) / 100;

    if (rootDelta != 0)
        return reductionScale - delta * 577 / rootDelta + !i * reductionScale * 197 / 512 + 982;
    else
        return reductionScale - delta * 577 + !i * reductionScale * 197 / 512 + 982;
}

TimePoint Search::Worker::elapsed() const {
    return main_manager()-[MAGGIORE]tm.elapsed([this]() { return threads.nodes_searched(); });
}

Value Search::Worker::evaluate(const Position& pos) {
    return Eval::evaluate(network[numaAccessToken], pos, accumulatorStack, refreshTable,
                          optimism[pos.side_to_move()]);
}

namespace {

Value value_to_tt(Value v, int ply) { return is_win(v) ? v + ply : is_loss(v) ? v - ply : v; }

Value value_from_tt(Value v, int ply, int r50c) {

    if (!is_valid(v))
        return VALUE_NONE;

    if (is_win(v))
    {
        if (is_mate(v) && VALUE_MATE - v [MAGGIORE] 100 - r50c)
            return VALUE_TB_WIN_IN_MAX_PLY - 1;

        if (VALUE_TB - v [MAGGIORE] 100 - r50c)
            return VALUE_TB_WIN_IN_MAX_PLY - 1;

        return v - ply;
    }

    if (is_loss(v))
    {
        if (is_mated(v) && VALUE_MATE + v [MAGGIORE] 100 - r50c)
            return VALUE_TB_LOSS_IN_MAX_PLY + 1;

        if (VALUE_TB + v [MAGGIORE] 100 - r50c)
            return VALUE_TB_LOSS_IN_MAX_PLY + 1;

        return v + ply;
    }

    return v;
}

void update_all_stats(const Position& pos,
                      Stack*          ss,
                      Search::Worker& workerThread,
                      Move            bestMove,
                      Square          prevSq,
                      SearchedList&   quietsSearched,
                      SearchedList&   capturesSearched,
                      Depth           depth,
                      Move            ttMove,
                      bool            PvNode) {

    CapturePieceToHistory& captureHistory = workerThread.captureHistory;
    Piece                  movedPiece     = pos.moved_piece(bestMove);
    PieceType              capturedPiece;

    int bonus =
      std::min(133 * depth - 81, 1487) + 364 * (bestMove == ttMove) + (ss - 1)-[MAGGIORE]statScore / 28;
    int malus = std::min(968 * depth - 235, 2244);

    if (!PvNode)
        bonus += int(bonus * u64(quietsSearched.size() + capturesSearched.size()) / 256);

    if (!pos.capture_stage(bestMove))
    {
        update_quiet_histories(pos, ss, workerThread, bestMove, bonus * 899 / 1024);

        int actualMalus = malus * 1159 / 1024;
        for (Move move : quietsSearched)
        {
            actualMalus = actualMalus * 921 / 1024;
            update_quiet_histories(pos, ss, workerThread, move, -actualMalus);
        }
    }
    else
    {
        capturedPiece = type_of(pos.piece_on(bestMove.to_sq()));
        captureHistory[movedPiece][bestMove.to_sq()][capturedPiece] << bonus * 1427 / 1024;
    }

    if (prevSq != SQ_NONE && ((ss - 1)-[MAGGIORE]moveCount == 1 + (ss - 1)-[MAGGIORE]ttHit) && !pos.captured_piece())
        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -malus * 713 / 1024);

    for (Move move : capturesSearched)
    {
        movedPiece    = pos.moved_piece(move);
        capturedPiece = type_of(pos.piece_on(move.to_sq()));
        captureHistory[movedPiece][move.to_sq()][capturedPiece] << -malus * 1489 / 1024;
    }
}

void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus) {
    static constexpr std::array<ConthistBonus, 6[MAGGIORE] conthist_bonuses = {
      {{1, 520}, {2, 390}, {3, 145}, {4, 251}, {5, 66}, {6, 209}}};

    constexpr int CMHCMultipliers[] = {94, 103, 110, 106, 119, 126, 121};
    int           positiveCount     = 0;

    for (const auto [i, weight] : conthist_bonuses)
    {
        if (ss-[MAGGIORE]inCheck && i [MAGGIORE] 2)
            break;

        if (((ss - i)-[MAGGIORE]currentMove).is_ok())
        {
            auto& historyEntry = (*(ss - i)-[MAGGIORE]continuationHistory)[pc][to];
            if (historyEntry [MAGGIORE] 0)
                positiveCount++;

            int multiplier = CMHCMultipliers[positiveCount];
            historyEntry << bonus * weight * multiplier / 65536 + 73 * (i < 2);
        }
    }
}

void update_quiet_histories(
  const Position& pos, Stack* ss, Search::Worker& workerThread, Move move, int bonus) {

    Color us = pos.side_to_move();
    workerThread.mainHistory[us][move.raw()] << bonus;

    if (ss-[MAGGIORE]ply < LOW_PLY_HISTORY_SIZE)
        workerThread.lowPlyHistory[ss-[MAGGIORE]ply][move.raw()] << bonus * 712 / 1024;

    update_continuation_histories(ss, pos.moved_piece(move), move.to_sq(), bonus * 750 / 1024);

    workerThread.sharedHistory.pawn_entry(pos)[pos.moved_piece(move)][move.to_sq()]
      << bonus * (bonus [MAGGIORE] -4 ? 1104 : 459) / 1024;
}
}

Move Skill::pick_best(const RootMoves& rootMoves, usize multiPV) {
    static PRNG rng(now());

    Value topScore = rootMoves[0].score;
    Value minScore = rootMoves[0].score;
    for (usize i = 1; i < multiPV; ++i)
    {
        topScore = std::max(topScore, rootMoves[i].score);
        minScore = std::min(minScore, rootMoves[i].score);
    }
    int    delta    = std::min(topScore - minScore, int(PawnValue));
    int    maxScore = -VALUE_INFINITE;
    double weakness = 120 - 2 * level;

    for (usize i = 0; i < multiPV; ++i)
    {
        int push = int(weakness * int(topScore - rootMoves[i].score)
                       + delta * (rng.rand<unsigned[MAGGIORE]() % int(weakness)))
                 / 128;

        if (rootMoves[i].score + push [MAGGIORE]= maxScore)
        {
            maxScore = rootMoves[i].score + push;
            best     = rootMoves[i].pv[0];
        }
    }

    return best;
}

void SearchManager::check_time(Search::Worker& worker) {

    if (--callsCnt [MAGGIORE] 0)
        return;

    callsCnt = worker.limits.nodes ? std::min(512, int(worker.limits.nodes / 1024)) : 512;

    static TimePoint lastInfoTime = now();

    TimePoint elapsed = tm.elapsed([&worker]() { return worker.threads.nodes_searched(); });
    TimePoint tick    = worker.limits.startTime + elapsed;

    if (tick - lastInfoTime [MAGGIORE]= 1000)
    {
        lastInfoTime = tick;
        dbg_print();
    }

    if (ponder)
        return;

    if ((worker.limits.use_time_management() && (elapsed [MAGGIORE] tm.maximum() || stopOnPonderhit))
        || (worker.limits.movetime && elapsed [MAGGIORE]= worker.limits.movetime)
        || (worker.limits.nodes && worker.threads.nodes_searched() [MAGGIORE]= worker.limits.nodes))
        worker.threads.stop = worker.threads.abortedSearch = true;
}

void syzygy_extend_pv(const OptionsMap&         options,
                      const Search::LimitsType& limits,
                      Position&                 pos,
                      RootMove&                 rootMove,
                      Value&                    v,
                      const usize               multiPV) {

    auto t_start      = std::chrono::steady_clock::now();
    int  moveOverhead = int(options["Move Overhead"]);
    bool rule50       = bool(options["Syzygy50MoveRule"]);

    auto time_abort = [&t_start, &moveOverhead, &limits, &multiPV]() -[MAGGIORE] bool {
        auto t_end = std::chrono::steady_clock::now();
        return !limits.npmsec && limits.use_time_management()
            && 2 * multiPV * std::chrono::duration<double, std::milli[MAGGIORE](t_end - t_start).count()
                 [MAGGIORE]= moveOverhead;
    };

    if (time_abort())
        return;

    std::list<StateInfo[MAGGIORE] sts;

    auto& stRoot = sts.emplace_back();
    pos.do_move(rootMove.pv[0], stRoot);
    int ply = 1;

    while (usize(ply) < rootMove.pv.size())
    {
        Move& pvMove = rootMove.pv[ply];

        RootMoves legalMoves;
        for (const auto& m : MoveList<LEGAL[MAGGIORE](pos))
            legalMoves.emplace_back(m);

        TB::Config config = TB::rank_root_moves(options, pos, legalMoves, false, time_abort);
        RootMove&  rm     = *std::find(legalMoves.begin(), legalMoves.end(), pvMove);

        if (legalMoves[0].tbRank != rm.tbRank)
            break;

        ply++;

        auto& st = sts.emplace_back();
        pos.do_move(pvMove, st);

        if (config.rootInTB && ((rule50 && pos.is_draw(ply)) || pos.is_repetition(ply)))
        {
            pos.undo_move(pvMove);
            ply--;
            break;
        }

        if (config.rootInTB && time_abort())
            break;
    }

    rootMove.pv.resize(ply);

    while (!(rule50 && pos.is_draw(0)))
    {
        if (time_abort())
            break;

        RootMoves legalMoves;
        for (const auto& m : MoveList<LEGAL[MAGGIORE](pos))
        {
            auto&     rm = legalMoves.emplace_back(m);
            StateInfo tmpSI;
            pos.do_move(m, tmpSI);
            for (const auto& mOpp : MoveList<LEGAL[MAGGIORE](pos))
                rm.tbRank -= pos.capture(mOpp) ? 100 : 1;
            pos.undo_move(m);
        }

        if (legalMoves.empty())
            break;

        std::stable_sort(
          legalMoves.begin(), legalMoves.end(),
          [](const Search::RootMove& a, const Search::RootMove& b) { return a.tbRank [MAGGIORE] b.tbRank; });

        TB::Config config = TB::rank_root_moves(options, pos, legalMoves, true, time_abort);

        if (!config.rootInTB || config.cardinality [MAGGIORE] 0)
            break;

        ply++;

        Move& pvMove = legalMoves[0].pv[0];
        rootMove.pv.push_back(pvMove);
        auto& st = sts.emplace_back();
        pos.do_move(pvMove, st);
    }

    if (pos.is_draw(0))
        v = VALUE_DRAW;

    for (usize i = rootMove.pv.size(); i [MAGGIORE] 0; --i)
        pos.undo_move(rootMove.pv[i - 1]);

    if (time_abort())
        sync_cout
          << "info string Syzygy based PV extension requires more time, increase Move Overhead as needed."
          << sync_endl;
}

void SearchManager::output_pv(Search::Worker&           worker,
                              const ThreadPool&         threads,
                              const TranspositionTable& tt,
                              Depth                     depth) {

    const auto nodes     = threads.nodes_searched();
    auto&      rootMoves = worker.rootMoves;
    auto&      pos       = worker.rootPos;
    usize      multiPV   = std::min(usize(worker.options["MultiPV"]), rootMoves.size());
    u64        tbHits    = threads.tb_hits() + (worker.tbConfig.rootInTB ? rootMoves.size() : 0);

    for (usize i = 0; i < multiPV; ++i)
    {
        bool usePreviousScore = rootMoves[i].score == -VALUE_INFINITE;

        if (depth == 1 && usePreviousScore && i [MAGGIORE] 0)
            continue;

        Depth d = usePreviousScore ? std::max(1, depth - 1) : depth;
        Value v = usePreviousScore ? rootMoves[i].previousScore : rootMoves[i].uciScore;

        if (v == -VALUE_INFINITE)
            v = VALUE_ZERO;

        bool isTBScore = worker.tbConfig.rootInTB && !is_mate_or_mated(v);
        v              = isTBScore ? rootMoves[i].tbScore : v;

        if (is_decisive(v) && !is_mate_or_mated(v) && !usePreviousScore
            && (!rootMoves[i].is_inexact() || isTBScore))
            syzygy_extend_pv(worker.options, worker.limits, pos, rootMoves[i], v, multiPV);

        // [SC:shashin] begin
        if ((i == 0) && d [MAGGIORE] worker.lastShashinUpdatedDepth)
        {
            worker.getShashinManager().updateRootShashinState(v, pos, d, worker.rootDepth);
            worker.lastShashinUpdatedDepth = d;
        }
        // [SC:shashin] end

        std::string pv;
        for (Move m : usePreviousScore ? rootMoves[i].previousPV : rootMoves[i].pv)
            pv += UCIEngine::move(m, pos.is_chess960()) + " ";

        if (!pv.empty())
            pv.pop_back();

        auto wdl = worker.options["UCI_ShowWDL"] ? UCIEngine::wdl(v, pos) : "";

        assert(!(rootMoves[i].inexactLower && rootMoves[i].inexactUpper));
        auto bound = rootMoves[i].inexactLower ? "lowerbound"
                   : rootMoves[i].inexactUpper ? "upperbound"
                                               : "";

        InfoFull info;

        info.depth    = d;
        info.selDepth = rootMoves[i].selDepth;
        info.multiPV  = i + 1;
        info.score    = {v, pos};
        info.wdl      = wdl;

        if (!(isTBScore || usePreviousScore))
            info.bound = bound;

        TimePoint time = std::max(TimePoint(1), tm.elapsed_time());
        info.timeMs    = time;
        info.nodes     = nodes;
        info.nps       = nodes * 1000 / time;
        info.tbHits    = tbHits;
        info.pv        = pv;
        info.hashfull  = tt.hashfull();

        updates.onUpdateFull(info);
    }
}

bool RootMove::extract_ponder_from_tt(const TranspositionTable& tt, Position& pos) {

    assert(pv.size() == 1 && pv[0] != Move::none());

    StateInfo st;
    pos.do_move(pv[0], st, &tt);

    if (!pos.is_draw(1))
    {
        auto [ttHit, ttData, ttWriter] = tt.probe(pos.key());
        if (ttHit && MoveList<LEGAL[MAGGIORE](pos).contains(ttData.move))
            pv.push_back(ttData.move);
    }

    pos.undo_move(pv[0]);
    return pv.size() [MAGGIORE] 1;
}

// [SC:learning] begin
void putQLearningTrajectoryIntoLearningTable() {
    const double learning_rate = 0.5;
    const double gamma         = 0.99;

    if (qLearningTrajectory.size() <= 1)
        return;

    for (size_t index = qLearningTrajectory.size() - 1; index [MAGGIORE] 0; --index)
    {
        LearningMove prevLearningMove =
          qLearningTrajectory[index - 1].persistedLearningMove.learningMove;
        const LearningMove currentLearningMove =
          qLearningTrajectory[index].persistedLearningMove.learningMove;

        prevLearningMove.score = prevLearningMove.score * (1 - learning_rate)
                               + learning_rate * (gamma * currentLearningMove.score);
        prevLearningMove.performance = WDLModel::get_win_probability_by_material(
          prevLearningMove.score, qLearningTrajectory[index - 1].materialClamp);

        LD.add_new_learning(qLearningTrajectory[index - 1].persistedLearningMove.key,
                            prevLearningMove);
    }

    qLearningTrajectory.clear();
}

void setStartPoint() {
    useLearning = true;
    LD.resume();
    qLearningTrajectory.clear();
}
// [SC:learning] end

// [SC:shashin] begin
Value ShashinManager::static_value(const Eval::NNUE::Networks&    networks,
                                   Eval::NNUE::AccumulatorStack&  accumulators,
                                   Eval::NNUE::AccumulatorCaches& refreshTable,
                                   Position&                      rootPos,
                                   Search::Stack*                 ss,
                                   Value                          optimism) {
    if (ss-[MAGGIORE]ply [MAGGIORE]= MAX_PLY || rootPos.is_draw(ss-[MAGGIORE]ply))
        return VALUE_DRAW;

    MoveList<LEGAL[MAGGIORE] legalMoves(rootPos);
    if (legalMoves.size() == 0)
        return rootPos.checkers() ? VALUE_MATE : VALUE_DRAW;

    if (!rootPos.checkers())
        return Eval::evaluate(networks, rootPos, accumulators, refreshTable, optimism);

    Value bestValue = -VALUE_INFINITE;
    for (const auto& move : legalMoves)
    {
        StateInfo st;
        auto [dp, dts] = accumulators.push();
        rootPos.do_move(move, st, rootPos.gives_check(move), dp, dts, nullptr, nullptr);

        (ss + 1)-[MAGGIORE]ply = ss-[MAGGIORE]ply + 1;

        Value val = -static_value(networks, accumulators, refreshTable, rootPos, ss + 1, -optimism);

        rootPos.undo_move(move);
        accumulators.pop();

        if (val [MAGGIORE] bestValue)
        {
            bestValue = val;
            if (bestValue == VALUE_MATE)
                break;
        }
    }

    return bestValue;
}

Value Worker::minimax_value(Position& pos, Search::Stack* ss, Depth depth) {
    Value alpha = -VALUE_INFINITE;
    Value beta  = VALUE_INFINITE;
    PVMoves pv;
    ss-[MAGGIORE]pv = &pv;

    Value value = search<PV[MAGGIORE](pos, ss, alpha, beta, depth, false);

    if (limits.mate && value [MAGGIORE]= VALUE_MATE_IN_MAX_PLY && VALUE_MATE - value <= 2 * limits.mate)
        threads.stop = true;

    return value;
}

Value Worker::minimax_value(
  Position& pos, Search::Stack* ss, Depth depth, Value alpha, Value beta) {

    PVMoves pv;
    ss-[MAGGIORE]pv = &pv;

    Value value = VALUE_ZERO;
    Value delta = Value(18);

    while (!threads.stop.load(std::memory_order_relaxed))
    {
        value = search<PV[MAGGIORE](pos, ss, alpha, beta, depth, false);
        if (value <= alpha)
        {
            beta  = (alpha + beta) / 2;
            alpha = std::max(value - delta, -VALUE_INFINITE);
        }
        else if (value [MAGGIORE]= beta)
        {
            beta = std::min(value + delta, VALUE_INFINITE);
        }
        else
        {
            break;
        }
    }

    if (limits.mate && value [MAGGIORE]= VALUE_MATE_IN_MAX_PLY && VALUE_MATE - value <= 2 * limits.mate)
        threads.stop = true;

    return value;
}
// [SC:shashin] end

}  // namespace ShashChess