//
// Agent for collecting ML training data via per-step MCTS search.
// Delegates policy logic (which card to pick, which event option, etc.)
// using the same heuristics as ScumSearchAgent2, but searches independently
// at every battle step and logs all decisions to DataCollector.
//

#include "sim/search/DataCollectionAgent.h"
#include "sim/search/BattleScumSearcher2.h"
#include "sim/search/ExpertKnowledge.h"
#include "game/Game.h"
#include "game/Neow.h"

using namespace sts;

void search::DataCollectionAgent::takeAction(GameContext &gc, search::GameAction a) {
    a.execute(gc);
}

void search::DataCollectionAgent::takeAction(BattleContext &bc, search::Action a) {
    a.execute(bc);
}

void search::DataCollectionAgent::playout(GameContext &gc) {
    collector->startGame(gc.seed, gc.ascension);

    BattleContext bc;
    while (gc.outcome == GameOutcome::UNDECIDED) {
        if (gc.screenState == ScreenState::BATTLE) {
            bc = BattleContext();
            bc.init(gc);
            playoutBattle(bc);
            bc.exitBattle(gc);
            continue;
        }
        stepOutOfCombatPolicy(gc);
    }

    collector->endGame(gc.outcome, gc.floorNum, gc.curHp);
}

void search::DataCollectionAgent::playoutBattle(BattleContext &bc) {
    while (bc.outcome == Outcome::UNDECIDED) {
        const std::int64_t simulationCount = isBossEncounter(bc.encounter) ?
            static_cast<std::int64_t>(bossSimulationMultiplier * simulationCountBase) : simulationCountBase;

        search::BattleScumSearcher2 searcher(bc);
        searcher.search(simulationCount);
        collector->logBattleDecision(bc, searcher);
        simulationCountTotal += searcher.root.simulationCount;

        // Execute the single best action (most visited edge)
        std::int64_t maxVisits = -1;
        const search::BattleScumSearcher2::Edge *bestEdge = nullptr;
        for (const auto &edge : searcher.root.edges) {
            if (edge.node.simulationCount > maxVisits) {
                maxVisits = edge.node.simulationCount;
                bestEdge = &edge;
            }
        }
        if (bestEdge == nullptr) break;
        takeAction(bc, bestEdge->action);
    }
}

// ---- Out-of-combat policies (same heuristics as ScumSearchAgent2, with logging) ----

void search::DataCollectionAgent::stepOutOfCombatPolicy(GameContext &gc) {
    switch (gc.screenState) {
        case ScreenState::EVENT_SCREEN: {
            Event eventId = gc.curEvent;
            auto validBits = search::GameAction::getValidEventSelectBits(gc);
            stepEventPolicy(gc, eventId, validBits);
            break;
        }

        case ScreenState::REWARDS:
            stepRewardsPolicy(gc);
            break;

        case ScreenState::TREASURE_ROOM: {
            bool takeChest = true;
            if (gc.relics.has(RelicId::CURSED_KEY)) {
                takeChest = gc.info.chestSize == ChestSize::LARGE;
            }
            collector->logStrategicDecision(gc, "TREASURE", takeChest ? 0 : 1,
                {{0, 1.0f}, {1, 0.0f}});
            takeAction(gc, takeChest);
            break;
        }

        case ScreenState::SHOP_ROOM: {
            bool purchased = false;
            int purchasedIdx = -1;
            for (int i = 0; i < 3; ++i) {
                if (gc.info.shop.relicPrice(i) != -1 && gc.gold >= gc.info.shop.relicPrice(i)) {
                    purchasedIdx = i;
                    purchased = true;
                    break;
                }
            }
            std::vector<std::pair<int,float>> shopOptions;
            for (int i = 0; i < 3; ++i) {
                int price = gc.info.shop.relicPrice(i);
                if (price != -1) {
                    shopOptions.push_back({i, static_cast<float>(price)});
                }
            }
            shopOptions.push_back({-1, 0.0f}); // skip
            collector->logStrategicDecision(gc, "SHOP",
                purchased ? purchasedIdx : static_cast<int>(shopOptions.size()) - 1, shopOptions);
            if (purchased) {
                takeAction(gc, GameAction(GameAction::RewardsActionType::RELIC, purchasedIdx));
            } else {
                // Random fallback
                std::vector<search::GameAction> possibleActions(search::GameAction::getAllActionsInState(gc));
                std::uniform_int_distribution<int> distr(0, static_cast<int>(possibleActions.size()) - 1);
                takeAction(gc, possibleActions[distr(rng)]);
            }
            break;
        }

        case ScreenState::CARD_SELECT: {
            // Replicate cardSelectPolicy logic to get chosenIdx before executing
            fixed_list<std::pair<int,int>, Deck::MAX_SIZE> selectOrder;
            for (int i = 0; i < gc.info.toSelectCards.size(); ++i) {
                const auto &c = gc.info.toSelectCards[i].card;
                auto obtainWeight = search::Expert::getObtainWeight(c.getId());
                auto playOrder = search::Expert::getPlayOrdering(c.getId());
                switch (gc.info.selectScreenType) {
                    case CardSelectScreenType::TRANSFORM:
                    case CardSelectScreenType::TRANSFORM_UPGRADE:
                        selectOrder.push_back({i, c.getType() == CardType::CURSE ? playOrder : obtainWeight});
                        break;
                    case CardSelectScreenType::BONFIRE_SPIRITS:
                    case CardSelectScreenType::REMOVE:
                    case CardSelectScreenType::UPGRADE:
                    case CardSelectScreenType::DUPLICATE:
                    case CardSelectScreenType::OBTAIN:
                    case CardSelectScreenType::BOTTLE:
                        selectOrder.push_back({i, -obtainWeight});
                        break;
                    default:
                        selectOrder.push_back({i, 0});
                        break;
                }
            }
            std::sort(selectOrder.begin(), selectOrder.end(), [](auto a, auto b) { return a.second < b.second; });
            int chosenIdx = selectOrder.front().first;

            std::vector<std::pair<int,float>> selectOptions;
            for (int i = 0; i < gc.info.toSelectCards.size(); ++i) {
                const auto &c = gc.info.toSelectCards[i].card;
                int encoded = (static_cast<int>(c.getId()) << 1) | (c.isUpgraded() ? 1 : 0);
                selectOptions.push_back({encoded, static_cast<float>(i)});
            }
            std::string dtype = "CARD_SELECT:" + std::to_string(static_cast<int>(gc.info.selectScreenType));
            collector->logStrategicDecision(gc, dtype, chosenIdx, selectOptions);

            takeAction(gc, chosenIdx);
            break;
        }

        case ScreenState::BOSS_RELIC_REWARDS: {
            int best = 10000;
            int bestIdx = 0;
            std::vector<std::pair<int,float>> bossRelicOptions;
            for (int i = 0; i < 3; ++i) {
                int value = search::Expert::getBossRelicOrdering(gc.info.bossRelics[i]);
                bossRelicOptions.push_back({static_cast<int>(gc.info.bossRelics[i]),
                                            static_cast<float>(10000 - value)});
                if (value < best) {
                    best = value;
                    bestIdx = i;
                }
            }
            collector->logStrategicDecision(gc, "BOSS_RELIC", bestIdx, bossRelicOptions);
            takeAction(gc, bestIdx);
            break;
        }

        case ScreenState::REST_ROOM: {
            int restChoice = -1;
            if (gc.curHp > 50 && gc.deck.getUpgradeableCount() > 0 && !gc.hasRelic(RelicId::FUSION_HAMMER)) {
                restChoice = 1; // upgrade
            } else if (gc.curHp < 15 && !gc.relics.has(RelicId::COFFEE_DRIPPER)) {
                restChoice = 0; // rest
            }
            if (restChoice < 0) {
                // Random fallback — pick from valid actions using action value, not index
                std::vector<search::GameAction> possibleActions(search::GameAction::getAllActionsInState(gc));
                std::uniform_int_distribution<int> distr(0, static_cast<int>(possibleActions.size()) - 1);
                restChoice = possibleActions[distr(rng)].bits;
            }
            collector->logStrategicDecision(gc, "REST", restChoice,
                {{0, 1.0f}, {1, 1.0f}});
            takeAction(gc, restChoice);
            break;
        }

        case ScreenState::MAP_SCREEN: {
            std::vector<search::GameAction> possibleActions(search::GameAction::getAllActionsInState(gc));
            std::uniform_int_distribution<int> distr(0, static_cast<int>(possibleActions.size()) - 1);
            const int randomChoice = distr(rng);
            std::vector<std::pair<int,float>> pathOptions;
            for (int i = 0; i < static_cast<int>(possibleActions.size()); ++i) {
                pathOptions.push_back({i, 1.0f});
            }
            collector->logStrategicDecision(gc, "PATH", randomChoice, pathOptions);
            takeAction(gc, possibleActions[randomChoice]);
            break;
        }

        case ScreenState::BATTLE:
        case ScreenState::INVALID:
            assert(false);
            break;

        default: {
            // Unknown screen — random action, no logging
            std::vector<search::GameAction> possibleActions(search::GameAction::getAllActionsInState(gc));
            std::uniform_int_distribution<int> distr(0, static_cast<int>(possibleActions.size()) - 1);
            takeAction(gc, possibleActions[distr(rng)]);
            break;
        }
    }
}

void search::DataCollectionAgent::stepEventPolicy(GameContext &gc, Event eventId, std::uint32_t validBits) {
    // Build option list
    std::vector<std::pair<int,float>> eventOptions;
    if (eventId == Event::NEOW) {
        for (int i = 0; i < 4; ++i) {
            int encoded = (static_cast<int>(gc.info.neowRewards[i].r) << 8)
                        | static_cast<int>(gc.info.neowRewards[i].d);
            eventOptions.push_back({encoded, (validBits & (1 << i)) ? 1.0f : 0.0f});
        }
    } else {
        for (int i = 0; i < 16; ++i) {
            if (validBits & (1 << i)) {
                eventOptions.push_back({i, 1.0f});
            }
        }
    }

    int choice = -1;
    switch (gc.curEvent) {
        case Event::NEOW:
            if (gc.info.neowRewards[1].d == Neow::Drawback::CURSE || gc.info.neowRewards[2].d == Neow::Drawback::CURSE) {
                choice = 0;
            } else {
                std::vector<search::GameAction> possibleActions(search::GameAction::getAllActionsInState(gc));
                std::uniform_int_distribution<int> distr(0, static_cast<int>(possibleActions.size()) - 1);
                choice = distr(rng);
                // Log before executing (Neow uses possibleActions index)
                std::string decisionType = "EVENT:" + std::to_string(static_cast<int>(eventId));
                collector->logStrategicDecision(gc, decisionType, choice, eventOptions);
                takeAction(gc, possibleActions[choice]);
                return;
            }
            break;

        case Event::NOTE_FOR_YOURSELF:
        case Event::THE_DIVINE_FOUNTAIN:
            choice = 0;
            break;

        case Event::BIG_FISH:
            choice = 1;
            break;

        case Event::GOLDEN_IDOL:
            choice = gc.hasRelic(RelicId::GOLDEN_IDOL) ? 4 : 0;
            break;

        case Event::GHOSTS:
        case Event::MASKED_BANDITS:
            choice = 0;
            break;

        case Event::CURSED_TOME:
            choice = (gc.info.eventData == 0) ? 0 : gc.info.eventData + 1;
            break;

        case Event::KNOWING_SKULL:
            choice = 3;
            break;

        default: {
            std::vector<search::GameAction> possibleActions(search::GameAction::getAllActionsInState(gc));
            std::uniform_int_distribution<int> distr(0, static_cast<int>(possibleActions.size()) - 1);
            choice = distr(rng);
            std::string decisionType = "EVENT:" + std::to_string(static_cast<int>(eventId));
            collector->logStrategicDecision(gc, decisionType, choice, eventOptions);
            takeAction(gc, possibleActions[choice]);
            return;
        }
    }

    std::string decisionType = "EVENT:" + std::to_string(static_cast<int>(eventId));
    collector->logStrategicDecision(gc, decisionType, choice, eventOptions);
    takeAction(gc, choice);
}

void search::DataCollectionAgent::cardSelectPolicy(GameContext &gc) {
    fixed_list<std::pair<int,int>, Deck::MAX_SIZE> selectOrder;

    for (int i = 0; i < gc.info.toSelectCards.size(); ++i) {
        const auto &c = gc.info.toSelectCards[i].card;
        auto playOrder = search::Expert::getPlayOrdering(c.getId());
        auto obtainWeight = search::Expert::getObtainWeight(c.getId());

        switch (gc.info.selectScreenType) {
            case CardSelectScreenType::TRANSFORM:
            case CardSelectScreenType::TRANSFORM_UPGRADE:
                selectOrder.push_back({i, c.getType() == CardType::CURSE ? playOrder : obtainWeight});
                break;
            case CardSelectScreenType::BONFIRE_SPIRITS:
            case CardSelectScreenType::REMOVE:
            case CardSelectScreenType::UPGRADE:
            case CardSelectScreenType::DUPLICATE:
            case CardSelectScreenType::OBTAIN:
            case CardSelectScreenType::BOTTLE:
                selectOrder.push_back({i, -obtainWeight});
                break;
            default:
                selectOrder.push_back({i, 0});
                break;
        }
    }
    std::sort(selectOrder.begin(), selectOrder.end(), [](auto a, auto b) { return a.second < b.second; });
    takeAction(gc, selectOrder.front().first);
}

void search::DataCollectionAgent::stepRewardsPolicy(GameContext &gc) {
    // Neow (floor 0) has no COMBAT_REWARD screen in real game — skip reward logging
    bool logRewards = gc.floorNum > 0;
    auto &r = gc.info.rewardsContainer;

    if (r.goldRewardCount > 0) {
        if (logRewards) collector->logStrategicDecision(gc, "REWARD_GOLD", 0,
            {{r.gold[0], 1.0f}});
        takeAction(gc, GameAction(GameAction::RewardsActionType::GOLD, 0));
    } else if (r.relicCount > 0) {
        if (logRewards) collector->logStrategicDecision(gc, "REWARD_RELIC", 0,
            {{static_cast<int>(r.relics[0]), 1.0f}});
        takeAction(gc, GameAction(GameAction::RewardsActionType::RELIC, 0));
    } else if (r.potionCount > 0) {
        if (logRewards) collector->logStrategicDecision(gc, "REWARD_POTION", 0,
            {{static_cast<int>(r.potions[0]), 1.0f}});
        takeAction(gc, GameAction(GameAction::RewardsActionType::POTION, 0));
    } else if (r.sapphireKey || r.emeraldKey) {
        int keyType = r.emeraldKey ? 1 : 2;  // 1=emerald, 2=sapphire
        if (logRewards) collector->logStrategicDecision(gc, "REWARD_KEY", 0,
            {{keyType, 1.0f}});
        takeAction(gc, GameAction(GameAction::RewardsActionType::KEY));
    } else if (r.cardRewardCount == 0) {
        if (logRewards) collector->logStrategicDecision(gc, "REWARD_SKIP", 0, {});
        takeAction(gc, GameAction(GameAction::RewardsActionType::SKIP));
    } else {
        weightedCardRewardPolicy(gc);
    }
}

static double getAvgDeckWeight(const GameContext &gc) {
    int sum = 0;
    for (const auto &c : gc.deck.cards) {
        sum += search::Expert::getObtainWeight(c.getId(), c.isUpgraded());
    }
    return static_cast<double>(sum) / gc.deck.size();
}

void search::DataCollectionAgent::weightedCardRewardPolicy(GameContext &gc) {
    auto &r = gc.info.rewardsContainer;
    for (int rIdx = r.cardRewardCount - 1; rIdx >= 0; --rIdx) {
        const auto deckWeight = getAvgDeckWeight(gc);

        fixed_list<std::pair<int,double>, 4> weights;
        double weightSum = 0;
        for (int cIdx = 0; cIdx < r.cardRewards[rIdx].size(); ++cIdx) {
            constexpr double act1AttackMultiplier = 1.4;
            const auto &c = r.cardRewards[rIdx][cIdx];
            double weight = std::pow(search::Expert::getObtainWeight(c.getId(), c.isUpgraded()), 1.2);
            if (gc.act == 1 && c.getType() == CardType::ATTACK) {
                weight *= act1AttackMultiplier;
            }
            weights.push_back({cIdx, weight});
            weightSum += weight;
        }

        // Weighted random selection
        int selection = 0;
        {
            std::uniform_real_distribution<double> distr(0, weightSum);
            double roll = distr(rng);
            double acc = 0;
            for (int i = 0; i < weights.size(); ++i) {
                acc += weights[i].second;
                if (roll <= acc) {
                    selection = weights[i].first;
                }
            }
        }

        // Skip vs take
        bool skipCard = true;
        {
            std::uniform_real_distribution<double> distr(0, weights[selection].second + deckWeight * 0.6);
            double roll = distr(rng);
            if (roll < weights[selection].second) {
                skipCard = false;
            }
        }

        // Log
        std::vector<std::pair<int,float>> cardOptions;
        for (int i = 0; i < weights.size(); ++i) {
            cardOptions.push_back({static_cast<int>(r.cardRewards[rIdx][weights[i].first].getId()),
                                   static_cast<float>(weights[i].second)});
        }
        cardOptions.push_back({-1, static_cast<float>(deckWeight * 0.6)}); // skip
        int chosenLogIdx = skipCard ? static_cast<int>(cardOptions.size()) - 1 : selection;
        collector->logStrategicDecision(gc, "CARD_REWARD", chosenLogIdx, cardOptions);

        if (skipCard) {
            takeAction(gc, GameAction(GameAction::RewardsActionType::CARD, rIdx, 5));
        } else {
            takeAction(gc, GameAction(GameAction::RewardsActionType::CARD, rIdx, weights[selection].first));
        }
    }
}
