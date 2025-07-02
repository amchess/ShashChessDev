#ifndef SHASHIN_MANAGER_H
#define SHASHIN_MANAGER_H

#include "shashin_position.h"
#include "shashin_params.h"
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
    explicit ShashinManager(const ShashinParams& params_ = {});

    // Core interface
    ShashinPosition         getResilientShashinRange(ShashinPosition range, int depth = 0);
    void                    updateRootShashinState(Value score, const Position& rootPos, int depth);
    ShashinPosition         getShashinRange(Value value, const Position& rootPos);
    const RootShashinState& getState() const { return state; }
    void                    setDepth(int depth) { state.dynamicBase.currentDepth = depth; }
    int                     getDepth() const { return state.dynamicBase.currentDepth; }

    ShashinPosition
    getInitialShashinRange(Position& rootPos, Value staticValue, const ShashinConfig& config);

    // Position analysis
    bool isFortress(const Position& pos);
    void setStaticState(const Position& rootPos);
    bool isSacrificialPosition(const Position& rootPos);
    bool isPawnNearPromotion(const Position& rootPos);

    // Evaluation
    Value static_value(const Eval::NNUE::Networks&   networks,
                       Eval::NNUE::AccumulatorStack& accumulators,
                       Eval::NNUE::AccumulatorCaches refreshTable,
                       Position&                     rootPos,
                       Search::Stack*                ss,
                       Value                         optimism);

    // Initialization
    void initDynamicRootShashinState(const Eval::NNUE::Networks&   networks,
                                     Eval::NNUE::AccumulatorStack& accumulators,
                                     Eval::NNUE::AccumulatorCaches refreshTable,
                                     Position&                     rootPos,
                                     Search::Stack*                ss,
                                     Value                         optimism,
                                     const ShashinConfig&          config);
    void setDynamicBaseState(Value score, const Position& rootPos, int depth);
    void setDynamicDerivedState();
    void initDynamicBaseState(Value currentValue, Position& rootPos, const ShashinConfig& config);

    // Feature flags
    bool useMoveGenCrystalLogic() const;
    bool useStep17CrystalLogic() const;
    bool isComplexPosition() const;
    bool isLowActivity() const { return state.staticState.legalMoveCount < 20; }
    bool isTacticalInitiative() const;
    bool isTacticalReactive() const;
    bool isTacticalDefensive() const;

    // Style checks
    bool isShashinStyle(std::initializer_list<ShashinPosition> positions) const;
    bool isTillCategory(ShashinPosition lowerBound, ShashinPosition upperBound) const;

    // Search optimizations
    bool allowCrystalProbCut() const;
    bool allowStockfishProbCut() const;
    bool useNullMoveByShashinForStockfish() const;
    bool isSimpleIntermediate() const;
    bool isMCTSApplicableByValue() const;
    bool isMCTSExplorationApplicable() const;
    bool isTal() const;
    bool isCapablanca() const;
    bool isHighPieceDensityCapablancaPosition() const;
    bool isTacticalReduction() const;
    bool avoidStep10() const;
    bool isTalTacticalHighMiddle() const;
    bool useCrystalFutility() const;
    bool isKingExposed() const { return state.staticState.kingSafetyScore < -30; }

    // Inline methods for performance
    template<ShashinPosition P>
    inline __attribute__((always_inline)) bool inRange() const {
        return state.dynamicBase.currentRange == P;
    }

    inline __attribute__((always_inline)) bool isStrategical() const {
        return inRange<ShashinPosition::CAPABLANCA>() && !state.staticState.kingDanger;
    }

    inline __attribute__((always_inline)) bool isPassive() const {
        return anyOf(state.dynamicBase.currentRange, ShashinPosition::MIDDLE_HIGH_PETROSIAN,
                     ShashinPosition::HIGH_PETROSIAN);
    }

    inline __attribute__((always_inline)) bool isTactical() const {
        return isTal() || isPetrosian() || state.staticState.kingDanger;
    }

    inline __attribute__((always_inline)) bool isPetrosian() const {
        return isShashinStyle({ShashinPosition::LOW_PETROSIAN, ShashinPosition::MIDDLE_PETROSIAN,
                               ShashinPosition::HIGH_PETROSIAN});
    }

    inline __attribute__((always_inline)) bool isHighPieceDensity() const {
        return state.staticState.allPiecesCount > 14;
    }

    inline __attribute__((always_inline)) bool isAggressive() const {
        return anyOf(state.dynamicBase.currentRange, ShashinPosition::MIDDLE_LOW_TAL,
                     ShashinPosition::MIDDLE_TAL);
    }

    inline __attribute__((always_inline)) bool isInRange(ShashinPosition position) const {
        return state.dynamicBase.currentRange == position;
    }

   private:
    ShashinParams params;
    struct PositionCache {
        uint64_t posHash       = 0;
        bool     fortress      = false;
        bool     pawnNearPromo = false;
        bool     sacrificial   = false;
    };
    RootShashinState state;
    PositionCache    positionCache;
    // Helper to calculate the position hash
    uint64_t computePositionHash(const Position& pos) {
        return Shashin::shashin_position_hash(pos);
    }
    // Helper to invalidate caches when position changes
    void invalidateCaches() {
        positionCache.posHash = 0;  // It invalidates all at the position change
    }
};

}  // namespace ShashChess

#endif  // SHASHIN_MANAGER_H