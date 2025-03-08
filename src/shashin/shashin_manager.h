#ifndef SHASHIN_MANAGER_H
#define SHASHIN_MANAGER_H

#include "../position.h"
#include "../types.h"
#include "../movegen.h"
#include "../movepick.h"
#include "../evaluate.h"
#include <unordered_map>
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
    ShashinPosition getResilientShashinRange(ShashinPosition range, int depth = 0);
    // Manage state
    void updateRootShashinState(Value score, const Position& rootPos, int depth);
    ShashinPosition getShashinRange(Value value, const Position& rootPos);
    const RootShashinState& getState() const noexcept;
    void setDepth(int depth) noexcept;
    int getDepth() const noexcept;

    ShashinPosition getInitialShashinRange(Position& rootPos, Value staticValue, const ShashinConfig& config);
    // Fortress detection method
    bool isFortress(const Position& pos) const;
    void setStaticState(const Position& rootPos);
    bool isSacrificialPosition(const Position& rootPos) const;
    bool isPawnNearPromotion(const Position& rootPos) const;
    Value static_value(const Eval::NNUE::Networks& networks,
                       Eval::NNUE::AccumulatorCaches refreshTable,
                       Position& rootPos,
                       Search::Stack* ss,
                       int optimism);
    void initDynamicRootShashinState(const Eval::NNUE::Networks& networks,
                                     Eval::NNUE::AccumulatorCaches refreshTable,
                                     Position& rootPos,
                                     Search::Stack* ss,
                                     int optimism,
                                     const ShashinConfig& config);
    void setDynamicBaseState(Value score, const Position& rootPos, int depth);
    void setDynamicDerivedState();
    void initDynamicBaseState(Value currentValue, Position& rootPos, const ShashinConfig& config);
    bool useMoveGenAndStep17CrystalLogic() const noexcept;
    bool isComplexPosition() const noexcept;
    bool useEarlyPruning() const noexcept;
    bool isLowActivity() const noexcept;
    bool isTacticalInitiative() const noexcept;
    bool isTacticalReactive() const noexcept;
    bool isTacticalDefensive() const noexcept;
    bool isShashinStyle(std::initializer_list<ShashinPosition> positions) const noexcept;
    bool isTillCategory(ShashinPosition lowerBound, ShashinPosition upperBound) const noexcept;
    bool useCrystalFutilityPruningParent() const noexcept;
    bool useCrystalFutilityPruningChild() const noexcept;
    bool useCrystalNullMoveSearch() const noexcept;
    bool isSimpleIntermediate() const noexcept;
    bool isMCTSApplicableByValue() const noexcept;
    bool isMCTSExplorationApplicable() const noexcept;
    bool isTal() const noexcept;
    bool isHighPieceDensityCapablancaPosition() const noexcept;
    bool allowCrystalProbCut() const noexcept;
    bool isTacticalReduction() const noexcept;
    bool avoidStep10() const noexcept;
    bool isTalTacticalHighMiddle() const noexcept;
    bool isKingExposed() const noexcept;

    // Inline methods
    inline bool isStrategical() const noexcept {
        return ((state.dynamicBase.currentRange == ShashinPosition::CAPABLANCA) && (!kingDanger));
    }
    inline bool isPassive() const noexcept {
        return isInRanges(state.dynamicBase.currentRange, ShashinPosition::MIDDLE_HIGH_PETROSIAN,
                          ShashinPosition::HIGH_PETROSIAN);
    }
    inline bool isTactical() const noexcept { return isTal() || isPetrosian() || kingDanger; }
    inline bool isPetrosian() const noexcept {
        return isShashinStyle({ShashinPosition::LOW_PETROSIAN, ShashinPosition::MIDDLE_PETROSIAN, ShashinPosition::HIGH_PETROSIAN});
    }
    inline bool isHighPieceDensity() const noexcept {
        return allPiecesCount > 14;
    }
    inline bool isAggressive() const noexcept {
        return isInRanges(state.dynamicBase.currentRange, ShashinPosition::MIDDLE_LOW_TAL,
                          ShashinPosition::MIDDLE_TAL);
    }
    inline bool isInRange(ShashinPosition position) const noexcept { return state.dynamicBase.currentRange == position; }

private:
    RootShashinState state;
    bool highMaterial = false, kingDanger = false, pawnsNearPromotion = false;
    int allPiecesCount = 0, kingSafetyScore = 0;
    mutable bool fortressCached = false;
    mutable bool fortressResult = false;
};

}  // namespace ShashChess

#endif  // SHASHIN_MANAGER_H