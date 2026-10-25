#include <algorithm>
#include <cmath>
#include <queue>
#include <set>
#include <stdexcept>
#include <utility>
#include "PreviewData.h"
#include "ApplicationConfiguration.h"
#include "Constants.h"
#include "ResourceManager.h"
#include "ScoreContext.h"
#include "ScoreStats.h"

namespace MikuMikuWorld::Engine
{
	namespace
	{
		constexpr float SCORE_TEAM_POWER = 300000.0f;
		constexpr float SCORE_RATING = 26.0f;

		float clamp01(float value)
		{
			return std::max(0.0f, std::min(1.0f, value));
		}

		float lerpScore(float value, float start, float end, float startPos, float endPos)
		{
			if (end <= start)
				return endPos;

			return ((value - start) / (end - start)) * (endPos - startPos) + startPos;
		}

		std::pair<char, float> scoreRankAndBar(float score)
		{
			const float rankBorder = 1200000.0f + (SCORE_RATING - 5.0f) * 4100.0f;
			const float rankS = 1040000.0f + (SCORE_RATING - 5.0f) * 5200.0f;
			const float rankA = 840000.0f + (SCORE_RATING - 5.0f) * 4200.0f;
			const float rankB = 400000.0f + (SCORE_RATING - 5.0f) * 2000.0f;
			const float rankC = 20000.0f + (SCORE_RATING - 5.0f) * 100.0f;

			constexpr float rankBorderPos = 1650.0f / 1650.0f;
			constexpr float rankSPos = 1478.0f / 1650.0f;
			constexpr float rankAPos = 1234.0f / 1650.0f;
			constexpr float rankBPos = 990.0f / 1650.0f;
			constexpr float rankCPos = 746.0f / 1650.0f;

			if (score >= rankBorder)
				return { 's', rankBorderPos };
			if (score >= rankS)
				return { 's', clamp01(lerpScore(score, rankS, rankBorder, rankSPos, rankBorderPos)) };
			if (score >= rankA)
				return { 'a', clamp01(lerpScore(score, rankA, rankS, rankAPos, rankSPos)) };
			if (score >= rankB)
				return { 'b', clamp01(lerpScore(score, rankB, rankA, rankBPos, rankAPos)) };
			if (score >= rankC)
				return { 'c', clamp01(lerpScore(score, rankC, rankB, rankCPos, rankBPos)) };

			return { 'd', clamp01((score / std::max(rankC, 1.0f)) * rankCPos) };
		}
	}

	struct DrawingHoldStep
	{
		int tick;
		double time;
		float left;
		float right;
		EaseType ease;
	};

	static void addHoldNote(DrawData& drawData, const HoldNote& holdNote, Score const &score);

	void DrawData::calculateDrawData(Score const &score)
	{
		this->clear();
		try
		{
			this->noteSpeed = config.pvNoteSpeed;
			if (score.tempoChanges.empty())
				return;

			const std::vector<ComboEvent> comboEvents = calculateComboEvents(score);
			comboTimes.reserve(comboEvents.size());
			hudScores.reserve(comboEvents.size());
			hudScoreGains.reserve(comboEvents.size());
			hudRanks.reserve(comboEvents.size());
			hudScoreBars.reserve(comboEvents.size());
			hudJudgeTimes.reserve(comboEvents.size());

			float weightedCount = 0.0f;
			for (const ComboEvent& event : comboEvents)
				weightedCount += std::max(0.0f, event.weight);
			weightedCount = std::max(weightedCount, 1.0f);

			const float levelFactor = (SCORE_RATING - 5.0f) * 0.005f + 1.0f;
			int combo = 0;
			float comboFactor = 1.0f;
			float hudScore = 0.0f;
			int previousHudScore = 0;
			for (const ComboEvent& event : comboEvents)
			{
				const float timeSec = accumulateDuration(event.tick, TICKS_PER_BEAT, score.tempoChanges);
				const float weight = std::max(0.0f, event.weight);
				combo++;
				if (combo % 100 == 1 && combo > 1)
					comboFactor = std::min(comboFactor + 0.01f, 1.1f);

				hudScore += (SCORE_TEAM_POWER / weightedCount) * 4.0f * weight * levelFactor * comboFactor;
				const int currentHudScore = std::max(0, static_cast<int>(std::lround(hudScore)));
				const auto [rank, scoreBar] = scoreRankAndBar(hudScore);

				comboTimes.push_back(timeSec);
				hudScores.push_back(currentHudScore);
				hudScoreGains.push_back(currentHudScore - previousHudScore);
				previousHudScore = currentHudScore;
				hudRanks.push_back(rank);
				hudScoreBars.push_back(scoreBar);
				if (event.showJudge)
					hudJudgeTimes.push_back(timeSec);
			}

			std::map<int, Range> simBuilder;
			std::map<int, float> simDurationBuilder;
			std::map<int, float> simSpeedRatioBuilder;
			std::set<int> speedMismatchedSimTicks;
			for (auto rit = score.notes.rbegin(), rend = score.notes.rend(); rit != rend; rit++)
			{
				auto& [id, note] = *rit;
				maxTicks = std::max(note.tick, maxTicks);
				NoteType type = note.getType();
				if (type == NoteType::HoldMid
					|| (type == NoteType::Hold && score.holdNotes.at(id).startType != HoldNoteType::Normal)
					|| (type == NoteType::HoldEnd && score.holdNotes.at(note.parentID).endType != HoldNoteType::Normal))
					continue;
				if (type == NoteType::HoldMid)
					continue;
					
				auto visual_tm = getNoteVisualTime(note, score, noteSpeed);
				drawingNotes.push_back(DrawingNote{note.ID, visual_tm});

				// Find the max and min lane within the same height (visual_tm.max)
				float center = getNoteCenter(note);
				float speedRatio = getEffectiveSpeedRatio(note, score);
				float visibleDuration = getNoteVisibleDuration(note, score, noteSpeed);
				auto&& [it, has_emplaced] = simBuilder.try_emplace(note.tick, Range{center, center});
				auto&& [dit, has_dur] = simDurationBuilder.try_emplace(note.tick, visibleDuration);
				if (!has_dur)
					dit->second = std::max(dit->second, visibleDuration);
				auto&& [sit, has_speed] = simSpeedRatioBuilder.try_emplace(note.tick, speedRatio);
				if (!has_speed && std::abs(sit->second - speedRatio) > 0.0001f)
					speedMismatchedSimTicks.insert(note.tick);
				auto& x_range = it->second;
				if (has_emplaced)
					continue;
				if (center < x_range.min)
					x_range.min = center;
				if (center > x_range.max)
					x_range.max = center;
				continue;
			}

			for (const auto& [line_tick, x_range] : simBuilder)
			{
				if (x_range.min != x_range.max && speedMismatchedSimTicks.find(line_tick) == speedMismatchedSimTicks.end())
				{
					double targetTime = accumulateScaledDuration(line_tick, TICKS_PER_BEAT, score.tempoChanges, score.hiSpeedChanges);
					drawingLines.push_back(DrawingLine{ x_range, Range{ targetTime - simDurationBuilder.at(line_tick), targetTime }, simDurationBuilder.at(line_tick) });
				}
			}

			for (auto rit = score.holdNotes.rbegin(), rend = score.holdNotes.rend(); rit != rend; rit++)
			{
				addHoldNote(*this, rit->second, score);
			}
		}
		catch(const std::out_of_range& ex)
		{
			this->clear();
		}
	}

	static void pushPlaytestEvent(DrawData& d, double time, int kind, float weight)
        {
                constexpr float multipliers[] = { 1.0f, 0.9f, 0.8f, 0.7f, 0.0f };
                kind = std::clamp(kind, 0, 4);
                const float levelFactor = (SCORE_RATING - 5.0f) * 0.005f + 1.0f;

                if (kind >= 3)
                {
                        d.playtestCombo = 0;
                        d.playtestComboFactor = 1.0f;
                }
                else
                {
                        d.playtestCombo++;
                        if (d.playtestCombo % 100 == 1 && d.playtestCombo > 1)
                                d.playtestComboFactor = std::min(d.playtestComboFactor + 0.01f, 1.1f);
                }

                d.playtestScore += (SCORE_TEAM_POWER / d.playtestWeightedCount) * 4.0f * weight * multipliers[kind] * levelFactor * d.playtestComboFactor;
                const int currentScore = std::max(0, static_cast<int>(std::lround(d.playtestScore)));
                const auto [rank, scoreBar] = scoreRankAndBar(d.playtestScore);

                time = std::max(time, d.playtestLastEventTime);
                d.playtestLastEventTime = time;

                d.comboTimes.push_back(static_cast<float>(time));
                d.hudScores.push_back(currentScore);
                d.hudScoreGains.push_back(currentScore - d.playtestPrevScore);
                d.playtestPrevScore = currentScore;
                d.hudRanks.push_back(rank);
                d.hudScoreBars.push_back(scoreBar);
                d.hudComboValues.push_back(d.playtestCombo);
                d.hudJudgeTimes.push_back(static_cast<float>(time));
                d.hudJudgeKinds.push_back(kind);
        }

        const char* DrawData::judgeNameAt(size_t index) const
        {
                static constexpr const char* names[] = { "perfect", "great", "good", "bad", "miss" };
                if (index < hudJudgeKinds.size() && hudJudgeKinds[index] >= 0 && hudJudgeKinds[index] < 5)
                        return names[hudJudgeKinds[index]];
                return names[0];
        }

        void DrawData::beginPlaytest(Score const& score, double startTime)
        {
                comboTimes.clear();
                hudScores.clear();
                hudScoreGains.clear();
                hudRanks.clear();
                hudScoreBars.clear();
                hudJudgeTimes.clear();
                hudComboValues.clear();
                hudJudgeKinds.clear();
                playtestNotes.clear();

                float total = 0.0f;
                for (const auto& [id, note] : score.notes)
                {
                        (void)id;
                        if (note.getType() != NoteType::Tap)
                                continue;
                        const float weight = std::max(0.0f, getTapComboWeight(note));
                        total += weight;
                        const double time = accumulateDuration(note.tick, TICKS_PER_BEAT, score.tempoChanges);
                        playtestNotes.push_back({ time, note.lane, note.width, weight, time < startTime, note.ID });
                }
                std::sort(playtestNotes.begin(), playtestNotes.end(), [](const PlaytestNote& a, const PlaytestNote& b) { return a.time < b.time; });

                playtestWeightedCount = std::max(total, 1.0f);
                playtestCombo = 0;
                playtestComboFactor = 1.0f;
                playtestScore = 0.0f;
                playtestPrevScore = 0;
                playtestLastEventTime = 0.0;
                playtestHits.clear();
                playtestHitTime.clear();
                playtestActive = true;
        }

        void DrawData::playtestTouch(int lane, double touchTime)
        {
                if (!playtestActive)
                        return;

                constexpr double window = 0.15;
                PlaytestNote* best = nullptr;
                double bestDelta = window + 1.0;
                for (auto& n : playtestNotes)
                {
                        if (n.time > touchTime + window)
                                break;
                        if (n.judged || lane < n.lane || lane >= n.lane + n.width)
                                continue;
                        const double delta = std::abs(n.time - touchTime);
                        if (delta <= window && delta < bestDelta)
                        {
                                best = &n;
                                bestDelta = delta;
                        }
                }
                if (!best)
                        return;

                best->judged = true;
                playtestHitTime[best->id] = touchTime;
                const int kind = bestDelta <= 0.06 ? 0 : (bestDelta <= 0.09 ? 1 : (bestDelta <= 0.12 ? 2 : 3));
                pushPlaytestEvent(*this, touchTime, kind, best->weight);
                if (kind <= 2)
                        playtestHits.emplace_back(best->id, touchTime);
        }

        void DrawData::playtestAdvance(double now)
        {
                if (!playtestActive)
                        return;

                constexpr double window = 0.15;
                for (auto& n : playtestNotes)
                {
                        if (n.time > now - window)
                                break;
                        if (n.judged)
                                continue;
                        n.judged = true;
                        pushPlaytestEvent(*this, n.time + window, 4, n.weight);
                }
        }

        void DrawData::clear()
	{
		drawingLines.clear();
		drawingNotes.clear();
		drawingHoldTicks.clear();
		drawingHoldSegments.clear();
		comboTimes.clear();
		hudScores.clear();
		hudScoreGains.clear();
		hudRanks.clear();
		hudScoreBars.clear();
		hudJudgeTimes.clear();
                hudComboValues.clear();
                hudJudgeKinds.clear();
                playtestNotes.clear();
                playtestActive = false;
                playtestHitTime.clear();
                playtestHits.clear();
		effectView.reset();

		maxTicks = 1;
	}

	void addHoldNote(DrawData &drawData, const HoldNote &holdNote, Score const &score)
	{
		const float noteDuration = getNoteVisibleDuration(score.notes.at(holdNote.start.ID), score, drawData.noteSpeed);
		const Note& startNote = score.notes.at(holdNote.start.ID), endNote = score.notes.at(holdNote.end);
		float activeTime = accumulateDuration(startNote.tick, TICKS_PER_BEAT, score.tempoChanges);
		float startTime = activeTime;
		DrawingHoldStep head = {
			startNote.tick,
			accumulateScaledDuration(startNote.tick, TICKS_PER_BEAT, score.tempoChanges, score.hiSpeedChanges),
			Engine::laneToLeft(startNote.lane),
			Engine::laneToLeft(startNote.lane) + startNote.width,
			holdNote.start.ease
		};
		for (ptrdiff_t headIdx = -1, tailIdx = 0, stepSz = holdNote.steps.size(); headIdx < stepSz; ++tailIdx)
		{
			if (tailIdx < stepSz && holdNote.steps[tailIdx].type == HoldStepType::Skip)
				continue;
			HoldStep tailStep = tailIdx == stepSz ? HoldStep{ holdNote.end, HoldStepType::Hidden } : holdNote.steps[tailIdx];
			const Note& tailNote = score.notes.at(tailStep.ID);
			auto easeFunction = getEaseFunction(head.ease);
			DrawingHoldStep tail = {
				tailNote.tick,
				accumulateScaledDuration(tailNote.tick, TICKS_PER_BEAT, score.tempoChanges, score.hiSpeedChanges),
				Engine::laneToLeft(tailNote.lane),
				Engine::laneToLeft(tailNote.lane) + tailNote.width,
				tailStep.ease
			};
			float endTime = accumulateDuration(tailNote.tick, TICKS_PER_BEAT, score.tempoChanges);
			drawData.drawingHoldSegments.push_back(DrawingHoldSegment {
				holdNote.end, 
				head.ease,
			holdNote.isGuide(),
			tailIdx,
			head.time, tail.time,
			head.left, head.right,
			tail.left, tail.right,
			startTime, endTime,
			activeTime,
			noteDuration,
		});
			startTime = endTime;
			while ((headIdx + 1) < tailIdx)
			{
				const HoldStep& skipStep = holdNote.steps[headIdx + 1];
				assert(skipStep.type == HoldStepType::Skip);
				const Note& skipNote = score.notes.at(skipStep.ID);
				if (skipNote.tick > tail.tick)
					break;
				double tickTime = accumulateScaledDuration(skipNote.tick, TICKS_PER_BEAT, score.tempoChanges, score.hiSpeedChanges);
				double tick_t = unlerpD(head.time, tail.time, tickTime);
				float skipLeft = easeFunction(head.left, tail.left, tick_t);
				float skipRight = easeFunction(head.right, tail.right, tick_t);
				drawData.drawingHoldTicks.push_back(DrawingHoldTick{
					skipStep.ID,
					skipLeft + (skipRight - skipLeft) / 2,
					Range{tickTime - noteDuration, tickTime},
					noteDuration
				});
				++headIdx;
			}
			if (tailStep.type != HoldStepType::Hidden)
			{
				double tickTime = accumulateScaledDuration(tailNote.tick, TICKS_PER_BEAT, score.tempoChanges, score.hiSpeedChanges);
				drawData.drawingHoldTicks.push_back(DrawingHoldTick{
					tailNote.ID,
					getNoteCenter(tailNote),
					{tickTime - noteDuration, tickTime},
					noteDuration
				});
			}
			head = tail;
			++headIdx;
		}
	}
}
