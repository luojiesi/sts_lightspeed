//
// Data collection for ML training data from MCTS playouts
//

#ifndef STS_LIGHTSPEED_DATACOLLECTOR_H
#define STS_LIGHTSPEED_DATACOLLECTOR_H

#include <fstream>
#include <string>
#include <vector>
#include <cstdint>

namespace sts {
    class BattleContext;
    class GameContext;
    enum class GameOutcome;
}

namespace sts::search {

    class BattleScumSearcher2;

    struct DataCollector {
        std::ofstream battleFile;
        std::ofstream strategicFile;
        bool headerWritten_battle = false;
        bool headerWritten_strategic = false;

        int battleRecordCount = 0;
        int strategicRecordCount = 0;

        // current game info
        std::uint64_t gameSeed = 0;
        int gameAscension = 0;

        explicit DataCollector(const std::string &outputDir, int threadId);
        ~DataCollector();

        void startGame(std::uint64_t seed, int ascension);
        void logBattleDecision(const BattleContext &bc, const BattleScumSearcher2 &searcher);
        void logStrategicDecision(const GameContext &gc, const std::string &decisionType,
                                  int chosenIdx, const std::vector<std::pair<int, float>> &optionWeights);
        void endGame(GameOutcome outcome, int finalFloor, int finalHp);
        void flush();

    private:
        void writeBattleHeader();
        void writeStrategicHeader();
    };

}

#endif //STS_LIGHTSPEED_DATACOLLECTOR_H
