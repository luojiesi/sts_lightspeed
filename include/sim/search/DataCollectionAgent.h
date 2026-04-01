//
// Agent for collecting ML training data via per-step MCTS search
//

#ifndef STS_LIGHTSPEED_DATACOLLECTIONAGENT_H
#define STS_LIGHTSPEED_DATACOLLECTIONAGENT_H

#include "game/GameContext.h"
#include "sim/search/Action.h"
#include "sim/search/GameAction.h"
#include "sim/search/DataCollector.h"

#include <random>

namespace sts::search {

    class BattleScumSearcher2;

    struct DataCollectionAgent {
        DataCollector *collector;

        int simulationCountBase = 500;
        double bossSimulationMultiplier = 3;
        std::int64_t simulationCountTotal = 0;

        std::default_random_engine rng;

        // public interface
        void playout(GameContext &gc);

        // private methods
        void playoutBattle(BattleContext &bc);

        static void takeAction(GameContext &gc, GameAction a);
        static void takeAction(BattleContext &bc, Action a);

        void stepOutOfCombatPolicy(GameContext &gc);
        void stepEventPolicy(GameContext &gc, Event eventId, std::uint32_t validBits);
        void cardSelectPolicy(GameContext &gc);
        void stepRewardsPolicy(GameContext &gc);
        void weightedCardRewardPolicy(GameContext &gc);
    };

}

#endif //STS_LIGHTSPEED_DATACOLLECTIONAGENT_H
