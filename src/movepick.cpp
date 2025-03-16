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

#include "movepick.h"

#include <cassert>
#include <limits>
#include <cmath>  //for ShashChess

#include "bitboard.h"
#include "misc.h"
//for ShashChess

namespace ShashChess {

namespace {

enum Stages {
    // generate main search moves
    MAIN_TT,
    CAPTURE_INIT,
    GOOD_CAPTURE,
    QUIET_INIT,
    GOOD_QUIET,
    BAD_CAPTURE,
    BAD_QUIET,

    // generate evasion moves
    EVASION_TT,
    EVASION_INIT,
    EVASION,

    // generate probcut moves
    PROBCUT_TT,
    PROBCUT_INIT,
    PROBCUT,

    // generate qsearch moves
    QSEARCH_TT,
    QCAPTURE_INIT,
    QCAPTURE
};

// Sort moves in descending order up to and including a given limit.
// The order of moves smaller than the limit is left unspecified.
void partial_insertion_sort(ExtMove* begin, ExtMove* end, int limit) {

    for (ExtMove *sortedEnd = begin, *p = begin + 1; p < end; ++p)
        if (p->value >= limit)
        {
            ExtMove tmp = *p, *q;
            *p          = *++sortedEnd;
            for (q = sortedEnd; q != begin && *(q - 1) < tmp; --q)
                *q = *(q - 1);
            *q = tmp;
        }
}

}  // namespace


// Constructors of the MovePicker class. As arguments, we pass information
// to decide which class of moves to emit, to help sorting the (presumably)
// good moves first, and how important move ordering is at the current node.

// MovePicker constructor for the main search and for the quiescence search
MovePicker::MovePicker(const Position&              p,
                       Move                         ttm,
                       Depth                        d,
                       const ButterflyHistory*      mh,
                       const LowPlyHistory*         lph,
                       const CapturePieceToHistory* cph,
                       const PieceToHistory**       ch,
                       const PawnHistory*           ph,
                       int                          pl) :
    pos(p),
    mainHistory(mh),
    lowPlyHistory(lph),
    captureHistory(cph),
    continuationHistory(ch),
    pawnHistory(ph),
    ttMove(ttm),
    depth(d),
    ply(pl) {

    stage = (pos.checkers() ? EVASION_TT : (depth > 0 ? MAIN_TT : QSEARCH_TT))
          + !(ttm && pos.pseudo_legal(ttm));  //faster than Stockfish
}

// MovePicker constructor for ProbCut: we generate captures with Static Exchange
// Evaluation (SEE) greater than or equal to the given threshold.
MovePicker::MovePicker(const Position& p, Move ttm, int th, const CapturePieceToHistory* cph) :
    pos(p),
    captureHistory(cph),
    ttMove(ttm),
    threshold(th) {
    assert(!pos.checkers());

    stage = PROBCUT_TT
          + !(ttm && pos.capture_stage(ttm) && pos.pseudo_legal(ttm) && pos.see_ge(ttm, threshold));
}

// Assigns a numerical value to each move in a list, used for sorting.
// Captures are ordered by Most Valuable Victim (MVV), preferring captures
// with a good history. Quiets moves are ordered using the history tables.
template<GenType Type>
void MovePicker::score() {

    static_assert(Type == CAPTURES || Type == QUIETS || Type == EVASIONS, "Wrong type");

    Bitboard threatenedByPawn, threatenedByMinor, threatenedByRook,
      threatenedPieces;  //used variables for ShashChess
    if constexpr (Type == QUIETS)
    {
        Color us = pos.side_to_move();

        threatenedByPawn = pos.attacks_by<PAWN>(~us);
        threatenedByMinor =
          pos.attacks_by<KNIGHT>(~us) | pos.attacks_by<BISHOP>(~us) | threatenedByPawn;
        threatenedByRook = pos.attacks_by<ROOK>(~us) | threatenedByMinor;

        // Pieces threatened by pieces of lesser material value
        threatenedPieces = (pos.pieces(us, QUEEN) & threatenedByRook)
                         | (pos.pieces(us, ROOK) & threatenedByMinor)
                         | (pos.pieces(us, KNIGHT, BISHOP) & threatenedByPawn);
    }
    //fortress by Shashin begin
    // Compute a scaling factor for the Shashin bonus.
    static double precomputedShashinScaling =
      std::min(1.0, static_cast<double>(pos.game_ply()) / 20.0);
    double dynamicBonus = std::log(1.0 + precomputedShashinScaling * 9.0) / std::log(10.0);
    // Read the fortress flag (set in initDynamicRootShashinState using the root position)
    const bool fortress            = MoveConfig::isFortress;
    const bool useMoveShashinLogic = MoveConfig::useMoveShashinLogic;
    const bool isStrategical       = MoveConfig::isStrategical;
    const bool isAggressive        = MoveConfig::isAggressive;

    // Iterate over each move in the list
    for (auto& m : *this)
    {
        // Apply Shashin logic: Adjust bonus based on position type,
        // using the dynamic (log-scaled) bonus factor.
        Piece  movedPiece = pos.moved_piece(m);
        Square to         = m.to_sq();
        int    baseValue = 0, shashinBonus = 0, fortressPenalty = 0;

        if (useMoveShashinLogic)
        {
            shashinBonus = isStrategical ? static_cast<int>(600 * dynamicBonus)
                         : isAggressive  ? static_cast<int>(1200 * dynamicBonus)
                                         : 0;
        }
        if constexpr (Type == CAPTURES)
        {
            baseValue = 7 * int(PieceValue[pos.piece_on(to)])
                      + (*captureHistory)[movedPiece][to][type_of(pos.piece_on(to))];
            if (isAggressive)
                baseValue += 5000;  // extra bonus for aggressive captures
        }
        else if constexpr (Type == QUIETS)
        {
            PieceType pt   = type_of(movedPiece);
            Square    from = m.from_sq();
            baseValue      = 2 * (*mainHistory)[pos.side_to_move()][m.from_to()]
                      + 2 * (*pawnHistory)[pawn_structure_index(pos)][movedPiece][to]
                      + (*continuationHistory[0])[movedPiece][to]
                      + (*continuationHistory[1])[movedPiece][to]
                      + (*continuationHistory[2])[movedPiece][to]
                      + (*continuationHistory[3])[movedPiece][to]
                      + (*continuationHistory[4])[movedPiece][to] / 3
                      + (*continuationHistory[5])[movedPiece][to]
                      + (pos.check_squares(pt) & to ? 16384 : 0);  // bonus for checks
            // Bonus per fuggire dalla cattura
            if (threatenedPieces & from)
            {
                if (pt == QUEEN && !(to & threatenedByRook))
                    baseValue += 51700;
                else if (pt == ROOK && !(to & threatenedByMinor))
                    baseValue += 25600;
                else if (!(to & threatenedByPawn))
                    baseValue += 14450;
            }

            // Malus per mettere il pezzo in presa
            if (pt == QUEEN && (to & threatenedByRook))
                baseValue -= 49000;
            else if (pt == ROOK && (to & threatenedByMinor))
                baseValue -= 24335;

            // Bonus per le prime profondità basato sulla history table
            if (ply < LOW_PLY_HISTORY_SIZE)
                baseValue += 8 * (*lowPlyHistory)[ply][m.from_to()] / (1 + 2 * ply);
            if (isStrategical)
                baseValue += 2000;  // bonus for safe, positional moves
        }
        else
        {  // EVASIONS
            if (pos.capture_stage(m))
                baseValue = PieceValue[pos.piece_on(m.to_sq())] + (1 << 28);
            else
                baseValue = (*mainHistory)[pos.side_to_move()][m.from_to()]
                          + (*continuationHistory[0])[pos.moved_piece(m)][to]
                          + (*pawnHistory)[pawn_structure_index(pos)][pos.moved_piece(m)][to];
        }

        // If in a fortress, penalize moves that break the defensive setup
        // and give a slight bonus for moves that preserve it.
        if (fortress)
        {
            if (pos.is_fortress_breaking_move(m))
                fortressPenalty = 10000;
            if (pos.is_fortress_preserving_move(m))
                baseValue += 500;
        }

        // Compute the final move value
        m.value = baseValue + shashinBonus - fortressPenalty;
    }
}
//From Shashin end

// Returns the next move satisfying a predicate function.
// This never returns the TT move, as it was emitted before.
template<typename Pred>
Move MovePicker::select(Pred filter) {

    for (; cur < endMoves; ++cur)
        if (*cur != ttMove && filter())
            return *cur++;

    return Move::none();
}

// This is the most important method of the MovePicker class. We emit one
// new pseudo-legal move on every call until there are no more moves left,
// picking the move with the highest score from a list of generated moves.
Move MovePicker::next_move() {

    auto quiet_threshold = [](Depth d) { return -3560 * d; };

top:
    switch (stage)
    {

    case MAIN_TT :
    case EVASION_TT :
    case QSEARCH_TT :
    case PROBCUT_TT :
        ++stage;
        return ttMove;

    case CAPTURE_INIT :
    case PROBCUT_INIT :
    case QCAPTURE_INIT :
        cur = endBadCaptures = moves;
        endMoves             = generate<CAPTURES>(pos, cur);

        score<CAPTURES>();
        partial_insertion_sort(cur, endMoves, std::numeric_limits<int>::min());
        ++stage;
        goto top;

    case GOOD_CAPTURE :
        if (select([&]() {
                // Move losing capture to endBadCaptures to be tried later
                return pos.see_ge(*cur, -cur->value / 18) ? true
                                                          : (*endBadCaptures++ = *cur, false);
            }))
            return *(cur - 1);

        ++stage;
        [[fallthrough]];

    case QUIET_INIT :
        if (!skipQuiets)
        {
            cur      = endBadCaptures;
            endMoves = beginBadQuiets = endBadQuiets = generate<QUIETS>(pos, cur);

            score<QUIETS>();
            partial_insertion_sort(cur, endMoves, quiet_threshold(depth));
        }

        ++stage;
        [[fallthrough]];

    case GOOD_QUIET :
        if (!skipQuiets && select([]() { return true; }))
        {
            if ((cur - 1)->value > -7998 || (cur - 1)->value <= quiet_threshold(depth))
                return *(cur - 1);

            // Remaining quiets are bad
            beginBadQuiets = cur - 1;
        }

        // Prepare the pointers to loop over the bad captures
        cur      = moves;
        endMoves = endBadCaptures;

        ++stage;
        [[fallthrough]];

    case BAD_CAPTURE :
        if (select([]() { return true; }))
            return *(cur - 1);

        // Prepare the pointers to loop over the bad quiets
        cur      = beginBadQuiets;
        endMoves = endBadQuiets;

        ++stage;
        [[fallthrough]];

    case BAD_QUIET :
        if (!skipQuiets)
            return select([]() { return true; });

        return Move::none();

    case EVASION_INIT :
        cur      = moves;
        endMoves = generate<EVASIONS>(pos, cur);

        score<EVASIONS>();
        partial_insertion_sort(cur, endMoves, std::numeric_limits<int>::min());
        ++stage;
        [[fallthrough]];

    case EVASION :
    case QCAPTURE :
        return select([]() { return true; });

    case PROBCUT :
        return select([&]() { return pos.see_ge(*cur, threshold); });
    }

    assert(false);
    return Move::none();  // Silence warning
}

void MovePicker::skip_quiet_moves() { skipQuiets = true; }

}  // namespace ShashChess
