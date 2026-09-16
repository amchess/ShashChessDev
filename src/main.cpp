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

#include <iostream>
#include <memory>

#include "bitboard.h"
#include "misc.h"
#include "position.h"
#include "tune.h"
#include "uci.h"

using namespace ShashChess;

int main(int argc, char* argv[]) {
    std::cout << engine_info() << std::endl;

    Bitboards::init();
    Position::init();

    auto uci = std::make_unique<UCIEngine>(argc, argv);

    Tune::init(uci->engine_options());

    uci->loop();

    return 0;
}
