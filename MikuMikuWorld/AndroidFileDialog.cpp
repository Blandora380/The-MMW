#include "AndroidFileDialog.h"
#include "AndroidImGuiBackend.h"

#ifdef __ANDROID__
#include <jni.h>
#include <EGL/egl.h>
#include <android/looper.h>
#include <android/log.h>
#include <condition_variable>
#include <chrono>
#include <mutex>

extern "C" JNIEXPORT void JNICALL Java_com_mikumikuworld_editor_MainActivity_nativePickerResult(
	JNIEnv*, jclass, jlong, jstring, jboolean);
extern "C" JNIEXPORT void JNICALL Java_com_mikumikuworld_editor_MainActivity_nativeTextInput(
	JNIEnv*, jclass, jstring);
extern "C" JNIEXPORT void JNICALL Java_com_mikumikuworld_editor_MainActivity_nativeKeyInput(
	JNIEnv*, jclass, jint, jboolean);

namespace MikuMikuWorld
{
	namespace
	{
		JavaVM* javaVm{};
		android_app* nativeApp{};
		jobject activityObject{};
		std::mutex pickerMutex;
		std::condition_variable pickerFinished;
		jlong nextPickerId = 1;
		jlong pendingPickerId{};
		bool pickerDone{};
		std::string pickerResult;
		bool textInputVisible{};
		bool backgroundExportActive{};
		std::chrono::steady_clock::time_point lastProgressUpdate{};

		JNIEnv* getEnvironment(bool& attached)
		{
			attached = false;
			JNIEnv* environment = nullptr;
			if (!javaVm)
				return nullptr;
			if (javaVm->GetEnv(reinterpret_cast<void**>(&environment), JNI_VERSION_1_6) == JNI_EDETACHED)
			{
				if (javaVm->AttachCurrentThread(&environment, nullptr) != JNI_OK)
					return nullptr;
				attached = true;
			}
			return environment;
		}

		std::string fromJava(JNIEnv* environment, jstring value)
		{
			if (!value)
				return {};
			const char* chars = environment->GetStringUTFChars(value, nullptr);
			std::string result = chars ? chars : "";
			if (chars)
				environment->ReleaseStringUTFChars(value, chars);
			return result;
		}
	}

	void AndroidFileDialog_Initialize(android_app* app)
	{
		nativeApp = app;
		ANativeActivity* activity = app->activity;
		javaVm = activity->vm;
		bool attached = false;
		JNIEnv* environment = getEnvironment(attached);
		if (environment)
		{
			activityObject = environment->NewGlobalRef(activity->clazz);
			const jclass activityClass = environment->GetObjectClass(activityObject);
			const JNINativeMethod methods[] = {
				{ const_cast<char*>("nativePickerResult"), const_cast<char*>("(JLjava/lang/String;Z)V"), reinterpret_cast<void*>(Java_com_mikumikuworld_editor_MainActivity_nativePickerResult) },
				{ const_cast<char*>("nativeTextInput"), const_cast<char*>("(Ljava/lang/String;)V"), reinterpret_cast<void*>(Java_com_mikumikuworld_editor_MainActivity_nativeTextInput) },
				{ const_cast<char*>("nativeKeyInput"), const_cast<char*>("(IZ)V"), reinterpret_cast<void*>(Java_com_mikumikuworld_editor_MainActivity_nativeKeyInput) }
			};
			if (environment->RegisterNatives(activityClass, methods, sizeof(methods) / sizeof(methods[0])) != JNI_OK)
			{
				__android_log_print(ANDROID_LOG_ERROR, "MikuMikuWorld", "Could not register MainActivity native callbacks");
				if (environment->ExceptionCheck())
				{
					environment->ExceptionDescribe();
					environment->ExceptionClear();
				}
			}
			environment->DeleteLocalRef(activityClass);
		}
		if (attached)
			javaVm->DetachCurrentThread();
	}

	void AndroidFileDialog_UpdateTextInput(bool visible)
	{
		if (textInputVisible == visible)
			return;
		textInputVisible = visible;
		bool attached = false;
		JNIEnv* environment = getEnvironment(attached);
		if (!environment || !activityObject)
			return;
		const jclass activityClass = environment->GetObjectClass(activityObject);
		const jmethodID method = environment->GetMethodID(activityClass, "setTextInputEnabled", "(Z)V");
		if (method)
			environment->CallVoidMethod(activityObject, method, visible ? JNI_TRUE : JNI_FALSE);
		if (environment->ExceptionCheck())
			environment->ExceptionClear();
		if (attached)
			javaVm->DetachCurrentThread();
	}

	std::string AndroidFileDialog_Pick(bool save, const std::string& title,
		const std::vector<std::string>& patterns, const std::string& defaultExtension,
		const std::string& suggestedName)
	{
		bool attached = false;
		JNIEnv* environment = getEnvironment(attached);
		if (!environment || !activityObject)
			return {};

		std::string extensionList;
		for (const auto& pattern : patterns)
		{
			if (!extensionList.empty())
				extensionList.push_back(';');
			extensionList.append(pattern);
		}

		const jclass activityClass = environment->GetObjectClass(activityObject);
		const jmethodID requestMethod = environment->GetMethodID(activityClass, "requestDocument",
			"(JLjava/lang/String;ZLjava/lang/String;Ljava/lang/String;)V");
		if (!requestMethod)
		{
			environment->ExceptionClear();
			if (attached) javaVm->DetachCurrentThread();
			return {};
		}

		jlong requestId;
		{
			std::lock_guard<std::mutex> lock(pickerMutex);
			requestId = nextPickerId++;
			pendingPickerId = requestId;
			pickerResult.clear();
			pickerDone = false;
		}
		jstring javaTitle = environment->NewStringUTF(title.c_str());
		jstring javaExtensions = environment->NewStringUTF(extensionList.c_str());
		jstring javaName = environment->NewStringUTF((suggestedName + (defaultExtension.empty() ? "" : "." + defaultExtension)).c_str());
		environment->CallVoidMethod(activityObject, requestMethod, requestId, javaTitle, save ? JNI_TRUE : JNI_FALSE, javaExtensions, javaName);
		environment->DeleteLocalRef(javaTitle);
		environment->DeleteLocalRef(javaExtensions);
		environment->DeleteLocalRef(javaName);
		if (environment->ExceptionCheck())
		{
			environment->ExceptionClear();
			if (attached) javaVm->DetachCurrentThread();
			return {};
		}
		if (attached)
			javaVm->DetachCurrentThread();

		for (;;)
		{
			bool resultReady = false;
			{
				std::lock_guard<std::mutex> lock(pickerMutex);
				if (pickerDone && pendingPickerId == requestId)
					resultReady = true;
			}
			// Android may deliver onActivityResult before the NativeActivity has
			// recreated and rebound its EGL window surface. The caller can load an
			// image immediately after this function returns, so wait until the
			// renderer's context is current again before releasing the picker.
			if (resultReady && (eglGetCurrentContext() != EGL_NO_CONTEXT ||
				(nativeApp && nativeApp->destroyRequested)))
			{
				std::lock_guard<std::mutex> lock(pickerMutex);
				return pickerResult;
			}

			// Keep processing NativeActivity lifecycle commands while the system
			// picker is open. Blocking on a condition variable here prevents
			// APP_CMD_PAUSE/APP_CMD_TERM_WINDOW from being acknowledged and can
			// leave Android waiting on the app's UI thread (or surface) indefinitely.
			int events = 0;
			android_poll_source* source = nullptr;
			const int pollResult = ALooper_pollOnce(16, nullptr, &events, reinterpret_cast<void**>(&source));
			if (pollResult >= 0 && source)
				source->process(nativeApp, source);
		}
	}

	bool AndroidFileDialog_Commit(const std::string& filename)
	{
		bool attached = false;
		JNIEnv* environment = getEnvironment(attached);
		if (!environment || !activityObject)
			return false;
		const jclass activityClass = environment->GetObjectClass(activityObject);
		const jmethodID commitMethod = environment->GetMethodID(activityClass, "commitDocument", "(Ljava/lang/String;)Z");
		if (!commitMethod)
		{
			environment->ExceptionClear();
			if (attached) javaVm->DetachCurrentThread();
			return false;
		}
		jstring javaFilename = environment->NewStringUTF(filename.c_str());
		const jboolean result = environment->CallBooleanMethod(activityObject, commitMethod, javaFilename);
		environment->DeleteLocalRef(javaFilename);
		if (environment->ExceptionCheck())
		{
			environment->ExceptionClear();
			if (attached) javaVm->DetachCurrentThread();
			return false;
		}
		if (attached)
			javaVm->DetachCurrentThread();
		return result == JNI_TRUE;
	}

	int64_t AndroidFileDialog_StartFfmpeg(const std::vector<std::string>& arguments)
	{
		bool attached = false;
		JNIEnv* environment = getEnvironment(attached);
		if (!environment || !activityObject)
			return 0;

		const jclass activityClass = environment->GetObjectClass(activityObject);
		const jmethodID method = environment->GetMethodID(activityClass, "startFfmpegExport", "([Ljava/lang/String;)J");
		if (!method)
		{
			environment->ExceptionClear();
			if (attached) javaVm->DetachCurrentThread();
			return 0;
		}
		jclass stringClass = environment->FindClass("java/lang/String");
		jobjectArray javaArguments = environment->NewObjectArray(static_cast<jsize>(arguments.size()), stringClass, nullptr);
		for (jsize i = 0; i < static_cast<jsize>(arguments.size()); ++i)
		{
			jstring argument = environment->NewStringUTF(arguments[i].c_str());
			environment->SetObjectArrayElement(javaArguments, i, argument);
			environment->DeleteLocalRef(argument);
		}
		const jlong sessionId = environment->CallLongMethod(activityObject, method, javaArguments);
		environment->DeleteLocalRef(javaArguments);
		environment->DeleteLocalRef(stringClass);
		environment->DeleteLocalRef(activityClass);
		if (environment->ExceptionCheck())
		{
			environment->ExceptionClear();
			if (attached) javaVm->DetachCurrentThread();
			return 0;
		}
		if (attached) javaVm->DetachCurrentThread();
		return static_cast<int64_t>(sessionId);
	}

	int AndroidFileDialog_GetFfmpegStatus(int64_t sessionId)
	{
		bool attached = false;
		JNIEnv* environment = getEnvironment(attached);
		if (!environment || !activityObject)
			return -1;
		const jclass activityClass = environment->GetObjectClass(activityObject);
		const jmethodID method = environment->GetMethodID(activityClass, "getFfmpegExportStatus", "(J)I");
		if (!method)
		{
			environment->ExceptionClear();
			if (attached) javaVm->DetachCurrentThread();
			return -1;
		}
		const jint status = environment->CallIntMethod(activityObject, method, static_cast<jlong>(sessionId));
		environment->DeleteLocalRef(activityClass);
		if (environment->ExceptionCheck())
		{
			environment->ExceptionClear();
			if (attached) javaVm->DetachCurrentThread();
			return -1;
		}
		if (attached) javaVm->DetachCurrentThread();
		return status;
	}

	void AndroidFileDialog_CancelFfmpeg(int64_t sessionId)
	{
		bool attached = false;
		JNIEnv* environment = getEnvironment(attached);
		if (!environment || !activityObject)
			return;
		const jclass activityClass = environment->GetObjectClass(activityObject);
		const jmethodID method = environment->GetMethodID(activityClass, "cancelFfmpegExport", "(J)V");
		if (method)
			environment->CallVoidMethod(activityObject, method, static_cast<jlong>(sessionId));
		environment->DeleteLocalRef(activityClass);
		if (environment->ExceptionCheck())
			environment->ExceptionClear();
		if (attached) javaVm->DetachCurrentThread();
	}

	void AndroidFileDialog_ShowExportVideoPlayer()
	{
		backgroundExportActive = true;
		lastProgressUpdate = {};
		bool attached = false;
		JNIEnv* environment = getEnvironment(attached);
		if (!environment || !activityObject)
			return;
		const jclass activityClass = environment->GetObjectClass(activityObject);
		const jmethodID method = environment->GetMethodID(activityClass, "showExportVideoPlayer", "()V");
		if (method)
			environment->CallVoidMethod(activityObject, method);
		environment->DeleteLocalRef(activityClass);
		if (environment->ExceptionCheck())
			environment->ExceptionClear();
		if (attached) javaVm->DetachCurrentThread();
	}

	void AndroidFileDialog_HideExportVideoPlayer()
	{
		backgroundExportActive = false;
		bool attached = false;
		JNIEnv* environment = getEnvironment(attached);
		if (!environment || !activityObject)
			return;
		const jclass activityClass = environment->GetObjectClass(activityObject);
		const jmethodID method = environment->GetMethodID(activityClass, "hideExportVideoPlayer", "()V");
		if (method)
			environment->CallVoidMethod(activityObject, method);
		environment->DeleteLocalRef(activityClass);
		if (environment->ExceptionCheck())
			environment->ExceptionClear();
		if (attached) javaVm->DetachCurrentThread();
	}

	bool AndroidFileDialog_IsBackgroundExportActive()
	{
		return backgroundExportActive;
	}

	void AndroidFileDialog_UpdateExportProgress(uint64_t frames, uint64_t total, bool finalizing)
	{
		const auto now = std::chrono::steady_clock::now();
		if (!finalizing && frames > 0 && now - lastProgressUpdate < std::chrono::milliseconds(500))
			return;
		lastProgressUpdate = now;
		bool attached = false;
		JNIEnv* environment = getEnvironment(attached);
		if (!environment || !activityObject)
			return;
		const jclass activityClass = environment->GetObjectClass(activityObject);
		const jmethodID method = environment->GetMethodID(activityClass, "updateExportProgress", "(JJZ)V");
		if (method)
			environment->CallVoidMethod(activityObject, method, static_cast<jlong>(frames), static_cast<jlong>(total), finalizing ? JNI_TRUE : JNI_FALSE);
		if (environment->ExceptionCheck())
			environment->ExceptionClear();
		environment->DeleteLocalRef(activityClass);
		if (attached) javaVm->DetachCurrentThread();
	}

	void AndroidFileDialog_ShowPermissions()
	{
		bool attached = false;
		JNIEnv* environment = getEnvironment(attached);
		if (!environment || !activityObject)
			return;
		const jclass activityClass = environment->GetObjectClass(activityObject);
		const jmethodID method = environment->GetMethodID(activityClass, "showAppPermissions", "()V");
		if (method)
			environment->CallVoidMethod(activityObject, method);
		if (environment->ExceptionCheck())
			environment->ExceptionClear();
		environment->DeleteLocalRef(activityClass);
		if (attached) javaVm->DetachCurrentThread();
	}

	void AndroidFileDialog_OpenUrl(const std::string& url)
	{
		bool attached = false;
		JNIEnv* environment = getEnvironment(attached);
		if (!environment || !activityObject)
			return;
		const jclass activityClass = environment->GetObjectClass(activityObject);
		const jmethodID method = environment->GetMethodID(activityClass, "openUrl", "(Ljava/lang/String;)V");
		if (method)
		{
			jstring javaUrl = environment->NewStringUTF(url.c_str());
			environment->CallVoidMethod(activityObject, method, javaUrl);
			environment->DeleteLocalRef(javaUrl);
		}
		if (environment->ExceptionCheck())
			environment->ExceptionClear();
		environment->DeleteLocalRef(activityClass);
		if (attached) javaVm->DetachCurrentThread();
	}
}

extern "C" JNIEXPORT void JNICALL
Java_com_mikumikuworld_editor_MainActivity_nativePickerResult(JNIEnv* environment, jclass, jlong requestId, jstring filename, jboolean)
{
	std::lock_guard<std::mutex> lock(MikuMikuWorld::pickerMutex);
	if (requestId != MikuMikuWorld::pendingPickerId)
		return;
	MikuMikuWorld::pickerResult = MikuMikuWorld::fromJava(environment, filename);
	MikuMikuWorld::pickerDone = true;
	MikuMikuWorld::pickerFinished.notify_all();
}

extern "C" JNIEXPORT void JNICALL
Java_com_mikumikuworld_editor_MainActivity_nativeTextInput(JNIEnv* environment, jclass, jstring value)
{
	const std::string text = MikuMikuWorld::fromJava(environment, value);
	MikuMikuWorld::AndroidImGui_QueueText(text.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_mikumikuworld_editor_MainActivity_nativeKeyInput(JNIEnv*, jclass, jint keyCode, jboolean down)
{
	MikuMikuWorld::AndroidImGui_QueueKey(keyCode, down == JNI_TRUE);
}
#endif
