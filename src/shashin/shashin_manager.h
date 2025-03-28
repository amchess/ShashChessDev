#ifndef SHASHIN_MANAGER_H
#define SHASHIN_MANAGER_H

#include "../position.h"
#include "../types.h"
#include "../movegen.h"
#include "../movepick.h"
#include "../evaluate.h"
#include <array>
#include "../wdl/win_probability.h"
#include "shashin_types.h"
#include "shashin_helper.h"

namespace ShashChess {
namespace Eval {
namespace NNUE {
struct Networks;
struct AccumulatorCaches;
}
}

namespace Search {
class Worker;
struct Stack;
}

class ShashinManager {
public:
    explicit ShashinManager();
    
    // Core interface
    ShashinPosition getResilientShashinRange(ShashinPosition range, int depth = 0);
    void updateRootShashinState(Value score, const Position& rootPos, int depth);
    ShashinPosition getShashinRange(Value value, const Position& rootPos);
    const RootShashinState& getState() const { return state; }
    void setDepth(int depth) { state.dynamicBase.currentDepth = depth; }
    int getDepth() const { return state.dynamicBase.currentDepth; }

    ShashinPosition getInitialShashinRange(Position& rootPos, Value staticValue, const ShashinConfig& config);
    
    // Position analysis
    bool isFortress(const Position& pos);
    void setStaticState(const Position& rootPos);
    bool isSacrificialPosition(const Position& rootPos);
    bool isPawnNearPromotion(const Position& rootPos);
    
    // Evaluation
    Value static_value(const Eval::NNUE::Networks& networks,
                      Eval::NNUE::AccumulatorCaches refreshTable,
                      Position& rootPos,
                      Search::Stack* ss,
                      int optimism);
    
    // Initialization
    void initDynamicRootShashinState(const Eval::NNUE::Networks& networks,
                                    Eval::NNUE::AccumulatorCaches refreshTable,
                                    Position& rootPos,
                                    Search::Stack* ss,
                                    int optimism,
                                    const ShashinConfig& config);
    void setDynamicBaseState(Value score, const Position& rootPos, int depth);
    void setDynamicDerivedState();
    void initDynamicBaseState(Value currentValue, Position& rootPos, const ShashinConfig& config);
    
    // Feature flags
    bool useMoveGenAndStep17CrystalLogic() const;
    bool isComplexPosition() const;
    bool useEarlyPruning() const;
    bool isLowActivity() const { return state.staticState.legalMoveCount < 20; }
    bool isTacticalInitiative() const;
    bool isTacticalReactive() const;
    bool isTacticalDefensive() const;
    
    // Style checks
    bool isShashinStyle(std::initializer_list<ShashinPosition> positions) const;
    bool isTillCategory(ShashinPosition lowerBound, ShashinPosition upperBound) const;
    
    // Search optimizations
    bool useCrystalFutilityPruningParent() const;
    bool useCrystalFutilityPruningChild() const;
    bool useCrystalNullMoveSearch() const;
    bool isSimpleIntermediate() const;
    bool isMCTSApplicableByValue() const;
    bool isMCTSExplorationApplicable() const;
    bool isTal() const;
    bool isHighPieceDensityCapablancaPosition() const;
    bool allowCrystalProbCut() const;
    bool isTacticalReduction() const;
    bool avoidStep10() const;
    bool isTalTacticalHighMiddle() const;
    bool isKingExposed() const { return state.staticState.kingSafetyScore < -30; }
    
    // Inline methods for performance
    template<ShashinPosition P>
    __attribute__((always_inline)) bool inRange() const {
        return state.dynamicBase.currentRange == P;
    }

    inline bool isStrategical() const {
        return inRange<ShashinPosition::CAPABLANCA>() && 
               !state.staticState.kingDanger;
    }
    
    inline bool isPassive() const {
        return isInRanges(state.dynamicBase.currentRange, 
                         ShashinPosition::MIDDLE_HIGH_PETROSIAN,
                         ShashinPosition::HIGH_PETROSIAN);
    }
    
    inline bool isTactical() const { 
        return isTal() || isPetrosian() || state.staticState.kingDanger; 
    }
    
    inline bool isPetrosian() const {
        return isShashinStyle({ShashinPosition::LOW_PETROSIAN, 
                              ShashinPosition::MIDDLE_PETROSIAN,
                              ShashinPosition::HIGH_PETROSIAN});
    }
    
    inline bool isHighPieceDensity() const {
        return state.staticState.allPiecesCount > 14;
    }
    
    inline bool isAggressive() const {
        return isInRanges(state.dynamicBase.currentRange, 
                         ShashinPosition::MIDDLE_LOW_TAL,
                         ShashinPosition::MIDDLE_TAL);
    }
    
    inline bool isInRange(ShashinPosition position) const {
        return state.dynamicBase.currentRange == position;
    }

private:
    RootShashinState state;
    mutable bool fortressCached = false;
    mutable bool fortressResult = false;
    mutable bool pawnNearPromoCacheValid = false;
    mutable bool pawnNearPromoResult = false;
    mutable bool sacrificialCacheValid = false;
    mutable bool sacrificialResult = false;
    
    // Helper to invalidate caches when position changes
    void invalidateCaches() {
        fortressCached = false;
        pawnNearPromoCacheValid = false;
        sacrificialCacheValid = false;
    }
};

} // namespace ShashChess

#endif // SHASHIN_MANAGER_H