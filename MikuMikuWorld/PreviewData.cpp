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

	static void pushPlaytestEvent(DrawData& d, double time, int kind, float weight, bool midHold = false)
        {
                constexpr float multipliers[] = { 1.0f, 0.9f, 0.8f, 0.7f, 0.0f };
                kind = std::clamp(kind, 0, 4);
                if (kind == 3)
                        d.playtestLife = std::max(0.0f, d.playtestLife - 50.0f);
                else if (kind == 4)
                        d.playtestLife = std::max(0.0f, d.playtestLife - (midHold ? 40.0f : 80.0f));
                const float levelFactor = (SCORE_RATING - 5.0f) * 0.005f + 1.0f;

                if (kind >= 2)
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
                d.hudLifeValues.push_back(d.playtestLife);
        }

        const char* DrawData::judgeNameAt(size_t index) const
        {
                static constexpr const char* names[] = { "perfect", "great", "good", "bad", "miss" };
                if (index < hudJudgeKinds.size() && hudJudgeKinds[index] >= 0 && hudJudgeKinds[index] < 5)
                        return names[hudJudgeKinds[index]];
                return names[0];
        }

        static int playtestJudgeKind(double delta)
        {
                return delta <= 0.08 ? 0 : (delta <= 0.12 ? 1 : (delta <= 0.16 ? 2 : 3));
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
                hudLifeValues.clear();
                playtestNotes.clear();
                playtestHolds.clear();
                playtestHits.clear();
                playtestPending.clear();
                playtestHitTime.clear();

                float total = 0.0f;
                for (const auto& [id, note] : score.notes)
                {
                        (void)id;
                        if (note.getType() != NoteType::Tap)
                                continue;
                        const float weight = std::max(0.0f, getTapComboWeight(note));
                        total += weight;
                        const double time = accumulateDuration(note.tick, TICKS_PER_BEAT, score.tempoChanges);
                        PlaytestNote n{};
                        n.time = time;
                        n.lane = note.lane;
                        n.width = note.width;
                        n.weight = weight;
                        n.judged = time < startTime;
                        n.id = note.ID;
                        n.flick = note.isFlick();
                        n.trace = note.friction;
                        n.flickDir = note.flick == FlickType::Left ? (config.pvMirrorScore ? 2 : 1)
                                : (note.flick == FlickType::Right ? (config.pvMirrorScore ? 1 : 2) : 0);
                        playtestNotes.push_back(n);
                }

                constexpr int halfBeat = TICKS_PER_BEAT / 2;
                for (const auto& [holdId, hold] : score.holdNotes)
                {
                        if (hold.isGuide() || hold.startType != HoldNoteType::Normal)
                                continue;
                        const auto startIt = score.notes.find(holdId);
                        const auto endIt = score.notes.find(hold.end);
                        if (startIt == score.notes.end() || endIt == score.notes.end())
                                continue;
                        const Note& head = startIt->second;
                        const Note& tail = endIt->second;

                        PlaytestHold h;
                        h.headId = head.ID;
                        h.lane = head.lane;
                        h.width = head.width;
                        h.endId = tail.ID;
                        h.startTime = accumulateDuration(head.tick, TICKS_PER_BEAT, score.tempoChanges);
                        h.endTime = accumulateDuration(tail.tick, TICKS_PER_BEAT, score.tempoChanges);
                        h.hasEnd = hold.endType == HoldNoteType::Normal;
                        h.endWeight = std::max(0.0f, getTapComboWeight(tail));

                        const float headWeight = std::max(0.0f, getTapComboWeight(head));
                        total += headWeight;
                        if (h.hasEnd)
                                total += h.endWeight;

                        for (const auto& step : hold.steps)
                        {
                                if (step.type == HoldStepType::Hidden)
                                        continue;
                                const auto stepIt = score.notes.find(step.ID);
                                if (stepIt == score.notes.end())
                                        continue;
                                const float w = std::max(0.0f, getTapComboWeight(stepIt->second));
                                total += w;
                                h.ticks.emplace_back(accumulateDuration(stepIt->second.tick, TICKS_PER_BEAT, score.tempoChanges), w);
                        }

                        int eighthTick = head.tick + halfBeat;
                        if (eighthTick % halfBeat)
                                eighthTick -= eighthTick % halfBeat;
                        if (eighthTick != head.tick && eighthTick != tail.tick)
                        {
                                int endTick = tail.tick;
                                if (endTick % halfBeat)
                                        endTick += halfBeat - (endTick % halfBeat);
                                const float w = head.critical ? 0.2f : 0.1f;
                                for (int tick = eighthTick; tick < endTick; tick += halfBeat)
                                {
                                        total += w;
                                        h.ticks.emplace_back(accumulateDuration(tick, TICKS_PER_BEAT, score.tempoChanges), w);
                                }
                        }
                        std::sort(h.ticks.begin(), h.ticks.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

                        if (h.startTime < startTime)
                                continue;

                        PlaytestNote n{};
                        n.time = h.startTime;
                        n.lane = head.lane;
                        n.width = head.width;
                        n.weight = headWeight;
                        n.judged = false;
                        n.id = head.ID;
                        n.trace = head.friction;
                        n.holdIndex = static_cast<int>(playtestHolds.size());
                        playtestNotes.push_back(n);
                        playtestHolds.push_back(std::move(h));
                }

                std::sort(playtestNotes.begin(), playtestNotes.end(), [](const PlaytestNote& a, const PlaytestNote& b) { return a.time < b.time; });

                playtestWeightedCount = std::max(total, 1.0f);
                playtestCombo = 0;
                playtestComboFactor = 1.0f;
                playtestScore = 0.0f;
                playtestLife = 1000.0f;
                playtestPrevScore = 0;
                playtestLastEventTime = 0.0;
                playtestActive = true;
        }

        void DrawData::playtestTouch(int pointerId, int lane, double touchTime)
        {
                if (!playtestActive)
                        return;

                constexpr double window = 0.2;
                PlaytestNote* best = nullptr;
                double bestDelta = window + 1.0;
                for (auto& n : playtestNotes)
                {
                        if (n.time > touchTime + window)
                                break;
                        if (n.judged || n.flick || n.trace || lane < n.lane || lane >= n.lane + n.width)
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
                const int kind = playtestJudgeKind(bestDelta);
                pushPlaytestEvent(*this, touchTime, kind, best->weight);
                if (kind <= 2)
                        playtestHits.emplace_back(best->id, touchTime);

                if (best->holdIndex >= 0)
                {
                        PlaytestHold& h = playtestHolds[best->holdIndex];
                        h.active = true;
                        h.pointer = pointerId;
                }
        }

        bool DrawData::playtestFlick(int lane, int anchorLane, double time, float dx, float dy)
        {
                if (!playtestActive)
                        return false;

                constexpr double window = 0.2;
                PlaytestNote* best = nullptr;
                double bestDelta = window + 1.0;
                const float adx = std::abs(dx);
                const float ady = std::abs(dy);
                for (auto& n : playtestNotes)
                {
                        if (n.time > time + window)
                                break;
                        if (n.judged || !n.flick || n.trace)
                                continue;
                        if (n.flickDir == 1 && !(dx < 0.0f && adx >= ady * 0.5f))
                                continue;
                        if (n.flickDir == 2 && !(dx > 0.0f && adx >= ady * 0.5f))
                                continue;
                        const bool inLane = (lane >= n.lane && lane < n.lane + n.width) || (anchorLane >= n.lane && anchorLane < n.lane + n.width);
                        if (!inLane)
                                continue;
                        const double delta = std::abs(n.time - time);
                        if (delta <= window && delta < bestDelta)
                        {
                                best = &n;
                                bestDelta = delta;
                        }
                }
                if (!best)
                        return false;

                best->judged = true;
                const int kind = playtestJudgeKind(bestDelta);
                if (time < best->time)
                {
                        best->pending = true;
                        best->pendingKind = kind;
                        playtestPending.push_back(static_cast<size_t>(best - playtestNotes.data()));
                        return true;
                }

                playtestHitTime[best->id] = time;
                pushPlaytestEvent(*this, time, kind, best->weight);
                if (kind <= 2)
                        playtestHits.emplace_back(best->id, time);
                return true;
        }

        void DrawData::playtestTrace(unsigned int heldMask, const double* laneMoveAt, double now)
        {
                if (!playtestActive)
                        return;

                constexpr double traceWindow = 0.1;
                for (auto& n : playtestNotes)
                {
                        if (n.time > now)
                                break;
                        if (n.judged || !n.trace || now > n.time + traceWindow)
                                continue;

                        const unsigned int laneMask = ((1u << n.width) - 1u) << n.lane;
                        if ((heldMask & laneMask) == 0)
                                continue;

                        if (n.flick)
                        {
                                bool moved = false;
                                for (int l = n.lane; l < n.lane + n.width && l < 12; ++l)
                                {
                                        if (laneMoveAt[l] >= n.time - 0.25)
                                        {
                                                moved = true;
                                                break;
                                        }
                                }
                                if (!moved)
                                        continue;
                        }

                        n.judged = true;
                        playtestHitTime[n.id] = n.time;
                        pushPlaytestEvent(*this, n.time, 0, n.weight);
                        playtestHits.emplace_back(n.id, n.time);
                        if (n.holdIndex >= 0)
                        {
                                playtestHolds[n.holdIndex].active = true;
                                playtestHolds[n.holdIndex].pointer = -2;
                        }
                }
        }

        void DrawData::playtestRelease(int pointerId, double time)
        {
                if (!playtestActive)
                        return;

                constexpr double window = 0.2;
                for (auto& h : playtestHolds)
                {
                        if (h.done || !h.active || h.pointer != pointerId)
                                continue;

                        while (h.nextTick < h.ticks.size() && h.ticks[h.nextTick].first <= time)
                        {
                                pushPlaytestEvent(*this, h.ticks[h.nextTick].first, 0, h.ticks[h.nextTick].second);
                                ++h.nextTick;
                        }

                        h.active = false;
                        const double delta = std::abs(h.endTime - time);
                        if (delta <= window)
                        {
                                while (h.nextTick < h.ticks.size())
                                {
                                        pushPlaytestEvent(*this, time, 0, h.ticks[h.nextTick].second);
                                        ++h.nextTick;
                                }
                                if (h.hasEnd)
                                {
                                        const int kind = playtestJudgeKind(delta);
                                        pushPlaytestEvent(*this, time, kind, h.endWeight);
                                        if (kind <= 2)
                                                playtestHits.emplace_back(h.endId, time);
                                }
                                h.done = true;
                        }
                        else if (time < h.endTime)
                                h.failed = true;
                        else
                                h.done = true;
                }
        }

        void DrawData::playtestAdvance(double now)
        {
                if (!playtestActive)
                        return;

                constexpr double window = 0.2;
                constexpr double traceWindow = 0.1;
                for (size_t idx : playtestPending)
                {
                        PlaytestNote& pn = playtestNotes[idx];
                        if (!pn.pending || pn.time > now)
                                continue;
                        pn.pending = false;
                        playtestHitTime[pn.id] = pn.time;
                        pushPlaytestEvent(*this, pn.time, pn.pendingKind, pn.weight);
                        if (pn.pendingKind <= 2)
                                playtestHits.emplace_back(pn.id, pn.time);
                }
                playtestPending.erase(std::remove_if(playtestPending.begin(), playtestPending.end(),
                        [this](size_t idx) { return !playtestNotes[idx].pending; }), playtestPending.end());

                for (auto& n : playtestNotes)
                {
                        if (n.time > now - traceWindow)
                                break;
                        if (n.judged)
                                continue;
                        const double w = n.trace ? traceWindow : window;
                        if (n.time > now - w)
                                continue;
                        n.judged = true;
                        pushPlaytestEvent(*this, n.time + w, 4, n.weight);
                        if (n.holdIndex >= 0)
                                playtestHolds[n.holdIndex].failed = true;
                }

                for (auto& h : playtestHolds)
                {
                        if (h.done)
                                continue;

                        if (h.active)
                        {
                                while (h.nextTick < h.ticks.size() && h.ticks[h.nextTick].first <= now)
                                {
                                        pushPlaytestEvent(*this, h.ticks[h.nextTick].first, 0, h.ticks[h.nextTick].second);
                                        ++h.nextTick;
                                }
                                if (now >= h.endTime)
                                {
                                        if (h.hasEnd)
                                        {
                                                pushPlaytestEvent(*this, h.endTime, 0, h.endWeight);
                                                playtestHits.emplace_back(h.endId, h.endTime);
                                        }
                                        h.active = false;
                                        h.done = true;
                                }
                        }
                        else if (h.failed)
                        {
                                while (h.nextTick < h.ticks.size() && h.ticks[h.nextTick].first <= now)
                                {
                                        pushPlaytestEvent(*this, h.ticks[h.nextTick].first, 4, h.ticks[h.nextTick].second, true);
                                        ++h.nextTick;
                                }
                                if (now >= h.endTime)
                                {
                                        if (h.hasEnd)
                                                pushPlaytestEvent(*this, h.endTime, 4, h.endWeight);
                                        h.done = true;
                                }
                        }
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
                hudLifeValues.clear();
                playtestNotes.clear();
                playtestActive = false;
                playtestPending.clear();
                playtestHolds.clear();
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
