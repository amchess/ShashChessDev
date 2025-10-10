/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2025 The ShashChess developers (see AUTHORS file)

  ShashChess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  ShashChess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "evaluate.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <tuple>

#include "nnue/network.h"
#include "nnue/nnue_misc.h"
#include "position.h"
#include "types.h"
#include "uci.h"
#include "nnue/nnue_accumulator.h"

namespace ShashChess {

// Returns a static, purely materialistic evaluation of the position from
// the point of view of the side to move. It can be divided by PawnValue to get
// an approximation of the material advantage on the board in terms of pawns.
int Eval::simple_eval(const Position& pos) {
    Color c = pos.side_to_move();
    return PawnValue * (pos.count<PAWN>(c) - pos.count<PAWN>(~c))
         + (pos.non_pawn_material(c) - pos.non_pawn_material(~c));
}

bool Eval::use_smallnet(const Position& pos) { return std::abs(simple_eval(pos)) > 962; }

// Evaluate is the evaluator for the outer world. It returns a static evaluation
// of the position from the point of view of the side to move.
Value Eval::evaluate(const Eval::NNUE::Networks&    networks,
                     const Position&                pos,
                     Eval::NNUE::AccumulatorStack&  accumulators,
                     Eval::NNUE::AccumulatorCaches& caches,
                     int                            optimism) {

    assert(!pos.checkers());

    bool smallNet           = use_smallnet(pos);
    auto [psqt, positional] = smallNet ? networks.small.evaluate(pos, accumulators, &caches.small)
                                       : networks.big.evaluate(pos, accumulators, &caches.big);

    Value nnue = (125 * psqt + 131 * positional) / 128;

    // Re-evaluate the position when higher eval accuracy is worth the time spent
    if (smallNet && (std::abs(nnue) < 236))
    {
        std::tie(psqt, positional) = networks.big.evaluate(pos, accumulators, &caches.big);
        nnue                       = (125 * psqt + 131 * positional) / 128;
        smallNet                   = false;
    }

    // Blend optimism and eval with nnue complexity
    int nnueComplexity = std::abs(psqt - positional);
    optimism += optimism * nnueComplexity / 468;
    nnue -= nnue * nnueComplexity / 18000;

    int material = 535 * pos.count<PAWN>() + pos.non_pawn_material();
    int v        = (nnue * (77777 + material) + optimism * (7777 + material)) / 77777;

    // Damp down the evaluation linearly when shuffling
    v -= v * pos.rule50_count() / 212;

    // Guarantee evaluation does not hit the tablebase range
    v = std::clamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);

    return v;
}
//more expressive trace begin
namespace {

// Funzione per determinare la zona di Shashin basata sulla win probability
std::string get_shashin_zone(uint8_t winProb, Color sideToMove) {
    if (sideToMove == WHITE)
    {
        if (winProb <= 2)
            return "High Petrosian (Losing position)";
        else if (winProb <= 5)
            return "High-Middle Petrosian (Decisive disadvantage)";
        else if (winProb <= 10)
            return "Middle Petrosian (Clear disadvantage)";
        else if (winProb <= 20)
            return "Middle-Low Petrosian (Significant disadvantage)";
        else if (winProb <= 35)
            return "Low Petrosian (Slight disadvantage)";
        else if (winProb <= 45)
            return "Chaos: Capablanca-Petrosian (Balanced with opponent pressure)";
        else if (winProb >= 55 && winProb <= 65)
            return "Capablanca (Equal position)";
        else if (winProb <= 55)
            return "Chaos: Capablanca-Petrosian (Balanced with opponent pressure)";
        else if (winProb <= 75)
            return "Chaos: Capablanca-Tal (Initiative)";
        else if (winProb <= 80)
            return "Low Tal (Slight advantage)";
        else if (winProb <= 85)
            return "Middle-Low Tal (Growing advantage)";
        else if (winProb <= 90)
            return "Middle Tal (Clear advantage)";
        else if (winProb <= 95)
            return "High-Middle Tal (Dominant position)";
        else
            return "High Tal (Winning position)";
    }
    else
    {
        // Per il nero, invertiamo le probabilità
        uint8_t blackWinProb = 100 - winProb;
        if (blackWinProb <= 2)
            return "High Petrosian (Losing position)";
        else if (blackWinProb <= 5)
            return "High-Middle Petrosian (Decisive disadvantage)";
        else if (blackWinProb <= 10)
            return "Middle Petrosian (Clear disadvantage)";
        else if (blackWinProb <= 20)
            return "Middle-Low Petrosian (Significant disadvantage)";
        else if (blackWinProb <= 35)
            return "Low Petrosian (Slight disadvantage)";
        else if (blackWinProb <= 45)
            return "Chaos: Capablanca-Petrosian (Balanced with opponent pressure)";
        else if (blackWinProb >= 55 && blackWinProb <= 65)
            return "Capablanca (Equal position)";
        else if (blackWinProb <= 55)
            return "Chaos: Capablanca-Petrosian (Balanced with opponent pressure)";
        else if (blackWinProb <= 75)
            return "Chaos: Capablanca-Tal (Initiative)";
        else if (blackWinProb <= 80)
            return "Low Tal (Slight advantage)";
        else if (blackWinProb <= 85)
            return "Middle-Low Tal (Growing advantage)";
        else if (blackWinProb <= 90)
            return "Middle Tal (Clear advantage)";
        else if (blackWinProb <= 95)
            return "High-Middle Tal (Dominant position)";
        else
            return "High Tal (Winning position)";
    }
}

// Funzione per analizzare l'attività dei pezzi in termini di win probability
std::string analyze_piece_activity_win_prob(const Position& pos) {
    std::vector<std::pair<std::string, int>> whitePieces, blackPieces;

    // Analizza i pezzi bianchi
    Bitboard whiteBB = pos.pieces(WHITE);
    while (whiteBB)
    {
        Square    sq = pop_lsb(whiteBB);
        Piece     pc = pos.piece_on(sq);
        PieceType pt = type_of(pc);
        if (pt == KING)
            continue;

        std::string pieceName;
        switch (pt)
        {
        case PAWN :
            pieceName = "P";
            break;
        case KNIGHT :
            pieceName = "N";
            break;
        case BISHOP :
            pieceName = "B";
            break;
        case ROOK :
            pieceName = "R";
            break;
        case QUEEN :
            pieceName = "Q";
            break;
        default :
            continue;
        }

        // Calcola attività più accurata
        Bitboard attacks  = attacks_bb(pt, sq, pos.pieces());
        int      activity = popcount(attacks) * 2;

        // Bonus per controllo centro
        constexpr Bitboard Center = (FileDBB | FileEBB) & (Rank4BB | Rank5BB);
        if (attacks & Center)
            activity += 5;

        // Bonus per attacchi al re avversario
        Square   enemyKingSq = pos.square<KING>(~WHITE);
        Bitboard kingZone    = attacks_bb<KING>(enemyKingSq) | enemyKingSq;
        if (attacks & kingZone)
            activity += 8;

        // Bonus per pezzi sviluppati
        if (pt != PAWN)
        {
            if (color_of(pc) == WHITE && rank_of(sq) > RANK_2)
                activity += 3;
            if (color_of(pc) == BLACK && rank_of(sq) < RANK_7)
                activity += 3;
        }

        whitePieces.emplace_back(pieceName + UCIEngine::square(sq), activity);
    }

    // Analizza i pezzi neri (stessa logica)
    Bitboard blackBB = pos.pieces(BLACK);
    while (blackBB)
    {
        Square    sq = pop_lsb(blackBB);
        Piece     pc = pos.piece_on(sq);
        PieceType pt = type_of(pc);
        if (pt == KING)
            continue;

        std::string pieceName;
        switch (pt)
        {
        case PAWN :
            pieceName = "P";
            break;
        case KNIGHT :
            pieceName = "N";
            break;
        case BISHOP :
            pieceName = "B";
            break;
        case ROOK :
            pieceName = "R";
            break;
        case QUEEN :
            pieceName = "Q";
            break;
        default :
            continue;
        }

        Bitboard attacks  = attacks_bb(pt, sq, pos.pieces());
        int      activity = popcount(attacks) * 2;

        constexpr Bitboard Center = (FileDBB | FileEBB) & (Rank4BB | Rank5BB);
        if (attacks & Center)
            activity += 5;

        Square   enemyKingSq = pos.square<KING>(~BLACK);
        Bitboard kingZone    = attacks_bb<KING>(enemyKingSq) | enemyKingSq;
        if (attacks & kingZone)
            activity += 8;

        if (pt != PAWN)
        {
            if (color_of(pc) == WHITE && rank_of(sq) > RANK_2)
                activity += 3;
            if (color_of(pc) == BLACK && rank_of(sq) < RANK_7)
                activity += 3;
        }

        blackPieces.emplace_back(pieceName + UCIEngine::square(sq), activity);
    }

    // Ordina per attività
    std::sort(whitePieces.begin(), whitePieces.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    std::sort(blackPieces.begin(), blackPieces.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    std::stringstream ss;
    ss << "White pieces (worst to best activity):\n";
    for (size_t i = 0; i < whitePieces.size(); ++i)
    {
        ss << "  " << whitePieces[i].first << " (score: " << whitePieces[i].second << ")";
        if (i == 0)
            ss << " <-- Worst unit (Makogonov Principle)";
        ss << "\n";
    }

    ss << "Black pieces (worst to best activity):\n";
    for (size_t i = 0; i < blackPieces.size(); ++i)
    {
        ss << "  " << blackPieces[i].first << " (score: " << blackPieces[i].second << ")";
        if (i == 0)
            ss << " <-- Worst unit (Makogonov Principle)";
        ss << "\n";
    }

    return ss.str();
}
}  // namespace


// Like evaluate(), but instead of returning a value, it returns
// a string (suitable for outputting to stdout) that contains the detailed
// descriptions and values of each evaluation term. Useful for debugging.
// Trace scores are from white's point of view
std::string Eval::trace(Position& pos, const Eval::NNUE::Networks& networks, bool showWDL) {

    if (pos.checkers())
        return "Final evaluation: none (in check)";

    Eval::NNUE::AccumulatorStack accumulators;
    auto                         caches = std::make_unique<Eval::NNUE::AccumulatorCaches>(networks);

    std::stringstream ss;

    // Inizializza il modello WDL se necessario
    if (!WDLModel::is_initialized())
    {
        WDLModel::init();
    }

    // Calcola la valutazione finale
    Value v       = evaluate(networks, pos, accumulators, *caches, VALUE_ZERO);
    Value v_white = pos.side_to_move() == WHITE ? v : -v;

    if (showWDL)
    {
        // Calcola win probability e WDL
        uint8_t       winProb = WDLModel::get_win_probability(v, pos);
        WDLModel::WDL wdl     = WDLModel::get_wdl(v, pos);

        // Determina la zona di Shashin
        std::string shashinZone = get_shashin_zone(winProb, pos.side_to_move());

        // Mostra la trace NNUE in centipawn
        ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);
        ss << '\n' << NNUE::trace(pos, networks, *caches) << '\n';  // Sempre false per centipawn

        ss << "=== SHASHIN STATIC ANALYSIS ===\n";
        ss << "*** Static evaluation based on NNUE network ***\n";
        ss << "*** All NNUE values shown in centipawn for technical accuracy ***\n";
        ss << "*** Win probability derived from overall evaluation ***\n\n";

        // Analisi dell'attività dei pezzi
        ss << "--- PIECE ACTIVITY ANALYSIS ---\n";
        ss << analyze_piece_activity_win_prob(pos) << "\n";

        ss << "================================\n";
        ss << "Final Evaluation (Static): " << static_cast<int>(winProb) << "% win probability\n";
        ss << "WDL (Win/Draw/Loss): " << static_cast<int>(wdl.win) << "%/"
           << static_cast<int>(wdl.draw) << "%/" << static_cast<int>(wdl.loss) << "%\n";
        ss << "Shashin Zone: " << shashinZone << "\n";
        ss << "*** Note: Win probability analysis based on current piece placement ***\n";
    }
    else
    {
        // Comportamento tradizionale
        ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);
        ss << '\n' << NNUE::trace(pos, networks, *caches) << '\n';
        ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2) << std::setw(15);

        auto [psqt, positional] = networks.big.evaluate(pos, accumulators, &caches->big);
        Value v_nnue            = psqt + positional;
        v_nnue                  = pos.side_to_move() == WHITE ? v_nnue : -v_nnue;
        ss << "NNUE evaluation        " << 0.01 * UCIEngine::to_cp(v_nnue, pos)
           << " (white side)\n";

        ss << "Final evaluation       " << 0.01 * UCIEngine::to_cp(v_white, pos) << " (white side)";
        ss << " [with scaled NNUE, ...]";
        ss << "\n";
    }

    return ss.str();
}
//more expressive trace end
}  // namespace ShashChess