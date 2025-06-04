//search.cpp only framework
#include "search.h"
...
#include <cstdlib>
//from shashchess
#include <iostream>
//shashin
#include "ucioption.h"
#include "wdl/win_probability.h"  //shashin theory
...
namespace ShashChess {
using namespace ShashChess::Shashin;  //shashin
...
Search::Worker::Worker(SharedState&                    sharedState,
                       std::unique_ptr<ISearchManager> sm,
                       size_t                          threadId,
                       NumaReplicatedAccessToken       token) :
	...    
	shashinManager(std::make_unique<ShashinManager>()),
    shConfig(sharedState.shashinConfig) {
    clear();
}
//from shashin begin
Search::Worker::~Worker() = default;
ShashinManager& Worker::getShashinManager() { return *shashinManager; }
//fom Shashin end
...
void Search::Worker::start_searching() {
	...
    // Non-main threads go directly to iterative_deepening()
    if (!is_mainthread())
    {
        iterative_deepening();
        return;
    }
    else
    {
        shashinManager = std::make_unique<ShashinManager>();
        // It inizializes the rootPosition static state
        shashinManager->setStaticState(rootPos);
    }
	...

    // Send again PV info if we have a new best thread
    if (bestThread != this)
    {                                                                                    //Kelly
        main_manager()->pv(*bestThread, threads, tt, bestThread->completedDepth, true);  //shashin
    }  //Kelly

	...
}
...
void Search::Worker::iterative_deepening() {
    auto&          localShashinManager = *this->shashinManager;
	...
            if (mainThread
                && (threads.stop || pvIdx + 1 == multiPV || nodes > 10000000)
                && !(threads.abortedSearch && is_loss(rootMoves[0].uciScore)))
            {                                                             //no warning
                main_manager()->pv(*this, threads, tt, rootDepth, true);  //shashin
            }  //no warning
			...        
		}
	...
}
...
// Main search function for both PV and non-PV nodes
template<NodeType nodeType>
Value Search::Worker::search(
  Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode) {
	...    
	auto&                   localShashinManager = *this->shashinManager;
    const RootShashinState& rootShashinState    = localShashinManager.getState();  //for ShashChess
	...
    bool givesCheck, improving = false, priorCapture, opponentWorsening = false,
                     expTTHit = false;  //shashin
    //from Shashin-Crystal begin
    bool isMate, gameCycle, capture, ttCapture, kingDanger, quietKingSafe = false,
                                                            nullParity = false;
    //from shashin crystal end
	...
    // from Crystal-shashin begin
    gameCycle = kingDanger = false;
    rootDepth              = thisThread->rootDepth;

    ss->secondaryLine = false;
    ss->mainLine      = false;
    // from Crystal-shashin end
	...
    //from shashin-crystal begin
    int baseRazoringMargin = 0;
    //from shashin-crystal end
	...    
	//from Shashin begin
    int  nullThreshold;
    bool commonNullMoveConditions, useNullMoveByShashinForStockfish, positionSafe,
      allowNullMoveOnlyFromStockfish, allowNullMoveOnlyFromCrystal;
    //from Shashin end
	...

    return bestValue;
}

...
template<NodeType nodeType>
Value Search::Worker::qsearch(Position& pos, Stack* ss, Value alpha, Value beta) {

    static_assert(nodeType != Root);
    const RootShashinState& rootShashinState = shashinManager->getState();
	...
    return bestValue;
}
...
namespace {
...
}
void SearchManager::pv(Search::Worker&           worker,
                       const ThreadPool&         threads,
                       const TranspositionTable& tt,
                       Depth                     depth,
                       bool                      updateShashin) {  //shashin

		...        
		//from shashin begin
        // Update the root Shashin state only for the main (first) PV line.
        if (updateShashin && (i == 0) && d > worker.lastShashinUpdatedDepth)
        {
            worker.getShashinManager().updateRootShashinState(v, pos, d);
            worker.lastShashinUpdatedDepth = d;
        }
        //from shashin end
	...
}

...
}  // namespace ShashChess