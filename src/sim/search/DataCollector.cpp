//
// Data collection for ML training data from MCTS playouts
//

#include "sim/search/DataCollector.h"
#include "sim/search/BattleScumSearcher2.h"
#include "combat/BattleContext.h"
#include "game/GameContext.h"

#include <sstream>
#include <filesystem>
#include <cassert>

using namespace sts;
using namespace sts::search;

// Top 30 most common PlayerStatus values for the fixed-width encoding
static const PlayerStatus TRACKED_PLAYER_STATUSES[] = {
    PS::VULNERABLE, PS::WEAK, PS::FRAIL, PS::STRENGTH, PS::DEXTERITY,
    PS::ARTIFACT, PS::METALLICIZE, PS::PLATED_ARMOR, PS::THORNS, PS::BARRICADE,
    PS::DEMON_FORM, PS::FEEL_NO_PAIN, PS::DARK_EMBRACE, PS::CORRUPTION, PS::EVOLVE,
    PS::FIRE_BREATHING, PS::COMBUST, PS::RAGE, PS::BRUTALITY, PS::JUGGERNAUT,
    PS::FLAME_BARRIER, PS::REGEN, PS::VIGOR, PS::RITUAL, PS::DOUBLE_TAP,
    PS::INTANGIBLE, PS::BUFFER, PS::DRAW_CARD_NEXT_TURN, PS::ENERGIZED, PS::NOXIOUS_FUMES,
};
static constexpr int NUM_TRACKED_PLAYER_STATUSES = 30;
static constexpr int MAX_HAND_SLOTS = 10;
static constexpr int MAX_ENEMIES = 5;
static constexpr int MAX_EDGES = 200;

DataCollector::DataCollector(const std::string &outputDir, int threadId) {
    std::filesystem::create_directories(outputDir);

    std::string battlePath = outputDir + "/battle_data_t" + std::to_string(threadId) + ".csv";
    std::string strategicPath = outputDir + "/strategic_data_t" + std::to_string(threadId) + ".csv";

    // Check if files exist and have content (resume mode)
    bool battleExists = std::filesystem::exists(battlePath) && std::filesystem::file_size(battlePath) > 0;
    bool strategicExists = std::filesystem::exists(strategicPath) && std::filesystem::file_size(strategicPath) > 0;

    battleFile.open(battlePath, std::ios::out | std::ios::app);
    strategicFile.open(strategicPath, std::ios::out | std::ios::app);

    // If appending to existing files, headers are already there
    if (battleExists) headerWritten_battle = true;
    if (strategicExists) headerWritten_strategic = true;

    assert(battleFile.is_open());
    assert(strategicFile.is_open());
}

DataCollector::~DataCollector() {
    flush();
}

void DataCollector::startGame(std::uint64_t seed, int ascension) {
    gameSeed = seed;
    gameAscension = ascension;
}

void DataCollector::writeBattleHeader() {
    // Build CSV header
    std::ostringstream h;
    h << "seed,ascension";

    // Player core (6)
    h << ",p_cur_hp,p_max_hp,p_energy,p_block,p_strength,p_dexterity";

    // Player tracked statuses (30)
    for (int i = 0; i < NUM_TRACKED_PLAYER_STATUSES; ++i) {
        h << ",p_status_" << i;
    }

    // Hand cards: 10 slots x 3 values (30)
    for (int i = 0; i < MAX_HAND_SLOTS; ++i) {
        h << ",hand" << i << "_id,hand" << i << "_upg,hand" << i << "_cost";
    }

    // Enemies: 5 x 8 values (40)
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        h << ",e" << i << "_hp,e" << i << "_maxhp,e" << i << "_block"
          << ",e" << i << "_poison,e" << i << "_weak,e" << i << "_vuln"
          << ",e" << i << "_str,e" << i << "_move";
    }

    // Pile sizes (3)
    h << ",draw_pile_size,discard_pile_size,exhaust_pile_size";

    // Turn, floor, act (3)
    h << ",turn,floor_num,act";

    // MCTS results: num_edges, then variable-length edges, then chosen action
    h << ",num_edges,edge_data,chosen_action,select_card_info";

    h << "\n";
    battleFile << h.str();
    headerWritten_battle = true;
}

void DataCollector::writeStrategicHeader() {
    std::ostringstream h;
    h << "seed,ascension,floor_num,act,cur_hp,max_hp,gold,deck_size"
      << ",decision_type,num_options,chosen_idx,options_data,deck_data,relic_data"
      << "\n";
    strategicFile << h.str();
    headerWritten_strategic = true;
}

void DataCollector::logBattleDecision(const BattleContext &bc, const BattleScumSearcher2 &searcher) {
    if (!headerWritten_battle) {
        writeBattleHeader();
    }

    std::ostringstream row;
    row << gameSeed << "," << gameAscension;

    // Player core
    row << "," << bc.player.curHp
        << "," << bc.player.maxHp
        << "," << bc.player.energy
        << "," << bc.player.block
        << "," << bc.player.strength
        << "," << bc.player.dexterity;

    // Player tracked statuses — use direct map lookup to avoid hasStatusRuntime/getStatusRuntime
    // which can trigger map::at on inconsistent state
    for (int i = 0; i < NUM_TRACKED_PLAYER_STATUSES; ++i) {
        PlayerStatus s = TRACKED_PLAYER_STATUSES[i];
        int val = 0;
        switch (s) {
            case PS::STRENGTH: val = bc.player.strength; break;
            case PS::DEXTERITY: val = bc.player.dexterity; break;
            case PS::ARTIFACT: val = bc.player.artifact; break;
            default: {
                auto it = bc.player.statusMap.find(s);
                if (it != bc.player.statusMap.end()) {
                    val = it->second;
                }
                break;
            }
        }
        row << "," << val;
    }

    // Hand cards (10 slots, pad with -1)
    for (int i = 0; i < MAX_HAND_SLOTS; ++i) {
        if (i < bc.cards.cardsInHand) {
            const auto &c = bc.cards.hand[i];
            row << "," << static_cast<int>(c.id)
                << "," << (c.upgraded ? 1 : 0)
                << "," << static_cast<int>(c.costForTurn);
        } else {
            row << ",-1,-1,-1";
        }
    }

    // Enemies (5 slots, pad with 0)
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        if (i < bc.monsters.monsterCount) {
            const auto &m = bc.monsters.arr[i];
            row << "," << m.curHp
                << "," << m.maxHp
                << "," << m.block
                << "," << static_cast<int>(m.poison)
                << "," << m.weak
                << "," << m.vulnerable
                << "," << m.strength
                << "," << static_cast<int>(m.moveHistory[0]);
        } else {
            row << ",0,0,0,0,0,0,0,0";
        }
    }

    // Pile sizes
    row << "," << bc.cards.drawPile.size()
        << "," << bc.cards.discardPile.size()
        << "," << bc.cards.exhaustPile.size();

    // Turn, floor, act (derive act from floor number)
    int act = bc.floorNum <= 17 ? 1 : (bc.floorNum <= 34 ? 2 : 3);
    row << "," << bc.turn
        << "," << bc.floorNum
        << "," << act;

    // MCTS edges: encode as semicolon-separated within quotes
    const auto &rootNode = searcher.root;
    int numEdges = static_cast<int>(rootNode.edges.size());
    row << "," << numEdges;

    // Edge data: "action_bits:visit_count:eval_sum;..."
    row << ",\"";
    for (int i = 0; i < numEdges; ++i) {
        const auto &edge = rootNode.edges[i];
        if (i > 0) row << ";";
        row << edge.action.bits
            << ":" << edge.node.simulationCount
            << ":" << edge.node.evaluationSum;
    }
    row << "\"";

    // Chosen action: the edge with highest visit count
    std::uint32_t chosenBits = 0;
    std::int64_t maxVisits = -1;
    for (const auto &edge : rootNode.edges) {
        if (edge.node.simulationCount > maxVisits) {
            maxVisits = edge.node.simulationCount;
            chosenBits = edge.action.bits;
        }
    }
    row << "," << chosenBits;

    // For SINGLE_CARD_SELECT: record the selected card ID from the correct source
    int chosenActionType = (chosenBits >> 29) & 0xF;
    if (chosenActionType == 2 && bc.inputState == InputState::CARD_SELECT) {
        int selectIdx = chosenBits & 0xFFFF;
        int cardId = -1;
        int cardUpg = 0;
        switch (bc.cardSelectInfo.cardSelectTask) {
            case CardSelectTask::ARMAMENTS:
            case CardSelectTask::DUAL_WIELD:
            case CardSelectTask::EXHAUST_ONE:
            case CardSelectTask::FORETHOUGHT:
            case CardSelectTask::WARCRY:
            case CardSelectTask::SETUP:
            case CardSelectTask::NIGHTMARE:
            case CardSelectTask::RECYCLE:
                // From hand
                if (selectIdx < bc.cards.cardsInHand) {
                    cardId = static_cast<int>(bc.cards.hand[selectIdx].id);
                    cardUpg = bc.cards.hand[selectIdx].upgraded ? 1 : 0;
                }
                break;
            case CardSelectTask::HEADBUTT:
            case CardSelectTask::LIQUID_MEMORIES_POTION:
                // From discard pile
                if (selectIdx < static_cast<int>(bc.cards.discardPile.size())) {
                    cardId = static_cast<int>(bc.cards.discardPile[selectIdx].id);
                    cardUpg = bc.cards.discardPile[selectIdx].upgraded ? 1 : 0;
                }
                break;
            case CardSelectTask::EXHUME:
                // From exhaust pile
                if (selectIdx < static_cast<int>(bc.cards.exhaustPile.size())) {
                    cardId = static_cast<int>(bc.cards.exhaustPile[selectIdx].id);
                    cardUpg = bc.cards.exhaustPile[selectIdx].upgraded ? 1 : 0;
                }
                break;
            case CardSelectTask::SECRET_TECHNIQUE:
            case CardSelectTask::SECRET_WEAPON:
            case CardSelectTask::SEEK:
                // From draw pile
                if (selectIdx < static_cast<int>(bc.cards.drawPile.size())) {
                    cardId = static_cast<int>(bc.cards.drawPile[selectIdx].id);
                    cardUpg = bc.cards.drawPile[selectIdx].upgraded ? 1 : 0;
                }
                break;
            case CardSelectTask::CODEX:
            case CardSelectTask::DISCOVERY:
                // From cardSelectInfo.cards
                if (selectIdx < 3) {
                    cardId = static_cast<int>(bc.cardSelectInfo.cards[selectIdx]);
                }
                break;
            default:
                break;
        }
        row << "," << cardId << ":" << cardUpg;
    } else {
        row << ",";
    }

    row << "\n";
    battleFile << row.str();
    ++battleRecordCount;
}

void DataCollector::logStrategicDecision(const GameContext &gc, const std::string &decisionType,
                                          int chosenIdx,
                                          const std::vector<std::pair<int, float>> &optionWeights) {
    if (!headerWritten_strategic) {
        writeStrategicHeader();
    }

    std::ostringstream row;
    row << gameSeed << "," << gameAscension
        << "," << gc.floorNum
        << "," << gc.act
        << "," << gc.curHp
        << "," << gc.maxHp
        << "," << gc.gold
        << "," << gc.deck.size();

    row << "," << decisionType;

    // Options data
    row << "," << optionWeights.size()
        << "," << chosenIdx;

    // Encode options as "idx:weight;idx:weight;..."
    row << ",\"";
    for (size_t i = 0; i < optionWeights.size(); ++i) {
        if (i > 0) row << ";";
        row << optionWeights[i].first << ":" << optionWeights[i].second;
    }
    row << "\"";

    // Deck data: "cardId:upgraded;cardId:upgraded;..."
    row << ",\"";
    for (int i = 0; i < gc.deck.cards.size(); ++i) {
        if (i > 0) row << ";";
        row << static_cast<int>(gc.deck.cards[i].id) << ":" << (gc.deck.cards[i].upgraded ? 1 : 0);
    }
    row << "\"";

    // Relic data: "relicId;relicId;..."
    row << ",\"";
    for (size_t i = 0; i < gc.relics.relics.size(); ++i) {
        if (i > 0) row << ";";
        row << static_cast<int>(gc.relics.relics[i].id);
    }
    row << "\"";

    row << "\n";
    strategicFile << row.str();
    ++strategicRecordCount;
}

void DataCollector::endGame(GameOutcome outcome, int finalFloor, int finalHp) {
    // Log a strategic decision record for game end
    std::ostringstream row;
    if (!headerWritten_strategic) {
        writeStrategicHeader();
    }
    row << gameSeed << "," << gameAscension
        << "," << finalFloor
        << ",0"  // act
        << "," << finalHp
        << ",0"  // maxHp
        << ",0"  // gold
        << ",0"  // deck_size
        << ",GAME_END"
        << ",0"  // num_options
        << "," << static_cast<int>(outcome) // chosenIdx repurposed as outcome
        << ",\"\""   // options_data
        << ",\"\""   // deck_data
        << ",\"\""   // relic_data
        << "\n";
    strategicFile << row.str();

    flush();
}

void DataCollector::flush() {
    if (battleFile.is_open()) battleFile.flush();
    if (strategicFile.is_open()) strategicFile.flush();
}
