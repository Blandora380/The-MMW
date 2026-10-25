#pragma once
#include <unordered_map>
#include <random>
#include "Score.h"
#include "Math.h"
#include "EffectView.h"

namespace MikuMikuWorld
{
	struct Score;
	struct Note;
	struct ScoreContext;
}

namespace MikuMikuWorld::Engine
{
	struct DrawingNote
	{
		int refID;
		Range visualTime;
	};

	struct DrawingLine
	{
		Range xPos;
		Range visualTime;
		float visibleDuration;
	};

	struct DrawingHoldTick
	{
		int refID;
		float center;
		Range visualTime;
		float visibleDuration;
	};

	struct DrawingHoldSegment
	{
		int endID;
		EaseType ease;
		bool isGuide;
		ptrdiff_t tailStepIndex;
		double headTime, tailTime;
		float headLeft, headRight;
		float tailLeft, tailRight;
		float startTime, endTime;
		double activeTime;
		float visibleDuration;
	};

	struct DrawData
	{
		float noteSpeed;
		int maxTicks;
		std::vector<DrawingNote> drawingNotes;
		std::vector<DrawingLine> drawingLines;
		std::vector<DrawingHoldTick> drawingHoldTicks;
		std::vector<DrawingHoldSegment> drawingHoldSegments;
		std::vector<float> comboTimes;
		std::vector<int> hudScores;
		std::vector<int> hudScoreGains;
		std::vector<char> hudRanks;
		std::vector<float> hudScoreBars;
		std::vector<float> hudJudgeTimes;
                std::vector<int> hudComboValues;
                std::vector<int> hudJudgeKinds;

                struct PlaytestNote
                {
                        double time;
                        int lane;
                        int width;
                        float weight;
                        bool judged;
                        int id;
                        bool flick{ false };
                        bool trace{ false };
                        int holdIndex{ -1 };
                };
                std::vector<PlaytestNote> playtestNotes;

                struct PlaytestHold
                {
                        double startTime{};
                        double endTime{};
                        bool hasEnd{ true };
                        float endWeight{};
                        int headId{};
                        int endId{};
                        std::vector<std::pair<double, float>> ticks;
                        size_t nextTick{ 0 };
                        int pointer{ -1 };
                        bool active{ false };
                        bool failed{ false };
                        bool done{ false };
                };
                std::vector<PlaytestHold> playtestHolds;
                bool playtestActive{ false };
                std::unordered_map<int, double> playtestHitTime;
                std::vector<std::pair<int, double>> playtestHits;
                int playtestCombo{ 0 };
                float playtestComboFactor{ 1.0f };
                float playtestScore{ 0.0f };
                int playtestPrevScore{ 0 };
                float playtestWeightedCount{ 1.0f };
                double playtestLastEventTime{ 0.0 };
		Effect::EffectView effectView;

		void clear();
		void calculateDrawData(Score const& score);
                void beginPlaytest(Score const& score, double startTime);
                void playtestTouch(int pointerId, int lane, double touchTime);
                void playtestFlick(int lane, int anchorLane, double time);
                void playtestRelease(int pointerId, double time);
                void playtestTrace(unsigned int heldMask, const double* laneMoveAt, double now);
                void playtestAdvance(double now);
                const char* judgeNameAt(size_t index) const;
	};
}
