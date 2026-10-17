#include "Application.h"
#include "ScoreEditorWindows.h"
#include "UI.h"
#include "File.h"
#include "Constants.h"
#include "Utilities.h"
#include "ImGui/imgui_stdlib.h"
#include "ApplicationConfiguration.h"
#include "NoteSkin.h"
#include "ResourceManager.h"
#include "ScorePreview.h"
#if defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
#include "AndroidFileDialog.h"
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

namespace MikuMikuWorld
{
	namespace
	{
		constexpr uint64_t MAX_CUSTOM_MP4_PIXELS = 7680ull * 4320ull;

		bool isValidCustomMp4Resolution(int width, int height)
		{
			return width >= 2 && height >= 2 && width <= 8192 && height <= 8192 &&
				width % 2 == 0 && height % 2 == 0 &&
				static_cast<uint64_t>(width) * static_cast<uint64_t>(height) <= MAX_CUSTOM_MP4_PIXELS;
		}
	}

	void ScorePropertiesWindow::statsTableRow(const char* lbl, size_t row)
	{
		ImGui::TableSetColumnIndex(0);

		int index = ResourceManager::getTexture("note_stats");
		if (isArrayIndexInBounds(index, ResourceManager::textures))
		{
			const Texture& tex = ResourceManager::textures[index];
			if (row < scoreStatsImages.size() && isArrayIndexInBounds(scoreStatsImages[row], tex.sprites))
			{
				const Sprite& spr = tex.sprites[scoreStatsImages[row]];
				ImVec2 uv0{ spr.getX1() / tex.getWidth(), spr.getY1() / tex.getHeight() };
				ImVec2 uv1{ spr.getX2() / tex.getWidth(), spr.getY2() / tex.getHeight() };
				ImGui::Image((ImTextureID)(size_t)tex.getID(), { 20, 20 }, uv0, uv1);
			}
			else
			{
				ImGui::Text("%s", getString(lbl));
			}
		}
		else
		{
			ImGui::Text("%s", getString(lbl));
		}

		ImGui::TableSetColumnIndex(1);
	}

	void ScorePropertiesWindow::update(ScoreContext& context)
	{
		if (ImGui::CollapsingHeader(IO::concat(ICON_FA_ALIGN_LEFT, getString("metadata"), " ").c_str(), ImGuiTreeNodeFlags_DefaultOpen))
		{
			UI::beginPropertyColumns();
			UI::addStringProperty(getString("title"), context.workingData.title);
			UI::addStringProperty(getString("designer"), context.workingData.designer);
			UI::addStringProperty(getString("artist"), context.workingData.artist);
			UI::addIntProperty(getString("music_id"), context.workingData.musicId);

			std::string jacketFile = context.workingData.jacket.getFilename();

			ImGui::BeginDisabled(isLoadingJacket);
			int result = UI::addFileProperty(getString("jacket"), isLoadingJacket ? loadingText : jacketFile);
			ImGui::EndDisabled();
			
			if (result == 1)
			{
				context.workingData.jacket.load(jacketFile);
			}
			else if (result == 2)
			{
				IO::FileDialog fileDialog{};
				fileDialog.title = "Open Image File";
				fileDialog.filters = { IO::imageFilter, IO::allFilter };
				fileDialog.parentWindowHandle = Application::windowState.windowHandle;

				if (fileDialog.openFile() == IO::FileDialogResult::OK)
					context.workingData.jacket.load(fileDialog.outputFilename);
			}
			 
			context.workingData.jacket.draw();
			UI::endPropertyColumns();
		}

		if (ImGui::CollapsingHeader(IO::concat(ICON_FA_VOLUME_UP, getString("audio"), " ").c_str(), ImGuiTreeNodeFlags_DefaultOpen))
		{
			UI::beginPropertyColumns();
			
			std::string musicFilename = context.workingData.musicFilename;

			ImGui::BeginDisabled(isLoadingMusic);
			int filePickResult = UI::addFileProperty(getString("music_file"), isLoadingMusic ? loadingText : musicFilename);
			ImGui::EndDisabled();

			if (filePickResult == 1 && musicFilename != context.workingData.musicFilename)
			{
				isPendingLoadMusic = true;
				pendingLoadMusicFilename = musicFilename;
			}
			else if (filePickResult == 2)
			{
				IO::FileDialog fileDialog{};
				fileDialog.title = "Open Audio File";
				fileDialog.filters = { IO::audioFilter, IO::allFilter };	
				fileDialog.parentWindowHandle = Application::windowState.windowHandle;

				if (fileDialog.openFile() == IO::FileDialogResult::OK)
				{
					pendingLoadMusicFilename = fileDialog.outputFilename;
					isPendingLoadMusic = true;
				}
			}

			float offset = context.workingData.musicOffset;
			UI::addDragFloatProperty(getString("music_offset"), offset, "%.3fms");
			if (offset != context.workingData.musicOffset)
			{
				context.workingData.musicOffset = offset;
				context.audio.setMusicOffset(context.getTimeAtCurrentTick(), offset);
			}

			std::string mvFilename = context.workingData.mvFilename;
			int mvPickResult = UI::addFileProperty(getString("mv_video"), mvFilename);
			if (mvPickResult == 1 && mvFilename != context.workingData.mvFilename)
			{
				context.workingData.mvFilename = mvFilename;
				context.upToDate = false;
			}
			else if (mvPickResult == 2)
			{
				IO::FileDialog fileDialog{};
				fileDialog.title = "Open MV Video";
				fileDialog.filters = { { "Video Files", "*.mp4;*.mov;*.mkv;*.webm;*.avi" }, IO::allFilter };
				fileDialog.parentWindowHandle = Application::windowState.windowHandle;
				if (fileDialog.openFile() == IO::FileDialogResult::OK && fileDialog.outputFilename != context.workingData.mvFilename)
				{
					context.workingData.mvFilename = fileDialog.outputFilename;
					context.upToDate = false;
				}
			}

			float mvOffset = context.workingData.mvOffset;
			UI::addDragFloatProperty(getString("mv_offset"), mvOffset, "%.3fms");
			if (mvOffset != context.workingData.mvOffset)
			{
				context.workingData.mvOffset = mvOffset;
				context.upToDate = false;
			}
			if (!context.workingData.mvFilename.empty())
			{
				UI::propertyLabel(getString("clear_mv_video"));
				if (ImGui::Button(getString("clear_mv_video")))
				{
					context.workingData.mvFilename.clear();
					context.upToDate = false;
				}
				ImGui::NextColumn();
			}

			// volume controls
			float master = context.audio.getMasterVolume();
			float bgm = context.audio.getMusicVolume();
			float se = context.audio.getSoundEffectsVolume();

			UI::addPercentSliderProperty(getString("volume_master"), master);
			UI::addPercentSliderProperty(getString("volume_bgm"), bgm);
			UI::addPercentSliderProperty(getString("volume_se"), se);
			UI::endPropertyColumns();

			if (master != context.audio.getMasterVolume())
				context.audio.setMasterVolume(master);

			if (bgm != context.audio.getMusicVolume())
				context.audio.setMusicVolume(bgm);

			if (se != context.audio.getSoundEffectsVolume())
				context.audio.setSoundEffectsVolume(se);
		}

		if (ImGui::CollapsingHeader(IO::concat(ICON_FA_CHART_BAR, getString("statistics"), " ").c_str(), ImGuiTreeNodeFlags_DefaultOpen))
		{
			constexpr ImGuiTableFlags tableFlags
			{	ImGuiTableFlags_BordersInner |
				ImGuiTableFlags_ScrollY |
				ImGuiTableFlags_SizingStretchSame |
				ImGuiTableFlags_PadOuterX |
				ImGuiTableFlags_RowBg
			};

			float tableHeight{ std::min(ImGui::GetFrameHeight() * (scoreStatsImages.size() + 2), ImGui::GetContentRegionAvail().y) };
			if (ImGui::BeginTable("stats_table", 2, tableFlags, { 0, tableHeight }))
			{
				ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, { 10, 1 });

				ImGui::TableNextRow();
				statsTableRow("tap", 0);
				ImGui::Text("%d", context.scoreStats.getTaps());

				ImGui::TableNextRow();
				statsTableRow("flick", 1);
				ImGui::Text("%d", context.scoreStats.getFlicks());

				ImGui::TableNextRow();
				statsTableRow("hold", 2);
				ImGui::Text("%d", context.scoreStats.getHolds());

				ImGui::TableNextRow();
				statsTableRow("step", 3);
				ImGui::Text("%d", context.scoreStats.getSteps());

				ImGui::TableNextRow();
				statsTableRow("trace", 4);
				ImGui::Text("%d", context.scoreStats.getTraces());

				ImGui::TableNextRow();
				statsTableRow("total", 5);
				ImGui::Text("%d", context.scoreStats.getTotal());

				ImGui::TableNextRow();
				statsTableRow("combo", 6);
				ImGui::Text("%d", context.scoreStats.getCombo());

				ImGui::PopStyleVar();
				ImGui::EndTable();
			}
		}
	}

	void ScoreOptionsWindow::update(ScoreContext& context, EditArgs& edit, TimelineMode currentMode)
	{
		UI::beginPropertyColumns();
		switch (currentMode)
		{
		case TimelineMode::InsertBPM:
			UI::addFloatProperty(getString("bpm"), edit.bpm, "%g BPM");
			edit.bpm = std::clamp(edit.bpm, MIN_BPM, MAX_BPM);
			break;

		case TimelineMode::InsertTimeSign:
			UI::timeSignatureSelect(edit.timeSignatureNumerator, edit.timeSignatureDenominator);
			break;

		case TimelineMode::InsertHiSpeed:
			UI::addFloatProperty(getString("hi_speed_speed"), edit.hiSpeed, "%gx");
			break;

		case TimelineMode::InsertSEVolume:
			UI::addFloatProperty(getString("se_volume_value"), edit.seVolume, "%g");
			break;

		default:
			UI::addIntProperty(getString("note_width"), edit.noteWidth, MIN_NOTE_WIDTH, MAX_NOTE_WIDTH);
			UI::addSelectProperty(getString("step_type"), edit.stepType, stepTypes, arrayLength(stepTypes));
			UI::addSelectProperty(getString("ease_type"), edit.easeType, easeTypes, arrayLength(easeTypes));
			UI::addSelectProperty<FlickType>(getString("flick"), edit.flickType, flickTypes, arrayLength(flickTypes));
			break;
		}
		UI::endPropertyColumns();
	}

	void PresetsWindow::update(ScoreContext& context, PresetManager& presetManager)
	{
		if (ImGui::Begin(IMGUI_TITLE(ICON_FA_DRAFTING_COMPASS, "presets")))
		{
			int removePattern = -1;

			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
			float filterWidth = ImGui::GetContentRegionAvail().x - UI::btnSmall.x - 2;

			if (Utilities::ImGuiTextFilterWithHint(&presetFilter, "##preset_filter", IO::concat(ICON_FA_SEARCH, getString("search"), " ").c_str(), filterWidth))
			{
				memoizeFilterMatches(presetManager.presets);
			}
			ImGui::SameLine();
			if (ImGui::Button(ICON_FA_TIMES, UI::btnSmall))
			{
				presetFilter.Clear();
				memoizeFilterMatches(presetManager.presets);
			}

			ImGui::PopStyleVar();
			ImGui::PopStyleColor();

			const int deletePresetId{presetManager.deletedPreset.getID()};
			if (deletePresetId > -1)
			{
				const float itemSpacingX = ImGui::GetStyle().ItemSpacing.x * 2;
				float availableWidth = ImGui::GetContentRegionAvail().x + (UI::btnSmall.x * 2) + itemSpacingX;
				ImGui::SetNextItemWidth(availableWidth);
				ImGui::Text("%s", getString("preset_deleted"));
				ImGui::SameLine();

				// Shift undo and close to the right end of the window
				ImGui::SetCursorPosX(ImGui::GetWindowWidth() - (UI::btnSmall.x * 2) - itemSpacingX - ImGui::GetStyle().WindowPadding.x);
				if (UI::transparentButton(ICON_FA_UNDO, UI::btnSmall, false))
				{
					presetManager.undoDeletePreset();
					memoizeFilterMatches(presetManager.presets);
				}

				UI::tooltip(getString("undo"));
				ImGui::SameLine();

				// Conflict with preset filter clear button
				ImGui::PushID("undo_preset_delete");
				if (UI::transparentButton(ICON_FA_TIMES, UI::btnSmall, false))
					presetManager.deletedPreset = {};

				UI::tooltip(getString("close"));
				ImGui::PopID();
			}

			float presetButtonHeight = ImGui::GetFrameHeight();
			float windowHeight = ImGui::GetContentRegionAvail().y - presetButtonHeight - ImGui::GetStyle().WindowPadding.y;
			if (ImGui::BeginChild("presets_child_window", ImVec2(-1, windowHeight), true))
			{
				if (loadingPresets)
				{
					Utilities::ImGuiCenteredText("Loading...");
				}
				else if (presetManager.presets.empty())
				{
					ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().DisabledAlpha);
					Utilities::ImGuiCenteredText(getString("no_presets"));
					ImGui::PopStyleVar();
				}
				else
				{
					listClipper.Begin(filterMatchPresets.size(), presetButtonHeight + ImGui::GetStyle().ItemSpacing.y);
					while (listClipper.Step())
					{
						for (size_t i = listClipper.DisplayStart; i < listClipper.DisplayEnd; i++)
						{
							const auto& preset = presetManager.presets.at(filterMatchPresets.at(i));
							if (!presetFilter.PassFilter(preset.getName().c_str()))
								continue;

							ImGui::PushID(preset.getID());

							if (ImGui::Button(preset.getName().c_str(), ImVec2(ImGui::GetContentRegionAvail().x - UI::btnSmall.x - 2.0f, presetButtonHeight)))
								presetManager.applyPreset(filterMatchPresets.at(i), context);

							if (!preset.description.empty())
								UI::tooltip(preset.description.c_str());

							ImGui::SameLine();
							if (UI::transparentButton(ICON_FA_TRASH, ImVec2(UI::btnSmall.x, presetButtonHeight)))
								removePattern = filterMatchPresets.at(i);

							ImGui::PopID();
						}
					}
					listClipper.End();
				}
			}
			ImGui::EndChild();
			ImGui::Separator();

			ImGui::BeginDisabled(context.selectedNotes.empty() || loadingPresets);
			if (ImGui::Button(getString("create_preset"), ImVec2(-1, presetButtonHeight)))
				dialogOpen = true;

			ImGui::EndDisabled();

			if (removePattern != -1)
			{
				presetManager.removePreset(removePattern);
				memoizeFilterMatches(presetManager.presets);
			}
		}

		ImGui::End();

		if (dialogOpen)
		{
			ImGui::OpenPopup(MODAL_TITLE("create_preset"));
			dialogOpen = false;
		}

		if (updateCreationDialog() == DialogResult::Ok)
		{
			presetManager.createPreset(context, presetName, presetDesc);
			presetName.clear();
			presetDesc.clear();
			memoizeFilterMatches(presetManager.presets);
		}
	}

	DialogResult PresetsWindow::updateCreationDialog()
	{
		DialogResult result = DialogResult::None;

		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(500, 300), ImGuiCond_Always);
		ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
		if (ImGui::BeginPopupModal(MODAL_TITLE("create_preset"), NULL, ImGuiWindowFlags_NoResize))
		{
			ImVec2 padding = ImGui::GetStyle().WindowPadding;
			ImVec2 spacing = ImGui::GetStyle().ItemSpacing;

			float xPos = padding.x;
			float yPos = ImGui::GetWindowSize().y - UI::btnSmall.y - 2.0f - (padding.y * 2);

			ImGui::Text("%s", getString("name"));
			ImGui::SetNextItemWidth(-1);
			ImGui::InputText("##preset_name", &presetName);

			ImGui::Text("%s", getString("description"));
			ImGui::InputTextMultiline("##preset_desc", &presetDesc,
				{ -1, ImGui::GetContentRegionAvail().y - UI::btnSmall.y - 10.0f - padding.y });

			ImVec2 btnSz{ (ImGui::GetContentRegionAvail().x - spacing.x - (padding.x * 0.5f)) / 2.0f, ImGui::GetFrameHeight() };
			ImGui::SetCursorPos(ImVec2(xPos, yPos));
			if (ImGui::Button(getString("cancel"), btnSz))
			{
				result = DialogResult::Cancel;
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();
			ImGui::BeginDisabled(presetName.empty());
			if (ImGui::Button(getString("confirm"), btnSz))
			{
				result = DialogResult::Ok;
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndDisabled();
			ImGui::EndPopup();
		}

		return result;
	}

	void PresetsWindow::notifyPresetsLoading()
	{
		loadingPresets = true;
	}

	void PresetsWindow::notifyPresetsLoaded(const PresetManager& presetManager)
	{
		loadingPresets = false;
		memoizeFilterMatches(presetManager.presets);
	}

	void PresetsWindow::memoizeFilterMatches(const std::vector<NotesPreset>& presets)
	{
		filterMatchPresets.clear();
		for (size_t i = 0; i < presets.size(); i++)
		{
			if (presetFilter.PassFilter(presets.at(i).getName().c_str()))
				filterMatchPresets.push_back(i);
		}
	}

	DialogResult RecentFileNotFoundDialog::update()
	{
		DialogResult result{ DialogResult::None };
		if (open)
		{
			ImGui::OpenPopup(MODAL_TITLE("file_not_found"));
			open = false;
		}

		std::string dialogText = IO::formatString("%s \"%s\" %s. %s",
			getString("file_not_found_msg1"),
			removeFilename.c_str(),
			getString("file_not_found_msg2"),
			getString("remove_recent_file_not_found")
		);

		float maxDialogSizeX{ ImGui::GetMainViewport()->WorkSize.x * 0.80f };
		ImVec2 padding = ImGui::GetStyle().WindowPadding;
		ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
		ImVec2 textSize = ImGui::CalcTextSize(dialogText.c_str());

		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(std::min(maxDialogSizeX, textSize.x + (padding.x * 2) + spacing.x ), 0), ImGuiCond_Always);
		ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
		if (ImGui::BeginPopupModal(MODAL_TITLE("file_not_found"), NULL, ImGuiWindowFlags_NoResize))
		{
			ImGui::TextWrapped("%s", dialogText.c_str());
			
			// New line to move the buttons a bit down
			ImGui::Text("\n");

			ImVec2 btnSz{ (ImGui::GetContentRegionAvail().x - spacing.x - (padding.x * 0.5f)) / 2.0f, ImGui::GetFrameHeight() };
			btnSz.x = std::min(btnSz.x, 150.0f);
			
			// Right align buttons
			ImGui::SetCursorPos(ImVec2(ImGui::GetWindowSize().x - (btnSz.x * 2) - spacing.x - padding.x, ImGui::GetCursorPosY()));

			if (ImGui::Button(getString("yes"), btnSz))
			{
				close();
				result = DialogResult::Yes;
			}

			ImGui::SameLine();
			if (ImGui::Button(getString("no"), btnSz))
			{
				close();
				result = DialogResult::No;
			}

			ImGui::EndPopup();
		}

		return result;
	}

	DialogResult UnsavedChangesDialog::update()
	{
		DialogResult result = DialogResult::None;
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(450, 200), ImGuiCond_Always);
		ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
		if (ImGui::BeginPopupModal(MODAL_TITLE("unsaved_changes"), NULL, ImGuiWindowFlags_NoResize))
		{
			ImGui::Text("%s", getString("ask_save"));
			ImGui::Text("%s", getString("warn_unsaved"));

			ImVec2 padding = ImGui::GetStyle().WindowPadding;
			ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
			
			float btnsHeight = ImGui::GetFrameHeight();
			float xPos = padding.x;
			float yPos = ImGui::GetWindowSize().y - btnsHeight - padding.y;
			ImGui::SetCursorPos(ImVec2(xPos, yPos));

			ImVec2 btnSz{ (ImGui::GetContentRegionAvail().x - spacing.x - (padding.x * 0.5f)) / 3.0f, btnsHeight };

			if (ImGui::Button(getString("save_changes"), btnSz))
			{
				close();
				result = DialogResult::Yes;
			}

			ImGui::SameLine();
			if (ImGui::Button(getString("discard_changes"), btnSz))
			{
				close();
				result = DialogResult::No;
			}

			ImGui::SameLine();
			if (ImGui::Button(getString("cancel"), btnSz))
			{
				close();
				result = DialogResult::Cancel;
			}

			ImGui::EndPopup();
		}

		return result;
	}

	DialogResult AboutDialog::update()
	{
		constexpr const char* PROJECT_URL = "https://github.com/watagashi-uni/External-Unofficial-ChartMaker";
		constexpr const char* PROJECT_LABEL = "watagashi-uni/External-Unofficial-ChartMaker";
		constexpr const char* MIKUMIKUWORLD_LICENSE =
			"MIT License\n"
			"\n"
			"Copyright (c) 2022 Crash5b\n"
			"\n"
			"Permission is hereby granted, free of charge, to any person obtaining a copy\n"
			"of this software and associated documentation files (the \"Software\"), to deal\n"
			"in the Software without restriction, including without limitation the rights\n"
			"to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\n"
			"copies of the Software, and to permit persons to whom the Software is\n"
			"furnished to do so, subject to the following conditions:\n"
			"\n"
			"The above copyright notice and this permission notice shall be included in all\n"
			"copies or substantial portions of the Software.\n"
			"\n"
			"THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
			"IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
			"FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n"
			"AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
			"LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
			"OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\n"
			"SOFTWARE.";

		if (open)
		{
			ImGui::OpenPopup(MODAL_TITLE("about"));
			open = false;
		}

		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(650, 520), ImGuiCond_Always);
		ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
		if (ImGui::BeginPopupModal(MODAL_TITLE("about"), NULL, ImGuiWindowFlags_NoResize))
		{
			ImGui::Text("MikuMikuWorld\nCopyright (C) 2022 Crash5b\n\n");
			ImGui::Separator();

			float okButtonHeight = ImGui::GetFrameHeight();

			ImGui::Text("Version %s", Application::getAppVersion().c_str());
			ImGui::Text("Project");
			ImGui::SameLine();
			ImGui::TextLinkOpenURL(PROJECT_LABEL, PROJECT_URL);

			ImGui::Separator();
			ImGui::Text("Original MikuMikuWorld License");

			const ImGuiStyle& style = ImGui::GetStyle();
			const float okButtonY = ImGui::GetWindowSize().y - okButtonHeight - style.WindowPadding.y;
			const float licenseHeight = okButtonY - ImGui::GetCursorPosY() - style.ItemSpacing.y;
			if (licenseHeight > 0.0f)
			{
				ImGui::BeginChild("##mikumiku_world_license", ImVec2(-1.0f, licenseHeight), ImGuiChildFlags_Borders);
				ImGui::TextWrapped("%s", MIKUMIKUWORLD_LICENSE);
				ImGui::EndChild();
			}

			ImGui::SetCursorPos({ style.WindowPadding.x, okButtonY });

			if (ImGui::Button("OK", { ImGui::GetContentRegionAvail().x, okButtonHeight }))
			{
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return DialogResult::Ok;
			}

			ImGui::EndPopup();
		}

		return DialogResult::None;
	}

	void DebugWindow::update(ScoreContext& context, ScoreEditorTimeline& timeline)
	{
		if (ImGui::Begin(IMGUI_TITLE(ICON_FA_BUG, "debug")))
		{
			constexpr ImGuiTreeNodeFlags headerFlags = ImGuiTreeNodeFlags_DefaultOpen;
			constexpr ImGuiTreeNodeFlags treeNodeFlags = headerFlags | ImGuiTreeNodeFlags_Framed;
			if (ImGui::TreeNodeEx("Audio", treeNodeFlags))
			{
				if (ImGui::CollapsingHeader("Engine", headerFlags))
				{
					UI::beginPropertyColumns();
					UI::addReadOnlyProperty("Sample Rate", context.audio.getDeviceSampleRate());
					UI::addReadOnlyProperty("Channel Count", context.audio.getDeviceChannelCount());
					UI::addReadOnlyProperty("Latency", IO::formatString("%.2fms", context.audio.getDeviceLatency() * 1000));
					UI::endPropertyColumns();
				}

				if (ImGui::CollapsingHeader("Music", headerFlags))
				{
					UI::beginPropertyColumns();
					UI::addReadOnlyProperty("Music Initialized", boolToString(context.audio.isMusicInitialized()));
					UI::addReadOnlyProperty("Music Filename", context.audio.musicBuffer.name);

					float musicTime = context.audio.getMusicPosition(), musicLength = context.audio.getMusicLength();
					int musicTimeSeconds = static_cast<int>(musicTime), musicLengthSeconds = static_cast<int>(musicLength);
					UI::addReadOnlyProperty("Music Time",IO::formatString("%02d:%02d/%02d:%02d",
						musicTimeSeconds, static_cast<int>((musicTime - musicTimeSeconds) * 100),
						musicLengthSeconds, static_cast<int>((musicLength - musicLengthSeconds) * 100)
					));

					UI::addReadOnlyProperty("Sample Rate", context.audio.musicBuffer.sampleRate);
					UI::addReadOnlyProperty("Effective Sample Rate", context.audio.musicBuffer.effectiveSampleRate);
					UI::addReadOnlyProperty("Channel Count", context.audio.musicBuffer.channelCount);
					UI::endPropertyColumns();
				}

				if (ImGui::CollapsingHeader("Waveform", headerFlags))
				{
					UI::beginPropertyColumns();
					UI::addReadOnlyProperty("Waveform L", boolToString(!context.waveformL.isEmpty()));
					UI::addReadOnlyProperty("Waveform L Mip Count", context.waveformL.getUsedMipCount());
					UI::addReadOnlyProperty("Waveform L Samples", context.waveformL.mips->absoluteSamples.size());
					UI::addReadOnlyProperty("Waveform R", boolToString(!context.waveformR.isEmpty()));
					UI::addReadOnlyProperty("Waveform R Mip Count", context.waveformR.getUsedMipCount());
					UI::addReadOnlyProperty("Waveform R Samples", context.waveformL.mips->absoluteSamples.size());
					UI::endPropertyColumns();

					if (ImGui::Button("Re-Generate Waveform", { -1, UI::btnSmall.y }))
					{
						context.waveformL.generateMipChainsFromSampleBuffer(context.audio.musicBuffer, 0);
						context.waveformR.generateMipChainsFromSampleBuffer(context.audio.musicBuffer, 1);
					}
				}

				if (ImGui::CollapsingHeader("Sound Test", headerFlags))
				{
					constexpr ImGuiTableFlags tableFlags =
						ImGuiTableFlags_BordersOuter |
						ImGuiTableFlags_BordersInnerH |
						ImGuiTableFlags_BordersInnerV |
						ImGuiTableFlags_ScrollY |
						ImGuiTableFlags_RowBg;

					const int rowHeight = ImGui::GetFrameHeight() + 5;

					if (ImGui::BeginTable("##sound_test_table", 3, tableFlags, { -1, 200 }))
					{
						ImGui::TableSetupScrollFreeze(0, 1);
						ImGui::TableSetupColumn("Name");
						ImGui::TableSetupColumn("Duration (sec)", ImGuiTableColumnFlags_WidthFixed);
						ImGui::TableSetupColumn("Play/Stop", ImGuiTableColumnFlags_WidthFixed);
						ImGui::TableHeadersRow();

						for (size_t i = 0; i < arrayLength(SE_NAMES) * Audio::soundEffectsProfileCount; i++)
						{
							Audio::SoundInstance& sound = context.audio.debugSounds[i];

							ImGui::PushID(i);
							ImGui::TableNextRow(0, rowHeight);
							ImGui::TableSetColumnIndex(0);

							float ratio = sound.getCurrentFrame() / static_cast<float>(sound.getLengthInFrames());
							if (!sound.isPlaying())
								ratio = 0.0f;

							ImGui::ProgressBar(ratio, { -1, 0 }, sound.name.c_str());
							
							float duration = sound.getDuration(); int durationSecondsOnly = duration;
							float time = sound.isPlaying() ? sound.getCurrentTime() : 0.0f; int timeSecondsOnly = time;
							ImGui::TableSetColumnIndex(1);
							ImGui::Text("%02d:%02d/%02d:%02d",
								timeSecondsOnly,
								static_cast<int>((time - timeSecondsOnly) * 100),
								durationSecondsOnly,
								static_cast<int>((duration - durationSecondsOnly) * 100)
							);

							ImGui::TableSetColumnIndex(2);
							if (UI::transparentButton(ICON_FA_PLAY, UI::btnSmall))
							{
								sound.seek(0);
								sound.play();
							}

							ImGui::SameLine();
							ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
							ImGui::SameLine();
							if (UI::transparentButton(ICON_FA_STOP, UI::btnSmall))
								sound.stop();

							ImGui::PopID();
						}

						ImGui::EndTable();
					}
				}

				ImGui::TreePop();
			}

			if (ImGui::TreeNodeEx("Timeline", treeNodeFlags))
			{
				timeline.debug(context);
				ImGui::TreePop();
			}
		}

		ImGui::End();
	}

	void SettingsWindow::updateKeyConfig(MultiInputBinding* bindings[], int count)
	{
		ImVec2 size = ImVec2(-1, ImGui::GetContentRegionAvail().y * 0.7);
		constexpr ImGuiTableFlags tableFlags =
			ImGuiTableFlags_BordersOuter |
			ImGuiTableFlags_BordersInnerH |
			ImGuiTableFlags_ScrollY |
			ImGuiTableFlags_RowBg;

		constexpr ImGuiSelectableFlags selectionFlags =
			ImGuiSelectableFlags_SpanAllColumns |
			ImGuiSelectableFlags_AllowOverlap;

		const int rowHeight = ImGui::GetFrameHeight() + 5;

		if (ImGui::BeginTable("##commands_table", 2, tableFlags, size))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn(getString("action"));
			ImGui::TableSetupColumn(getString("keys"));
			ImGui::TableHeadersRow();

			keyConfigListClipper.Begin(count, rowHeight);
			while (keyConfigListClipper.Step())
			{
				for (int i = keyConfigListClipper.DisplayStart; i < keyConfigListClipper.DisplayEnd; ++i)
				{
					ImGui::TableNextRow(0, rowHeight);

					ImGui::TableSetColumnIndex(0);
					ImGui::PushID(i);
					if (ImGui::Selectable(getString(bindings[i]->name), i == selectedBindingIndex, selectionFlags))
					{
						listeningForInput = false;
						selectedBindingIndex = i;
					}

					ImGui::PopID();
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%s", ToFullShortcutsString(*bindings[i]).c_str());
				}
			}
			keyConfigListClipper.End();
			ImGui::EndTable();
		}
		ImGui::Separator();

		if (selectedBindingIndex > -1 && selectedBindingIndex < count)
		{
			int deleteBinding = -1;
			int moveIndex = -1;
			int moveDirection = 0;

			const float btnHeight = ImGui::GetFrameHeight();

			UI::beginPropertyColumns();
			ImGui::Text("%s", getString(bindings[selectedBindingIndex]->name));
			ImGui::NextColumn();

			ImGui::BeginDisabled(!bindings[selectedBindingIndex]->canAdd());
			if (ImGui::Button(getString("add"), {-1, btnHeight}))
				bindings[selectedBindingIndex]->addBinding(InputBinding{});
			ImGui::EndDisabled();

			UI::endPropertyColumns();

			ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5, 5));
			ImGui::BeginChild("##binding_keys_edit_window", ImVec2(-1, -1), true);

			float btnWidth = (UI::btnSmall.x * 2) + 100 + (ImGui::GetStyle().ItemSpacing.x * 3);
			for (int b = 0; b < bindings[selectedBindingIndex]->getCount(); ++b)
			{
				const bool canMoveDown = !(bindings[selectedBindingIndex]->getCount() <= b + 1);
				const bool canMoveUp = !(b < 1);
				ImGui::PushID(b);

				std::string buttonText = ToShortcutString(bindings[selectedBindingIndex]->bindings[b]);;
				if (!buttonText.size())
					buttonText = getString("none");

				if (listeningForInput && editBindingIndex == b)
					buttonText = getString("cmd_key_listen");

				if (ImGui::Button(buttonText.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - btnWidth, btnHeight)))
				{
					listeningForInput = true;
					inputTimer.reset();
					editBindingIndex = b;
				}

				ImGui::SameLine();
				ImGui::BeginDisabled(!canMoveUp);
				if (ImGui::Button(ICON_FA_CARET_UP, { btnHeight, btnHeight }))
				{
					listeningForInput = false;
					moveIndex = b;
					moveDirection = -1;
				}
				ImGui::EndDisabled();

				ImGui::SameLine();
				ImGui::BeginDisabled(!canMoveDown);
				if (ImGui::Button(ICON_FA_CARET_DOWN, { btnHeight, btnHeight }))
				{
					listeningForInput = false;
					moveIndex = b;
					moveDirection = 1;
				}
				ImGui::EndDisabled();

				ImGui::SameLine();
				if (ImGui::Button(getString("remove"), { -1, btnHeight }))
					deleteBinding = b;

				ImGui::PopID();
			}
			ImGui::EndChild();
			ImGui::PopStyleVar();
			ImGui::PopStyleColor();

			if (moveIndex > -1)
			{
				listeningForInput = false;
				if (moveDirection == -1) bindings[selectedBindingIndex]->moveUp(moveIndex);
				if (moveDirection == 1) bindings[selectedBindingIndex]->moveDown(moveIndex);
			}

			if (deleteBinding > -1)
			{
				listeningForInput = false;
				bindings[selectedBindingIndex]->removeAt(deleteBinding);
			}
		}

		if (listeningForInput)
		{
			if (inputTimer.elapsed() >= inputTimeoutSeconds)
			{
				listeningForInput = false;
				editBindingIndex = -1;
			}
			else
			{
				for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_MouseLeft; ++key)
				{
					bool isCtrl = key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl || key == ImGuiMod_Ctrl;
					bool isShift = key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift || key == ImGuiMod_Shift;
					bool isAlt = key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt || key == ImGuiMod_Alt;
					bool isSuper = key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper || key == ImGuiMod_Super;

					// execute if a non-modifier key is tapped
					if (ImGui::IsKeyPressed((ImGuiKey)key) && !isCtrl && !isShift && !isAlt && !isSuper)
					{
						bindings[selectedBindingIndex]->bindings[editBindingIndex] = InputBinding((ImGuiKey)key, (ImGuiKey)ImGui::GetIO().KeyMods);
						listeningForInput = false;
						editBindingIndex = -1;
						break;
					}
				}
			}
		}
	}

	void SettingsWindow::setEffectsProfile(int profile)
	{
		if (profile != config.pvEffectsProfile && profile >= 0 && profile <= 1)
		{
			isEffectProfileChangePending = true;
			config.pvEffectsProfile = profile;
		}
	}

	DialogResult SettingsWindow::update()
	{
		if (open)
		{
			ImGui::OpenPopup(MODAL_TITLE("settings"));
			open = false;
		}

		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(750, 600), ImGuiCond_Always);
		ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));

		if (ImGui::BeginPopupModal(MODAL_TITLE("settings"), NULL, ImGuiWindowFlags_NoResize))
		{
			ImVec2 padding = ImGui::GetStyle().WindowPadding;
			ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
			ImVec2 confirmBtnPos = ImGui::GetWindowSize() + ImVec2(-100, -UI::btnNormal.y) - padding;

			if (ImGui::BeginTabBar("##settings_tabs"))
			{
				const ImVec2 tabChildWindowSize = ImGui::GetContentRegionAvail() - ImVec2(0, UI::btnNormal.y + padding.y);
				if (ImGui::BeginTabItem(IMGUI_TITLE("", "general")))
				{
					ImGui::BeginChild("##settings_tab_general", tabChildWindowSize, ImGuiChildFlags_Borders);
					if (ImGui::CollapsingHeader(getString("language"), ImGuiTreeNodeFlags_DefaultOpen))
					{
						UI::beginPropertyColumns();
						UI::propertyLabel(getString("language"));

						std::string curr = getString("auto");
						auto langIt = Localization::languages.find(config.language);
						if (langIt != Localization::languages.end())
							curr = langIt->second->getDisplayName();

						if (ImGui::BeginCombo("##language", curr.c_str()))
						{
							if (ImGui::Selectable(getString("auto"), config.language == "auto"))
								config.language = "auto";

							for (const auto& [code, language] : Localization::languages)
							{
								const bool selected = curr == code;
								std::string str = language->getDisplayName();

								if (ImGui::Selectable(str.c_str(), selected))
									config.language = code;
							}

							ImGui::EndCombo();
						}

						UI::endPropertyColumns();
					}

					if (ImGui::CollapsingHeader(getString("auto_save"), ImGuiTreeNodeFlags_DefaultOpen))
					{
						UI::beginPropertyColumns();
						UI::addCheckboxProperty(getString("auto_save_enable"), config.autoSaveEnabled);
						UI::addIntProperty(getString("auto_save_interval"), config.autoSaveInterval);
						UI::addIntProperty(getString("auto_save_count"), config.autoSaveMaxCount);
						UI::endPropertyColumns();
					}

					if (ImGui::CollapsingHeader(getString("theme"), ImGuiTreeNodeFlags_DefaultOpen))
					{
						UI::beginPropertyColumns();
						UI::addSelectProperty(getString("base_theme"), config.baseTheme, baseThemes, (int)BaseTheme::BASE_THEME_MAX);
						UI::endPropertyColumns();

						ImGui::TextWrapped("%s", getString("accent_color_help"));
						ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x + 3, 15));
						ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);

						for (int i = 0; i < UI::accentColors.size(); ++i)
						{
							bool apply = false;
							std::string id = i == config.accentColor ? ICON_FA_CHECK : i == 0 ? "C" : "##" + std::to_string(i);
							ImGui::PushStyleColor(ImGuiCol_Button, UI::accentColors[i]);
							ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UI::accentColors[i]);
							ImGui::PushStyleColor(ImGuiCol_ButtonActive, UI::accentColors[i]);

							apply = ImGui::Button(id.c_str(), UI::btnNormal);

							ImGui::PopStyleColor(3);

							if ((i < UI::accentColors.size() - 1) && ImGui::GetCursorPosX() < ImGui::GetWindowSize().x - UI::btnNormal.x - 50.0f)
								ImGui::SameLine();

							if (apply)
								config.accentColor = i;
						}
						ImGui::PopStyleVar(2);

						ImVec4& customColor = UI::accentColors[0];
						float col[]{ customColor.x, customColor.y, customColor.z };
						static ColorDisplay displayMode = ColorDisplay::HEX;

						ImGui::Separator();
						ImGui::Text("%s", getString("select_accent_color"));
						UI::beginPropertyColumns();
						UI::propertyLabel(getString("display_mode"));
						if (ImGui::BeginCombo("##color_display_mode", colorDisplayStr[(int)displayMode]))
						{
							for (int i = 0; i < 3; ++i)
							{
								const bool selected = (int)displayMode == i;
								if (ImGui::Selectable(colorDisplayStr[i], selected))
									displayMode = (ColorDisplay)i;
							}

							ImGui::EndCombo();
						}
						ImGui::NextColumn();
						UI::propertyLabel(getString("custom_color"));

						ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x + 3, 15));
						ImGuiColorEditFlags flags = 1 << (20 + (int)displayMode);
						if (ImGui::ColorEdit3("##custom_accent_color", col, flags))
							customColor.x = col[0]; customColor.y = col[1]; customColor.z = col[2];

						UI::endPropertyColumns();
						ImGui::PopStyleVar();
					}

					if (ImGui::CollapsingHeader(getString("video"), ImGuiTreeNodeFlags_DefaultOpen))
					{
						bool vsync = Application::windowState.vsync;
						UI::beginPropertyColumns();
						UI::addCheckboxProperty(getString("vsync"), Application::windowState.vsync);
						UI::endPropertyColumns();

						if (vsync != Application::windowState.vsync)
						{
#if !defined(__ANDROID__) && !defined(MIKUMIKUWORLD_IOS)
							glfwSwapInterval(Application::windowState.vsync ? 1 : 0);
#endif
						}
					}

					ImGui::EndChild();
					ImGui::EndTabItem();
				}

				if (ImGui::BeginTabItem(IMGUI_TITLE("", "timeline")))
				{
					ImGui::BeginChild("##settings_tab_timeline", tabChildWindowSize, ImGuiChildFlags_Borders);
					if (ImGui::CollapsingHeader(getString("timeline"), ImGuiTreeNodeFlags_DefaultOpen))
					{
						UI::beginPropertyColumns();
						UI::addCheckboxProperty(getString("match_timeline_size_to_window"), config.matchTimelineSizeToScreen);
						UI::addCheckboxProperty(getString("match_notes_size_to_timeline"), config.matchNotesSizeToTimeline);
						UI::addIntProperty(getString("lane_width"), config.timelineWidth, "%dpx", MIN_LANE_WIDTH, MAX_LANE_WIDTH);
						
						UI::addIntProperty(getString("notes_height"), config.notesHeight, "%dpx", MIN_NOTES_HEIGHT, MAX_NOTES_HEIGHT);
						ImGui::Separator();

						UI::addCheckboxProperty(getString("draw_waveform"), config.drawWaveform);
						UI::addCheckboxProperty(getString("return_to_last_tick"), config.returnToLastSelectedTickOnPause);
						UI::addCheckboxProperty(getString("cursor_auto_scroll"), config.followCursorInPlayback);
						UI::addPercentSliderProperty(getString("cursor_auto_scroll_amount"), config.cursorPositionThreshold);
						UI::endPropertyColumns();
					}

					if (ImGui::CollapsingHeader(getString("scrolling"), ImGuiTreeNodeFlags_DefaultOpen))
					{
						UI::beginPropertyColumns();
						UI::addFloatProperty(getString("scroll_speed_normal"), config.scrollSpeedNormal, "%.1fx");
						UI::addFloatProperty(getString("scroll_speed_shift"), config.scrollSpeedShift, "%.1fx");
						config.scrollSpeedNormal = std::max(0.1f, config.scrollSpeedNormal);
						config.scrollSpeedShift = std::max(0.1f, config.scrollSpeedShift);
						ImGui::Separator();

						UI::addCheckboxProperty(getString("use_smooth_scroll"), config.useSmoothScrolling);
						UI::addSliderProperty(getString("smooth_scroll_time"), config.smoothScrollingTime, 10.0f, 150.0f, "%.2fms");
						UI::endPropertyColumns();
					}

					if (ImGui::CollapsingHeader(getString("audio"), ImGuiTreeNodeFlags_DefaultOpen))
					{
						UI::beginPropertyColumns();
						UI::addSelectProperty(getString("notes_se"), config.seProfileIndex, Audio::soundEffectsProfileNames, Audio::soundEffectsProfileCount);
						UI::endPropertyColumns();
					}

					if (ImGui::CollapsingHeader(getString("background"), ImGuiTreeNodeFlags_DefaultOpen))
					{
						UI::beginPropertyColumns();
						UI::addCheckboxProperty(getString("draw_background"), config.drawBackground);

						std::string backgroundFile = config.backgroundImage;
						int result = UI::addFileProperty(getString("background_image"), backgroundFile);
						if (result == 1)
						{
							config.backgroundImage = backgroundFile;
							isBackgroundChangePending = true;
						}
						else if (result == 2)
						{
							IO::FileDialog fileDialog{};
							fileDialog.title = "Open Image File";
							fileDialog.filters = { IO::imageFilter, IO::allFilter };
							fileDialog.parentWindowHandle = Application::windowState.windowHandle;

							if (fileDialog.openFile() == IO::FileDialogResult::OK)
							{
								config.backgroundImage = fileDialog.outputFilename;
								isBackgroundChangePending = true;
							}
						}

						UI::addPercentSliderProperty(getString("background_brightnes"), config.backgroundBrightness);
						ImGui::Separator();

						UI::addPercentSliderProperty(getString("lanes_opacity"), config.laneOpacity);
						UI::endPropertyColumns();
					}

					ImGui::EndChild();
					ImGui::EndTabItem();
				}

				if (ImGui::BeginTabItem(IMGUI_TITLE("", "preview")))
				{
					ImGui::BeginChild("##settings_tab_preview", tabChildWindowSize, ImGuiChildFlags_Borders);
					if (ImGui::CollapsingHeader(getString("general"), ImGuiTreeNodeFlags_DefaultOpen))
					{
						ImGui::TextUnformatted(getString("preview_intro_layout"));
						if (ImGui::RadioButton(getString("preview_intro_normal_layout"), !config.pvCustomChartLayout))
							config.pvCustomChartLayout = false;
						if (ImGui::RadioButton(getString("preview_intro_custom_layout"), config.pvCustomChartLayout))
							config.pvCustomChartLayout = true;
						ImGui::Spacing();
						UI::beginPropertyColumns();
						UI::addSliderProperty(getString("notes_speed"), config.pvNoteSpeed, 1, 12, "%.2f");
						UI::addSliderProperty(getString("preview_note_margin"), config.pvNoteMargin, 0.0f, 0.1f, "%.3f lanes");
#ifdef MIKUMIKUWORLD_SEKAI_LIVE_RENDERING
						UI::addCheckboxProperty(getString("preview_live_note_rendering"), config.pvLiveNoteRendering);
						if (config.pvLiveNoteRendering)
							UI::addPercentSliderProperty(getString("preview_live_note_lean"), config.pvLiveNoteLean);
#endif
						UI::addCheckboxProperty(getString("mirror_score"), config.pvMirrorScore);
						UI::addCheckboxProperty(getString("preview_draw_toolbar"), config.pvDrawToolbar);
						UI::addCheckboxProperty(getString("preview_draw_score_hud"), config.pvDrawScoreHud);
						UI::addCheckboxProperty(getString("preview_draw_score_add_animation"), config.pvDrawScorePopup);
						UI::addCheckboxProperty(getString("preview_draw_life_hud"), config.pvDrawLifeHud);
						UI::addCheckboxProperty(getString("preview_draw_combo"), config.pvDrawCombo);
						UI::addCheckboxProperty(getString("preview_draw_perfect"), config.pvDrawPerfect);
						UI::addSliderProperty("Score popup X position", config.pvScorePopupOffsetX, -400.0f, 400.0f, "%+.0f px");
						UI::addSliderProperty("Score popup Y position", config.pvScorePopupOffsetY, -250.0f, 250.0f, "%+.0f px");
						UI::addSliderProperty("Score popup size", config.pvScorePopupSize, 50.0f, 200.0f, "%.0f%%");
						UI::endPropertyColumns();
#ifdef __ANDROID__
						if (ImGui::Button("Android permissions..."))
							AndroidFileDialog_ShowPermissions();
#endif
					}

					if (ImGui::CollapsingHeader(getString("background"), ImGuiTreeNodeFlags_DefaultOpen))
					{
						UI::beginPropertyColumns();
						UI::addCheckboxProperty(getString("draw_background"), config.drawBackground);
						UI::addCheckboxProperty(getString("preview_draw_mv_line_overlay"), config.pvDrawMvLineOverlay);
						UI::addCheckboxProperty(getString("sekai_2dmv_scaling"), config.pvSekai2DMVScaling);

						std::string backgroundFile = config.backgroundImage;
						int result = UI::addFileProperty(getString("background_image"), backgroundFile);
						if (result == 1)
						{
							config.backgroundImage = backgroundFile;
							isBackgroundChangePending = true;
						}
						else if (result == 2)
						{
							IO::FileDialog fileDialog{};
							fileDialog.title = "Open Image File";
							fileDialog.filters = { IO::imageFilter, IO::allFilter };
							fileDialog.parentWindowHandle = Application::windowState.windowHandle;

							if (fileDialog.openFile() == IO::FileDialogResult::OK)
							{
								config.backgroundImage = fileDialog.outputFilename;
								isBackgroundChangePending = true;
							}
						}

						UI::addPercentSliderProperty(getString("background_brightnes"), config.pvBackgroundBrightness);
						ImGui::Separator();

						UI::addPercentSliderProperty(getString("stage_opacity"), config.pvStageOpacity);
						UI::addPercentSliderProperty(getString("stage_cover"), config.pvStageCover);
						UI::endPropertyColumns();
					}

					if (ImGui::CollapsingHeader(getString("audio"), ImGuiTreeNodeFlags_DefaultOpen))
					{
						UI::beginPropertyColumns();
						UI::addSelectProperty(getString("notes_se"), config.seProfileIndex, Audio::soundEffectsProfileNames, Audio::soundEffectsProfileCount);
						UI::endPropertyColumns();
					}
					
					if (ImGui::CollapsingHeader(getString("visuals"), ImGuiTreeNodeFlags_DefaultOpen))
					{
						const std::vector<std::string>& noteSkinNames = noteSkins.getSkinNames();

						ImGui::Text("%s", getString("notes_skin"));
						if (noteSkins.count() > 1)
						{
							float spacing = ImGui::GetContentRegionAvail().x * 0.5f;
							if (ImGui::RadioButton(getString(noteSkinNames[0]), config.notesSkin == 0))
								config.notesSkin = 0;

							ImGui::SameLine(spacing);
							if (ImGui::RadioButton(getString(noteSkinNames[1]), config.notesSkin == 1))
								config.notesSkin = 1;

							ImGui::Separator();
						}

						{
							float spacing = ImGui::GetContentRegionAvail().x * 0.5f;
							ImGui::Text("%s", getString("notes_effect"));
							if (ImGui::RadioButton(getString("effects_normal"), config.pvEffectsProfile == 0))
								setEffectsProfile(0);

							ImGui::SameLine(spacing);
							if (ImGui::RadioButton(getString("effects_reduced"), config.pvEffectsProfile == 1))
								setEffectsProfile(1);
							ImGui::Separator();
						}

						UI::beginPropertyColumns();
						UI::addCheckboxProperty(getString("flicks_animation"), config.pvFlickAnimation);
						UI::addCheckboxProperty(getString("holds_animation"), config.pvHoldAnimation);
						UI::addCheckboxProperty(getString("simultaneous_lines"), config.pvSimultaneousLine);
						ImGui::Separator();

						float hold_alpha = config.pvHoldAlpha * 100.f;
						UI::addSliderProperty(getString("holds_alpha"), hold_alpha, 10, 100, "%.0f%%");
						config.pvHoldAlpha = hold_alpha / 100.f;

						float guide_alpha = config.pvGuideAlpha * 100.f;
						UI::addSliderProperty(getString("guides_alpha"), guide_alpha, 10, 100, "%.0f%%");
						config.pvGuideAlpha = guide_alpha / 100.f;
						
						UI::endPropertyColumns();
					}
					
					ImGui::EndChild();
					ImGui::EndTabItem();
				}

				if (ImGui::BeginTabItem(IMGUI_TITLE("", "key_config")))
				{
					ImGui::BeginChild("##settings_tab_key_config", tabChildWindowSize, ImGuiChildFlags_Borders);
					updateKeyConfig(bindings, sizeof(bindings) / sizeof(MultiInputBinding*));
					
					ImGui::EndChild();
					ImGui::EndTabItem();
				}
			}

			ImGui::EndTabBar();
			ImGui::SetCursorPos(confirmBtnPos);
			if (ImGui::Button("OK", ImVec2(100, UI::btnNormal.y - 5)))
				ImGui::CloseCurrentPopup();

			ImGui::EndPopup();
		}

		ImGui::PopStyleVar();
		return DialogResult::None;
	}

	bool Mp4ExportWindow::beginExport(ScoreContext& context, ScoreEditorTimeline& timeline, ScorePreviewWindow& preview)
	{
		if (startTime < 0.0f || endTime <= startTime || timeline.isPlaying())
			return false;

		static constexpr int widths[] = { 1920, 1280, 3840, 1080 };
		static constexpr int heights[] = { 1080, 720, 2160, 1920 };
		static constexpr int frameRates[] = { 24, 30, 60, 120 };
		if (resolutionIndex == 4)
		{
			if (!isValidCustomMp4Resolution(customWidth, customHeight))
				return false;
			outputWidth = customWidth;
			outputHeight = customHeight;
		}
		else
		{
			const int presetIndex = std::clamp(resolutionIndex, 0, 3);
			outputWidth = widths[presetIndex];
			outputHeight = heights[presetIndex];
		}
		outputFps = frameRates[std::clamp(fpsIndex, 0, 3)];
		introFrames = includeSekaiIntro
			? static_cast<uint64_t>(std::ceil(std::max(0.1f, sekaiIntroDuration) * outputFps))
			: 0;
		chartLeadInFrames = static_cast<uint64_t>(std::ceil(std::clamp(chartLeadInDuration, 0.0f, 30.0f) * outputFps));
		const float introDuration = static_cast<float>(introFrames) / outputFps;
		const float chartStartOffset = introDuration + static_cast<float>(chartLeadInFrames) / outputFps;

		IO::FileDialog fileDialog{};
		fileDialog.title = "Export as MP4";
		fileDialog.filters = { { "MP4 Video", "*.mp4" } };
		fileDialog.defaultExtension = "mp4";
		fileDialog.parentWindowHandle = Application::windowState.windowHandle;
		fileDialog.inputFilename = IO::File::getFilenameWithoutExtension(context.workingData.filename);
		if (fileDialog.saveFile() != IO::FileDialogResult::OK)
			return false;

		outputFilename = fileDialog.outputFilename;
		const float chartDuration = endTime - startTime;
		const uint64_t chartFrameCount = static_cast<uint64_t>(std::ceil(chartDuration * outputFps));
		totalFrames = introFrames + chartLeadInFrames + chartFrameCount;
		const float outputDuration = static_cast<float>(totalFrames) / outputFps;
		framesWritten = 0;

		std::string audioFilename;
		std::string soundEffectsFilename;
		std::string error;
		float audioStart = 0.0f;
		float audioDelay = 0.0f;
		if (includeAudio)
		{
			if (!context.workingData.musicFilename.empty() && IO::File::exists(context.workingData.musicFilename))
			{
				audioFilename = context.workingData.musicFilename;
				const float musicOffset = context.workingData.musicOffset / 1000.0f;
				audioStart = std::max(0.0f, startTime - musicOffset);
				audioDelay = chartStartOffset + std::max(0.0f, musicOffset - startTime);
			}

			std::vector<Audio::OfflineSoundEffectEvent> soundEvents;
			for (const auto& noteEntry : context.score.notes)
			{
				const Note& note = noteEntry.second;
				const float noteTime = accumulateDuration(note.tick, TICKS_PER_BEAT, context.score.tempoChanges);
				const float noteRelativeTime = chartStartOffset + noteTime - startTime;
				const float noteEndTime = noteRelativeTime;
				bool playNoteSound = true;
				if (note.getType() == NoteType::Hold)
					playNoteSound = context.score.holdNotes.at(note.ID).startType == HoldNoteType::Normal;
				else if (note.getType() == NoteType::HoldEnd)
					playNoteSound = context.score.holdNotes.at(note.parentID).endType == HoldNoteType::Normal;

				if (playNoteSound && noteTime >= startTime && noteTime < endTime)
				{
					const std::string_view sound = getNoteSE(note, context.score);
					if (!sound.empty())
						soundEvents.push_back({ std::string(sound), noteEndTime, -1.0f });
				}

				if (note.getType() == NoteType::Hold && !context.score.holdNotes.at(note.ID).isGuide())
				{
					const int holdEndTick = context.score.notes.at(context.score.holdNotes.at(note.ID).end).tick;
					const float holdEndTime = accumulateDuration(holdEndTick, TICKS_PER_BEAT, context.score.tempoChanges);
					if (noteTime < endTime && holdEndTime > startTime)
					{
						const float holdStart = chartStartOffset + std::max(0.0f, noteTime - startTime);
						const float holdEnd = chartStartOffset + std::min(chartDuration, holdEndTime - startTime);
						if (holdEnd > holdStart)
							soundEvents.push_back({ note.critical ? SE_CRITICAL_CONNECT : SE_CONNECT, holdStart, holdEnd });
					}
				}
			}

			if (!soundEvents.empty())
			{
				std::vector<Audio::OfflineSoundEffectVolumeChange> volumeChanges;
				const int startTick = accumulateTicks(startTime, TICKS_PER_BEAT, context.score.tempoChanges);
				const int startVolumeIndex = findSEVolumeChange(startTick, context.score.seVolumeChanges);
				volumeChanges.push_back({ chartStartOffset, startVolumeIndex == -1 ? 1.0f : context.score.seVolumeChanges[startVolumeIndex].volume });
				for (const auto& volumeChange : context.score.seVolumeChanges)
				{
					const float changeTime = accumulateDuration(volumeChange.tick, TICKS_PER_BEAT, context.score.tempoChanges);
					if (changeTime > startTime && changeTime < endTime)
						volumeChanges.push_back({ chartStartOffset + changeTime - startTime, volumeChange.volume });
				}
				std::stable_sort(volumeChanges.begin(), volumeChanges.end(), [](const auto& a, const auto& b) { return a.time < b.time; });

				try
				{
					const auto tempPath = std::filesystem::temp_directory_path() /
						("mmw_mp4_sfx_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
					soundEffectsFilename = tempPath.u8string();
				}
				catch (const std::filesystem::filesystem_error&)
				{
					resultMessage = "Could not create a temporary sound effects track.";
					return false;
				}

				if (!context.audio.renderSoundEffectsToWav(soundEffectsFilename, outputDuration, soundEvents, volumeChanges, error))
				{
					std::error_code removeError;
					std::filesystem::remove(std::filesystem::u8path(soundEffectsFilename), removeError);
					soundEffectsFilename.clear();
					resultMessage = error;
					return false;
				}
			}
		}

		if (!encoder.start(outputFilename, outputWidth, outputHeight, outputFps,
			audioFilename, audioStart, audioDelay, soundEffectsFilename, outputDuration, error))
		{
			if (!soundEffectsFilename.empty())
			{
				std::error_code removeError;
				std::filesystem::remove(std::filesystem::u8path(soundEffectsFilename), removeError);
				soundEffectsFilename.clear();
			}
			resultMessage = error;
			return false;
		}

		originalTick = context.currentTick;
		originalPlaybackSpeed = timeline.getPlaybackSpeed();
		originalStopAtEnd = config.stopAtEnd;
		config.stopAtEnd = false;
		timeline.setPlaybackSpeed(context, 1.0f);
		timeline.setTime(context, startTime);
		timeline.setPlaying(context, true);
		preview.setExportResolution(outputWidth, outputHeight, includeUI);
		preview.setExportSekaiIntro(includeSekaiIntro, 0.0f, introDuration, sekaiIntroTitle, sekaiIntroDesigner,
			sekaiIntroLyricist, sekaiIntroComposer, sekaiIntroArrangement, sekaiIntroVocal,
			sekaiIntroCollabTag, sekaiIntroDifficulty, sekaiIntroChartId);
		preview.setExportStageFadeIn(1.0f);
		resultMessage.clear();
		exporting = true;
		#if defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
		AndroidFileDialog_ShowExportVideoPlayer();
		AndroidFileDialog_UpdateExportProgress(0, totalFrames);
		#endif
		return true;
	}

	void Mp4ExportWindow::finishExport(ScoreContext& context, ScoreEditorTimeline& timeline, ScorePreviewWindow& preview, bool cancelled)
	{
		#if defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
		if (!cancelled)
			AndroidFileDialog_UpdateExportProgress(framesWritten, totalFrames, true);
		#endif
		std::string error;
		bool success = false;
		if (cancelled)
			encoder.cancel();
		else
			success = encoder.finish(error);
		#if defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
		if (success && !AndroidFileDialog_Commit(outputFilename))
		{
			success = false;
			error = "Could not save the exported MP4 to the selected location.";
		}
		#endif
		if (!soundEffectsFilename.empty())
		{
			std::error_code removeError;
			std::filesystem::remove(std::filesystem::u8path(soundEffectsFilename), removeError);
			soundEffectsFilename.clear();
		}

		if (timeline.isPlaying())
			timeline.setPlaying(context, false);
		context.currentTick = originalTick;
		timeline.setTime(context, static_cast<float>(accumulateDuration(originalTick, TICKS_PER_BEAT, context.score.tempoChanges)));
		timeline.setPlaybackSpeed(context, originalPlaybackSpeed);
		config.stopAtEnd = originalStopAtEnd;
		preview.clearExportResolution();
		exporting = false;
		captureThisFrame = false;
		// Keep the common 1080p buffers for the next export, but release very
		// large custom-resolution allocations so they don't stay resident.
		constexpr size_t MAX_RETAINED_FRAME_BUFFERS = 64ull * 1024ull * 1024ull;
		if (framePixels.capacity() > MAX_RETAINED_FRAME_BUFFERS)
			std::vector<uint8_t>().swap(framePixels);
		if (cancelled)
			resultMessage = getString("export_mp4_cancelled");
		else if (success)
			resultMessage = getString("export_mp4_success");
		else
			resultMessage = error.empty() ? getString("export_mp4_failed") : error;
#if defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
		AndroidFileDialog_HideExportVideoPlayer();
#endif
	}

	void Mp4ExportWindow::prepareFrame(ScoreContext& context, ScoreEditorTimeline& timeline, ScorePreviewWindow& preview)
	{
		captureThisFrame = false;
		if (!exporting)
			return;

		if (framesWritten >= totalFrames)
			return;

		captureThisFrame = true;
		const bool drawIntroFrame = framesWritten < introFrames;
		const uint64_t chartStartFrame = introFrames + chartLeadInFrames;
		preview.setExportSekaiIntro(drawIntroFrame,
			static_cast<float>(framesWritten) / outputFps,
			static_cast<float>(introFrames) / outputFps,
			sekaiIntroTitle, sekaiIntroDesigner,
			sekaiIntroLyricist, sekaiIntroComposer, sekaiIntroArrangement, sekaiIntroVocal,
			sekaiIntroCollabTag, sekaiIntroDifficulty, sekaiIntroChartId);
		const uint64_t chartFrame = framesWritten >= chartStartFrame ? framesWritten - chartStartFrame : 0;
		const float fadeFrames = std::max(1.0f, outputFps * 1.6f);
		const uint64_t framesAfterIntro = framesWritten >= introFrames ? framesWritten - introFrames : 0;
		preview.setExportStageFadeIn(introFrames > 0 && !drawIntroFrame
			? std::clamp(static_cast<float>(framesAfterIntro) / fadeFrames, 0.0f, 1.0f)
			: 1.0f);
		timeline.setTime(context, startTime + static_cast<float>(chartFrame) / outputFps);
	}

	void Mp4ExportWindow::captureFrame(ScoreContext& context, ScoreEditorTimeline& timeline, ScorePreviewWindow& preview)
	{
		if (!exporting || !captureThisFrame)
			return;

		preview.readPreviewPixels(framePixels);
		if (!encoder.writeFrame(framePixels))
		{
			// A failed frame means FFmpeg stopped consuming the input pipe. Do not
			// run the normal finish path here: it waits for FFmpeg to drain and can
			// block the Android render thread while the app appears black.
			finishExport(context, timeline, preview, true);
			resultMessage = getString("export_mp4_failed");
			return;
		}
		++framesWritten;
#if defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
		AndroidFileDialog_UpdateExportProgress(framesWritten, totalFrames);
#endif

		captureThisFrame = false;
		if (framesWritten >= totalFrames)
			finishExport(context, timeline, preview, false);
	}

	void Mp4ExportWindow::update(ScoreContext& context, ScoreEditorTimeline& timeline, ScorePreviewWindow& preview)
	{
		if (open)
		{
			sekaiIntroTitle = context.workingData.title;
			sekaiIntroDesigner = context.workingData.designer;
			sekaiIntroVocal = context.workingData.artist;
			sekaiIntroChartId = context.workingData.musicId > 0 ? std::to_string(context.workingData.musicId) : "";
			ImGui::OpenPopup(MODAL_TITLE("export_mp4"));
			open = false;
		}

		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
		ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 0.0f), ImVec2(560.0f, 760.0f));
		ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
		if (ImGui::BeginPopupModal(MODAL_TITLE("export_mp4"), NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize))
		{
			if (exporting)
			{
				ImGui::TextWrapped("%s", getString("export_mp4_progress"));
				ImGui::ProgressBar(totalFrames > 0 ? (float)framesWritten / (float)totalFrames : 0.0f, ImVec2(-1, 0));
				if (ImGui::Button(getString("cancel"), ImVec2(120, ImGui::GetFrameHeight())))
					finishExport(context, timeline, preview, true);
				ImGui::EndPopup();
				return;
			}

			if (!resultMessage.empty())
			{
				ImGui::TextWrapped("%s", resultMessage.c_str());
				if (ImGui::Button(getString("close"), ImVec2(-1, ImGui::GetFrameHeight())))
				{
					resultMessage.clear();
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
				return;
			}

			const char* resolutions[] = { "1920 x 1080", "1280 x 720", "3840 x 2160", "1080 x 1920", getString("export_mp4_custom_resolution") };
			static const char* frameRates[] = { "24 FPS", "30 FPS", "60 FPS", "120 FPS" };

			ImGui::TextUnformatted(getString("export_mp4_resolution"));
			ImGui::SetNextItemWidth(-1);
			ImGui::Combo("##mp4_resolution", &resolutionIndex, resolutions, IM_ARRAYSIZE(resolutions));
			bool customResolutionValid = true;
			if (resolutionIndex == 4)
			{
				ImGui::SetNextItemWidth(180.0f);
				ImGui::InputInt(getString("export_mp4_width"), &customWidth, 2, 16);
				ImGui::SetNextItemWidth(180.0f);
				ImGui::InputInt(getString("export_mp4_height"), &customHeight, 2, 16);
				customResolutionValid = isValidCustomMp4Resolution(customWidth, customHeight);
				if (!customResolutionValid)
					ImGui::TextWrapped("%s", getString("export_mp4_resolution_invalid"));
			}

			ImGui::TextUnformatted(getString("export_mp4_fps"));
			ImGui::SetNextItemWidth(-1);
			ImGui::Combo("##mp4_fps", &fpsIndex, frameRates, IM_ARRAYSIZE(frameRates));

			ImGui::TextUnformatted(getString("export_mp4_range"));
			ImGui::SetNextItemWidth(150.0f);
			ImGui::InputFloat("##mp4_start", &startTime, 1.0f, 5.0f, "%.2f s");
			ImGui::SameLine();
			ImGui::TextUnformatted("to");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(150.0f);
			ImGui::InputFloat("##mp4_end", &endTime, 1.0f, 5.0f, "%.2f s");

			ImGui::Checkbox(getString("export_mp4_audio"), &includeAudio);
			ImGui::Checkbox(getString("export_mp4_ui"), &includeUI);
			ImGui::SetNextItemWidth(180.0f);
			ImGui::InputFloat("Chart lead-in (seconds)", &chartLeadInDuration, 0.5f, 1.0f, "%.1f s");
			chartLeadInDuration = std::clamp(chartLeadInDuration, 0.0f, 30.0f);
			ImGui::TextWrapped("The video preview is visible during this lead-in; chart audio starts afterward.");
			ImGui::Checkbox("Use SEKAI Intro", &includeSekaiIntro);
			if (includeSekaiIntro)
			{
				ImGui::Indent();
				ImGui::SetNextItemWidth(160.0f);
				ImGui::InputFloat("Intro duration (seconds)", &sekaiIntroDuration, 0.5f, 1.0f, "%.1f s");
				sekaiIntroDuration = std::clamp(sekaiIntroDuration, 0.1f, 30.0f);
				ImGui::TextWrapped("Original timing: 5 seconds. Custom duration scales the intro animation.");
				static const char* difficulties[] = { "EASY", "NORMAL", "HARD", "EXPERT", "MASTER", "APPEND", "ETERNAL" };
				ImGui::SetNextItemWidth(180.0f);
				ImGui::Combo("Difficulty", &sekaiIntroDifficulty, difficulties, IM_ARRAYSIZE(difficulties));
				ImGui::TextUnformatted(getString(config.pvCustomChartLayout ? "preview_intro_custom_layout" : "preview_intro_normal_layout"));
				ImGui::InputText("Title", &sekaiIntroTitle);
				if (config.pvCustomChartLayout)
				{
					ImGui::InputText("Chart Designer", &sekaiIntroDesigner);
					ImGui::InputText("Chart ID (optional)", &sekaiIntroChartId);
				}
				ImGui::InputText("Lyricist", &sekaiIntroLyricist);
				ImGui::InputText("Composer", &sekaiIntroComposer);
				ImGui::InputText("Arrangement", &sekaiIntroArrangement);
				ImGui::InputText("Vocal", &sekaiIntroVocal);
				ImGui::InputText("Collab tag (optional)", &sekaiIntroCollabTag);
				ImGui::TextWrapped("The intro plays before the selected chart range. The overlay assets come from pjsekai-overlay-APPEND; its terms request attribution in your video description.");
				ImGui::Unindent();
			}
			ImGui::Separator();
#ifdef MIKUMIKUWORLD_IOS
			const char* encoderStatus = "iOS video encoder is ready. Export pauses when you switch apps and resumes when you return.";
#else
			const char* encoderStatus = FfmpegEncoder::isAvailable()
				? getString("export_mp4_encoder_ready")
				: getString("export_mp4_encoder_unavailable");
#endif
			ImGui::TextWrapped("%s", encoderStatus);
			ImGui::TextWrapped("%s", getString("export_mp4_scope"));
			if (includeAudio && (context.workingData.musicFilename.empty() || !IO::File::exists(context.workingData.musicFilename)))
				ImGui::TextWrapped("%s", getString("export_mp4_no_music"));

			const ImVec2 buttonSize{ 120.0f, ImGui::GetFrameHeight() };
			if (ImGui::Button(getString("cancel"), buttonSize))
				ImGui::CloseCurrentPopup();
			ImGui::SameLine();
			const bool canExport = FfmpegEncoder::isAvailable() && endTime > startTime && startTime >= 0.0f && !timeline.isPlaying() && customResolutionValid;
			ImGui::BeginDisabled(!canExport);
			if (ImGui::Button(getString("export_mp4"), buttonSize))
				beginExport(context, timeline, preview);
			ImGui::EndDisabled();
			ImGui::EndPopup();
		}
	}
}
