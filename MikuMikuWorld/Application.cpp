#include "Application.h"
#include "ResourceManager.h"
#include "IO.h"
#include "Utilities.h"
#include "Localization.h"
#include "ApplicationConfiguration.h"
#include "ScoreSerializer.h"
#include "NoteSkin.h"
#include "AndroidImGuiBackend.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace MikuMikuWorld
{
#ifndef MIKUMIKUWORLD_VERSION
#define MIKUMIKUWORLD_VERSION "1.0.0"
#endif

	std::string Application::version{ "1.0.0" };
	std::string Application::appDir{ "" };
	static std::string userDataDir{ "" };
	std::string Application::pendingLoadScoreFile{ "" };
	WindowState Application::windowState{};

	namespace
	{
		std::string ensureTrailingSlash(std::string path)
		{
			if (!path.empty() && path.back() != '/')
				path.push_back('/');

			return path;
		}

		std::string resolveUserDataDir(const std::string& resourceDir)
		{
#ifdef __APPLE__
			const char* homeDir = std::getenv("HOME");
			if (homeDir && *homeDir)
				return ensureTrailingSlash(std::string(homeDir) + "/Library/Application Support/MikuMikuWorld");
#endif
			return resourceDir;
		}

		void ensureDirectoryExists(const std::string& path)
		{
			if (path.empty())
				return;

			std::error_code error;
#ifdef _WIN32
			std::filesystem::create_directories(IO::mbToWideStr(path), error);
#else
			std::filesystem::create_directories(path, error);
#endif
		}
	}

	Application::Application() : 
		initialized{ false }, language{ "" }
	{
	}

	Result Application::initialize(const std::string& root)
	{
		if (initialized)
			return Result(ResultStatus::Success, "App is already initialized");

		appDir = ensureTrailingSlash(root);
		userDataDir = resolveUserDataDir(appDir);
		ensureDirectoryExists(userDataDir);
		version = getVersion();
		language = "";
		// Report an incomplete installation before shader/texture consumers
		// try to use missing resources during editor construction.
		for (const char* resource : { "res/shaders/basic2d.vert", "res/notes/01/notes.png", "res/fonts/NotoSansCJK-Regular.ttc" })
			if (!IO::File::exists(appDir + resource))
				return Result(ResultStatus::Error, "Missing application resource: " + appDir + resource +
					"\nExtract the complete app, including the res folder, before starting it.");

		const std::string userConfigPath = userDataDir + APP_CONFIG_FILENAME;
		const std::string bundledConfigPath = appDir + APP_CONFIG_FILENAME;
		if (IO::File::exists(userConfigPath))
			config.read(userConfigPath);
		else
			config.read(bundledConfigPath);
		readSettings();

		Result result = initOpenGL();
		if (!result.isOk())
			return result;

		imgui = std::make_unique<ImGuiManager>();
		result = imgui->initialize(window);
		if (!result.isOk())
			return result;

		imgui->setBaseTheme(config.baseTheme);
		imgui->applyAccentColor(config.accentColor);

		loadResources();
		startupWarningShouldOpen = !config.hideStartupWarning && startupWarningTextureIndex >= 0;
#if defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
		startupCreditsShouldOpen = !startupWarningShouldOpen;
#endif

		editor = std::make_unique<ScoreEditor>();
		editor->loadPresets();

		initialized = true;
		return Result::Ok();
	}

	const std::string& Application::getAppDir()
	{
		return appDir;
	}

	const std::string& Application::getUserDataDir()
	{
		return userDataDir;
	}

	std::string Application::getVersion()
	{
#ifdef _WIN32
		std::wstring filename = IO::mbToWideStr(appDir + "MikuMikuWorld.exe");

		DWORD  verHandle = 0;
		UINT   size = 0;
		LPBYTE lpBuffer = NULL;
		DWORD  verSize = GetFileVersionInfoSizeW(filename.c_str(), &verHandle);
		if (verSize == 0)
			return MIKUMIKUWORLD_VERSION;

		int major = 0, minor = 0, build = 0, rev = 0;
		if (verSize != NULL)
		{
			LPSTR verData = new char[verSize];

			if (GetFileVersionInfoW(filename.c_str(), verHandle, verSize, verData))
			{
				if (VerQueryValue(verData, TEXT("\\"), (VOID FAR * FAR*) & lpBuffer, &size))
				{
					if (size)
					{
						VS_FIXEDFILEINFO* verInfo = (VS_FIXEDFILEINFO*)lpBuffer;
						if (verInfo->dwSignature == 0xfeef04bd)
						{
							major = (verInfo->dwFileVersionMS >> 16) & 0xffff;
							minor = (verInfo->dwFileVersionMS >> 0) & 0xffff;
							rev = (verInfo->dwFileVersionLS >> 16) & 0xffff;
						}
					}
				}
			}
			delete[] verData;
		}

		return IO::formatString("%d.%d.%d", major, minor, rev);
#else
		return MIKUMIKUWORLD_VERSION;
#endif
	}

	const std::string& Application::getAppVersion()
	{
		return version;
	}

	void Application::dispose()
	{
		if (initialized)
		{
			editor->uninitialize();
			imgui->shutdown();
#if !defined(__ANDROID__) && !defined(MIKUMIKUWORLD_IOS)
			glfwDestroyWindow(window);
			glfwTerminate();
#endif
		}
		initialized = false;
	}

	void Application::readSettings()
	{
		windowState.position = config.windowPos;
		windowState.size = config.windowSize;
		windowState.maximized = config.maximized;
		windowState.vsync = config.vsync;
		windowState.fullScreen = config.fullScreen;
		UI::accentColors[0] = config.userColor.toImVec4();
	}

	void Application::writeSettings()
	{
		config.maximized = windowState.maximized;
		config.vsync = windowState.vsync;
		config.windowPos = windowState.position;
		config.windowSize = windowState.size;
		config.fullScreen = windowState.fullScreen;
		config.userColor = Color::fromImVec4(UI::accentColors[0]);

		if (editor)
		{
			editor->writeSettings();
			config.write(userDataDir + APP_CONFIG_FILENAME);
		}
	}

	void Application::appendOpenFile(const std::string& filename)
	{
		pendingOpenFiles.push_back(filename);
		windowState.dragDropHandled = false;
	}

	void Application::handlePendingOpenFiles()
	{
		std::string scoreFile{};
		std::string musicFile{};

		for (auto it = pendingOpenFiles.rbegin(); it != pendingOpenFiles.rend(); ++it)
		{
			std::string extension = IO::File::getFileExtension(*it);
			std::transform(extension.begin(), extension.end(), extension.begin(), tolower);

			if (ScoreSerializeController::isValidFormat(ScoreSerializeController::toSerializeFormat(*it)))
				scoreFile = *it;
			else if (Audio::isSupportedFileFormat(extension))
				musicFile = *it;

			if (!scoreFile.empty() && !musicFile.empty())
				break;
		}

		if (!scoreFile.empty())
		{
			windowState.resetting = true;
			pendingLoadScoreFile = scoreFile;
		}

		if (!musicFile.empty())
			editor->asyncLoadMusic(musicFile);

		pendingOpenFiles.clear();
		windowState.dragDropHandled = true;
	}

	void Application::update()
	{
		if (config.language != language)
		{
			std::string locale = config.language == "auto" ? Utilities::getSystemLocale() : config.language;
			
			// Try to set the selected language and fallback to default (en) on failure
			if (!Localization::setLanguage(locale))
				Localization::setLanguage("en");

			language = config.language;
		}

		float dpiX = 1.0f, dpiY = 1.0f;
#if defined(__APPLE__) && !defined(MIKUMIKUWORLD_IOS)
		// GLFW reports Retina backing scale here, but the current UI code already
		// renders in logical points. Applying that scale again makes the macOS UI oversized.
		float dpiScale = 1.0f;
#elif defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
		float dpiScale = AndroidImGui_GetDensity();
		dpiX = dpiY = dpiScale;
#else
		GLFWmonitor* mainMonitor = glfwGetPrimaryMonitor();
		if (mainMonitor)
		{
			glfwGetMonitorContentScale(mainMonitor, &dpiX, &dpiY);
		}

		float dpiScale = (dpiX + dpiY) * 0.5f;
#endif
		if (dpiScale != windowState.lastDpiScale)
		{
			imgui->buildFonts(dpiScale);
			windowState.lastDpiScale = dpiScale;
		}

		imgui->begin();

		// Inform ImGui of dpi changes
		ImGui::GetMainViewport()->DpiScale = dpiX;
		UI::updateBtnSizesDpiScaling(dpiScale);

		if (!windowState.dragDropHandled)
			handlePendingOpenFiles();

		imgui->initializeLayout();

		if (config.accentColor != imgui->getAccentColor())
			imgui->applyAccentColor(config.accentColor);

		if (config.userColor != Color::fromImVec4(UI::accentColors[0]) && config.accentColor == 0)
			imgui->applyAccentColor(config.accentColor);

		if (config.baseTheme != imgui->getBaseTheme())
			imgui->setBaseTheme(config.baseTheme);

		if ((windowState.closing || windowState.resetting) && !editor->isUpToDate() && !unsavedChangesDialog.open)
		{
			unsavedChangesDialog.open = true;
			ImGui::OpenPopup(MODAL_TITLE("unsaved_changes"));
		}

		auto unsavedChangesResult = unsavedChangesDialog.update();

		if (windowState.closing)
		{
			if (!editor->isUpToDate())
			{
				switch (unsavedChangesResult)
				{
				case DialogResult::Yes:
					editor->trySave(editor->getWorkingFilename().data());
#if !defined(__ANDROID__) && !defined(MIKUMIKUWORLD_IOS)
					glfwSetWindowShouldClose(window, 1);
#else
					AndroidRequestExit();
#endif
					break;

				case DialogResult::No:
#if !defined(__ANDROID__) && !defined(MIKUMIKUWORLD_IOS)
					glfwSetWindowShouldClose(window, 1);
#else
					AndroidRequestExit();
#endif
					break;

				case DialogResult::Cancel:
					windowState.closing = false;
					break;

				default:
					break;
				}
			}
			else
			{
#if !defined(__ANDROID__) && !defined(MIKUMIKUWORLD_IOS)
				glfwSetWindowShouldClose(window, 1);
#else
				AndroidRequestExit();
#endif
			}
		}

		if (windowState.resetting)
		{
			if (!editor->isUpToDate())
			{
				switch (unsavedChangesResult)
				{
				case DialogResult::Yes:
					editor->trySave(editor->getWorkingFilename().data());
					break;

				case DialogResult::Cancel:
					windowState.resetting = shouldPickScore = false;
					pendingLoadScoreFile.clear();
					break;

				default:
					break;
				}
			}

			// Already saved or clicked save changes or discard changes
			if (editor->isUpToDate() || (unsavedChangesResult != DialogResult::Cancel && unsavedChangesResult != DialogResult::None))
			{
				if (windowState.shouldPickScore)
				{
					editor->open();
					windowState.shouldPickScore = false;
				}
				else if (pendingLoadScoreFile.size())
				{
					editor->loadScore(pendingLoadScoreFile);
					pendingLoadScoreFile.clear();
				}
				else
				{
					editor->reset();
				}

				windowState.resetting = false;
			}
		}

		editor->update();

		bool isFullScreen = config.fullScreen;
		if (ImGui::IsAnyPressed(config.input.toggleFullscreen)) setFullScreen(!isFullScreen);

		if (!editor->isFullScreenPreview())
		{
			ImGui::BeginMainMenuBar();
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 2));

			if (ImGui::BeginMenu(getString("window")))
			{
				if (ImGui::MenuItem(getString("fullscreen"), ToShortcutString(config.input.toggleFullscreen), &isFullScreen))
					setFullScreen(isFullScreen);

				ImGui::EndMenu();
			}

			ImGui::PopStyleVar();
			ImGui::EndMainMenuBar();
		}

		updateStartupWarningDialog();

		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		imgui->draw(window);
#if !defined(__ANDROID__) && !defined(MIKUMIKUWORLD_IOS)
		glfwSwapBuffers(window);
#endif
	}

	void Application::updateStartupWarningDialog()
	{
#if !defined(__ANDROID__) && !defined(MIKUMIKUWORLD_IOS)
		if (config.hideStartupWarning || startupWarningTextureIndex < 0)
			return;
#endif

		if (startupWarningShouldOpen)
		{
			startupWarningDontShowAgain = false;
			ImGui::OpenPopup(MODAL_TITLE("startup_warning"));
			startupWarningShouldOpen = false;
		}

		bool warningClosedThisFrame = false;
		if (!config.hideStartupWarning && startupWarningTextureIndex >= 0)
		{
			const Texture& warningTexture = ResourceManager::textures[startupWarningTextureIndex];
			const ImGuiViewport* viewport = ImGui::GetMainViewport();
			const ImVec2 viewportSize = viewport->WorkSize;
			const float maxImageWidth = std::min(viewportSize.x * 0.68f, 980.0f);
			const float maxImageHeight = std::min(viewportSize.y * 0.58f, 620.0f);
			const float widthScale = maxImageWidth / static_cast<float>(warningTexture.getWidth());
			const float heightScale = maxImageHeight / static_cast<float>(warningTexture.getHeight());
			const float imageScale = std::min(1.0f, std::min(widthScale, heightScale));
			const ImVec2 imageSize{
				warningTexture.getWidth() * imageScale,
				warningTexture.getHeight() * imageScale
			};

			ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowViewport(viewport->ID);
			bool popupOpen = true;
			if (ImGui::BeginPopupModal(MODAL_TITLE("startup_warning"), &popupOpen, ImGuiWindowFlags_AlwaysAutoResize))
			{
				bool shouldClose = false;

				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max((ImGui::GetContentRegionAvail().x - imageSize.x) * 0.5f, 0.0f));
				ImGui::Image((ImTextureID)(intptr_t)warningTexture.getID(), imageSize);

				ImGui::Spacing();
				const char* caption = "the the the the the the\nthe the the the";
				const ImVec2 captionSize = ImGui::CalcTextSize(caption);
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max((ImGui::GetContentRegionAvail().x - captionSize.x) * 0.5f, 0.0f));
				ImGui::TextUnformatted(caption);
				ImGui::Spacing();
				ImGui::Checkbox(getString("startup_warning_hide"), &startupWarningDontShowAgain);
				ImGui::Spacing();

				if (ImGui::Button(getString("startup_warning_confirm"), { ImGui::GetContentRegionAvail().x, 0.0f }))
					shouldClose = true;

				if (!popupOpen)
					shouldClose = true;

				if (shouldClose)
				{
					config.hideStartupWarning = startupWarningDontShowAgain;
					config.write(userDataDir + APP_CONFIG_FILENAME);
					ImGui::CloseCurrentPopup();
#if defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
					startupCreditsShouldOpen = true;
					warningClosedThisFrame = true;
#endif
				}

				ImGui::EndPopup();
			}
		}

#if defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
		if (warningClosedThisFrame)
			return;

		if (startupCreditsShouldOpen)
		{
			ImGui::OpenPopup("Original Authors");
			startupCreditsShouldOpen = false;
		}

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		const float dialogWidth = std::min(viewport->WorkSize.x * 0.82f, 560.0f);
		ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowViewport(viewport->ID);
		ImGui::SetNextWindowSizeConstraints(
			ImVec2(dialogWidth, 0.0f),
			ImVec2(dialogWidth, viewport->WorkSize.y * 0.85f));
		if (ImGui::BeginPopupModal("Original Authors", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted("Original Repo by:");
			ImGui::SameLine();
			ImGui::TextLinkOpenURL("https://github.com/crash5band/", "https://github.com/crash5band/");
			ImGui::TextUnformatted("Original Fork by:");
			ImGui::SameLine();
			ImGui::TextLinkOpenURL("https://github.com/watagashi-uni/", "https://github.com/watagashi-uni/");
			ImGui::TextWrapped("Android port by: -YOUKAI- and the (Discord @desktop2202, @blandora2000)");
			ImGui::Spacing();
			if (ImGui::Button("Continue", { ImGui::GetContentRegionAvail().x, 0.0f }))
			{
				ImGui::CloseCurrentPopup();
				startupAgreementShouldOpen = !config.acceptedUserAgreement;
				ImGui::EndPopup();
				return;
			}
			ImGui::EndPopup();
		}

		if (startupAgreementShouldOpen)
		{
			ImGui::OpenPopup("User Agreement");
			startupAgreementShouldOpen = false;
		}
		ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowViewport(viewport->ID);
		ImGui::SetNextWindowSizeConstraints(
			ImVec2(dialogWidth, 0.0f),
			ImVec2(dialogWidth, viewport->WorkSize.y * 0.85f));
		if (ImGui::BeginPopupModal("User Agreement", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			const float agreementTextHeight = viewport->WorkSize.y * 0.58f;
			if (ImGui::BeginChild("agreement_text", ImVec2(-1.0f, agreementTextHeight), ImGuiChildFlags_Borders))
			{
				ImGui::TextWrapped("1. Credit and attribution\nYou may use this editor to create and edit charts. When you post screenshots, videos, or other media made with this Android port, credit the original project authors and the Android port creator. Include links to the original repository and fork when practical.");
				ImGui::Spacing();
				ImGui::TextWrapped("2. Your charts and media\nYou are responsible for the charts and files you create, import, or export. Make sure you have permission to use and share any songs, jacket art, video, images, or other material included in your work. This app does not grant rights to third-party content.");
				ImGui::Spacing();
				ImGui::TextWrapped("3. Third-party software and assets\nThe app may include software and assets from other authors. Their license terms continue to apply. Keep required notices and follow those terms if you modify or redistribute the app.");
				ImGui::Spacing();
				ImGui::TextWrapped("4. Community port\nThis Android port is an independent community modification. It is not an official app and is not affiliated with SEGA, Colorful Palette, or the Project SEKAI / HATSUNE MIKU: COLORFUL STAGE! services.");
				ImGui::Spacing();
				ImGui::TextWrapped("5. Files and exports\nKeep backups of your charts and review exported files before sharing them. You are responsible for how you use and distribute your work and for complying with applicable rules.");
				ImGui::Spacing();
				ImGui::TextWrapped("6. No warranty\nThis community port is provided as-is. Features may change, and the authors do not guarantee that every chart, device, or media file will work as expected.");
				ImGui::Spacing();
				ImGui::TextWrapped("If you disagree with these terms, choose Disagree to close the app. You can use the app after agreeing to this User Agreement.");
				ImGui::Spacing();
				ImGui::Separator();
				ImGui::Spacing();
				ImGui::TextWrapped("By Agreeing to the User Agreement, you are required to provide credit to the original creators and the creator of the port when posting on social media.");
			}
			ImGui::EndChild();
			ImGui::Spacing();
			const float buttonWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
			if (ImGui::Button("Disagree", { buttonWidth, 0.0f }))
			{
				ImGui::CloseCurrentPopup();
				AndroidRequestExit();
			}
			ImGui::SameLine();
			if (ImGui::Button("Agree", { buttonWidth, 0.0f }))
			{
				config.acceptedUserAgreement = true;
				config.write(userDataDir + APP_CONFIG_FILENAME);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
#endif
	}

	void Application::loadResources()
	{
		ResourceManager::loadShader(appDir + "res/shaders/basic2d");
#ifdef MIKUMIKUWORLD_SEKAI_LIVE_RENDERING
		ResourceManager::loadShader(appDir + "res/shaders/live_note");
#endif
		ResourceManager::loadShader(appDir + "res/shaders/masking");
		ResourceManager::loadShader(appDir + "res/shaders/particles");

		// TODO: Do not set the note skin texture indexes manually!
		const std::string notes01TexDir = appDir + "res/notes/01/";
		ResourceManager::loadTexture(notes01TexDir + "notes.png");
		ResourceManager::loadTexture(notes01TexDir + "longNoteLine.png");
		ResourceManager::loadTexture(notes01TexDir + "touchLine_eff.png");
		noteSkins.add("Notes 01", 0, 1, 2);

		const std::string notes02TexDir = appDir + "res/notes/02/";
		ResourceManager::loadTexture(notes02TexDir + "notes.png");
		ResourceManager::loadTexture(notes02TexDir + "longNoteLine.png");
		ResourceManager::loadTexture(notes02TexDir + "touchLine_eff.png");
		noteSkins.add("Notes 02", 3, 4, 5);

		const std::string editorAssetsDir = appDir + "res/editor/";
		ResourceManager::loadTexture(editorAssetsDir + "timeline_tools.png");
		ResourceManager::loadTexture(editorAssetsDir + "note_stats.png");
		ResourceManager::loadTexture(editorAssetsDir + "stage.png");
		const std::string comboAssetsDir = editorAssetsDir + "combo/";
		ResourceManager::loadTexture(comboAssetsDir + "pt.png");
		ResourceManager::loadTexture(comboAssetsDir + "pe.png");
		for (char digit = '0'; digit <= '9'; ++digit)
		{
			ResourceManager::loadTexture(comboAssetsDir + std::string("p") + digit + ".png");
			ResourceManager::loadTexture(comboAssetsDir + std::string("b") + digit + ".png");
		}
		const std::string hudAssetsDir = editorAssetsDir + "hud/";
		ResourceManager::loadTexture(hudAssetsDir + "score/bg.png");
		ResourceManager::loadTexture(hudAssetsDir + "score/bar.png");
		ResourceManager::loadTexture(hudAssetsDir + "score/fg.png");
		for (char rank : std::string("dcbas"))
		{
			ResourceManager::loadTexture(hudAssetsDir + std::string("score/rank/chr/") + rank + ".png");
			ResourceManager::loadTexture(hudAssetsDir + std::string("score/rank/txt/en/") + rank + ".png");
		}
		for (char digit : std::string("0123456789n"))
		{
			ResourceManager::loadTexture(hudAssetsDir + std::string("score/digit/") + digit + ".png");
			ResourceManager::loadTexture(hudAssetsDir + std::string("score/digit/s") + digit + ".png");
			ResourceManager::loadTexture(hudAssetsDir + std::string("life/digit/") + digit + ".png");
			ResourceManager::loadTexture(hudAssetsDir + std::string("life/digit/s") + digit + ".png");
		}
		ResourceManager::loadTexture(hudAssetsDir + "score/digit/plus.png");
		ResourceManager::loadTexture(hudAssetsDir + "life/bg.png");
		ResourceManager::loadTexture(hudAssetsDir + "life/normal.png");
		ResourceManager::loadTexture(hudAssetsDir + "judge/perfect.png");

		const std::string startupWarningImage = appDir + "res/warning.png";
		ResourceManager::loadTexture(startupWarningImage);
		startupWarningTextureIndex = ResourceManager::getTextureByFilename(startupWarningImage);

		ResourceManager::loadTransforms(appDir + "res/effect/transform.txt");

		// Load more languages here
		Localization::loadDefault();
		Localization::load("ja", u8"日本語", appDir + "res/i18n/ja.csv");
		Localization::load("zh-cn", u8"简体中文（中国大陆）", appDir + "res/i18n/zh-cn.csv");
		Localization::load("zh-tw", u8"繁體中文（台灣）", appDir + "res/i18n/zh-tw.csv");
	}

	void Application::setFullScreen(bool fullScreen)
	{
#if defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
		Application::windowState.fullScreen = config.fullScreen = fullScreen;
#else
		Application::windowState.fullScreen = config.fullScreen = fullScreen;

		GLFWmonitor* mainMonitor = glfwGetPrimaryMonitor();
		const GLFWvidmode* mode = glfwGetVideoMode(mainMonitor);
		if (fullScreen)
		{
			if (Application::windowState.maximized)
				glfwSetWindowAttrib(window, GLFW_MAXIMIZED, GLFW_FALSE);

			glfwSetWindowMonitor(window, mainMonitor, 0, 0, mode->width, mode->height, mode->refreshRate);
		}
		else
		{
			int width = Application::windowState.maximized ? mode->width : Application::windowState.size.x;
			int height = Application::windowState.maximized ? mode->height : Application::windowState.size.y;

			glfwSetWindowMonitor(
				window,
				NULL,
				Application::windowState.position.x,
				Application::windowState.position.y,
				width,
				height,
				mode->refreshRate
			);
		}
#endif
	}

	void Application::run()
	{
#if defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
		// The Android NativeActivity event loop calls update() while its EGL
		// surface is active.
#else
#ifdef _WIN32
		HWND hwnd = glfwGetWin32Window(window);

		/*
			Override the current GLFW/Imgui window procedure and store it in the GLFW window user pointer
		
			NOTE: For this to be safe, it should be only called AFTER ImGui is initialized
			so that the WndProc ImGui is expecting matches with our own WndProc
		*/
		glfwSetWindowUserPointer(window, (void*)::GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
		::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)wndProc);

		windowState.windowHandle = hwnd;
		windowState.windowTimerId = ::SetTimer(hwnd,
			reinterpret_cast<UINT_PTR>(&windowState.windowTimerId), USER_TIMER_MINIMUM, nullptr);

		::DragAcceptFiles(hwnd, TRUE);
#else
		windowState.windowHandle = nullptr;
#endif

		while (!glfwWindowShouldClose(window))
		{
			glfwPollEvents();
			update();
		}
		
		writeSettings();
#endif
	}

	bool Application::attemptSave()
	{
		return editor && editor->trySave(editor->getWorkingFilename().data());
	}

	bool Application::isEditorUpToDate() const
	{
		// Initialization can fail before the editor exists. Error reporting
		// must not turn that recoverable startup error into a null dereference.
		return !editor || editor->isUpToDate();
	}
}
