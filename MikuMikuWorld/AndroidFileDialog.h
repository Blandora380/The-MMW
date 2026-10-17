#pragma once

#ifdef __ANDROID__
#include <android_native_app_glue.h>
#endif
#if defined(__ANDROID__) || defined(MIKUMIKUWORLD_IOS)
#include <cstdint>
#include <string>
#include <vector>

namespace MikuMikuWorld
{
#ifdef __ANDROID__
	void AndroidFileDialog_Initialize(android_app* app);
#endif
	void AndroidFileDialog_UpdateTextInput(bool visible);
	std::string AndroidFileDialog_Pick(bool save, const std::string& title,
		const std::vector<std::string>& patterns, const std::string& defaultExtension,
		const std::string& suggestedName);
	bool AndroidFileDialog_Commit(const std::string& filename);
#ifdef __ANDROID__
	int64_t AndroidFileDialog_StartFfmpeg(const std::vector<std::string>& arguments);
	int AndroidFileDialog_GetFfmpegStatus(int64_t sessionId);
	void AndroidFileDialog_CancelFfmpeg(int64_t sessionId);
#endif
	void AndroidFileDialog_ShowExportVideoPlayer();
	void AndroidFileDialog_HideExportVideoPlayer();
	void AndroidFileDialog_UpdateExportProgress(uint64_t frames, uint64_t total, bool finalizing = false);
	void AndroidFileDialog_ShowPermissions();
	bool AndroidFileDialog_IsBackgroundExportActive();
	void AndroidFileDialog_OpenUrl(const std::string& url);
}
#endif
