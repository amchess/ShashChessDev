/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2025 Andrea Manzo, F. Ferraguti, K.Kiniama and ShashChess developers (see AUTHORS file)

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
#include "nnue/features/full_threats.h"
#include "position.h"
#include "tune.h"
//wdl model
#include "uci.h"
#include "wdl/win_probability.h"
#include "learn/learn.h"  //learning
using namespace ShashChess;

int main(int argc, char* argv[]) {

    std::cout << engine_info() << std::endl;

    WDLModel::init();  //wdl model

    Bitboards::init();
    Position::init();
    Eval::NNUE::Features::init_threat_offsets();

    auto uci = std::make_unique<UCIEngine>(argc, argv);
    LD.init(uci->engine_options());  //learning
    Tune::init(uci->engine_options());

    uci->loop();

    return 0;
}
