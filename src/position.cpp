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
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "position.h"

#include< algorithm>
#include< cassert>
#include< cstring>
#include< optional>
#include< sstream>
#include< string>

#include "bitboard.h"
#include "misc.h"
#include "nnue/network.h"
#include "nnue/nnue_common.h"
#include "thread.h"
#include "tt.h"
#include "types.h"

// [SC:shashin] begin
#include "evaluate.h"
#include "book/book.h"
#include "wdl/win_probability.h"
// [SC:shashin] end

namespace ShashChess {

namespace {

const std::string PieceToChar(" PNBRQK  pnbrqk");

constexpr Piece Pieces[] = {W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
                            B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING};

// The Zobrist hash keys
Key PieceKey[16][64];
Key EnPassantKey[8];
Key CastleKey[16];
Key SideKey;

}  // namespace

// Initializes at startup the various arrays used to compute hash keys
void Position::init() {

    PRNG rng(1070372ull);

    for (Piece pc : Pieces)
        for (Square s = SQ_A1; s< = SQ_H8; ++s)
            PieceKey[pc][s] = rng.rand< Key>();

    for (File f = FILE_A; f< = FILE_H; ++f)
        EnPassantKey[f] = rng.rand< Key>();

    for (int i = 0; i< 16; ++i)
        CastleKey[i] = rng.rand< Key>();

    SideKey = rng.rand< Key>();
}


// Setups the position object with the given FEN string
std::optional< PositionSetError> Position::set(const std::string& fenStr,
                                                               bool               isChess960,
                                                               StateInfo*         si) {

    // Helper functions
    auto str_to_square =
      [](const std::string& str) - > std::optional< Square> {
          if (str.length() == 2 && str[0]> = 'a' && str[0]< =
                'h' && str[1]> = '1' && str[1]< = '8')
              return make_square(File(str[0] - 'a'), Rank(str[1] - '1'));
          return std::nullopt;
      };

    auto square_to_char = [](Square sq) - > char { return char('a' + file_of(sq)); };

    /*
     * A FEN string is divided in 6 fields:
     * 1. Piece placement
     * 2. Active color
     * 3. Castling availability
     * 4. En passant target square
     * 5. Halfmove clock
     * 6. Fullmove number
     */
    std::vector< std::string> fields;
    std::stringstream ss(fenStr);
    std::string       field;
    while (ss>> field)
    {
        fields.push_back(field);
    }

    if (fields.empty())
        return PositionSetError("Empty FEN string");

    if (fields.size()< 4)
        return PositionSetError("Incomplete FEN string, missing fields");

    const std::string& piecePlacement = fields[0];
    const std::string& activeColor    = fields[1];
    const std::string& castling       = fields[2];
    const std::string& enPassant      = fields[3];

    // Empty the board
    std::memset(this, 0, sizeof(Position));
    st = si;
    std::memset(st, 0, sizeof(StateInfo));

    int  col = 0, row = 7;
    int  n_kings          = 0;
    bool white_king_found = false, black_king_found = false;

    // 1. Piece placement
    for (char token : piecePlacement)
    {
        if (token == '/')
        {
            if (col != 8)
                return PositionSetError("Invalid FEN: row " + std::to_string(row + 1)
                                        + " does not have 8 columns");
            col = 0;
            row--;
        }
        else if (std::isdigit(token))
        {
            col += token - '0';
        }
        else
        {
            size_t idx = PieceToChar.find(token);
            if (idx == std::string::npos)
                return PositionSetError("Invalid FEN: unknown piece '" + std::string(1, token)
                                        + "'");

            Piece piece = static_cast< Piece>(idx);

            if (col> = 8 || row< 0)
                return PositionSetError(
                  "Invalid FEN: board dimensions exceeded during piece placement");

            Square sq = make_square(File(col), Rank(row));
            put_piece(piece, sq);

            if (type_of(piece) == KING)
            {
                n_kings++;
                if (color_of(piece) == WHITE)
                    white_king_found = true;
                else
                    black_king_found = true;
            }

            col++;
        }
    }

    if (col != 8 || row != 0)
        return PositionSetError("Invalid FEN: piece placement does not describe a full 8x8 board");

    if (n_kings != 2 || !white_king_found || !black_king_found)
        return PositionSetError("Invalid FEN: exactly one white and one black king are required");

    // 2. Active color
    if (activeColor == "w")
        sideToMove = WHITE;
    else if (activeColor == "b")
        sideToMove = BLACK;
    else
        return PositionSetError("Invalid FEN: active color must be 'w' or 'b'");

    // 3. Castling availability
    if (castling != "-")
    {
        for (char token : castling)
        {
            Color c    = std::islower(token) ? BLACK : WHITE;
            Piece rook = make_piece(c, ROOK);
            token      = char(std::toupper(token));

            Square rsq;
            if (token == 'K')
            {
                // Find rightmost rook
                for (rsq = relative_square(c, SQ_H1); piece_on(rsq) != rook; --rsq)
                    if (file_of(rsq) == FILE_A)
                        return PositionSetError("Invalid FEN: no rook on kingside for castling");
            }
            else if (token == 'Q')
            {
                // Find leftmost rook
                for (rsq = relative_square(c, SQ_A1); piece_on(rsq) != rook; ++rsq)
                    if (file_of(rsq) == FILE_H)
                        return PositionSetError("Invalid FEN: no rook on queenside for castling");
            }
            else if (token> = 'A' && token< = 'H')
            {
                rsq = make_square(File(token - 'A'), relative_rank(c, RANK_1));
                if (piece_on(rsq) != rook)
                    return PositionSetError("Invalid FEN: specified castling square "
                                            + std::string(1, token) + " does not contain a rook");
            }
            else
            {
                return PositionSetError("Invalid FEN: unrecognized castling token '"
                                        + std::string(1, token) + "'");
            }

            set_castling_right(c, rsq);
        }
    }

    // 4. En passant target square
    if (enPassant != "-")
    {
        auto sq_opt = str_to_square(enPassant);
        if (!sq_opt)
            return PositionSetError("Invalid FEN: invalid en passant square '" + enPassant + "'");

        Square epSq = *sq_opt;

        // Check if the ep square is on the 3rd or 6th rank
        if (!(rank_of(epSq) == RANK_3 && sideToMove == BLACK)
            && !(rank_of(epSq) == RANK_6 && sideToMove == WHITE))
            return PositionSetError(
              "Invalid FEN: en passant square must be on the 3rd or 6th rank");

        // Check if the square in front of the ep square contains a pawn of the appropriate color
        Square expectedPawnSq = epSq + (sideToMove == WHITE ? SOUTH : NORTH);
        if (piece_on(expectedPawnSq) != make_piece(~sideToMove, PAWN))
            return PositionSetError(
              "Invalid FEN: no pawn on the square " + std::string(1, square_to_char(expectedPawnSq))
              + std::to_string(rank_of(expectedPawnSq) + 1) + " behind the en passant square");

        // Check if the ep square and the square behind it are empty
        if (piece_on(epSq) != NO_PIECE
            || piece_on(epSq + (sideToMove == WHITE ? NORTH : SOUTH)) != NO_PIECE)
            return PositionSetError(
              "Invalid FEN: en passant square and the square behind it must be empty");

        st - > epSquare = epSq;
    }

    // 5. Halfmove clock
    if (fields.size()> 4)
    {
        try
        {
            st - > rule50 = std::stoi(fields[4]);
            if (st - > rule50< 0)
                return PositionSetError("Invalid FEN: halfmove clock cannot be negative");
        } catch (...)
        { return PositionSetError("Invalid FEN: halfmove clock is not a valid integer"); }
    }

    // 6. Fullmove number
    int gamePly = 0;
    if (fields.size()> 5)
    {
        try
        {
            int fullMoveClock = std::stoi(fields[5]);
            if (fullMoveClock< 1)
                fullMoveClock = 1;  // Handle invalid fullmove numbers gracefully
            gamePly = std::max(2 * (fullMoveClock - 1), 0) + (sideToMove == BLACK);
        } catch (...)
        { return PositionSetError("Invalid FEN: fullmove number is not a valid integer"); }
    }

    chess960   = isChess960;
    thisThread = nullptr;
    set_state(st);

    if (checkers())
    {
        // Forbid castling while in check
        st - > castlingRights = 0;
        st - > epSquare       = SQ_NONE;

        if (attackers_to(square< KING>(~sideToMove)) & pieces(sideToMove))
            return PositionSetError("Invalid FEN: Both kings are in check.");
    }
    // Compute gamePly after set_state to avoid setting halfMoveClock
    // Since st is passed to StateInfo::previous, it inherits the rule50 value.
    gamePlyCount = gamePly;

    // Check if the fen is a valid chess position
    if (popcount(pieces(WHITE))> 16 || popcount(pieces(BLACK))> 16)
        return PositionSetError("Invalid FEN: Too many pieces.");

    if (popcount(pieces(PAWN))> 16)
        return PositionSetError("Invalid FEN: Too many pawns.");

    if (popcount(pieces(WHITE, PAWN))> 8 || popcount(pieces(BLACK, PAWN))> 8)
        return PositionSetError("Invalid FEN: Too many pawns.");

    if ((pieces(PAWN) & (Rank1BB | Rank8BB)))
        return PositionSetError("Invalid FEN: Pawns on the first or eighth rank.");

    if (attackers_to(square< KING>(~sideToMove)) & pieces(sideToMove))
        return PositionSetError("Invalid FEN: Opponent king is in check.");

    return std::nullopt;
}


// Flips position along the middle ranks
std::optional< PositionSetError> Position::flip() {

    std::string fen_str;
    int         empty_squares = 0;

    for (Rank r = RANK_1; r< = RANK_8; ++r)
    {
        for (File f = FILE_A; f< = FILE_H; ++f)
        {
            Square sq    = make_square(f, r);
            Piece  piece = piece_on(sq);

            if (piece == NO_PIECE)
                ++empty_squares;
            else
            {
                if (empty_squares)
                {
                    fen_str += std::to_string(empty_squares);
                    empty_squares = 0;
                }
                fen_str += PieceToChar[piece ^ 8];  // Toggle color
            }
        }
        if (empty_squares)
        {
            fen_str += std::to_string(empty_squares);
            empty_squares = 0;
        }
        if (r< RANK_8)
            fen_str += '/';
    }

    fen_str += sideToMove == WHITE ? " b " : " w ";

    std::string castling_str = "";
    if (can_castle(B_OO))
        castling_str += "K";
    if (can_castle(B_OOO))
        castling_str += "Q";
    if (can_castle(W_OO))
        castling_str += "k";
    if (can_castle(W_OOO))
        castling_str += "q";

    if (castling_str.empty())
        castling_str = "-";
    fen_str += castling_str + " ";

    if (ep_square() != SQ_NONE)
        fen_str +=
          std::string{char('a' + file_of(ep_square())), char('1' + rank_of(ep_square() ^ SQ_A8))}
          + " ";
    else
        fen_str += "- ";

    fen_str += std::to_string(rule50_count()) + " ";

    fen_str += std::to_string(1 + (gamePlyCount - (sideToMove == BLACK)) / 2);

    return set(fen_str, chess960, st);
}


// Computes the hash keys of the position, and other
// data that once computed is updated incrementally as moves are done.
// The function is only used when a new position is set up, and to verify
// the correctness of the StateInfo data when running in debug mode.
void Position::set_state(StateInfo* si) const {

    si - > key                    = VALUE_ZERO;
    si - > nonPawnMaterial[WHITE] = si - > nonPawnMaterial[BLACK] = VALUE_ZERO;
    si - > checkersBB =
      attackers_to(square< KING>(sideToMove)) & pieces(~sideToMove);

    set_castling_right(WHITE, square< KING>(WHITE));
    set_castling_right(BLACK, square< KING>(BLACK));

    for (Bitboard b = pieces(); b;)
    {
        Square sq = pop_lsb(b);
        Piece  pc = piece_on(sq);
        si - > key ^= PieceKey[pc][sq];

        if (type_of(pc) != PAWN)
            si - > nonPawnMaterial[color_of(pc)] += PieceValue[pc];
    }

    if (si - > epSquare != SQ_NONE)
        si - > key ^= EnPassantKey[file_of(si - > epSquare)];

    if (sideToMove == BLACK)
        si - > key ^= SideKey;

    si - > key ^= CastleKey[si - > castlingRights];
}


// Called when setting the position from a fen string
void Position::set_castling_right(Color c, Square rsq) {

    if (piece_on(rsq) != make_piece(c, ROOK))
        return;

    Square ksq = square< KING>(c);

    CastlingRights cr = c & (ksq< rsq ? KING_SIDE : QUEEN_SIDE);

    st - > castlingRights |= cr;
    castlingRookSquare[cr] = rsq;

    castlingPath[cr] = (between_bb(ksq, rsq) | rsq) & ~ksq;

    ksq = make_square(ksq< rsq ? FILE_G : FILE_C, rank_of(ksq));
    rsq = make_square(ksq< rsq ? FILE_F : FILE_D, rank_of(rsq));

    castlingRightsMask[ksq] |= cr;
    castlingRightsMask[rsq] |= cr;
}


// Returns an ASCII representation of the position
std::ostream& operator<<(std::ostream & os, const Position & pos) {

    os<< "\n +---+---+---+---+---+---+---+---+\n";

    for (Rank r = RANK_8; r> = RANK_1; --r)
    {
        for (File f = FILE_A; f< = FILE_H; ++f)
            os<< " | "<< PieceToChar[pos.piece_on(make_square(f, r))];

        os<< " |\n +---+---+---+---+---+---+---+---+\n";
    }

    os<< "\nFen: "<< pos
      .fen()<< "\nKey: "<< std::hex<< std::uppercase
        << std::setfill('0')<< std::setw(16)<< pos
      .key()<< std::setfill(
        ' ')<< std::dec<< "\nCheckers: "<< pos.checkers();

    return os;
}


// Returns the FEN string of the position
std::string Position::fen() const {

    int         emptyCnt;
    std::string fen;

    for (Rank r = RANK_8; r> = RANK_1; --r)
    {
        for (File f = FILE_A; f< = FILE_H; ++f)
        {

            for (emptyCnt = 0; f< = FILE_H && empty(make_square(f, r)); ++f)
                ++emptyCnt;

            if (emptyCnt)
                fen += std::to_string(emptyCnt);

            if (f< = FILE_H)
                fen += PieceToChar[piece_on(make_square(f, r))];
        }

        if (r> RANK_1)
            fen += '/';
    }

    fen += sideToMove == WHITE ? " w " : " b ";

    if (can_castle(WHITE_OO))
        fen += chess960 ? char('A' + file_of(castling_rook_square(WHITE_OO))) : 'K';

    if (can_castle(WHITE_OOO))
        fen += chess960 ? char('A' + file_of(castling_rook_square(WHITE_OOO))) : 'Q';

    if (can_castle(BLACK_OO))
        fen += chess960 ? char('a' + file_of(castling_rook_square(BLACK_OO))) : 'k';

    if (can_castle(BLACK_OOO))
        fen += chess960 ? char('a' + file_of(castling_rook_square(BLACK_OOO))) : 'q';

    if (!can_castle(ANY_CASTLING))
        fen += "-";

    fen += st - > epSquare == SQ_NONE
           ? " - "
           : " "
               + std::string{char('a' + file_of(st - > epSquare)),
                             char('1' + rank_of(st - > epSquare))}
               + " ";

    fen += std::to_string(st - > rule50) + " "
         + std::to_string(1 + (gamePlyCount - (sideToMove == BLACK)) / 2);

    return fen;
}


// Moves a piece to a new square. For castling moves, this
// handles only the king.
inline void Position::move_piece(Square from, Square to) {

    Piece    pc     = piece_on(from);
    Bitboard fromTo = square_bb(from) ^ square_bb(to);

    byTypeBB[ALL_PIECES] ^= fromTo;
    byTypeBB[type_of(pc)] ^= fromTo;
    byColorBB[color_of(pc)] ^= fromTo;
    board[from] = NO_PIECE;
    board[to]   = pc;
}


// Does the castling move. It behaves correctly also if rook and
// king are on the same square (this happens in chess960).
inline void Position::do_castling(
  Color us, Square from, Square& to, Square& rfrom, Square& rto, bool isChess960) {

    bool kingSide = to> from;
    rfrom         = castlingRookSquare[us & (kingSide ? KING_SIDE : QUEEN_SIDE)];
    rto           = make_square(kingSide ? FILE_F : FILE_D, rank_of(from));

    // A chess960 specific sanity check
    assert(!isChess960 || to == make_square(kingSide ? FILE_G : FILE_C, rank_of(from)));

    if (!isChess960)
        to = make_square(kingSide ? FILE_G : FILE_C, rank_of(from));

    remove_piece(rfrom);
    remove_piece(from);
    put_piece(make_piece(us, KING), to);
    put_piece(make_piece(us, ROOK), rto);
}


// Undoes the castling move. It behaves correctly also if rook and
// king are on the same square (this happens in chess960).
inline void Position::undo_castling(
  Color us, Square from, Square to, Square rfrom, Square rto, bool isChess960) {

    if (!isChess960)
    {
        bool kingSide = to> from;
        to            = make_square(kingSide ? FILE_G : FILE_C, rank_of(from));
    }

    remove_piece(rto);
    remove_piece(to);
    put_piece(make_piece(us, KING), from);
    put_piece(make_piece(us, ROOK), rfrom);
}

template< bool NNUE> void Position::do_move(Move                      m,
                                                            StateInfo&                newSt,
                                                            bool                      givesCheck,
                                                            Eval::NNUE::DirtyPiece&   dp,
                                                            Eval::NNUE::DirtyThreats& dts,
                                                            const TranspositionTable* tt,
                                                            const Search::SharedHistories* sh) {

    assert(m.is_ok());

    st - > currentMove = m;

    StateInfo* last          = st;
    st                       = &newSt;
    st - > previous = last;

    st - > pliesFromNull          = last - > pliesFromNull + 1;
    st - > checkersBB             = 0;
    st - > rule50                 = last - > rule50 + 1;
    st - > key                    = last - > key ^ SideKey;
    st - > nonPawnMaterial[WHITE] = last - > nonPawnMaterial[WHITE];
    st - > nonPawnMaterial[BLACK] = last - > nonPawnMaterial[BLACK];
    st - > castlingRights         = last - > castlingRights;
    st - > epSquare               = SQ_NONE;

    Color  us   = sideToMove;
    Color  them = ~us;
    Square from = m.from_sq();
    Square to   = m.to_sq();
    Piece  pc   = piece_on(from);
    Piece  cpc  = piece_on(to);

    assert(color_of(pc) == us);
    assert(empty(to) || color_of(cpc) == (m.type_of() == CASTLING ? us : them));
    assert(m.type_of() != CASTLING || cpc == make_piece(us, ROOK));

    gamePlyCount++;
    sideToMove = ~sideToMove;

    if (m.type_of() == CASTLING)
    {
        Square rfrom, rto;
        do_castling(us, from, to, rfrom, rto, chess960);

        if constexpr (NNUE)
        {
            dp.dirtyNum = 2;
            dp.pc[0]    = pc;
            dp.from[0]  = from;
            dp.to[0]    = to;
            dp.pc[1]    = cpc;
            dp.from[1]  = rfrom;
            dp.to[1]    = rto;
        }

        st - > capturedPiece = NO_PIECE;
    }
    else
    {
        if (m.type_of() == EN_PASSANT)
        {
            Square capsq = make_square(file_of(to), rank_of(from));
            cpc          = piece_on(capsq);

            assert(cpc == make_piece(them, PAWN));

            remove_piece(capsq);
            st - > key ^= PieceKey[cpc][capsq];

            if constexpr (NNUE)
            {
                dp.dirtyNum = 2;
                dp.pc[0]    = pc;
                dp.from[0]  = from;
                dp.to[0]    = to;
                dp.pc[1]    = cpc;
                dp.from[1]  = capsq;
                dp.to[1]    = SQ_NONE;
            }
        }
        else
        {
            if (cpc != NO_PIECE)
            {
                st - > rule50 = 0;
                st - > key ^= PieceKey[cpc][to];
                if (type_of(cpc) != PAWN)
                    st - > nonPawnMaterial[them] -= PieceValue[cpc];

                // If the captured piece is a rook, update castling rights if needed
                if (type_of(cpc) == ROOK && castlingRightsMask[to])
                {
                    st - > castlingRights &= ~castlingRightsMask[to];
                    st - > key ^= CastleKey[last - > castlingRights]
                                         ^ CastleKey[st - > castlingRights];
                }
            }
            if constexpr (NNUE)
            {
                dp.dirtyNum = cpc != NO_PIECE ? 2 : 1;
                dp.pc[0]    = pc;
                dp.from[0]  = from;
                dp.to[0]    = to;
                dp.pc[1]    = cpc;
                dp.from[1]  = to;
                dp.to[1]    = SQ_NONE;
            }
        }

        move_piece(from, to);
        st - > key ^= PieceKey[pc][from] ^ PieceKey[pc][to];
        st - > capturedPiece = cpc;

        // If the moved piece is a pawn, update rule50, en-passant square, and promotion
        if (type_of(pc) == PAWN)
        {
            st - > rule50 = 0;

            if (m.type_of() == PROMOTION)
            {
                Piece promotion = make_piece(us, m.promotion_type());
                remove_piece(to);
                put_piece(promotion, to);
                st - > key ^= PieceKey[pc][to] ^ PieceKey[promotion][to];
                st - > nonPawnMaterial[us] += PieceValue[promotion];
                if constexpr (NNUE)
                {
                    dp.pc[0] = promotion;
                }
            }
            else if (std::abs(rank_of(to) - rank_of(from)) == 2)
            {
                Square epSq = to + (us == WHITE ? SOUTH : NORTH);

                // Set epSquare only if the pawn can be captured
                if (pawn_attacks_bb(us, epSq) & pieces(them, PAWN))
                {
                    st - > epSquare = epSq;
                    st - > key ^= EnPassantKey[file_of(st - > epSquare)];
                }
            }
        }

        // If the moved piece is a king or a rook, update castling rights if needed
        if (castlingRightsMask[from])
        {
            st - > castlingRights &= ~castlingRightsMask[from];
            st - > key ^= CastleKey[last - > castlingRights]
                                 ^ CastleKey[st - > castlingRights];
        }
    }

    if (givesCheck)
    {
        st - > checkersBB = attackers_to(square< KING>(them)) & pieces(us);
    }
    // Update threat evaluation of the position
    if constexpr (NNUE)
    {
        if (givesCheck || cpc != NO_PIECE || m.type_of() == EN_PASSANT || m.type_of() == PROMOTION
            || (type_of(pc) == PAWN
                && std::abs(rank_of(from) - rank_of(to)) == 2))  // Pawn push 2 spaces
        {
            dts.threatsUpdated = false;
        }
        else
        {
            dts.threatsUpdated    = true;
            dts.king_attackers[0] = last - > king_attackers[0];
            dts.king_attackers[1] = last - > king_attackers[1];
            Eval::NNUE::update_threats(*this, dp, dts, tt, sh);
        }
    }
}

// Explicit template instantiations
template void Position::do_move< true>(Move                           m,
                                                       StateInfo&                     newSt,
                                                       bool                           givesCheck,
                                                       Eval::NNUE::DirtyPiece&        dp,
                                                       Eval::NNUE::DirtyThreats&      dts,
                                                       const TranspositionTable*      tt,
                                                       const Search::SharedHistories* sh);
template void Position::do_move< false>(Move                           m,
                                                        StateInfo&                     newSt,
                                                        bool                           givesCheck,
                                                        Eval::NNUE::DirtyPiece&        dp,
                                                        Eval::NNUE::DirtyThreats&      dts,
                                                        const TranspositionTable*      tt,
                                                        const Search::SharedHistories* sh);

void Position::undo_move(Move m) {

    assert(m.is_ok());

    sideToMove = ~sideToMove;
    gamePlyCount--;

    Color  us   = sideToMove;
    Square from = m.from_sq();
    Square to   = m.to_sq();
    Piece  pc   = piece_on(to);

    assert(empty(from) || m.type_of() == CASTLING);

    if (m.type_of() == CASTLING)
    {
        Square rfrom, rto;
        undo_castling(us, from, to, rfrom, rto, chess960);
    }
    else
    {
        if (m.type_of() == PROMOTION)
        {
            remove_piece(to);
            pc = make_piece(us, PAWN);
            put_piece(pc, to);
        }

        move_piece(to, from);

        if (m.type_of() == EN_PASSANT)
        {
            Square capsq = make_square(file_of(to), rank_of(from));
            put_piece(make_piece(~us, PAWN), capsq);
        }
        else if (st - > capturedPiece != NO_PIECE)
        {
            put_piece(st - > capturedPiece, to);
        }
    }

    st = st - > previous;
}


// Does a null move. We simply copy some st data to newSt
// and invert the side to move, but we don't update the hashing key for the
// pawn structure.
void Position::do_null_move(StateInfo& newSt) {

    assert(!checkers());

    StateInfo* last          = st;
    st                       = &newSt;
    st - > previous = last;

    st - > pliesFromNull          = 0;
    st - > rule50                 = last - > rule50 + 1;
    st - > nonPawnMaterial[WHITE] = last - > nonPawnMaterial[WHITE];
    st - > nonPawnMaterial[BLACK] = last - > nonPawnMaterial[BLACK];
    st - > castlingRights         = last - > castlingRights;
    st - > epSquare               = SQ_NONE;

    st - > key = last - > key ^ SideKey;
    if (last - > epSquare != SQ_NONE)
        st - > key ^= EnPassantKey[file_of(last - > epSquare)];

    st - > checkersBB    = 0;
    st - > capturedPiece = NO_PIECE;
    st - > currentMove   = Move::null();

    gamePlyCount++;
    sideToMove = ~sideToMove;
}


void Position::undo_null_move() {

    assert(st - > capturedPiece == NO_PIECE);

    sideToMove = ~sideToMove;
    gamePlyCount--;
    st = st - > previous;
}

// Tests if the position has a move which draws by repetition
// or an earlier position has a draw by 50-move rule.
bool Position::is_draw(int ply) const {
    if (st - > rule50> 99
        && (!checkers() || MoveList< LEGAL>(*this).size()> 0))
        return true;

    return st - > rule50> = 4 && is_repetition(ply);
}

// Check if a move returns the position to a state seen earlier in the game.
// It detects upcoming repetitions in the search. We test to avoid repetitions
// and to play them when the score is lower than VALUE_DRAW.
bool Position::upcoming_repetition(int ply) const {

    StateInfo* stp = st - > previous;
    for (int i = 2; i< = st - > rule50; i += 2)
    {
        stp = stp - > previous - > previous;
        if (stp - > key == st - > key)
        {
            // It's a repetition, but ensure that the previous ply was
            // not the root. In such a case, since it's the opponent's
            // turn to move, it's not a repetition but a 1-fold. (So
            // our ply distance from the root will be i - 1 instead
            // of i).
            return ply> i - 1;
        }
    }
    return false;
}

// Tests if the position has a draw by repetition.
// It is used to detect repetitions occurring in the game.
// We count 1-fold (i.e. first repetition) as a draw to
// avoid a situation where two engines evaluate the position
// favorably but neither wishes to deviate.
bool Position::is_repetition(int ply) const {

    int        limit = std::min(st - > rule50, st - > pliesFromNull);
    StateInfo* stp   = st - > previous;
    for (int i = 2; i< = limit; i += 2)
    {
        stp = stp - > previous - > previous;
        if (stp - > key == st - > key)
        {
            // If the position has been repeated at the root or earlier,
            // we have a draw.
            if (ply> = i)
                return true;

            // Or if it has occurred in the search tree already.
            // But if it happened at ply 1 we have a 1-fold
            // repetition, but we've not yet repeated the position
            // twice.
            return ply< i - 1;
        }
    }

    return false;
}


// Tests if the position has a draw by insufficient material.
bool Position::has_game_cycle(int ply) const {
    int        end = std::min(st - > rule50, st - > pliesFromNull);
    StateInfo* stp = st - > previous;
    for (int i = 3; i< = end; i += 2)
    {
        stp = stp - > previous - > previous;

        Key k = stp - > key ^ SideKey;
        if (k == st - > key)
            return ply> = i;
    }

    return false;
}


// Verifies if a pseudo-legal move is legal
bool Position::legal(Move m) const {

    assert(m.is_ok());

    Color  us   = sideToMove;
    Square from = m.from_sq();
    Square to   = m.to_sq();

    assert(color_of(piece_on(from)) == us);
    assert(piece_on(square< KING>(us)) == make_piece(us, KING));

    // En passant captures are a tricky special case. Because they are rather
    // uncommon, we do it simply by testing whether the king is attacked after
    // the move is made.
    if (m.type_of() == EN_PASSANT)
    {
        Square   ksq      = square< KING>(us);
        Square   cap_sq   = make_square(file_of(to), rank_of(from));
        Bitboard occupied = (pieces() ^ square_bb(from) ^ square_bb(cap_sq)) | square_bb(to);

        assert(to == st - > epSquare);
        assert(moved_piece(m) == make_piece(us, PAWN));
        assert(piece_on(cap_sq) == make_piece(~us, PAWN));
        assert(piece_on(to) == NO_PIECE);

        return !(attacks_bb< ROOK>(ksq, occupied) & pieces(~us, ROOK, QUEEN))
            && !(attacks_bb< BISHOP>(ksq, occupied) & pieces(~us, BISHOP, QUEEN));
    }

    // Castling moves
    if (m.type_of() == CASTLING)
    {
        assert(to == make_square(to> from ? FILE_G : FILE_C, rank_of(from))
               || to == castling_rook_square(us & (to> from ? KING_SIDE : QUEEN_SIDE)));

        // Not valid if castling rights have been lost (e.g., if king moved)
        if (!can_castle(us & (to> from ? KING_SIDE : QUEEN_SIDE)))
            return false;

        // Not valid if castling rights exist but king is in check
        if (checkers())
            return false;

        to             = make_square(to> from ? FILE_G : FILE_C, rank_of(from));
        Square   step  = to> from ? WEST : EAST;
        Bitboard enemy = pieces(~us);

        for (Square s = to; s != from; s += step)
            if (attackers_to(s, pieces()) & enemy)
                return false;

        // In case of chess960, verify that when the rook moves it does not expose
        // the king to check. For instance, castling with the rook on f1 and
        // the king on e1 is not legal if there is a black rook on f8.
        if (chess960)
        {
            Square rfrom = castling_rook_square(us & (to> from ? KING_SIDE : QUEEN_SIDE));
            Bitboard occupied =
              (pieces() ^ square_bb(from) ^ square_bb(rfrom)) | square_bb(to)
              | square_bb(make_square(to> from ? FILE_F : FILE_D, rank_of(from)));

            if ((attacks_bb< ROOK>(to, occupied) & pieces(~us, ROOK, QUEEN))
                || (attacks_bb< BISHOP>(to, occupied) & pieces(~us, BISHOP, QUEEN)))
                return false;
        }
        return true;
    }

    // If the moving piece is a king, check whether the destination
    // square is attacked by the opponent.
    if (type_of(piece_on(from)) == KING)
        return m.type_of() == CASTLING
            || !(attackers_to(to, pieces() ^ square_bb(from)) & pieces(~us));

    // A non-king move is legal if and only if it is not pinned or it
    // is moving along the ray towards or away from the king.
    return !(blockers_for_king(us) & from) || aligned(from, to, square< KING>(us));
}

// Verifies if a pseudo-legal move is pseudo-legal
// Only castling requires full legality check
bool Position::pseudo_legal(const Move m) const {

    assert(m.is_ok());

    Color us = sideToMove;

    if (m.type_of() == CASTLING)
        return MoveList< LEGAL>(*this).contains(m);

    Square from = m.from_sq();
    Square to   = m.to_sq();
    Piece  pc   = piece_on(from);

    // Use a slightly slower but more simple logic if the position is in check
    if (checkers())
        return MoveList< EVASIONS>(*this).contains(m);

    // If the from square is not occupied by a piece belonging to the side to move,
    // the move is obviously not legal.
    if (pc == NO_PIECE || color_of(pc) != us)
        return false;

    // The destination square cannot be occupied by a friendly piece
    if (pieces(us) & to)
        return false;

    // Handle the special case of a pawn move
    if (type_of(pc) == PAWN)
    {
        // We have already handled promotions, so destination square cannot be on the 8th/1st rank.
        if (rank_of(to) == RANK_8 || rank_of(to) == RANK_1)
            return false;

        if (m.type_of() == EN_PASSANT)
            return MoveList< LEGAL>(*this).contains(m);

        if (m.type_of() == NORMAL)
        {
            if (from + pawn_push(us) == to)
                return empty(to);
            if (from + pawn_push(us) + pawn_push(us) == to
                && rank_of(from) == relative_rank(us, RANK_2) && empty(from + pawn_push(us)))
                return empty(to);
            if (pawn_attacks_bb(us, from) & pieces(~us) & to)
                return true;
        }

        return false;
    }

    // Handle non-pawn moves
    return attacks_bb(type_of(pc), from, pieces()) & to;
}


// Tests whether the position has any pseudo legal
// moves that also are legal.
bool Position::has_legal_move() const {

    MoveList< LEGAL> legalMoves(*this);
    return legalMoves.size()> 0;
}

// Tests whether a pseudo legal move gives a check
bool Position::gives_check(Move m) const {

    assert(m.is_ok());
    assert(color_of(piece_on(m.from_sq())) == sideToMove);

    Square from = m.from_sq();
    Square to   = m.to_sq();
    Piece  pc   = piece_on(from);

    // Is there a direct check?
    if (check_squares(type_of(pc)) & to)
        return true;

    // Is there a discovered check?
    if ((blockers_for_king(~sideToMove) & from)
        && !aligned(from, to, square< KING>(~sideToMove)))
        return true;

    if (m.type_of() == NORMAL)
        return false;

    if (m.type_of() == PROMOTION)
        return attacks_bb(m.promotion_type(), to, pieces() ^ square_bb(from))
             & square< KING>(~sideToMove);

    // En passant capture with check? We have already handled the case
    // of direct checks and of ordinary discovered checks. What is left are
    // discovered checks caused by the removal of the captured pawn.
    if (m.type_of() == EN_PASSANT)
    {
        Square   capsq = make_square(file_of(to), rank_of(from));
        Bitboard b     = (pieces() ^ square_bb(from) ^ square_bb(capsq)) | square_bb(to);

        return (attacks_bb< ROOK>(square< KING>(~sideToMove), b)
                & pieces(sideToMove, ROOK, QUEEN))
            || (attacks_bb< BISHOP>(square< KING>(~sideToMove), b)
                & pieces(sideToMove, BISHOP, QUEEN));
    }

    // Castling with check?
    if (m.type_of() == CASTLING)
    {
        // Castling is encoded as 'King captures the rook'
        Square   rto = make_square(to> from ? FILE_F : FILE_D, rank_of(from));
        Bitboard b   = (pieces() ^ square_bb(from) ^ square_bb(to)) | square_bb(rto);

        return (attacks_bb< ROOK>(square< KING>(~sideToMove), b)
                & pieces(sideToMove, ROOK, QUEEN));
    }

    return false;
}

// Tests whether the side to move is doing a pseudo legal
// move that guarantees a draw by the 50-move rule or by repetition.
bool Position::is_draw_move(Move m) const {

    assert(m.is_ok());

    // 50 moves rule
    if (st - > rule50> =
          99 && type_of(piece_on(m.from_sq())) != PAWN && !capture(m))
        return true;

    // Repetitions
    // We start searching for a repetition at the current ply minus the rule50 counter
    int end = st - > rule50;

    // Only non-capture, non-pawn moves and non-castling moves are relevant
    if (type_of(piece_on(m.from_sq())) == PAWN || capture(m) || m.type_of() == CASTLING)
        return false;

    StateInfo* stp = st - > previous;
    for (int i = 1; i< = end; i += 2)
    {
        stp = stp - > previous - > previous;

        if (stp - > key == st - > key)
            return true;
    }

    return false;
}

// Calculates Static Exchange Evaluation (SEE) of a move.
// It tries to estimate the material gained/lost by the move.
// The algorithm is equivalent to exactly playing out the move and captures,
// returning the evaluation of the resulting position.
// There is one exception, we ignore en-passant capture.
Value Position::see(Move m) const {

    Square from = m.from_sq();
    Square to   = m.to_sq();

    Value swapList[32];
    int   slIndex = 0;

    // Find the piece on the 'from' and 'to' squares
    PieceType pc     = type_of(piece_on(from));
    PieceType target = type_of(piece_on(to));

    // Handle castling and en-passant
    if (m.type_of() != NORMAL)
    {
        if (m.type_of() == CASTLING)
            return VALUE_ZERO;

        if (m.type_of() == EN_PASSANT)
            target = PAWN;
        else if (m.type_of() == PROMOTION)
        {
            pc                  = m.promotion_type();
            swapList[slIndex++] = PieceValue[pc] - PieceValue[PAWN];
        }
    }

    // Initialize the swap list with the value of the target piece
    swapList[slIndex] = PieceValue[target];

    // Find all attackers to the 'to' square
    Bitboard occupied =
      pieces() ^ from
      ^ (m.type_of() == EN_PASSANT ? to + (sideToMove == WHITE ? SOUTH : NORTH) : 0);
    Bitboard attackers = attackers_to(to, occupied);
    Color    stm       = ~sideToMove;

    // Loop through all attackers to the 'to' square
    while (true)
    {
        // Find the least valuable attacker
        Bitboard stmAttackers = attackers & pieces(stm);
        if (!stmAttackers)
            break;

        PieceType attacker;
        if (stmAttackers & pieces(PAWN))
            attacker = PAWN;
        else if (stmAttackers & pieces(KNIGHT))
            attacker = KNIGHT;
        else if (stmAttackers & pieces(BISHOP))
            attacker = BISHOP;
        else if (stmAttackers & pieces(ROOK))
            attacker = ROOK;
        else if (stmAttackers & pieces(QUEEN))
            attacker = QUEEN;
        else
            attacker = KING;

        // Add the value of the attacked piece to the swap list
        swapList[slIndex + 1] = PieceValue[pc] - swapList[slIndex];
        slIndex++;

        // Remove the attacker from the occupied bitboard
        occupied ^= lsb(stmAttackers & pieces(attacker));

        // Add x-ray attackers
        if (attacker == PAWN || attacker == BISHOP || attacker == QUEEN)
            attackers |= attacks_bb< BISHOP>(to, occupied) & pieces(BISHOP, QUEEN);
        if (attacker == ROOK || attacker == QUEEN)
            attackers |= attacks_bb< ROOK>(to, occupied) & pieces(ROOK, QUEEN);

        attackers &= occupied;

        // Update the piece and side to move
        pc  = attacker;
        stm = ~stm;
    }

    // Negamax the swap list to find the best possible value
    while (--slIndex)
        swapList[slIndex - 1] = std::min(-swapList[slIndex], swapList[slIndex - 1]);

    return swapList[0];
}


// A less expensive version of see(), to test if SEE >= threshold.
// threshold is 0 when testing if the move is "good" or "bad".
// It is equivalent to see(m) >= threshold, but the logic is slightly
// different and requires less bitboard operations.
bool Position::see_ge(Move m, Value threshold) const {
    assert(m.is_ok());

    // Only deal with normal moves, assume others pass a simple see >= 0
    if (m.type_of() != NORMAL)
        return VALUE_ZERO> = threshold;

    Square from = m.from_sq();
    Square to   = m.to_sq();

    int swap = PieceValue[piece_on(to)] - threshold;
    if (swap< 0)
        return false;

    swap = PieceValue[piece_on(from)] - swap;
    if (swap< = 0)
        return true;

    Bitboard occupied  = pieces() ^ from;
    Color    stm       = sideToMove;
    Bitboard attackers = attackers_to(to, occupied);
    Bitboard stmAttackers, resAttackers;

    while (true)
    {
        stm          = ~stm;
        stmAttackers = attackers & pieces(stm);

        // Don't allow pinned pieces to attack (except if the pin is on the 'to' square
        // and we have a pin resolved by the king capturing the checking piece).
        if (stmAttackers & blockers_for_king(stm))
            resAttackers = stmAttackers & ~blockers_for_king(stm);
        else
            resAttackers = stmAttackers;

        if (!resAttackers)
            return stm != sideToMove;

        // Handle the case where the king is the attacker.
        // It is currently impossible to determine whether the pinned piece we
        // used to be able to capture with is now unpinned or still pinned, so
        // we conservatively say that the king can't capture if we found a
        // pinned piece earlier.
        if (resAttackers == pieces(stm, KING))
            return (stmAttackers & blockers_for_king(stm)) ? stm == sideToMove : stm != sideToMove;

        PieceType pc;
        if (resAttackers & pieces(PAWN))
        {
            pc = PAWN;
        }
        else if (resAttackers & pieces(KNIGHT))
        {
            pc = KNIGHT;
        }
        else if (resAttackers & pieces(BISHOP))
        {
            pc = BISHOP;
        }
        else if (resAttackers & pieces(ROOK))
        {
            pc = ROOK;
        }
        else if (resAttackers & pieces(QUEEN))
        {
            pc = QUEEN;
        }
        else
        {
            // We should have already handled the king attacker case above.
            assert(false);
            return false;
        }

        swap = PieceValue[pc] - swap;

        if (swap< 0)
        {
            // If the next piece in line can capture the previous piece and still be >= 0 in the SEE, we know that the initial capture is a loss.
            // Since the side to move might have chosen to not capture in the first place, we only return if the initial capture was made by the opponent.
            if (stmAttackers & blockers_for_king(stm))
                return stm != sideToMove;
            return stm == sideToMove;
        }

        occupied ^= lsb(resAttackers & pieces(pc));

        if (pc == PAWN || pc == BISHOP || pc == QUEEN)
            attackers |= attacks_bb< BISHOP>(to, occupied) & pieces(BISHOP, QUEEN);

        if (pc == ROOK || pc == QUEEN)
            attackers |= attacks_bb< ROOK>(to, occupied) & pieces(ROOK, QUEEN);

        attackers &= occupied;
    }
}

}  // namespace ShashChess