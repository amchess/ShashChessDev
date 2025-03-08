#include "shashin_manager.h"
#include "../nnue/network.h"
#include "../nnue/nnue_accumulator.h"
#include "../search.h"

namespace ShashChess {

// Constructor
ShashinManager::ShashinManager() {}

ShashinPosition ShashinManager::getResilientShashinRange(ShashinPosition range, int depth) {
    if (depth <= 10)
    {
        if (range == ShashinPosition::CAPABLANCA_TAL
            || range == ShashinPosition::TAL_CAPABLANCA_PETROSIAN)
            return ShashinPosition::LOW_TAL;
        if (range == ShashinPosition::CAPABLANCA_PETROSIAN)
            return ShashinPosition::CAPABLANCA;
    }
    else if (depth <= 20 && range == ShashinPosition::TAL_CAPABLANCA_PETROSIAN)
    {
        return ShashinPosition::CAPABLANCA;
    }
    return range;
}
bool ShashinManager::useMoveGenAndStep17CrystalLogic() const {

    if (isStrategical() && highMaterial && state.staticState.legalMoveCount >= 30)
    {
        return true;
    }
    // Tal logic: attacking with low complexity and king danger
    if (isAggressive() && state.staticState.legalMoveCount < 30 && kingDanger)
    {
        return true;
    }
    // Default: no Crystal logic
    return false;
}
// Defense degrees
bool ShashinManager::isTacticalReactive() const {

    return isInRanges(state.dynamicBase.currentRange, ShashinPosition::LOW_PETROSIAN);
}
bool ShashinManager::isTacticalDefensive() const {

    return isInRanges(state.dynamicBase.currentRange, ShashinPosition::MIDDLE_LOW_PETROSIAN,
                      ShashinPosition::MIDDLE_PETROSIAN);
}
// Utility methods
bool ShashinManager::isShashinStyle(std::initializer_list<ShashinPosition> positions) const {
    return std::any_of(positions.begin(), positions.end(), [this](ShashinPosition pos) {
        return state.dynamicBase.currentRange == pos;
    });
}
//isTill begin
bool ShashinManager::isTillCategory(ShashinPosition lowerBound, ShashinPosition upperBound) const {
    return !isInRanges(state.dynamicBase.currentRange, lowerBound, upperBound);
}
//isTill end
bool ShashinManager::useCrystalFutilityPruningParent() const {
    return isTalTacticalHighMiddle() && isSimpleIntermediate() && !kingDanger
        && !isHighPieceDensity()
        && state.dynamicBase.currentDepth < 12;  // Applica il pruning solo a profondità più basse
}
bool ShashinManager::useCrystalFutilityPruningChild() const {
    return (isAggressive() && state.staticState.legalMoveCount >= 20)
        || (isTacticalReactive() && (state.staticState.legalMoveCount < 20 || highMaterial))
        || (isStrategical() && state.staticState.legalMoveCount >= 30);
}
bool ShashinManager::useCrystalNullMoveSearch() const {

    return isInRange(ShashinPosition::HIGH_TAL) || (isAggressive() && highMaterial)
        || (isPassive() && state.staticState.legalMoveCount < 15);
}
// Simple intermediate positions: within bounds and low move count
bool ShashinManager::isSimpleIntermediate() const {
    return isTillCategory(ShashinPosition::MIDDLE_PETROSIAN, ShashinPosition::MIDDLE_TAL)
        && state.staticState.legalMoveCount <= 20 && !kingDanger && !isHighPieceDensity()
        && state.dynamicBase.currentDepth < 8;
}
//for mcts begin
bool ShashinManager::isMCTSApplicableByValue() const {
    // MCTS is applicable in specific Petrosian zones
    return isInRange(ShashinPosition::HIGH_PETROSIAN)
        || isInRange(ShashinPosition::MIDDLE_HIGH_PETROSIAN)
        || isInRange(ShashinPosition::MIDDLE_PETROSIAN);
}
bool ShashinManager::isMCTSExplorationApplicable() const {

    size_t legalMoveCount = state.staticState.legalMoveCount;
    return ((isInRange(ShashinPosition::MIDDLE_HIGH_PETROSIAN)
             || isInRange(ShashinPosition::HIGH_TAL))
            && legalMoveCount >= 30)
        || isInRange(ShashinPosition::CAPABLANCA) || isPetrosian();
}
//for mcts end
bool ShashinManager::isComplexPosition() const {
    size_t legalMoveCount = state.staticState.legalMoveCount;
    return (legalMoveCount >= 25 && highMaterial) || kingDanger || pawnsNearPromotion;
}
bool ShashinManager::isHighPieceDensityCapablancaPosition() const {
    return isInRange(ShashinPosition::CAPABLANCA) && isHighPieceDensity();
}
// Determines whether to allow Crystal's ProbCut logic (~4 Elo improvement)
// This logic prunes the search tree earlier when the position is non-critical.
bool ShashinManager::allowCrystalProbCut() const {

    return (isTalTacticalHighMiddle() && state.staticState.legalMoveCount < 30)
        || (isStrategical() && highMaterial);
}
bool ShashinManager::isTacticalReduction() const {
    return (isAggressive() && (highMaterial || !isHighPieceDensity()));
}
// Determines whether to skip Step 10: Internal Iterative Reductions (~9 Elo)
// This method adjusts the logic based on Shashin's position classification
// and the complexity of the current position.
bool ShashinManager::avoidStep10() const {

    return (isPassive() && state.staticState.legalMoveCount < 20)
        ||                                  // Skip for passive zones with low activity
           isStrategical() ||               // Skip for strategic zones
           (isAggressive() && kingDanger);  // Skip for aggressive zones with king danger
}
bool ShashinManager::isTalTacticalHighMiddle() const {
    return isInRange(ShashinPosition::HIGH_TAL) || isInRange(ShashinPosition::MIDDLE_HIGH_TAL);
}
bool ShashinManager::isKingExposed() const {
    return kingSafetyScore < -30;  // Arbitrary threeshold to define in danger king
}
bool ShashinManager::isLowActivity() const { return state.staticState.legalMoveCount < 20; }
bool ShashinManager::useEarlyPruning() const {
    // No pruning beyond depth 8
    if (state.dynamicBase.currentDepth > 6)
        return false;
    // Pruning allowed in simple strategic positions at shallow depths
    if (isStrategical() && !isComplexPosition() && state.staticState.legalMoveCount < 20
        && state.dynamicBase.currentDepth <= 6)
        return true;
    // Pruning allowed in simple tactical positions with low activity at very shallow depths
    if (isTactical() && state.staticState.legalMoveCount < 10
        && state.dynamicBase.currentDepth <= 4)
        return true;
    return false;  // Default: no pruning
}
//Tactical zones
bool ShashinManager::isTal() const {
    return isShashinStyle(
      {ShashinPosition::LOW_TAL, ShashinPosition::MIDDLE_TAL, ShashinPosition::HIGH_TAL});
}
// Attack degrees
bool ShashinManager::isTacticalInitiative() const {
    return isInRanges(state.dynamicBase.currentRange, ShashinPosition::LOW_TAL);
}
void ShashinManager::initDynamicBaseState(Value                currentValue,
                                          Position&            rootPos,
                                          const ShashinConfig& config) {
    state.dynamicBase.currentDepth = 0;
    ShashinPosition currentRange   = getInitialShashinRange(rootPos, currentValue, config);
    state.dynamicBase.currentRange = getResilientShashinRange(currentRange);
}
void ShashinManager::setDynamicBaseState(Value score, const Position& rootPos, int depth) {
    ShashinPosition range          = static_cast<ShashinPosition>(getShashinRange(score, rootPos));
    state.dynamicBase.currentRange = getResilientShashinRange(range, depth);
    state.dynamicBase.currentDepth = depth;
}

// Initializes Shashin values at the start of a search
void ShashinManager::initDynamicRootShashinState(const Eval::NNUE::Networks&   networks,
                                                 Eval::NNUE::AccumulatorCaches refreshTable,
                                                 Position&                     rootPos,
                                                 Search::Stack*                ss,
                                                 int                           optimism,
                                                 const ShashinConfig&          config) {
    initDynamicBaseState(static_value(networks, refreshTable, rootPos, ss, optimism), rootPos,
                         config);
    setDynamicDerivedState();
    MoveConfig::useMoveShashinLogic = useMoveGenAndStep17CrystalLogic();
    MoveConfig::isStrategical       = isStrategical();
    MoveConfig::isAggressive        = state.dynamicDerived.isAggressive;
}
void ShashinManager::updateRootShashinState(Value score, const Position& rootPos, int depth) {
    if (depth <= state.dynamicBase.currentDepth && depth != 0)
        return;

    ShashinPosition newRange = getShashinRange(score, rootPos);
    if (newRange == state.dynamicBase.currentRange && depth == state.dynamicBase.currentDepth)
        return;

    setDynamicBaseState(score, rootPos, depth);
    setDynamicDerivedState();

    bool newUseMoveShashinLogic = useMoveGenAndStep17CrystalLogic();
    if (MoveConfig::useMoveShashinLogic != newUseMoveShashinLogic
        || MoveConfig::isStrategical != state.dynamicDerived.isStrategical
        || MoveConfig::isAggressive != state.dynamicDerived.isAggressive)
    {
        MoveConfig::useMoveShashinLogic = newUseMoveShashinLogic;
        MoveConfig::isStrategical       = state.dynamicDerived.isStrategical;
        MoveConfig::isAggressive        = state.dynamicDerived.isAggressive;
    }
}
void ShashinManager::setDynamicDerivedState() {
    auto& derived              = state.dynamicDerived;  // Alias per ridurre la scrittura ripetitiva
    derived.isStrategical      = isStrategical();
    derived.isAggressive       = isAggressive();
    derived.isTacticalReactive = isTacticalReactive();

    derived.isHighTal = isInRange(ShashinPosition::HIGH_TAL);
}
// Getter and Setter for depth
void ShashinManager::setDepth(int depth) { state.dynamicBase.currentDepth = depth; }
int  ShashinManager::getDepth() const { return state.dynamicBase.currentDepth; }

// Returns a safe copy of the current state
const RootShashinState& ShashinManager::getState() const {
    return state;
}  // It returns a safe state copy

// Fortress detection method
bool ShashinManager::isFortress(const Position& pos) {
    if (!fortressCached)
    {
        // Do not consider early positions as fortress positions.
        if (pos.game_ply() < 30)
        {
            fortressResult = false;
        }
        else
        {
            // Check all conditions in a single if statement to leverage short-circuit evaluation.
            fortressResult = !pos.has_pawn_breaks() && pos.is_king_cutoff(pos.side_to_move())
                          && pos.has_safe_waiting_moves();
        }
        fortressCached = true;
    }
    return fortressResult;
}
// Computes the static evaluation of a position
Value ShashinManager::static_value(const Eval::NNUE::Networks&   networks,
                                   Eval::NNUE::AccumulatorCaches refreshTable,
                                   Position&                     rootPos,
                                   Search::Stack*                ss,
                                   int                           optimism) {
    MoveList<LEGAL> legalMoves(rootPos);  // It creates a legal moves list

    // If the king is in check and there are no legal moves, return VALUE_MATE
    if (legalMoves.size() == 0 && rootPos.checkers())
    {
        return VALUE_MATE;
    }

    // If the position is a draw or the king is not in check and there are no legal moves, return VALUE_DRAW
    if (ss->ply >= MAX_PLY || rootPos.is_draw(ss->ply) || legalMoves.size() == 0)
    {
        return VALUE_DRAW;
    }

    // Evaluate the position only if there are no checks
    if (!rootPos.checkers())
    {
        return Eval::evaluate(networks, rootPos, refreshTable, optimism);
    }

    // If the king is in check, evaluate the legal moves
    Value bestValue = -VALUE_INFINITE;
    for (const auto& move : legalMoves)
    {
        // Create a new position by applying the legal move
        StateInfo stateInfo;
        rootPos.do_move(move, stateInfo);

        // Evaluate the new position
        Value value = -static_value(networks, refreshTable, rootPos, ss + 1, -optimism);

        // Restore the original position
        rootPos.undo_move(move);

        // Update the best value found
        bestValue = std::max(bestValue, value);
    }

    return bestValue;
}

// Extracts key position characteristics for classification
void ShashinManager::setStaticState(const Position& rootPos) {
    state.staticState.legalMoveCount = MoveList<LEGAL>(rootPos).size();
    state.staticState.isSacrificial  = isSacrificialPosition(rootPos);
    state.staticState.kingExposed    = isKingExposed();
    MoveConfig::isFortress           = isFortress(rootPos);
    highMaterial       = rootPos.non_pawn_material(WHITE) + rootPos.non_pawn_material(BLACK) > 2400;
    kingDanger         = rootPos.king_danger(WHITE) || rootPos.king_danger(BLACK);
    pawnsNearPromotion = isPawnNearPromotion(rootPos);
    allPiecesCount     = rootPos.count<ALL_PIECES>();
    kingSafetyScore    = rootPos.king_safety_score(rootPos.side_to_move());
}

// Determines if the position is highly sacrificial
bool ShashinManager::isSacrificialPosition(const Position& rootPos) {
    MoveList<LEGAL> legalMoves(rootPos);
    for (const auto& move : legalMoves)
    {
        if (rootPos.is_sacrifice(move))
        {
            return true;
        }
    }
    return false;
}

// Checks if there are pawns close to promotion
bool ShashinManager::isPawnNearPromotion(const Position& rootPos) {
    // We consider the advanced pawns (near promotion)
    for (Square s = SQ_A2; s <= SQ_H7; ++s)
    {
        Piece piece = rootPos.piece_on(s);
        if (type_of(piece) == PAWN)
        {
            Rank rank = rank_of(s);
            if ((color_of(piece) == WHITE && rank >= RANK_5)
                || (color_of(piece) == BLACK && rank <= RANK_4))
            {
                return true;  // Return true as soon as we find an advanced pawn
            }
        }
    }
    return false;  // No advanced pawns found
}


// Static lookup table for Shashin ranges
static const std::array<ShashinPosition, 101> initializeLookupTable() {
    std::array<ShashinPosition, 101> lookup{};

    // Populate ranges using a lambda function
    auto assignRange = [&](int start, int end, ShashinPosition pos) {
        std::fill(lookup.begin() + start, lookup.begin() + end + 1, pos);
    };

    // Populate ranges
    assignRange(0, HIGH_PETROSIAN_MAX, ShashinPosition::HIGH_PETROSIAN);
    assignRange(HIGH_PETROSIAN_MAX + 1, MIDDLE_HIGH_PETROSIAN_MAX,
                ShashinPosition::MIDDLE_HIGH_PETROSIAN);
    assignRange(MIDDLE_HIGH_PETROSIAN_MAX + 1, MIDDLE_PETROSIAN_MAX,
                ShashinPosition::MIDDLE_PETROSIAN);
    assignRange(MIDDLE_PETROSIAN_MAX + 1, MIDDLE_LOW_PETROSIAN_MAX,
                ShashinPosition::MIDDLE_LOW_PETROSIAN);
    assignRange(MIDDLE_LOW_PETROSIAN_MAX + 1, LOW_PETROSIAN_MAX, ShashinPosition::LOW_PETROSIAN);
    assignRange(LOW_PETROSIAN_MAX + 1, CAPABLANCA_PETROSIAN_MAX,
                ShashinPosition::CAPABLANCA_PETROSIAN);

    // Special case: Chaos for Win Probability = 50
    lookup[CAPABLANCA_MAX] = ShashinPosition::TAL_CAPABLANCA_PETROSIAN;

    assignRange(CAPABLANCA_MAX + 1, CAPABLANCA_TAL_MAX, ShashinPosition::CAPABLANCA_TAL);
    assignRange(CAPABLANCA_TAL_MAX + 1, LOW_TAL_MAX, ShashinPosition::LOW_TAL);
    assignRange(LOW_TAL_MAX + 1, MIDDLE_LOW_TAL_MAX, ShashinPosition::MIDDLE_LOW_TAL);
    assignRange(MIDDLE_LOW_TAL_MAX + 1, MIDDLE_TAL_MAX, ShashinPosition::MIDDLE_TAL);
    assignRange(MIDDLE_TAL_MAX + 1, MIDDLE_HIGH_TAL_MAX, ShashinPosition::MIDDLE_HIGH_TAL);
    assignRange(MIDDLE_HIGH_TAL_MAX + 1, HIGH_TAL_MAX, ShashinPosition::HIGH_TAL);

    return lookup;
}

// Static lookup table (constexpr in C++17)
static const auto lookup = initializeLookupTable();

// Determines the Shashin position (range) type based on evaluation
ShashinPosition ShashinManager::getShashinRange(Value value, const Position& rootPos) {
    WDLModel::WDL wdl = WDLModel::get_wdl(value, rootPos);
    // Calculate directly Win Probability
    int winProbability = wdl.win + (wdl.draw / 2);

    // Special case for Win Probability = 50
    if (winProbability == CAPABLANCA_MAX)
    {
        // Check WDL to differentiate between Capablanca and Chaos: Capablanca-Petrosian-Tal
        if (wdl.win == 0 && wdl.loss == 0)
        {
            return ShashinPosition::CAPABLANCA;  // Purely equal position
        }
        else
        {
            return ShashinPosition::TAL_CAPABLANCA_PETROSIAN;  // Chaos
        }
    }

    // Use the lookup table if the value is within bounds
    if (winProbability >= 0 && winProbability <= 100)
    {
        return lookup[winProbability];
    }

    // Fallback for unhandled cases
    return ShashinPosition::TAL_CAPABLANCA_PETROSIAN;
}

ShashinPosition ShashinManager::getInitialShashinRange(Position&            rootPos,
                                                       Value                staticValue,
                                                       const ShashinConfig& config) {
    // Se tutte le configurazioni sono disabilitate, usa la valutazione standard
    if (!config.highTal && !config.middleTal && !config.lowTal && !config.capablanca
        && !config.highPetrosian && !config.middlePetrosian && !config.lowPetrosian)
    {
        return getShashinRange(staticValue, rootPos);
    }
    if (isFortress(rootPos))
    {
        return ShashinPosition::CAPABLANCA;
    }
    // Petrosian logic
    if (config.highPetrosian)
    {
        if (config.middlePetrosian)
            return ShashinPosition::MIDDLE_HIGH_PETROSIAN;
        return ShashinPosition::HIGH_PETROSIAN;
    }
    if (config.middlePetrosian)
    {
        if (config.lowPetrosian)
            return ShashinPosition::MIDDLE_LOW_PETROSIAN;
        return ShashinPosition::MIDDLE_PETROSIAN;
    }
    if (config.lowPetrosian)
    {
        if (config.capablanca)
            return ShashinPosition::CAPABLANCA_PETROSIAN;
        return ShashinPosition::LOW_PETROSIAN;
    }

    // Capablanca logic
    if (config.capablanca)
    {
        return ShashinPosition::CAPABLANCA;
    }

    // Tal logic
    if (config.highTal)
    {
        if (config.middleTal)
            return ShashinPosition::MIDDLE_HIGH_TAL;
        return ShashinPosition::HIGH_TAL;
    }
    if (config.middleTal)
    {
        if (config.lowTal)
            return ShashinPosition::MIDDLE_LOW_TAL;
        return ShashinPosition::MIDDLE_TAL;
    }
    if (config.lowTal)
    {
        if (config.capablanca)
            return ShashinPosition::CAPABLANCA_TAL;
        return ShashinPosition::LOW_TAL;
    }

    // Default static value
    return getShashinRange(staticValue, rootPos);
}
}  // namespace ShashChess
