#include "Application.h"
#include "AndroidImGuiBackend.h"
#include "AndroidFileDialog.h"

#ifdef __ANDROID__
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/configuration.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sstream>
#include <string>

namespace
{
	using namespace MikuMikuWorld;
	constexpr const char* logTag = "MikuMikuWorld";

	struct AndroidState
	{
		android_app* app{};
		Application application;
		EGLDisplay display{ EGL_NO_DISPLAY };
		EGLConfig config{};
		EGLSurface surface{ EGL_NO_SURFACE };
		EGLContext context{ EGL_NO_CONTEXT };
		bool offscreenSurface{};
		bool initialized{};
		bool hasFocus{ true };
		bool shouldExit{};
		float density{ 1.0f };
	};

	void copyAssetTree(AAssetManager* assets, const std::filesystem::path& destination)
	{
		AAsset* indexAsset = AAssetManager_open(assets, "res/android_asset_index.txt", AASSET_MODE_BUFFER);
		// Accept the old hidden filename for APKs produced by earlier asset tasks.
		if (!indexAsset)
			indexAsset = AAssetManager_open(assets, "res/.android_asset_index", AASSET_MODE_BUFFER);
		if (!indexAsset)
		{
			__android_log_print(ANDROID_LOG_ERROR, logTag, "Bundled asset index is missing");
			return;
		}
		std::string index(AAsset_getLength(indexAsset), '\0');
		AAsset_read(indexAsset, index.data(), index.size());
		AAsset_close(indexAsset);
		std::istringstream paths(index);
		std::string relativePath;
		int copiedAssets = 0;
		while (std::getline(paths, relativePath))
		{
			if (!relativePath.empty() && relativePath.back() == '\r')
				relativePath.pop_back();
			if (relativePath.empty())
				continue;
			const std::string childAsset = "res/" + relativePath;
			const auto childDestination = destination / std::filesystem::path(relativePath);
			std::filesystem::create_directories(childDestination.parent_path());
			AAsset* asset = AAssetManager_open(assets, childAsset.c_str(), AASSET_MODE_STREAMING);
			if (!asset)
				continue;

			std::ofstream output(childDestination, std::ios::binary | std::ios::trunc);
			char buffer[32 * 1024];
			int count = 0;
			while ((count = AAsset_read(asset, buffer, sizeof(buffer))) > 0)
				output.write(buffer, count);
			output.close();
			AAsset_close(asset);
			++copiedAssets;
		}
		__android_log_print(ANDROID_LOG_INFO, logTag, "Copied %d bundled assets", copiedAssets);
	}

	bool createEglSurface(AndroidState& state)
	{
		if (!state.app->window)
			return false;

		if (state.display == EGL_NO_DISPLAY)
		{
			state.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
			if (state.display == EGL_NO_DISPLAY || !eglInitialize(state.display, nullptr, nullptr))
				return false;

			const EGLint configAttributes[] = {
				EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
				EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
				EGL_RED_SIZE, 8,
				EGL_GREEN_SIZE, 8,
				EGL_BLUE_SIZE, 8,
				EGL_ALPHA_SIZE, 8,
				EGL_DEPTH_SIZE, 16,
				EGL_NONE
			};
			EGLint configCount = 0;
			if (!eglChooseConfig(state.display, configAttributes, &state.config, 1, &configCount) || configCount == 0)
				return false;

			const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
			state.context = eglCreateContext(state.display, state.config, EGL_NO_CONTEXT, contextAttributes);
			if (state.context == EGL_NO_CONTEXT)
				return false;
		}

		if (state.surface != EGL_NO_SURFACE)
		{
			eglMakeCurrent(state.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			eglDestroySurface(state.display, state.surface);
			state.surface = EGL_NO_SURFACE;
		}

		// Reuse the EGL config that created the context. Choosing a second config
		// after Android returns from the document picker can make it incompatible.
		state.surface = eglCreateWindowSurface(state.display, state.config, state.app->window, nullptr);
		if (state.surface == EGL_NO_SURFACE)
		{
			__android_log_print(ANDROID_LOG_ERROR, logTag, "Could not recreate EGL window surface (0x%x)", eglGetError());
			return false;
		}

		if (!eglMakeCurrent(state.display, state.surface, state.surface, state.context))
		{
			__android_log_print(ANDROID_LOG_ERROR, logTag, "Could not make EGL surface current (0x%x)", eglGetError());
			eglDestroySurface(state.display, state.surface);
			state.surface = EGL_NO_SURFACE;
			return false;
		}

		int width = ANativeWindow_getWidth(state.app->window);
		int height = ANativeWindow_getHeight(state.app->window);
		state.offscreenSurface = false;
		AndroidImGui_SetDisplay(width, height, state.density);
		return true;
	}

	bool createExportPbuffer(AndroidState& state)
	{
		if (state.display == EGL_NO_DISPLAY || state.config == nullptr || state.context == EGL_NO_CONTEXT)
			return false;
		const EGLint attributes[] = {
			EGL_WIDTH, std::max(AndroidImGui_GetViewportWidth(), 1),
			EGL_HEIGHT, std::max(AndroidImGui_GetViewportHeight(), 1),
			EGL_NONE
		};
		state.surface = eglCreatePbufferSurface(state.display, state.config, attributes);
		if (state.surface == EGL_NO_SURFACE)
		{
			__android_log_print(ANDROID_LOG_ERROR, logTag, "Could not create export pbuffer (0x%x)", eglGetError());
			return false;
		}
		if (!eglMakeCurrent(state.display, state.surface, state.surface, state.context))
		{
			__android_log_print(ANDROID_LOG_ERROR, logTag, "Could not activate export pbuffer (0x%x)", eglGetError());
			eglDestroySurface(state.display, state.surface);
			state.surface = EGL_NO_SURFACE;
			return false;
		}
		state.offscreenSurface = true;
		return true;
	}

	void destroyEglSurface(AndroidState& state)
	{
		if (state.display != EGL_NO_DISPLAY)
		{
			eglMakeCurrent(state.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			if (state.surface != EGL_NO_SURFACE)
				eglDestroySurface(state.display, state.surface);
		}
		state.surface = EGL_NO_SURFACE;
		state.offscreenSurface = false;
	}

	void handleCommand(android_app* app, int32_t command)
	{
		auto* state = static_cast<AndroidState*>(app->userData);
		switch (command)
		{
		case APP_CMD_INIT_WINDOW:
			if (createEglSurface(*state) && !state->initialized)
			{
				try
				{
					const std::filesystem::path root(app->activity->internalDataPath);
					copyAssetTree(app->activity->assetManager, root / "res");
					const auto result = state->application.initialize(root.string() + "/");
					if (!result.isOk())
						throw std::runtime_error(result.getMessage());
					state->initialized = true;
				}
				catch (const std::exception& exception)
				{
					__android_log_print(ANDROID_LOG_ERROR, logTag, "Startup failed: %s", exception.what());
					ANativeActivity_finish(app->activity);
				}
			}
			break;
		case APP_CMD_TERM_WINDOW:
			if (state->initialized)
				state->application.writeSettings();
			state->hasFocus = false;
			destroyEglSurface(*state);
			if (state->initialized && AndroidFileDialog_IsBackgroundExportActive())
				createExportPbuffer(*state);
			break;
		case APP_CMD_PAUSE:
			if (state->initialized)
				{
                                state->application.writeSettings();
                                state->application.autoSaveNow();
                        }
			break;
		case APP_CMD_GAINED_FOCUS:
			state->hasFocus = true;
			if (state->offscreenSurface && app->window)
				createEglSurface(*state);
			break;
		case APP_CMD_LOST_FOCUS:
			state->hasFocus = false;
			if (state->initialized && AndroidFileDialog_IsBackgroundExportActive() && !state->offscreenSurface)
			{
				destroyEglSurface(*state);
				createExportPbuffer(*state);
			}
			break;
		case APP_CMD_CONFIG_CHANGED:
			{
				AConfiguration* configuration = AConfiguration_new();
				AConfiguration_fromAssetManager(configuration, app->activity->assetManager);
				const int32_t deviceDensity = AConfiguration_getDensity(configuration);
				state->density = deviceDensity == ACONFIGURATION_DENSITY_DEFAULT ? 1.0f : deviceDensity / 160.0f;
				AConfiguration_delete(configuration);
				if (app->window)
					AndroidImGui_SetDisplay(ANativeWindow_getWidth(app->window), ANativeWindow_getHeight(app->window), state->density);
			}
			break;
		case APP_CMD_WINDOW_RESIZED:
			if (app->window)
				AndroidImGui_SetDisplay(ANativeWindow_getWidth(app->window), ANativeWindow_getHeight(app->window), state->density);
			break;
		case APP_CMD_DESTROY:
			state->shouldExit = true;
			break;
		default:
			break;
		}
	}

	int32_t handleInput(android_app* app, AInputEvent* event)
	{
		auto* state = static_cast<AndroidState*>(app->userData);
		if (!state->initialized)
			return 0;

		if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION)
		{
			AndroidImGui_HandleMotion(event);
			return 1;
		}
		if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY)
		{
			if (AKeyEvent_getKeyCode(event) == AKEYCODE_BACK && AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_UP)
			{
				Application::windowState.closing = true;
				return 1;
			}
			AndroidImGui_HandleKey(event);
			return 1;
		}
		return 0;
	}
}

void android_main(android_app* app)
{
	app_dummy();
	AConfiguration* configuration = AConfiguration_new();
	AConfiguration_fromAssetManager(configuration, app->activity->assetManager);
	const int32_t deviceDensity = AConfiguration_getDensity(configuration);
	AndroidState state{};
	state.app = app;
	state.density = deviceDensity == ACONFIGURATION_DENSITY_DEFAULT ? 1.0f : deviceDensity / 160.0f;
	AConfiguration_delete(configuration);
	app->userData = &state;
	app->onAppCmd = handleCommand;
	app->onInputEvent = handleInput;
	AndroidFileDialog_Initialize(app);

	while (!state.shouldExit && !app->destroyRequested)
	{
		int events = 0;
		android_poll_source* source = nullptr;
		// Re-evaluate after each command: losing focus can switch the surface
		// to a pbuffer inside this loop, and must not leave export blocked in poll.
		auto canRender = [&]() {
			return state.surface != EGL_NO_SURFACE &&
				(state.hasFocus || (AndroidFileDialog_IsBackgroundExportActive() && state.offscreenSurface));
		};
		while (ALooper_pollOnce(canRender() ? 0 : -1, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0)
		{
			if (source)
				source->process(app, source);
			if (app->destroyRequested)
				break;
		}

		if (state.initialized && canRender())
		{
			if (!state.offscreenSurface && app->window)
			{
				const int surfaceWidth = ANativeWindow_getWidth(app->window);
				const int surfaceHeight = ANativeWindow_getHeight(app->window);
				glViewport(0, 0, surfaceWidth, surfaceHeight);
				glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
				glClear(GL_COLOR_BUFFER_BIT);
			}
			glViewport(AndroidImGui_GetViewportLeft(), AndroidImGui_GetViewportTop(), AndroidImGui_GetViewportWidth(), AndroidImGui_GetViewportHeight());
			state.application.update();
			AndroidFileDialog_UpdateTextInput(ImGui::GetIO().WantTextInput);
			if (!state.offscreenSurface)
				eglSwapBuffers(state.display, state.surface);
			if (AndroidExitRequested())
				state.shouldExit = true;
		}
	}

	if (state.initialized)
	{
		state.application.writeSettings();
		state.application.dispose();
	}
	destroyEglSurface(state);
	if (state.context != EGL_NO_CONTEXT)
		eglDestroyContext(state.display, state.context);
	if (state.display != EGL_NO_DISPLAY)
		eglTerminate(state.display);
}
#endif
