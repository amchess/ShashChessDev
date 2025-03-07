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
    void             updateRootShashinState(Value score, const Position& rootPos, int depth);
    ShashinPosition  getShashinRange(Value value, const Position& rootPos);
    const RootShashinState& getState() const;
    void             setDepth(int depth);
    int              getDepth() const;

    ShashinPosition
    getInitialShashinRange(Position& rootPos, Value staticValue, const ShashinConfig& config);
    // Fortress detection method
    bool isFortress(const Position& pos);
    void  setStaticState(const Position& rootPos);
    bool  isSacrificialPosition(const Position& rootPos);
    bool  isPawnNearPromotion(const Position& rootPos);
    Value static_value(const Eval::NNUE::Networks&   networks,
                              Eval::NNUE::AccumulatorCaches refreshTable,
                              Position&                     rootPos,
                              Search::Stack*                ss,
                              int                           optimism);
    void         initDynamicRootShashinState(const Eval::NNUE::Networks&   networks,
                                   Eval::NNUE::AccumulatorCaches refreshTable,
                                   Position&                     rootPos,
                                   Search::Stack*                ss,
                                   int                           optimism,
                                   const ShashinConfig&          config);
    void setDynamicBaseState(Value score, const Position& rootPos, int depth);
    void setDynamicDerivedState();
    void initDynamicBaseState(Value currentValue, Position& rootPos, const ShashinConfig& config);                               
    bool useMoveGenAndStep17CrystalLogic() const;
    bool isComplexPosition() const;
    bool useEarlyPruning() const;
    bool isLowActivity() const;
    bool isTacticalInitiative() const;
    bool isTacticalReactive() const;
    bool isTacticalDefensive() const;
    bool isShashinStyle(std::initializer_list<ShashinPosition> positions) const;
    bool isTillCategory(ShashinPosition lowerBound, ShashinPosition upperBound) const;
    bool useCrystalFutilityPruningParent() const;
    bool useCrystalFutilityPruningChild() const;
    bool useCrystalNullMoveSearch() const;
    bool isSimpleIntermediate() const;
    bool isMCTSApplicableByValue() const;
    bool isMCTSExplorationApplicable() const;
    bool isTal() const;
    bool isHighPieceDensityCapablancaPosition() const;
    bool allowCrystalProbCut() const ;
    bool isTacticalReduction() const;
    bool avoidStep10() const;
    bool isTalTacticalHighMiddle() const;
    bool isKingExposed() const;
    //inline methods
    // Strategic zones: Only Capablanca is purely strategic
    inline bool isStrategical() const {
        return ((state.dynamicBase.currentRange == ShashinPosition::CAPABLANCA) && (!kingDanger));
    }
    inline bool isPassive() const {
    return isInRanges(state.dynamicBase.currentRange, ShashinPosition::MIDDLE_HIGH_PETROSIAN,
                        ShashinPosition::HIGH_PETROSIAN);
    }
    // Tactical zones
    inline bool isTactical() const { return isTal() || isPetrosian() || kingDanger; }
    inline bool isPetrosian() const {
        return isShashinStyle({ShashinPosition::LOW_PETROSIAN, ShashinPosition::MIDDLE_PETROSIAN, ShashinPosition::HIGH_PETROSIAN});
    }    
    inline bool isHighPieceDensity() const {
        const int pieceCount = allPiecesCount;
        return pieceCount > 14;
    }
    inline bool isAggressive() const {
        return isInRanges(state.dynamicBase.currentRange, ShashinPosition::MIDDLE_LOW_TAL,
                            ShashinPosition::MIDDLE_TAL);
    }
    //Utility methods
    inline bool isInRange(ShashinPosition position) const { return state.dynamicBase.currentRange == position; }

   private:
    RootShashinState state;
    bool highMaterial = false, kingDanger=false, pawnsNearPromotion = false;
    int allPiecesCount = 0, kingSafetyScore = 0;;
    mutable bool fortressCached = false;
    mutable bool fortressResult = false;
};
}  // namespace ShashChess
#endif  // SHASHIN_MANAGER_H