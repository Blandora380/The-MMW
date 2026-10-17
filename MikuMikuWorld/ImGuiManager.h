#pragma once
#include "UI.h"
#include <string>

struct GLFWwindow;

namespace MikuMikuWorld
{
	class Result;

	class ImGuiManager
	{
	private:
		std::string configFilename{};
		
		BaseTheme theme{};
		int accentColor{ 1 };
		float styleScale{ 1.0f };
		static ImFont* sekaiDifficultyFont;
		static ImFont* sekaiDetailsFont;

	public:
		ImGuiManager();

		Result initialize(GLFWwindow* window);
		void initializeLayout();
		void shutdown();
		void begin();
		ImFont* loadFont(const std::string& filename, float size, const ImWchar* glyphRanges = nullptr);
		void loadIconFont(const std::string& filename, int start, int end, float size);
		void buildFonts(float dpiScale = 1.0f);
		static ImFont* getSekaiDifficultyFont() { return sekaiDifficultyFont; }
		static ImFont* getSekaiDetailsFont() { return sekaiDetailsFont; }
		void draw(GLFWwindow* window);

		void setBaseTheme(BaseTheme theme);
		void applyAccentColor(int colIndex);

		inline constexpr BaseTheme getBaseTheme() const { return theme; }
		inline constexpr int getAccentColor() const { return accentColor; }
	};
}
