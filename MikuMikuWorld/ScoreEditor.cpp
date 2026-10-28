#ifdef __ANDROID__
#include "AndroidFileDialog.h"
#endif
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <system_error>
#ifdef __ANDROID__
#include "AndroidImGuiBackend.h"
#endif
#include <chrono>
extern double gExpTimelineMs, gExpPreviewMs;
#include "Application.h"
#include "ApplicationConfiguration.h"
#if defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
#include "AndroidFileDialog.h"
#endif
#include "File.h"
#include "UI.h"
#include "Constants.h"
#include "Utilities.h"
#include "ImageCrop.h"
#include "NativeScoreSerializer.h"
#include "ScoreSerializeWindow.h"
#include <filesystem>
#include <cstdlib>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace MikuMikuWorld
{
	static MultiInputBinding* timelineModeBindings[] =
	{
		&config.input.timelineSelect,
		&config.input.timelineTap,
		&config.input.timelineHold,
		&config.input.timelineHoldMid,
		&config.input.timelineFlick,
		&config.input.timelineCritical,
		&config.input.timelineFriction,
		&config.input.timelineGuide,
		&config.input.timelineBpm,
		&config.input.timelineTimeSignature,
		&config.input.timelineHiSpeed,
		&config.input.timelineSEVolume
	};

	ScoreEditor::ScoreEditor() : presetManager(Application::getUserDataDir() + "library")
	{
		renderer = std::make_unique<Renderer>();

		context.audio.initializeAudioEngine();
		context.audio.setMasterVolume(config.masterVolume);
		context.audio.setMusicVolume(config.bgmVolume);
		context.audio.setSoundEffectsVolume(config.seVolume);
		context.audio.loadSoundEffects();
		context.audio.setSoundEffectsProfileIndex(config.seProfileIndex);

		timeline.setDivision(config.division);
		timeline.setZoom(config.zoom);

		autoSavePath = Application::getUserDataDir() + "auto_save";

                {
                        std::error_code ec;
                        const std::string marker = autoSavePath + "/dirty_session";
                        if (std::filesystem::exists(marker, ec))
                        {
                                std::filesystem::path newest;
                                std::filesystem::file_time_type newestTime;
                                for (const auto& f : std::filesystem::directory_iterator(autoSavePath, ec))
                                {
                                        if (f.path().extension() != MMWS_EXTENSION)
                                                continue;
                                        const auto t = f.last_write_time();
                                        if (newest.empty() || t > newestTime)
                                        {
                                                newest = f.path();
                                                newestTime = t;
                                        }
                                }
                                if (newest.empty())
                                        std::filesystem::remove(marker, ec);
                                else
                                {
                                        recoverFile = newest.string();
                                        recoverPending = true;
                                }
                        }
                }
		autoSaveTimer.reset();

		preview.loadNoteEffects(context.scorePreviewDrawData.effectView);
	}

	void ScoreEditor::writeSettings()
	{
		config.masterVolume = context.audio.getMasterVolume();
		config.bgmVolume = context.audio.getMusicVolume();
		config.seVolume = context.audio.getSoundEffectsVolume();

		config.division = timeline.getDivision();
		config.zoom = timeline.getZoom();
	}

	void ScoreEditor::uninitialize()
	{
                {
                        std::error_code ec;
                        std::filesystem::remove(autoSavePath + "/dirty_session", ec);
                }
		context.audio.uninitializeAudioEngine();
		timeline.background.dispose();
	}

	void ScoreEditor::update()
	{
		if (!isFullScreenPreview())
		{
			drawMenubar();
			drawToolbar();
		}

		if (!ImGui::GetIO().WantCaptureKeyboard && !mp4ExportWindow.isExporting())
		{
			if (ImGui::IsAnyPressed(config.input.create)) Application::windowState.resetting = true;
			if (ImGui::IsAnyPressed(config.input.open))
			{
				Application::windowState.resetting = true;
				Application::windowState.shouldPickScore = true;
			}

			if (ImGui::IsAnyPressed(config.input.openSettings)) settingsWindow.open = true;
			if (ImGui::IsAnyPressed(config.input.openHelp)) help();
			if (ImGui::IsAnyPressed(config.input.save)) trySave(context.workingData.filename);
			if (ImGui::IsAnyPressed(config.input.saveAs)) saveAs();
			if (ImGui::IsAnyPressed(config.input.exportScore)) exportScore();
			if (ImGui::IsAnyPressed(config.input.togglePlayback)) timeline.setPlaying(context, !timeline.isPlaying());
			if (ImGui::IsAnyPressed(config.input.stop)) timeline.stop(context);
			if (ImGui::IsAnyPressed(config.input.previousTick, true)) timeline.previousTick(context);
			if (ImGui::IsAnyPressed(config.input.nextTick, true)) timeline.nextTick(context);
			if (ImGui::IsAnyPressed(config.input.togglePreviewFullWindow, false)) preview.setFullWindow(!preview.isFullWindow());

			if (!preview.isFullWindow())
			{
				if (ImGui::IsAnyPressed(config.input.selectAll)) context.selectAll();
				if (ImGui::IsAnyPressed(config.input.deleteSelection))
				{
					const bool deletedHiSpeed = timeline.deleteSelectedHiSpeed(context);
					const bool deletedSEVolume = timeline.deleteSelectedSEVolume(context);
					if (!deletedHiSpeed && !deletedSEVolume) context.deleteSelection();
				}
				if (ImGui::IsAnyPressed(config.input.cutSelection) && !timeline.cutSelectedHiSpeed(context) && !timeline.cutSelectedSEVolume(context)) context.cutSelection();
				if (ImGui::IsAnyPressed(config.input.copySelection) && !timeline.copySelectedHiSpeed(context) && !timeline.copySelectedSEVolume(context)) context.copySelection();
				if (ImGui::IsAnyPressed(config.input.paste) && !timeline.pasteHiSpeed(context, false) && !timeline.pasteSEVolume(context, false)) context.paste(false);
				if (ImGui::IsAnyPressed(config.input.flipPaste) && !timeline.pasteHiSpeed(context, true) && !timeline.pasteSEVolume(context, true)) context.paste(true);
				if (ImGui::IsAnyPressed(config.input.cancelPaste)) context.cancelPaste();
				if (ImGui::IsAnyPressed(config.input.flip)) context.flipSelection();
				if (ImGui::IsAnyPressed(config.input.undo)) context.undo();
				if (ImGui::IsAnyPressed(config.input.redo)) context.redo();
				if (ImGui::IsAnyPressed(config.input.zoomOut, true)) timeline.setZoom(timeline.getZoom() - 0.25f);
				if (ImGui::IsAnyPressed(config.input.zoomIn, true)) timeline.setZoom(timeline.getZoom() + 0.25f);
				if (ImGui::IsAnyPressed(config.input.decreaseNoteSize, true)) edit.noteWidth = std::clamp(edit.noteWidth - 1, MIN_NOTE_WIDTH, MAX_NOTE_WIDTH);
				if (ImGui::IsAnyPressed(config.input.increaseNoteSize, true)) edit.noteWidth = std::clamp(edit.noteWidth + 1, MIN_NOTE_WIDTH, MAX_NOTE_WIDTH);
				if (ImGui::IsAnyPressed(config.input.shrinkDown)) context.shrinkSelection(Direction::Down);
				if (ImGui::IsAnyPressed(config.input.shrinkUp)) context.shrinkSelection(Direction::Up);
				if (ImGui::IsAnyPressed(config.input.connectHolds)) context.connectHoldsInSelection();
				if (ImGui::IsAnyPressed(config.input.splitHold)) context.splitHoldInSelection();

				for (int i = 0; i < (int)TimelineMode::TimelineModeMax; ++i)
					if (ImGui::IsAnyPressed(*timelineModeBindings[i])) timeline.changeMode((TimelineMode)i, edit);

				if (ImGui::IsAnyPressed(config.input.insertSkill)) timeline.insertSkill(context, context.currentTick);
				if (ImGui::IsAnyPressed(config.input.insertFever)) timeline.beginInsertFever(context, context.currentTick);
			}
		}

		if (config.matchTimelineSizeToScreen)
		{
			int laneWidth = static_cast<int>(timeline.getWidth() * 0.45f / NUM_LANES);
			timeline.laneWidth = std::clamp(laneWidth, MIN_LANE_WIDTH, MAX_LANE_WIDTH);
		}
		else
		{
			timeline.laneWidth = config.timelineWidth;
		}

		timeline.notesHeight = config.matchNotesSizeToTimeline ? std::clamp(static_cast<int>(timeline.laneWidth), MIN_NOTES_HEIGHT, MAX_NOTES_HEIGHT) : config.notesHeight;

		if (config.backgroundBrightness != timeline.background.getBrightness())
			timeline.background.setBrightness(config.backgroundBrightness);

		if (settingsWindow.isBackgroundChangePending)
		{
			static const std::string defaultBackgroundPath = Application::getAppDir() + "res/editor/default.png";
			timeline.background.load(config.backgroundImage.empty() ? defaultBackgroundPath : config.backgroundImage);
			settingsWindow.isBackgroundChangePending = false;
		}

		if (config.seProfileIndex != context.audio.getSoundEffectsProfileIndex())
		{
			context.audio.stopSoundEffects(false);
			context.audio.setSoundEffectsProfileIndex(config.seProfileIndex);
		}

		if (settingsWindow.isEffectProfileChangePending)
		{
			preview.loadNoteEffects(context.scorePreviewDrawData.effectView);
			debugEffectView.loadEffects();
			settingsWindow.isEffectProfileChangePending = false;
		}

		if (propertiesWindow.isPendingLoadMusic)
		{
			asyncLoadMusic(propertiesWindow.pendingLoadMusicFilename);
			propertiesWindow.isPendingLoadMusic = false;
			propertiesWindow.pendingLoadMusicFilename.clear();
		}

		if (config.autoSaveEnabled && autoSaveTimer.elapsedMinutes() >= config.autoSaveInterval)
		{
			autoSaveIfModified();
			autoSaveTimer.reset();
		}

		if (recentFileNotFoundDialog.update() == DialogResult::Yes)
		{
			if (isArrayIndexInBounds(recentFileNotFoundDialog.removeIndex, config.recentFiles))
				config.recentFiles.erase(config.recentFiles.begin() + recentFileNotFoundDialog.removeIndex);
		}

		if (dirtyMarker && context.upToDate)
                {
                        std::error_code ec;
                        std::filesystem::remove(autoSavePath + "/dirty_session", ec);
                        dirtyMarker = false;
                }

                if (recoverPending)
                {
                        if (!ImGui::IsPopupOpen("###recover_autosave"))
                                ImGui::OpenPopup("###recover_autosave");

                        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26.0f, 0.0f), ImGuiCond_Always);
                        ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
                        if (ImGui::BeginPopupModal("Restore autosave###recover_autosave", NULL, ImGuiWindowFlags_NoResize))
                        {
                                ImGui::TextWrapped("The app closed with unsaved changes. Restore the latest autosave?");
                                const float btnWidth = ImGui::GetContentRegionAvail().x / 2 - ImGui::GetStyle().ItemSpacing.x;
                                const bool restore = ImGui::Button("Restore", { btnWidth, ImGui::GetFrameHeight() });
                                ImGui::SameLine();
                                const bool discard = ImGui::Button("Discard", { btnWidth, ImGui::GetFrameHeight() });
                                if (restore || discard)
                                {
                                        std::error_code ec;
                                        std::filesystem::remove(autoSavePath + "/dirty_session", ec);
                                        recoverPending = false;
                                        ImGui::CloseCurrentPopup();
                                        if (restore)
                                                loadScore(recoverFile);
                                }
                                ImGui::EndPopup();
                        }
                }

                #ifdef __ANDROID__
                AndroidImGui_SetSafeMargin(config.androidSafeMarginX, 0);
#endif
                #ifdef __ANDROID__
                if (propertiesWindow.requestMusicExplorer)
                {
                        propertiesWindow.requestMusicExplorer = false;
                        explorerMode = 1;
                        explorerOpen = true;
                        explorerNeedsRefresh = true;
                        explorerSelected.clear();
                        if (explorerDir.empty())
                                explorerDir = "/storage/emulated/0";
                }
#endif
                drawFileExplorer();
                settingsWindow.update();
		aboutDialog.update();
		mp4ExportWindow.update(context, timeline, preview);
#ifdef __ANDROID__
                {
                        const int multiTapRaw = AndroidImGui_ConsumeMultiTap();
                        const int multiTap = preview.isPlaytestOn() ? 0 : multiTapRaw;
                        if (multiTap == 2 && context.history.hasUndo() && !UI::isAnyPopupOpen())
                                context.undo();
                        else if (multiTap == 3 && !UI::isAnyPopupOpen())
                                timeline.setPlaying(context, !timeline.isPlaying());
                }
#endif
		serializeWindow.update(*this, context, timeline);

		const bool timeline_just_created = (ImGui::FindWindowByName("###notes_timeline") == NULL);
		ImGui::Begin(IMGUI_TITLE(ICON_FA_MUSIC, "notes_timeline"), NULL, ImGuiWindowFlags_Static | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		ImGuiID dockId = ImGui::GetWindowDockID();
		const auto tTl0 = std::chrono::steady_clock::now();
                timeline.update(context, edit, renderer.get());
                gExpTimelineMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tTl0).count();
		mp4ExportWindow.prepareFrame(context, timeline, preview);
		ImGui::End();

		ImGuiWindowFlags previewWindowFlags =
			ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoScrollWithMouse;

		ImGui::SetNextWindowDockID(dockId, ImGuiCond_FirstUseEver);
		if (isFullScreenPreview())
		{
			ImGuiViewport* viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(viewport->WorkPos);
			ImGui::SetNextWindowSize(viewport->WorkSize);
			ImGui::SetNextWindowViewport(viewport->ID);
			previewWindowFlags |= ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize;

			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0, 0 });

			ImGui::Begin(IMGUI_TITLE(ICON_FA_OBJECT_GROUP, "score_preview_full"), NULL, previewWindowFlags);
			ImGui::PopStyleVar(2);
		}
		else
		{
			ImGui::Begin(IMGUI_TITLE(ICON_FA_OBJECT_GROUP, "score_preview"), NULL, previewWindowFlags);
		}

		const auto tPv0 = std::chrono::steady_clock::now();
                preview.update(context, renderer.get());
                gExpPreviewMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tPv0).count();
		mp4ExportWindow.captureFrame(context, timeline, preview);
		preview.updateUI(timeline, context);
		ImGui::End();

		if (timeline_just_created) ImGui::SetWindowFocus("###notes_timeline");

		if (config.debugEnabled)
		{
			debugWindow.update(context, timeline);
			debugEffectView.update(renderer.get());
		}

		if (ImGui::Begin(IMGUI_TITLE(ICON_FA_ALIGN_LEFT, "chart_properties"), NULL, ImGuiWindowFlags_Static))
		{
			propertiesWindow.update(context);
		}
		ImGui::End();

		if (ImGui::Begin(IMGUI_TITLE(ICON_FA_WRENCH, "options"), NULL, ImGuiWindowFlags_Static))
		{
			optionsWindow.update(context, edit, timeline.getMode());
		}
		ImGui::End();

		if (ImGui::Begin(IMGUI_TITLE(ICON_FA_DRAFTING_COMPASS, "presets"), NULL, ImGuiWindowFlags_Static))
		{
			presetsWindow.update(context, presetManager);
		}
		ImGui::End();

#ifdef _DEBUG
		if (showImGuiDemoWindow)
			ImGui::ShowDemoWindow(&showImGuiDemoWindow);
#endif
	}

	size_t ScoreEditor::updateRecentFilesList(const std::string& entry)
	{
		while (config.recentFiles.size() >= maxRecentFilesEntries)
			config.recentFiles.pop_back();

		// Remove the entry (if found) to the beginning of the vector
		auto it = std::find(config.recentFiles.begin(), config.recentFiles.end(), entry);
		if (it != config.recentFiles.end())
			config.recentFiles.erase(it);

		config.recentFiles.insert(config.recentFiles.begin(), entry);
		return config.recentFiles.size();
	}

	void ScoreEditor::reset()
	{
		timeline.setPlaying(context, false);

		context.score = {};
		context.workingData = {};
		context.history.clear();
		context.scoreStats.reset();
		context.scorePreviewDrawData.clear();
		context.audio.disposeMusic();
		context.waveformL.clear();
		context.waveformR.clear();
		context.clearSelection();

		// New score; nothing to save
		context.upToDate = true; 

		UI::setWindowTitle(windowUntitled);
	}

	void ScoreEditor::loadScore(std::string filename)
	{
		if (!IO::File::exists(filename))
			return;

		timeline.setPlaying(context, false);
		serializeWindow.deserialize(filename);
	}

	void ScoreEditor::loadMusic(std::string filename)
	{
		propertiesWindow.isLoadingMusic = true;
		context.waveformL.clear();
		context.waveformR.clear();
		
		Result result = context.audio.loadMusic(filename);
		if (result.isOk() || filename.empty())
		{
			context.workingData.musicFilename = filename;
		}
		else
		{
			std::string errorMessage = IO::formatString("%s\n%s: %s\n%s",
				getString("error_load_music_file"),
				getString("music_file"),
				filename.c_str(),
				result.getMessage().c_str()
			);

			IO::messageBox(APP_NAME, errorMessage, IO::MessageBoxButtons::Ok, IO::MessageBoxIcon::Error, Application::windowState.windowHandle);
		}
		
		context.waveformL.generateMipChainsFromSampleBuffer(context.audio.musicBuffer, 0);
		context.waveformR.generateMipChainsFromSampleBuffer(context.audio.musicBuffer, 1);

		if (timeline.isPlaying())
		{
			bool prevPauseBehaviour = config.returnToLastSelectedTickOnPause;
			config.returnToLastSelectedTickOnPause = false;
			timeline.setPlaying(context, false);
			timeline.setPlaying(context, true);

			config.returnToLastSelectedTickOnPause = prevPauseBehaviour;
		}

		propertiesWindow.isLoadingMusic = false;
	}

	void ScoreEditor::asyncLoadMusic(std::string filename)
	{
		if (loadMusicFuture.valid())
			loadMusicFuture.get();
		
		loadMusicFuture = std::async(&ScoreEditor::loadMusic, this, filename);
	}

	void ScoreEditor::open()
        {
#ifdef __ANDROID__
                explorerMode = 0;
                explorerOpen = true;
                explorerNeedsRefresh = true;
                if (explorerDir.empty())
                        explorerDir = "/storage/emulated/0";
                return;
#endif
		IO::FileDialog fileDialog{};
		fileDialog.parentWindowHandle = Application::windowState.windowHandle;
		fileDialog.title = "Open Chart";
		fileDialog.filters = {
			IO::combineFilters("All Supported Files",{ IO::mmwsFilter, IO::susFilter, IO::customScoreJsonFilter }),
			IO::mmwsFilter,
			IO::susFilter,
			IO::customScoreJsonFilter,
			IO::allFilter
		};
		
		if (fileDialog.openFile() == IO::FileDialogResult::OK)
			loadScore(fileDialog.outputFilename);
	}

	bool ScoreEditor::trySave(std::string filename)
	{
		return filename.empty() ? saveAs() : save(filename);
	}

	bool ScoreEditor::save(std::string filename)
	{
		try
		{
			context.score.metadata = context.workingData.toScoreMetadata();
			NativeScoreSerializer().serialize(context.score, filename);
#if defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
			if (!AndroidFileDialog_Commit(filename))
				throw std::runtime_error("Could not save the document to its Android location.");
#endif

			UI::setWindowTitle(IO::File::getFilename(filename));
			context.upToDate = true;
		}
		catch (const std::exception& err)
		{
			IO::messageBox(
				APP_NAME,
				IO::formatString("%s\n%s: %s", getString("error_save_score_file"), getString("error"), err.what()),
				IO::MessageBoxButtons::Ok,
				IO::MessageBoxIcon::Error,
				Application::windowState.windowHandle
			);

			return false;
		}

		return true;
	}

	bool ScoreEditor::saveAs()
	{
		IO::FileDialog fileDialog{};
		fileDialog.title = "Save Chart";
		fileDialog.filters = { IO::mmwsFilter };
		fileDialog.defaultExtension = "mmws";
		fileDialog.parentWindowHandle = Application::windowState.windowHandle;
		fileDialog.inputFilename = IO::File::getFilenameWithoutExtension(context.workingData.filename);

		if (fileDialog.saveFile() == IO::FileDialogResult::OK)
		{
			context.workingData.filename = fileDialog.outputFilename;
			bool saved = save(context.workingData.filename);
			if (saved)
				updateRecentFilesList(fileDialog.outputFilename);

			return saved;
		}

		return false;
	}

	void ScoreEditor::exportScore()
	{
		serializeWindow.serialize(context);
	}

	void ScoreEditor::drawMenubar()
	{
		ImGui::BeginMainMenuBar();
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 2));

		if (ImGui::BeginMenu(getString("file")))
		{
			if (ImGui::MenuItem(getString("new"), ToShortcutString(config.input.create)))
				Application::windowState.resetting = true;

			if (ImGui::MenuItem(getString("open"), ToShortcutString(config.input.open)))
			{
				Application::windowState.resetting = true;
				Application::windowState.shouldPickScore = true;
			}

			if (ImGui::BeginMenu(getString("open_recent")))
			{
				for (size_t index = 0; index < config.recentFiles.size(); index++)
				{
					const std::string& entry = config.recentFiles[index];
					if (ImGui::MenuItem(entry.c_str()))
					{
						if (IO::File::exists(entry))
						{
							Application::windowState.resetting = true;
							Application::pendingLoadScoreFile = entry;
						}
						else
						{
							recentFileNotFoundDialog.removeFilename = entry;
							recentFileNotFoundDialog.removeIndex = index;
							recentFileNotFoundDialog.open = true;
						}
					}
				}

				ImGui::Separator();
				if (ImGui::MenuItem(getString("clear"), nullptr, false, !config.recentFiles.empty()))
					config.recentFiles.clear();

				ImGui::EndMenu();
			}

			ImGui::Separator();
			if (ImGui::MenuItem(getString("save"), ToShortcutString(config.input.save)))
				trySave(context.workingData.filename);

			if (ImGui::MenuItem(getString("save_as"), ToShortcutString(config.input.saveAs)))
				saveAs();

			if (ImGui::MenuItem(getString("export"), ToShortcutString(config.input.exportScore)))
				exportScore();

			if (ImGui::MenuItem(getString("export_mp4")))
				mp4ExportWindow.show();

			ImGui::Separator();
			if (ImGui::MenuItem(getString("exit"), ToShortcutString(ImGuiKey_F4, ImGuiMod_Alt)))
				Application::windowState.closing = true;

			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu(getString("edit")))
		{
			if (ImGui::MenuItem(getString("undo"), ToShortcutString(config.input.undo), false, context.history.hasUndo()))
				context.undo();

			if (ImGui::MenuItem(getString("redo"), ToShortcutString(config.input.redo), false, context.history.hasRedo()))
				context.redo();

			ImGui::Separator();
			const bool hasHiSpeedSelection = timeline.hasSelectedHiSpeed(context);
			const bool hasSEVolumeSelection = timeline.hasSelectedSEVolume(context);
			const bool hasAnySelection = hasHiSpeedSelection || hasSEVolumeSelection || context.selectedNotes.size();
			if (ImGui::MenuItem(getString("delete"), ToShortcutString(config.input.deleteSelection), false, hasAnySelection))
			{
				const bool deletedHiSpeed = timeline.deleteSelectedHiSpeed(context);
				const bool deletedSEVolume = timeline.deleteSelectedSEVolume(context);
				if (!deletedHiSpeed && !deletedSEVolume)
					context.deleteSelection();
			}

			if (ImGui::MenuItem(getString("cut"), ToShortcutString(config.input.cutSelection), false, hasAnySelection))
			{
				if (!timeline.cutSelectedHiSpeed(context) && !timeline.cutSelectedSEVolume(context))
					context.cutSelection();
			}

			if (ImGui::MenuItem(getString("copy"), ToShortcutString(config.input.copySelection), false, hasAnySelection))
			{
				if (!timeline.copySelectedHiSpeed(context) && !timeline.copySelectedSEVolume(context))
					context.copySelection();
			}

			if (ImGui::MenuItem(getString("paste"), ToShortcutString(config.input.paste)))
			{
				if (!timeline.pasteHiSpeed(context, false) && !timeline.pasteSEVolume(context, false))
					context.paste(false);
			}

			ImGui::Separator();
			if (ImGui::MenuItem(getString("select_all"), ToShortcutString(config.input.selectAll)))
				context.selectAll();

			ImGui::Separator();
			if (ImGui::MenuItem(getString("insert_skill"), ToShortcutString(config.input.insertSkill), nullptr, !timeline.isPlaying()))
				timeline.insertSkill(context, context.currentTick);

			if (ImGui::MenuItem(getString("insert_fever"), ToShortcutString(config.input.insertFever), nullptr, !timeline.isPlaying()))
				timeline.beginInsertFever(context, context.currentTick);

			ImGui::Separator();
			if (ImGui::MenuItem(getString("settings"), ToShortcutString(config.input.openSettings)))
				settingsWindow.open = true;

			ImGui::EndMenu();
		}
		
		if (ImGui::BeginMenu(getString("view")))
		{
			ImGui::MenuItem(getString("show_step_outlines"), NULL, &timeline.drawHoldStepOutlines);
			ImGui::MenuItem(getString("hide_step_outlines_in_playback"), NULL, &config.hideStepOutlinesInPlayback);
			ImGui::MenuItem(getString("cursor_auto_scroll"), NULL, &config.followCursorInPlayback);
			ImGui::MenuItem(getString("return_to_last_tick"), NULL, &config.returnToLastSelectedTickOnPause);
			ImGui::MenuItem(getString("draw_waveform"), NULL, &config.drawWaveform);
			ImGui::MenuItem(getString("stop_at_end"), NULL, &config.stopAtEnd);
			ImGui::MenuItem(getString("preview_draw_toolbar"), NULL, &config.pvDrawToolbar);
			ImGui::MenuItem(getString("preview_draw_score_hud"), NULL, &config.pvDrawScoreHud);
			ImGui::MenuItem(getString("preview_draw_score_add_animation"), NULL, &config.pvDrawScorePopup);
			ImGui::MenuItem(getString("preview_draw_life_hud"), NULL, &config.pvDrawLifeHud);
			ImGui::MenuItem(getString("preview_draw_combo"), NULL, &config.pvDrawCombo);
			ImGui::MenuItem(getString("preview_draw_perfect"), NULL, &config.pvDrawPerfect);
			
			bool isPreviewFullScreen = isFullScreenPreview();
			if (ImGui::MenuItem(getString("fullscreen_preview"), ToShortcutString(config.input.togglePreviewFullWindow), &isPreviewFullScreen))
				preview.setFullWindow(isPreviewFullScreen);

			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu(getString("presets")))
		{
			if (ImGui::MenuItem(getString("create_preset"), NULL, false, !context.selectedNotes.empty()))
				presetsWindow.openCreatePresetDialog();
			
			if (ImGui::MenuItem(getString("import_preset"), NULL, false, true))
				openImportPresetDialog();
			
			if (ImGui::MenuItem(getString("reload_presets"), NULL, false, true))
			{
				presetManager.presets.clear();
				loadPresets();
			}
			
			ImGui::Separator();
			if (ImGui::MenuItem(getString("open_presets_folder"), NULL, false, true))
			{
				if (!std::filesystem::exists(presetManager.getPresetsPath()))
					std::filesystem::create_directory(presetManager.getPresetsPath());
#ifdef _WIN32
				ShellExecuteW(0, 0, presetManager.getPresetsPath().c_str(), 0, 0, SW_SHOW);
#elif defined(__APPLE__) && !defined(MIKUMIKUWORLD_IOS)
				std::string command = "open \"" + presetManager.getPresetsPath().string() + "\"";
				std::system(command.c_str());
#endif
			}
			
			ImGui::EndMenu();
		}
		
		if (config.debugEnabled)
		{
			if (ImGui::BeginMenu(getString("debug")))
			{
#ifdef _DEBUG
				ImGui::MenuItem("ImGui Demo Window", NULL, &showImGuiDemoWindow);
#endif

				if (ImGui::MenuItem("Auto Save"))
					autoSave();

				if (ImGui::MenuItem("Delete Old Auto Save (1)"))
					deleteOldAutoSave(1);

				if (ImGui::MenuItem("Delete Old Auto Save (Max)"))
					deleteOldAutoSave(config.autoSaveMaxCount);

				bool audioRunning = context.audio.isEngineStarted();
				if (ImGui::MenuItem(audioRunning ? "Stop Audio" : "Start Audio",
					audioRunning ? ICON_FA_VOLUME_UP : ICON_FA_VOLUME_MUTE))
				{
					if (audioRunning)
						context.audio.stopEngine();
					else
						context.audio.startEngine();
				}

				ImGui::EndMenu();
			}
		}

		if (ImGui::BeginMenu(getString("window")))
		{
			if (ImGui::MenuItem(getString("vsync"), NULL, &Application::windowState.vsync))
#if !defined(__ANDROID__) && !defined(MIKUMIKUWORLD_IOS)
				glfwSwapInterval(Application::windowState.vsync);
#endif

			ImGui::MenuItem(getString("show_fps"), NULL, &config.showFPS);

			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu(getString("help")))
		{
			if (ImGui::MenuItem(getString("help"), ToShortcutString(config.input.openHelp)))
				help();

			if (ImGui::MenuItem(getString("about")))
				aboutDialog.open = true;

			ImGui::EndMenu();
		}

		if (config.showFPS)
		{
			std::string fps = IO::formatString("%.3fms (%.1fFPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
			ImGui::SetCursorPosX(ImGui::GetWindowSize().x - ImGui::CalcTextSize(fps.c_str()).x - ImGui::GetStyle().WindowPadding.x);
			ImGui::Text("%s", fps.c_str());
		}

		ImGui::PopStyleVar();
		ImGui::EndMainMenuBar();
	}

	void ScoreEditor::drawToolbar()
	{
		ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImVec2 toolbarSize{ viewport->WorkSize.x, UI::toolbarBtnSize.y + ImGui::GetStyle().WindowPadding.y + 5 };

		// keep toolbar on top in main viewport
		ImGui::SetNextWindowViewport(viewport->ID);
		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(toolbarSize, ImGuiCond_Always);

		// toolbar style
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0.0f, 0.0f, 0.0f, 0.0f });
		ImGui::PushStyleColor(ImGuiCol_Separator, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::Begin("##app_toolbar", NULL, ImGuiWindowFlags_Toolbar);
		
		if (UI::toolbarButton(ICON_FA_FILE, getString("new"), ToShortcutString(config.input.create)))
		{
			Application::windowState.resetting = true;
		}

		if (UI::toolbarButton(ICON_FA_FOLDER_OPEN, getString("open"), ToShortcutString(config.input.open)))
		{
			Application::windowState.resetting = true;
			Application::windowState.shouldPickScore = true;
		}

		if (UI::toolbarButton(ICON_FA_SAVE, getString("save"), ToShortcutString(config.input.save)))
			trySave(context.workingData.filename);

		if (UI::toolbarButton(ICON_FA_FILE_EXPORT, getString("export"), ToShortcutString(config.input.exportScore)))
			exportScore();

		UI::toolbarSeparator();

		const bool hasHiSpeedSelection = timeline.hasSelectedHiSpeed(context);
		const bool hasSEVolumeSelection = timeline.hasSelectedSEVolume(context);
		const bool hasAnySelection = hasHiSpeedSelection || hasSEVolumeSelection || context.selectedNotes.size() > 0;
		if (UI::toolbarButton(ICON_FA_CUT, getString("cut"), ToShortcutString(config.input.cutSelection), hasAnySelection))
		{
			if (!timeline.cutSelectedHiSpeed(context) && !timeline.cutSelectedSEVolume(context))
				context.cutSelection();
		}

		if (UI::toolbarButton(ICON_FA_COPY, getString("copy"), ToShortcutString(config.input.copySelection), hasAnySelection))
		{
			if (!timeline.copySelectedHiSpeed(context) && !timeline.copySelectedSEVolume(context))
				context.copySelection();
		}

		if (UI::toolbarButton(ICON_FA_PASTE, getString("paste"), ToShortcutString(config.input.paste)))
		{
			if (!timeline.pasteHiSpeed(context, false) && !timeline.pasteSEVolume(context, false))
				context.paste(false);
		}

		UI::toolbarSeparator();

		if (UI::toolbarButton(ICON_FA_UNDO, getString("undo"), ToShortcutString(config.input.undo), context.history.hasUndo()))
			context.undo();

		if (UI::toolbarButton(ICON_FA_REDO, getString("redo"), ToShortcutString(config.input.redo), context.history.hasRedo()))
			context.redo();

		UI::toolbarSeparator();

		for (int i = 0; i < (int)TimelineMode::TimelineModeMax; ++i)
		{
			if (i == (int)TimelineMode::InsertSEVolume)
			{
				if (UI::toolbarButton(ICON_FA_VOLUME_UP, getString(timelineModes[i]), ToShortcutString(*timelineModeBindings[i]), true, (int)timeline.getMode() == i))
					timeline.changeMode((TimelineMode)i, edit);
				continue;
			}

			std::string img{ IO::concat("timeline_tools", timelineModes[i], "_") };
			int sprIndex = (int)timelineSprites[i];
			if (i == (int)TimelineMode::InsertFlick)
			{
				img.append(IO::concat("", flickTypes[(int)edit.flickType], "_"));
				sprIndex = (int)flickToolSprites[(int)edit.flickType - 1];
			}
			else if (i == (int)TimelineMode::InsertLongMid)
			{
				img.append(IO::concat("", stepTypes[(int)edit.stepType], "_"));
				sprIndex = (int)stepToolSprites[(int)edit.stepType];
			}

			if (UI::toolbarImageButton("timeline_tools", sprIndex, getString(timelineModes[i]), ToShortcutString(*timelineModeBindings[i]), true, (int)timeline.getMode() == i))
				timeline.changeMode((TimelineMode)i, edit);
		}

		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar(2);
		ImGui::End();
	}

	void ScoreEditor::help()
	{
#ifdef _WIN32
		ShellExecuteW(0, 0, L"https://github.com/crash5band/MikuMikuWorld/wiki", 0, 0, SW_SHOW);
#elif defined(__APPLE__) && !defined(MIKUMIKUWORLD_IOS)
		std::system("open https://github.com/crash5band/MikuMikuWorld/wiki");
#endif
	}

	void ScoreEditor::refreshExplorer()
        {
                explorerEntries.clear();
                explorerDenied = false;

                std::error_code ec;
                std::filesystem::directory_iterator it(explorerDir, ec);
                const std::filesystem::directory_iterator end;
                if (ec)
                {
                        explorerDenied = true;
                        return;
                }

                for (; !ec && it != end; it.increment(ec))
                {
                        std::error_code entryEc;
                        const bool isDir = it->is_directory(entryEc);
                        const std::string name = it->path().filename().string();
                        if (name.empty() || name[0] == '.')
                                continue;

                        if (!isDir && !explorerShowAll)
                        {
                                std::string lower = name;
                                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                                auto endsWith = [&](const char* ext)
                                {
                                        const size_t n = std::strlen(ext);
                                        return lower.size() >= n && lower.compare(lower.size() - n, n, ext) == 0;
                                };
                                const bool allowed = explorerMode == 1
                                        ? (endsWith(".mp3") || endsWith(".wav") || endsWith(".flac") || endsWith(".ogg"))
                                        : (endsWith(".mmws") || endsWith(".sus") || endsWith(".json") || endsWith(".gz") || lower.find("leveldata") != std::string::npos);
                                if (!allowed)
                                        continue;
                        }

                        ExplorerEntry entry;
                        entry.name = name;
                        entry.isDir = isDir;
                        if (!isDir)
                        {
                                const auto size = it->file_size(entryEc);
                                entry.size = entryEc ? 0 : static_cast<unsigned long long>(size);
                        }
                        explorerEntries.push_back(std::move(entry));
                }

                auto lowerOf = [](std::string text)
                {
                        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        return text;
                };
                std::sort(explorerEntries.begin(), explorerEntries.end(), [&](const ExplorerEntry& a, const ExplorerEntry& b)
                {
                        if (a.isDir != b.isDir)
                                return a.isDir;
                        return lowerOf(a.name) < lowerOf(b.name);
                });
        }

        void ScoreEditor::drawFileExplorer()
        {
                if (!explorerOpen)
                        return;

                if (!ImGui::IsPopupOpen("###file_explorer"))
                        ImGui::OpenPopup("###file_explorer");

                ImGuiViewport* viewport = ImGui::GetMainViewport();
                ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.9f, viewport->WorkSize.y * 0.9f), ImGuiCond_Always);
                ImGui::SetNextWindowViewport(viewport->ID);
                if (!ImGui::BeginPopupModal(explorerMode == 1 ? "Open music###file_explorer" : "Open chart###file_explorer", NULL, ImGuiWindowFlags_NoResize))
                        return;

                if (explorerNeedsRefresh)
                {
                        refreshExplorer();
                        explorerNeedsRefresh = false;
                }

                const ImGuiStyle& style = ImGui::GetStyle();
                const float rowHeight = ImGui::GetFrameHeight() * 1.4f;
                auto goTo = [&](const std::string& dir)
                {
                        explorerDir = dir;
                        explorerSelected.clear();
                        explorerNeedsRefresh = true;
                };
                auto joinPath = [&](const std::string& name)
                {
                        return explorerDir + (explorerDir.back() == '/' ? "" : "/") + name;
                };

                if (ImGui::Button("Up", ImVec2(0.0f, rowHeight)))
                {
                        const std::string parent = std::filesystem::path(explorerDir).parent_path().string();
                        if (!parent.empty() && parent != explorerDir)
                                goTo(parent);
                }
                ImGui::SameLine();
                if (ImGui::Button("Storage", ImVec2(0.0f, rowHeight)))
                        goTo("/storage/emulated/0");
                ImGui::SameLine();
                if (ImGui::Button("Download", ImVec2(0.0f, rowHeight)))
                        goTo("/storage/emulated/0/Download");
                ImGui::SameLine();
                if (ImGui::Button("Documents", ImVec2(0.0f, rowHeight)))
                        goTo("/storage/emulated/0/Documents");

                ImGui::TextWrapped("%s", explorerDir.c_str());
                if (ImGui::Checkbox("Show all files", &explorerShowAll))
                        explorerNeedsRefresh = true;

                if (explorerDenied)
                {
                        ImGui::TextWrapped("Cannot read this folder. Allow 'All files access' for this app to browse storage.");
#ifdef __ANDROID__
                        if (ImGui::Button("Open permission settings", ImVec2(0.0f, rowHeight)))
                                AndroidFileDialog_ShowPermissions();
#endif
                }

                const float footer = rowHeight + style.ItemSpacing.y * 2.0f;
                if (ImGui::BeginChild("###explorer_list", ImVec2(0.0f, -footer), true))
                {
                        for (const auto& entry : explorerEntries)
                        {
                                std::string label = entry.isDir ? ("[folder] " + entry.name) : entry.name;
                                if (!entry.isDir)
                                {
                                        char sizeText[48];
                                        std::snprintf(sizeText, sizeof(sizeText), "  (%llu KB)", (entry.size + 1023ULL) / 1024ULL);
                                        label += sizeText;
                                }

                                const bool selected = !entry.isDir && entry.name == explorerSelected;
                                if (ImGui::Selectable(label.c_str(), selected, 0, ImVec2(0.0f, rowHeight)))
                                {
                                        if (entry.isDir)
                                        {
                                                goTo(joinPath(entry.name));
                                                break;
                                        }
                                        explorerSelected = entry.name;
                                }
                        }

                        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 8.0f))
                                ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
                }
                ImGui::EndChild();

                const float half = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f;
                const std::string chosen = explorerSelected.empty() ? std::string() : joinPath(explorerSelected);
                ImGui::BeginDisabled(explorerSelected.empty());
                const bool openPressed = ImGui::Button("Open", ImVec2(half, rowHeight));
                ImGui::EndDisabled();
                ImGui::SameLine();
                const bool cancelPressed = ImGui::Button("Cancel", ImVec2(half, rowHeight));

                if (openPressed || cancelPressed)
                {
                        explorerOpen = false;
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();

                if (openPressed && !chosen.empty())
                {
                        if (explorerMode == 1)
                        {
                                propertiesWindow.pendingLoadMusicFilename = chosen;
                                propertiesWindow.isPendingLoadMusic = true;
                        }
                        else
                                loadScore(chosen);
                }
        }

        void ScoreEditor::autoSaveIfModified()
        {
                if (!config.autoSaveEnabled || context.upToDate || context.score.notes.empty())
                        return;

                autoSave();
                autoSaveTimer.reset();
                if (!dirtyMarker)
                {
                        std::ofstream(autoSavePath + "/dirty_session") << 1;
                        dirtyMarker = true;
                }
        }

        void ScoreEditor::autoSave()
	{
#ifdef _WIN32
		std::wstring wAutoSaveDir = IO::mbToWideStr(autoSavePath);
		std::filesystem::create_directory(wAutoSaveDir);
#else
		std::filesystem::create_directory(autoSavePath);
#endif

		context.score.metadata = context.workingData.toScoreMetadata();
		NativeScoreSerializer().serialize(context.score, autoSavePath + "/mmw_auto_save_" + Utilities::getCurrentDateTime() + MMWS_EXTENSION);
		
		int mmwsCount = 0;
#ifdef _WIN32
		for (const auto& file : std::filesystem::directory_iterator(wAutoSaveDir))
#else
		for (const auto& file : std::filesystem::directory_iterator(autoSavePath))
#endif
		{
			std::string extension = file.path().extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
			mmwsCount += static_cast<int>(extension == MMWS_EXTENSION);
		}
		
		if (mmwsCount > config.autoSaveMaxCount)
			deleteOldAutoSave(mmwsCount - config.autoSaveMaxCount);
	}

	int ScoreEditor::deleteOldAutoSave(int count)
	{
		std::wstring wAutoSaveDir = IO::mbToWideStr(autoSavePath);
		if (!std::filesystem::exists(wAutoSaveDir))
			return 0;

		// get mmws files
		using entry = std::filesystem::directory_entry;
		std::vector<entry> deleteFiles;
		for (const auto& file : std::filesystem::directory_iterator(wAutoSaveDir))
		{
			std::string extension = file.path().extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
			if (extension == MMWS_EXTENSION)
				deleteFiles.push_back(file);
		}

		// sort files by modification date
		std::sort(deleteFiles.begin(), deleteFiles.end(), [](const entry& f1, const entry& f2) {
			return f1.last_write_time() < f2.last_write_time();
		});

		int deleteCount = std::min(static_cast<int>(deleteFiles.size()), count);
		for (int i = 0; i < deleteCount; i++)
			std::filesystem::remove(deleteFiles[i]);
		
		return deleteCount;
	}

	void ScoreEditor::loadPresets()
	{
		if (loadPresetsFuture.valid())
			loadPresetsFuture.get();
		
		loadPresetsFuture = std::async(std::launch::async, [this]()
		{
			presetsWindow.notifyPresetsLoading();
			presetManager.loadPresets();
			presetsWindow.notifyPresetsLoaded(presetManager);
		});
	}

	void ScoreEditor::importPreset(const std::string& path)
	{
		if (importPresetFuture.valid())
			importPresetFuture.get();

		presetsWindow.notifyPresetsLoading();
		importPresetFuture = std::async(std::launch::async, [this, path]()
		{
			Result result = presetManager.importPreset(path);
			presetsWindow.notifyPresetsLoaded(presetManager);

			if (result.getStatus() != ResultStatus::Success)
			{
				IO::MessageBoxIcon icon = result.getStatus() == ResultStatus::Error ?
					IO::MessageBoxIcon::Error :
					IO::MessageBoxIcon::Warning;
			
				IO::messageBox(APP_NAME, result.getMessage(), IO::MessageBoxButtons::Ok, icon, Application::windowState.windowHandle);
			}
		});
	}

	void ScoreEditor::openImportPresetDialog()
	{
		IO::FileDialog fileDialog{"Import Preset", { IO::presetFilter }, "", "", ".json", 0, Application::windowState.windowHandle};
		if (fileDialog.openFile() == IO::FileDialogResult::OK)
			importPreset(fileDialog.outputFilename);
	}
}
