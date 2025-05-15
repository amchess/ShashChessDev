#include "shashin_manager.h"
#include "../nnue/network.h"
#include "../nnue/nnue_accumulator.h"
#include "../search.h"
#include <algorithm>

namespace ShashChess {

ShashinManager::ShashinManager() {}

ShashinPosition ShashinManager::getResilientShashinRange(ShashinPosition range, int depth) {
    constexpr int HIGH_MOBILITY_MOVES  = 30;
    constexpr int MID_MOBILITY_MOVES   = 15;
    constexpr int HIGH_DEPTH_LIMIT     = 12;
    constexpr int MID_DEPTH_LIMIT      = 8;
    constexpr int LOW_DEPTH_LIMIT      = 5;
    constexpr int ADVANCED_DEPTH_LIMIT = 20;

    int mvCount          = state.staticState.legalMoveCount;
    int dynamicThreshold = (mvCount > HIGH_MOBILITY_MOVES) ? HIGH_DEPTH_LIMIT
                         : (mvCount > MID_MOBILITY_MOVES)  ? MID_DEPTH_LIMIT
                                                           : LOW_DEPTH_LIMIT;

    bool isTalRange = (range == ShashinPosition::CAPABLANCA_TAL
                       || range == ShashinPosition::TAL_CAPABLANCA_PETROSIAN);

    if (depth <= dynamicThreshold)
    {
        if (isTalRange)
            return ShashinPosition::LOW_TAL;
        if (range == ShashinPosition::CAPABLANCA_PETROSIAN)
            return ShashinPosition::CAPABLANCA;
    }
    else if (depth <= ADVANCED_DEPTH_LIMIT && range == ShashinPosition::TAL_CAPABLANCA_PETROSIAN)
    {
        return ShashinPosition::CAPABLANCA;
    }
    return range;
}

__attribute__((hot)) bool ShashinManager::useMoveGenAndStep17CrystalLogic() const {
    const auto& s = state.staticState;  // Accesso locale

    // Strategical with high material and many moves
    if (__builtin_expect((state.dynamicBase.currentRange == ShashinPosition::CAPABLANCA)
                           && !s.kingDanger && s.highMaterial && s.legalMoveCount >= 30,
                         0))
    {
        return true;
    }

    // Tal logic: attacking with low complexity and king danger
    if (__builtin_expect((state.dynamicBase.currentRange >= ShashinPosition::MIDDLE_LOW_TAL
                          && state.dynamicBase.currentRange <= ShashinPosition::MIDDLE_TAL)
                           && s.legalMoveCount < 30 && s.kingDanger,
                         1))
    {
        return true;
    }

    return false;
}

bool ShashinManager::useCrystalFutility() const {
    const auto& st = state.staticState;
    const auto& dd = state.dynamicDerived;
    // Applichiamo Crystal solo in posizioni tattiche “pulite”:
    return dd.isTacticalReactive && !st.kingExposed && !st.isSacrificial && st.legalMoveCount <= 20;
}

bool ShashinManager::allowCrystalProbCut() const {
    const auto& s              = state.staticState;
    const auto& d              = state.dynamicDerived;
    bool        highTalent     = d.isHighTal && s.legalMoveCount < 30 && !s.kingExposed;
    bool        reactiveTactic = d.isTacticalReactive && !s.kingExposed && s.legalMoveCount < 25;
    return highTalent || reactiveTactic;
}

bool ShashinManager::allowStockfishProbCut() const {
    const auto& s     = state.staticState;
    int         depth = state.dynamicBase.currentDepth;
    return (s.legalMoveCount < 40 && depth <= 6) || (s.legalMoveCount < 60 && depth <= 8);
}

bool ShashinManager::isTacticalReactive() const {
    return isInRanges(state.dynamicBase.currentRange, ShashinPosition::LOW_PETROSIAN);
}

bool ShashinManager::isTacticalDefensive() const {
    return isInRanges(state.dynamicBase.currentRange, ShashinPosition::MIDDLE_LOW_PETROSIAN,
                      ShashinPosition::MIDDLE_PETROSIAN);
}

bool ShashinManager::isShashinStyle(std::initializer_list<ShashinPosition> positions) const {
    return std::any_of(positions.begin(), positions.end(), [this](ShashinPosition pos) {
        return state.dynamicBase.currentRange == pos;
    });
}

bool ShashinManager::isTillCategory(ShashinPosition lowerBound, ShashinPosition upperBound) const {
    return !isInRanges(state.dynamicBase.currentRange, lowerBound, upperBound);
}


bool ShashinManager::useNullMoveByShashinForStockfish() const {
    const auto& staticState = state.staticState;
    return (!isInRange(ShashinPosition::HIGH_TAL) && (!isAggressive() || !staticState.highMaterial)
            && (!isPassive() || staticState.legalMoveCount >= 15));
}

bool ShashinManager::isSimpleIntermediate() const {
    const auto& staticState = state.staticState;
    return isTillCategory(ShashinPosition::MIDDLE_PETROSIAN, ShashinPosition::MIDDLE_TAL)
        && staticState.legalMoveCount <= 20 && !staticState.kingDanger && !isHighPieceDensity()
        && state.dynamicBase.currentDepth < 8;
}

bool ShashinManager::isMCTSApplicableByValue() const {
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

bool ShashinManager::isComplexPosition() const {
    const auto& staticState = state.staticState;
    return (staticState.legalMoveCount >= 25 && staticState.highMaterial) || staticState.kingDanger
        || staticState.pawnsNearPromotion;
}

bool ShashinManager::isHighPieceDensityCapablancaPosition() const {
    return isInRange(ShashinPosition::CAPABLANCA) && isHighPieceDensity();
}

bool ShashinManager::isTacticalReduction() const {
    const auto& staticState = state.staticState;
    return isAggressive() && (staticState.highMaterial || !isHighPieceDensity());
}

bool ShashinManager::avoidStep10() const {
    const auto& staticState = state.staticState;
    return (isPassive() && staticState.legalMoveCount < 20) || isStrategical()
        || (isAggressive() && staticState.kingDanger);
}

bool ShashinManager::isTalTacticalHighMiddle() const {
    return isInRange(ShashinPosition::HIGH_TAL) || isInRange(ShashinPosition::MIDDLE_HIGH_TAL);
}

bool ShashinManager::isTacticalInitiative() const {
    return isInRanges(state.dynamicBase.currentRange, ShashinPosition::LOW_TAL);
}

bool ShashinManager::isTal() const {
    return isShashinStyle(
      {ShashinPosition::LOW_TAL, ShashinPosition::MIDDLE_TAL, ShashinPosition::HIGH_TAL});
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

void ShashinManager::initDynamicRootShashinState(const Eval::NNUE::Networks&   networks,
                                                 Eval::NNUE::AccumulatorStack& accumulators,
                                                 Eval::NNUE::AccumulatorCaches refreshTable,
                                                 Position&                     rootPos,
                                                 Search::Stack*                ss,
                                                 int                           optimism,
                                                 const ShashinConfig&          config) {
    const auto& alignedNetworks = networks;
    initDynamicBaseState(
      static_value(alignedNetworks, accumulators, refreshTable, rootPos, ss, optimism), rootPos,
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

    // Direct comparison without temporaries
    const bool newUseLogic    = useMoveGenAndStep17CrystalLogic();
    const bool newStrategical = state.dynamicDerived.isStrategical;
    const bool newAggressive  = state.dynamicDerived.isAggressive;

    if (newUseLogic != MoveConfig::useMoveShashinLogic
        || newStrategical != MoveConfig::isStrategical || newAggressive != MoveConfig::isAggressive)
    {
        MoveConfig::useMoveShashinLogic = newUseLogic;
        MoveConfig::isStrategical       = newStrategical;
        MoveConfig::isAggressive        = newAggressive;
    }
}

void ShashinManager::setDynamicDerivedState() {
    auto& derived              = state.dynamicDerived;
    derived.isStrategical      = isStrategical();
    derived.isAggressive       = isAggressive();
    derived.isTacticalReactive = isTacticalReactive();
    derived.isHighTal          = isInRange(ShashinPosition::HIGH_TAL);
}

bool ShashinManager::isFortress(const Position& pos) {
    if (!fortressCached)
    {
        constexpr int MinPly         = 28;
        constexpr int DefenderPieces = 2;

        const int   gameStage = pos.game_ply();
        const Color us        = pos.side_to_move();

        const int ourPawns   = popcount(pos.pieces(PAWN, us));
        const int theirPawns = popcount(pos.pieces(PAWN, ~us));

        const bool enoughMaterial =
          (pos.count<ROOK>(us) + pos.count<KNIGHT>(us) + pos.count<BISHOP>(us)) >= DefenderPieces;

        const bool compactStructure = (ourPawns >= 4) && (ourPawns >= theirPawns);

        const bool kingSafety = (pos.king_safety_score(us) > 150)
                             && (distance(pos.square<KING>(us), pos.square<KING>(~us)) > 3);

        fortressResult = (gameStage >= MinPly) && (!pos.has_pawn_breaks()) && pos.is_king_cutoff(us)
                      && pos.has_safe_waiting_moves() && enoughMaterial && compactStructure
                      && kingSafety && (pos.non_pawn_material(us) > 2 * pos.non_pawn_material(~us));

        if (fortressResult)
        {
            const Bitboard ourPieces = pos.pieces(us);
            const Bitboard enemyMobility =
              pos.attacks_by<KNIGHT>(~us) | pos.attacks_by<BISHOP>(~us) | pos.attacks_by<ROOK>(~us);

            // Verifica che i pezzi avversari non abbiano accesso critico
            const int overlapping = popcount(enemyMobility & ourPieces);
            fortressResult        = overlapping < 3;

            // Controllo aggiuntivo: mobilità totale nemica
            if (fortressResult)
                fortressResult = popcount(enemyMobility) < 12;
        }

        fortressCached = true;
    }
    return fortressResult;
}

Value ShashinManager::static_value(
  const Eval::NNUE::Networks&   networks,
  Eval::NNUE::AccumulatorStack  accumulators,  // copiato per valore!
  Eval::NNUE::AccumulatorCaches refreshTable,
  Position&                     rootPos,
  Search::Stack*                ss,
  Value                         optimism) {

    MoveList<LEGAL> legalMoves(rootPos);

    if (legalMoves.size() == 0)
        return rootPos.checkers() ? VALUE_MATE : VALUE_DRAW;

    if (ss->ply >= MAX_PLY || rootPos.is_draw(ss->ply))
        return VALUE_DRAW;

    if (!rootPos.checkers())
        return Eval::evaluate(networks, rootPos, accumulators, refreshTable, optimism);

    Value bestValue = -VALUE_INFINITE;

    for (auto move : legalMoves)
    {
        StateInfo  st;
        DirtyPiece dp = rootPos.do_move(move, st, rootPos.gives_check(move), nullptr);
        accumulators.push(dp);
        Value val = -static_value(networks, accumulators, refreshTable, rootPos, ss + 1, -optimism);
        rootPos.undo_move(move);
        accumulators.pop();
        bestValue = std::max(bestValue, val);
    }

    return bestValue;
}


void ShashinManager::setStaticState(const Position& rootPos) {
    StaticState& staticState = state.staticState;
    invalidateCaches();  // Invalidate caches when position changes

    staticState.legalMoveCount = MoveList<LEGAL>(rootPos).size();
    staticState.isSacrificial  = isSacrificialPosition(rootPos);
    staticState.kingExposed    = isKingExposed();

    int nonPawnMaterial      = rootPos.non_pawn_material(WHITE) + rootPos.non_pawn_material(BLACK);
    staticState.highMaterial = (nonPawnMaterial > 2400);

    staticState.kingDanger         = rootPos.king_danger(WHITE) || rootPos.king_danger(BLACK);
    staticState.pawnsNearPromotion = isPawnNearPromotion(rootPos);
    staticState.allPiecesCount     = rootPos.count<ALL_PIECES>();
    staticState.kingSafetyScore    = rootPos.king_safety_score(rootPos.side_to_move());

    MoveConfig::isFortress = isFortress(rootPos);
}

bool ShashinManager::isSacrificialPosition(const Position& rootPos) {
    if (!sacrificialCacheValid)
    {
        sacrificialResult = false;
        MoveList<LEGAL> legalMoves(rootPos);
        for (const auto& move : legalMoves)
        {
            if (rootPos.is_sacrifice(move))
            {
                sacrificialResult = true;
                break;
            }
        }
        sacrificialCacheValid = true;
    }
    return sacrificialResult;
}

bool ShashinManager::isPawnNearPromotion(const Position& rootPos) {
    if (pawnNearPromoCacheValid)
        return pawnNearPromoResult;

    // Unroll the loop for better pipelining
    pawnNearPromoResult = rootPos.pieces(WHITE, PAWN) & (Rank5BB | Rank6BB | Rank7BB)
                       || rootPos.pieces(BLACK, PAWN) & (Rank2BB | Rank3BB | Rank4BB);

    pawnNearPromoCacheValid = true;
    return pawnNearPromoResult;
}

// Constexpr lookup table initialization
static constexpr std::array<ShashinPosition, 101> initializeLookupTable() {
    std::array<ShashinPosition, 101> lookup = {};  // Zero-initialize

    // Compile-time pattern matching
    for (int i = 0; i <= 100; ++i)
    {
        if (i <= HIGH_PETROSIAN_MAX)
            lookup[i] = ShashinPosition::HIGH_PETROSIAN;
        else if (i <= MIDDLE_HIGH_PETROSIAN_MAX)
            lookup[i] = ShashinPosition::MIDDLE_HIGH_PETROSIAN;
        // ... altri range ...
        else
            lookup[i] = ShashinPosition::TAL_CAPABLANCA_PETROSIAN;
    }
    return lookup;
}

static constexpr auto lookup = initializeLookupTable();

ShashinPosition ShashinManager::getShashinRange(Value value, const Position& rootPos) {
    WDLModel::WDL wdl            = WDLModel::get_wdl(value, rootPos);
    int           winProbability = wdl.win + (wdl.draw / 2);

    // Special case for Win Probability = 50
    if (winProbability == CAPABLANCA_MAX)
    {
        return (wdl.win == 0 && wdl.loss == 0) ? ShashinPosition::CAPABLANCA
                                               : ShashinPosition::TAL_CAPABLANCA_PETROSIAN;
    }

    // Use the lookup table for valid indices
    if (winProbability >= 0 && winProbability <= 100)
    {
        return lookup[winProbability];
    }

    return ShashinPosition::TAL_CAPABLANCA_PETROSIAN;
}

ShashinPosition ShashinManager::getInitialShashinRange(Position&            rootPos,
                                                       Value                staticValue,
                                                       const ShashinConfig& config) {
    // If all configs are disabled, use standard evaluation
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
        return config.middlePetrosian ? ShashinPosition::MIDDLE_HIGH_PETROSIAN
                                      : ShashinPosition::HIGH_PETROSIAN;
    }
    if (config.middlePetrosian)
    {
        return config.lowPetrosian ? ShashinPosition::MIDDLE_LOW_PETROSIAN
                                   : ShashinPosition::MIDDLE_PETROSIAN;
    }
    if (config.lowPetrosian)
    {
        return config.capablanca ? ShashinPosition::CAPABLANCA_PETROSIAN
                                 : ShashinPosition::LOW_PETROSIAN;
    }

    // Capablanca logic
    if (config.capablanca)
    {
        return ShashinPosition::CAPABLANCA;
    }

    // Tal logic
    if (config.highTal)
    {
        return config.middleTal ? ShashinPosition::MIDDLE_HIGH_TAL : ShashinPosition::HIGH_TAL;
    }
    if (config.middleTal)
    {
        return config.lowTal ? ShashinPosition::MIDDLE_LOW_TAL : ShashinPosition::MIDDLE_TAL;
    }
    if (config.lowTal)
    {
        return config.capablanca ? ShashinPosition::CAPABLANCA_TAL : ShashinPosition::LOW_TAL;
    }

    return getShashinRange(staticValue, rootPos);
}

}  // namespace ShashChess