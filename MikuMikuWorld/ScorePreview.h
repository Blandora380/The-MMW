#pragma once
#include "ScoreContext.h"
#include "ScoreEditorTimeline.h"
#include "Rendering/Framebuffer.h"
#include "Rendering/Renderer.h"
#include "Rendering/Camera.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ImDrawList;
struct ImVec2;

namespace MikuMikuWorld
{
	class ScorePreviewVideoDecoder;
	class Texture;

	class ScorePreviewBackground
	{
		std::string backgroundFile;
		std::string jacketFile;
		Framebuffer frameBuffer;
		float brightness;
		bool init;
		std::unique_ptr<ScorePreviewVideoDecoder> videoDecoder;
		std::unique_ptr<Texture> mvLineOverlay;
		
		struct DefaultJacket
		{
			static std::array<DirectX::XMFLOAT4, 4> getLeftUV();
			static std::array<DirectX::XMFLOAT4, 4> getRightUV();
			static std::array<DirectX::XMFLOAT4, 4> getLeftMirrorUV();
			static std::array<DirectX::XMFLOAT4, 4> getRightMirrorUV();
			static std::array<DirectX::XMFLOAT4, 4> getCenterUV();
			static std::array<DirectX::XMFLOAT4, 4> getMirrorCenterUV();
		};
		void updateDrawDefaultJacket(Renderer* renderer, const Jacket& jacket);
		public:
		ScorePreviewBackground();
		~ScorePreviewBackground();

		void setBrightness(float value);
		void update(Renderer* renderer, const Jacket& jacket);
		bool shouldUpdate(const Jacket& jacket) const;
		void draw(Renderer* renderer, float scrWidth, float scrHeight, float timelineTime, bool isPlaying,
			const std::string& mvFilename, float mvOffsetMs, bool waitForVideoFrame);
		unsigned int getTexture() const { return frameBuffer.getTexture(); }
		unsigned int getWidth() const { return frameBuffer.getWidth(); }
		unsigned int getHeight() const { return frameBuffer.getHeight(); }
	};

	class ScorePreviewWindow
	{
		Framebuffer previewBuffer;
		ScorePreviewBackground background;
		std::array<std::unique_ptr<Texture>, 7> sekaiDifficultyBackgrounds;
		std::unique_ptr<Texture> sekaiCollabTag;
		std::unique_ptr<Texture> sekaiIntroGradient;
		std::unique_ptr<Texture> sekaiMusicSheet;
		std::unique_ptr<Texture> sekaiPerson;
		float scaledAspectRatio;
		Camera noteEffectsCamera;

		mutable bool fullWindow{};
		int exportWidth{};
		int exportHeight{};
		bool exportIncludeUI{};
		bool exportSekaiIntro{};
		float sekaiIntroElapsed{};
		float sekaiIntroDuration{};
		float exportStageFadeIn{ 1.0f };
		std::string introTitle;
		std::string introDesigner;
		std::string introLyricist;
		std::string introComposer;
		std::string introArrangement;
		std::string introVocal;
		std::string introCollabTag;
		std::string introChartId;
		int introDifficulty{};

		const Texture& getNoteTexture();

		void drawNoteBase(Renderer* renderer, const Note& note, float left, float right, float y, float zScalar = 1);
		void drawTraceDiamond(Renderer* renderer, const Note& note, float left, float right, float y);
		void drawFlickArrow(Renderer* renderer, const Note& note, float y, double cur_time);

		void drawStageCoverMask(Renderer* renderer);
		void drawHudOverlay(const ScoreContext& context, ImDrawList* drawList, ImVec2 position, ImVec2 size);
		void drawSekaiIntroBackdrop(ImDrawList* drawList, ImVec2 size, float opacity, bool tinted = true);

		void updateHudControls(ScoreEditorTimeline& timeline, ScoreContext& context) const;
		void updateToolbar(ScoreEditorTimeline& timeline, ScoreContext& context) const;
		float getScrollbarWidth() const;
		void updateScrollbar(ScoreEditorTimeline& timeline, ScoreContext& context) const;
	public:
		ScorePreviewWindow(); 
		~ScorePreviewWindow();
		void update(ScoreContext& context, Renderer* renderer);
		void updateUI(ScoreEditorTimeline& timeline, ScoreContext& context);

		void drawNotes(const ScoreContext& context, Renderer* renderer);
		void drawLines(const ScoreContext& context, Renderer* renderer);
		void drawHoldTicks(const ScoreContext& context, Renderer* renderer);
		void drawHoldCurves(const ScoreContext& context, Renderer* renderer);
 		
		void drawStage(Renderer* renderer);
		void drawStageCover(Renderer* renderer);
		void drawStageCoverDecoration(Renderer* renderer);
		void drawSekaiIntro(const ScoreContext& context, ImDrawList* drawList, ImVec2 size, float elapsed);

		void setFullWindow(bool fullScreen);
		void setExportResolution(int width, int height, bool includeUI);
		void setExportSekaiIntro(bool enabled, float elapsed, float duration, const std::string& title,
			const std::string& designer, const std::string& lyricist, const std::string& composer,
			const std::string& arrangement, const std::string& vocal, const std::string& collabTag, int difficulty,
			const std::string& chartId);
		void setExportStageFadeIn(float alpha) { exportStageFadeIn = std::clamp(alpha, 0.0f, 1.0f); }
		void clearExportResolution();
		void readPreviewPixels(std::vector<uint8_t>& pixels) const;
		
		inline bool isFullWindow() const { return fullWindow; };

		void loadNoteEffects(Effect::EffectView& effectView);
	};
}
