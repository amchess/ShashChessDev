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
#include <utility>
#include <cmath>  //for ShashChess
#include "misc.h"
//for ShashChess

namespace ShashChess {
using namespace ShashChess::Shashin;  //shashin
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

template<GenType Type>
void MovePicker::score() {

    static_assert(Type == CAPTURES || Type == QUIETS || Type == EVASIONS, "Wrong type");

    Color      us       = pos.side_to_move();
    const bool fortress = MoveConfig::isFortress;  // Recupera lo stato della fortezza

    [[maybe_unused]] Bitboard threatenedPieces, threatByLesser[QUEEN + 1];
    if constexpr (Type == QUIETS)
    {
        threatByLesser[KNIGHT] = threatByLesser[BISHOP] = pos.attacks_by<PAWN>(~us);
        threatByLesser[ROOK] =
          pos.attacks_by<KNIGHT>(~us) | pos.attacks_by<BISHOP>(~us) | threatByLesser[KNIGHT];
        threatByLesser[QUEEN] = pos.attacks_by<ROOK>(~us) | threatByLesser[ROOK];
    }

    // Precalcolo bonus dinamico per Shashin
    const int phase           = std::clamp(pos.game_ply() / 25, 0, 3);
    const int breakingPenalty = 18000 + 6000 * phase;
    const int preservingBonus = 9000 + 3000 * (3 - phase);

    for (auto& m : *this)
    {
        const Square    from = m.from_sq();
        const Square    to   = m.to_sq();
        const Piece     pc   = pos.moved_piece(m);
        const PieceType pt   = type_of(pc);

        if constexpr (Type == CAPTURES)
        {
            // Logica base di Stockfish
            m.value = (*captureHistory)[pc][to][type_of(pos.piece_on(to))]
                    + 7 * int(PieceValue[pos.piece_on(to)])
                    + 1024 * bool(pos.check_squares(pt) & to);

            // Aggiunta specifica ShashChess
            if (MoveConfig::isAggressive)
                m.value += 5000;  // Extra bonus per catture aggressive
        }

        else if constexpr (Type == QUIETS)
        {
            // Logica base di Stockfish
            m.value = 2 * (*mainHistory)[us][m.from_to()]
                    + 2 * (*pawnHistory)[pawn_structure_index(pos)][pc][to];

            for (int i : {0, 1, 2, 3, 5})  // Loop ottimizzato
                m.value += (*continuationHistory[i])[pc][to];

            m.value += (bool(pos.check_squares(pt) & to) && pos.see_ge(m, -75)) * 16384;

            if (KNIGHT <= pt && pt <= QUEEN)
            {
                static constexpr int bonus[QUEEN + 1] = {0, 0, 144, 144, 256, 517};
                int v = (threatByLesser[pt] & to ? -95 : 100 * bool(threatByLesser[pt] & from));
                m.value += bonus[pt] * v;
            }

            if (ply < LOW_PLY_HISTORY_SIZE)
                m.value += 8 * (*lowPlyHistory)[ply][m.from_to()] / (1 + ply);

            // Aggiunte ShashChess
            if (MoveConfig::isStrategical)
                m.value += 2000;  // Bonus per mosse posizionali
        }

        else  // EVASIONS
        {
            // Logica base di Stockfish
            if (pos.capture_stage(m))
                m.value = PieceValue[pos.piece_on(to)] + (1 << 28);
            else
            {
                m.value = (*mainHistory)[us][m.from_to()] + (*continuationHistory[0])[pc][to];
                if (ply < LOW_PLY_HISTORY_SIZE)
                    m.value += 2 * (*lowPlyHistory)[ply][m.from_to()] / (1 + ply);
            }
        }

        // Sezione fortezze - ShashChess Core
        if (fortress)
        {
            if (is_fortress_breaking_move(pos, m))
            {
                m.value -= breakingPenalty;
                if (is_fortress_key_piece(pc))
                    m.value -= 4500;

                if (king_safety_score(pos, us) > 120)
                    m.value += 3000;
            }
            else if (is_fortress_preserving_move(pos, m))
            {
                m.value += preservingBonus;

                if (type_of(pc) == KING && phase == 3)
                    m.value += 2000;
            }

            // Gestione pedoni adiacenti
            if (is_fortress_preserving_move(pos, m))
            {
                File     f             = file_of(to);
                Bitboard adjacentPawns = pos.pieces(PAWN, us)
                                       & ((f > FILE_A ? file_bb(File(f - 1)) : 0)
                                          | (f < FILE_H ? file_bb(File(f + 1)) : 0));

                if (popcount(adjacentPawns) >= 2)
                    m.value += 5000;
            }

            // Regolazione finale
            m.value = no_progress_for(pos, 15)     ? m.value * 5 / 4
                    : !has_safe_waiting_moves(pos) ? m.value * 3 / 4
                                                   : m.value;
        }

        // Applica bonus dinamico globale
        if (MoveConfig::useMoveShashinLogic)
        {
            const double dynamicFactor =
              std::log(1.0 + std::min(pos.game_ply() / 20.0, 1.0) * 9.0) / std::log(10.0);
            m.value += MoveConfig::isStrategical ? static_cast<int>(600 * dynamicFactor)
                     : MoveConfig::isAggressive  ? static_cast<int>(1200 * dynamicFactor)
                                                 : 0;
        }
    }
}
//From Shashin end

// Returns the next move satisfying a predicate function.
// This never returns the TT move, as it was emitted before.
template<typename Pred>
Move MovePicker::select(Pred filter) {

    for (; cur < endMoves; ++cur)
    //from Shashin begin
    {
        if (*cur == ttMove)
            continue;
        if (!pos.legal(*cur))
            continue;
        if (filter())
            return *cur++;
    }
    //from Shashin end
    return Move::none();
}

// This is the most important method of the MovePicker class. We emit one
// new pseudo-legal move on every call until there are no more moves left,
// picking the move with the highest score from a list of generated moves.
Move MovePicker::next_move() {

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
                if (pos.see_ge(*cur, -cur->value / 18))
                    return true;
                std::swap(*endBadCaptures++, *cur);
                return false;
            }))
            return *(cur - 1);

        ++stage;
        [[fallthrough]];

    case QUIET_INIT :
        if (!skipQuiets)
        {
            cur = endBadQuiets = endBadCaptures;
            endMoves           = generate<QUIETS>(pos, cur);

            score<QUIETS>();
            partial_insertion_sort(cur, endMoves, -3560 * depth);
        }

        ++stage;
        [[fallthrough]];

    case GOOD_QUIET :
        if (!skipQuiets && select([&]() {
                return cur->value > -14000 ? true : (*endBadQuiets++ = *cur, false);
            }))
            return *(cur - 1);

        // Prepare the pointers to loop over the bad captures
        cur      = moves;
        endMoves = endBadCaptures;

        ++stage;
        [[fallthrough]];

    case BAD_CAPTURE :
        if (select([]() { return true; }))
            return *(cur - 1);

        // Prepare the pointers to loop over the bad quiets
        cur      = endBadCaptures;
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

// this function must be called after all quiet moves and captures have been generated
bool MovePicker::can_move_king_or_pawn() {
    assert(stage == GOOD_QUIET || stage == BAD_QUIET || stage == EVASION);

    for (ExtMove* m = moves; m < endMoves; ++m)
    {
        PieceType movedPieceType = type_of(pos.moved_piece(*m));
        if ((movedPieceType == PAWN || movedPieceType == KING) && pos.legal(*m))
            return true;
    }
    return false;
}

}  // namespace ShashChess