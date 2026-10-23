#ifdef __ANDROID__
#include <poll.h>
#endif
#include "FfmpegEncoder.h"
#include "IO.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <thread>
#include <csignal>
#ifdef MIKUMIKUWORLD_IOS
#include "../ios/IOSPlatform.h"
#endif

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

#ifdef __ANDROID__
#include "AndroidFileDialog.h"
#include <android/log.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
extern "C"
{
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
}
#endif

namespace MikuMikuWorld
{
#ifdef _WIN32
	namespace
	{
		std::wstring quoteWindowsArgument(const std::wstring& argument)
		{
			std::wstring quoted = L"\"";
			size_t backslashes = 0;
			for (wchar_t ch : argument)
			{
				if (ch == L'\\')
				{
					++backslashes;
					continue;
				}

				if (ch == L'\"')
				{
					quoted.append(backslashes * 2 + 1, L'\\');
					quoted.push_back(ch);
					backslashes = 0;
					continue;
				}

				quoted.append(backslashes, L'\\');
				backslashes = 0;
				quoted.push_back(ch);
			}

			quoted.append(backslashes * 2, L'\\');
			quoted.push_back(L'\"');
			return quoted;
		}

		std::wstring findFfmpeg()
		{
			DWORD pathSize = GetEnvironmentVariableW(L"PATH", nullptr, 0);
			if (!pathSize)
				return {};

			std::wstring path(pathSize, L'\0');
			DWORD pathLength = GetEnvironmentVariableW(L"PATH", path.data(), pathSize);
			if (!pathLength || pathLength >= pathSize)
				return {};
			path.resize(pathLength);

			std::wistringstream directories(path);
			std::wstring directory;
			while (std::getline(directories, directory, L';'))
			{
				if (directory.empty())
					continue;
				DWORD size = SearchPathW(directory.c_str(), L"ffmpeg.exe", nullptr, 0, nullptr, nullptr);
				if (!size)
					continue;

				std::wstring executable(size, L'\0');
				DWORD written = SearchPathW(directory.c_str(), L"ffmpeg.exe", nullptr, size, executable.data(), nullptr);
				if (written && written < size)
				{
					executable.resize(written);
					return executable;
				}
			}
			return {};
		}
	}
#endif

	FfmpegEncoder::~FfmpegEncoder()
	{
		cancel();
	}

	bool FfmpegEncoder::isAvailable()
	{
#ifdef _WIN32
		return !findFfmpeg().empty();
#elif defined(MIKUMIKUWORLD_IOS)
		return true; // AVFoundation H.264 writer, no external executable.
#elif defined(__ANDROID__)
		return (avcodec_find_encoder_by_name("libx264") != nullptr ||
			avcodec_find_encoder_by_name("h264_mediacodec") != nullptr) &&
			av_guess_format("mp4", nullptr, nullptr) != nullptr;
#else
		return false;
#endif
	}

	bool FfmpegEncoder::start(const std::string& outputFilename, int width, int height, int fps,
		const std::string& audioFilename, float audioStart, float audioDelay,
		const std::string& soundEffectsFilename, float duration, std::string& error)
	{
#ifdef _WIN32
		cancel();
		const std::wstring executable = findFfmpeg();
		if (executable.empty())
		{
			error = "FFmpeg was not found on PATH.";
			return false;
		}

		std::vector<std::wstring> arguments = {
			executable, L"-y", L"-hide_banner", L"-loglevel", L"error",
			L"-f", L"rawvideo", L"-pixel_format", L"rgba",
			L"-video_size", IO::mbToWideStr(IO::formatString("%dx%d", width, height)),
			L"-framerate", IO::mbToWideStr(std::to_string(fps)), L"-i", L"pipe:0"
		};

		int musicInputIndex = -1;
		int soundEffectsInputIndex = -1;
		int nextInputIndex = 1;
		if (!audioFilename.empty())
		{
			musicInputIndex = nextInputIndex++;
			if (audioDelay > 0.0f)
			{
				arguments.push_back(L"-itsoffset");
				arguments.push_back(IO::mbToWideStr(IO::formatString("%.6f", audioDelay)));
			}
			arguments.push_back(L"-ss");
			arguments.push_back(IO::mbToWideStr(IO::formatString("%.6f", std::max(0.0f, audioStart))));
			arguments.push_back(L"-i");
			arguments.push_back(IO::mbToWideStr(audioFilename));
		}
		if (!soundEffectsFilename.empty())
		{
			soundEffectsInputIndex = nextInputIndex++;
			arguments.push_back(L"-i");
			arguments.push_back(IO::mbToWideStr(soundEffectsFilename));
		}

		arguments.insert(arguments.end(), { L"-t", IO::mbToWideStr(IO::formatString("%.6f", duration)),
			L"-c:v", L"libx264", L"-preset", L"veryfast", L"-crf", L"18", L"-pix_fmt", L"yuv420p" });
		// readPreviewPixels supplies bottom-up OpenGL rows on every platform.
		// Match Android's row conversion while preserving left-to-right order.
		arguments.insert(arguments.end(), { L"-vf", L"vflip" });
		arguments.insert(arguments.end(), { L"-map", L"0:v:0" });
		if (musicInputIndex >= 0 && soundEffectsInputIndex >= 0)
		{
			const std::string mixGraph = IO::formatString("[%d:a:0][%d:a:0]amix=inputs=2:duration=longest[aout]",
				musicInputIndex, soundEffectsInputIndex);
			arguments.insert(arguments.end(), { L"-filter_complex", IO::mbToWideStr(mixGraph), L"-map", L"[aout]" });
		}
		else if (musicInputIndex >= 0)
		{
			arguments.insert(arguments.end(), { L"-map", IO::mbToWideStr(IO::formatString("%d:a:0", musicInputIndex)) });
		}
		else if (soundEffectsInputIndex >= 0)
		{
			arguments.insert(arguments.end(), { L"-map", IO::mbToWideStr(IO::formatString("%d:a:0", soundEffectsInputIndex)) });
		}
		if (musicInputIndex >= 0 || soundEffectsInputIndex >= 0)
			arguments.insert(arguments.end(), { L"-c:a", L"aac", L"-b:a", L"192k" });
		else
			arguments.push_back(L"-an");
		arguments.push_back(IO::mbToWideStr(outputFilename));

		std::wstring commandLine;
		for (const auto& argument : arguments)
		{
			if (!commandLine.empty()) commandLine.push_back(L' ');
			commandLine += quoteWindowsArgument(argument);
		}

		SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
		HANDLE pipeRead = nullptr, pipeWrite = nullptr;
		if (!CreatePipe(&pipeRead, &pipeWrite, &security, 0))
		{
			error = "Could not create the FFmpeg input pipe.";
			return false;
		}
		if (!SetHandleInformation(pipeWrite, HANDLE_FLAG_INHERIT, 0))
		{
			CloseHandle(pipeRead);
			CloseHandle(pipeWrite);
			error = "Could not configure the FFmpeg input pipe.";
			return false;
		}
		HANDLE nullOutput = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &security,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = pipeRead;
		startup.hStdOutput = nullOutput != INVALID_HANDLE_VALUE ? nullOutput : GetStdHandle(STD_OUTPUT_HANDLE);
		startup.hStdError = nullOutput != INVALID_HANDLE_VALUE ? nullOutput : GetStdHandle(STD_ERROR_HANDLE);
		PROCESS_INFORMATION process{};
		const BOOL launched = CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
			CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
		CloseHandle(pipeRead);
		if (nullOutput != INVALID_HANDLE_VALUE)
			CloseHandle(nullOutput);
		if (!launched)
		{
			CloseHandle(pipeWrite);
			error = "Could not start FFmpeg.";
			return false;
		}

		CloseHandle(process.hThread);
		processHandle = process.hProcess;
		inputHandle = pipeWrite;
		return true;
#elif defined(MIKUMIKUWORLD_IOS)
		cancel();
		processHandle = IOSVideoStart(outputFilename, width, height, fps, audioFilename,
			audioStart, audioDelay, soundEffectsFilename, duration, error);
		return processHandle != nullptr;
#else
	#ifdef __ANDROID__
		cancel();
		if (!isAvailable())
		{
			error = "The bundled FFmpeg build does not include an H.264 MP4 encoder.";
			return false;
		}
		try
		{
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			androidPipePath = outputFilename + ".mmw-" + std::to_string(stamp) + ".rgba";
			androidOutputPath = outputFilename;
		}
		catch (...)
		{
			error = "Could not create a temporary video input pipe.";
			return false;
		}
		if (mkfifo(androidPipePath.c_str(), 0600) != 0)
		{
			androidPipePath.clear();
			error = "Could not create a temporary video input pipe.";
			return false;
		}
		// Keep both FIFO ends open locally until finish(). Some Android FFmpegKit
		// builds otherwise observe EOF between opening the reader and the first frame.
		androidPipeHandle = open(androidPipePath.c_str(), O_RDWR | O_NONBLOCK);
		if (androidPipeHandle < 0)
		{
			unlink(androidPipePath.c_str());
			androidPipePath.clear();
			error = "Could not open the temporary FFmpeg input pipe.";
			return false;
		}
		std::signal(SIGPIPE, SIG_IGN);
#ifdef F_SETPIPE_SZ
                fcntl(androidPipeHandle, F_SETPIPE_SZ, 1 << 20);
#endif

		const bool useLibx264 = avcodec_find_encoder_by_name("libx264") != nullptr;
		const char* videoEncoder = useLibx264 ? "libx264" : "h264_mediacodec";
		std::vector<std::string> arguments = {
			"-y", "-hide_banner", "-loglevel", "error", "-f", "rawvideo", "-pixel_format", "rgba",
			"-video_size", IO::formatString("%dx%d", width, height), "-framerate", std::to_string(fps),
			"-i", androidPipePath
		};
		int musicInputIndex = -1;
		int soundEffectsInputIndex = -1;
		int nextInputIndex = 1;
		if (!audioFilename.empty())
		{
			musicInputIndex = nextInputIndex++;
			if (audioDelay > 0.0f)
			{
				arguments.push_back("-itsoffset");
				arguments.push_back(IO::formatString("%.6f", audioDelay));
			}
			arguments.push_back("-ss");
			arguments.push_back(IO::formatString("%.6f", std::max(0.0f, audioStart)));
			arguments.push_back("-i");
			arguments.push_back(audioFilename);
		}
		if (!soundEffectsFilename.empty())
		{
			soundEffectsInputIndex = nextInputIndex++;
			arguments.push_back("-i");
			arguments.push_back(soundEffectsFilename);
		}
		arguments.insert(arguments.end(), { "-t", IO::formatString("%.6f", duration), "-c:v", videoEncoder });
		if (useLibx264)
			arguments.insert(arguments.end(), { "-preset", "ultrafast", "-crf", "18", "-pix_fmt", "yuv420p" });
		else
		{
			const int64_t targetBitrate = std::clamp<int64_t>(
				static_cast<int64_t>(width) * height * fps / 8, 2000000, 60000000);
			arguments.insert(arguments.end(), { "-b:v", std::to_string(targetBitrate), "-pix_fmt", "yuv420p" });
		}
		// OpenGL readback is bottom-up; let FFmpeg flip by changing frame line
		// direction instead of copying every pixel through another full-size buffer.
		arguments.insert(arguments.end(), { "-vf", "vflip" });
		arguments.insert(arguments.end(), { "-map", "0:v:0" });
		if (musicInputIndex >= 0 && soundEffectsInputIndex >= 0)
		{
			arguments.insert(arguments.end(), { "-filter_complex",
				IO::formatString("[%d:a:0][%d:a:0]amix=inputs=2:duration=longest[aout]", musicInputIndex, soundEffectsInputIndex),
				"-map", "[aout]" });
		}
		else if (musicInputIndex >= 0)
			arguments.insert(arguments.end(), { "-map", IO::formatString("%d:a:0", musicInputIndex) });
		else if (soundEffectsInputIndex >= 0)
			arguments.insert(arguments.end(), { "-map", IO::formatString("%d:a:0", soundEffectsInputIndex) });
		if (musicInputIndex >= 0 || soundEffectsInputIndex >= 0)
			arguments.insert(arguments.end(), { "-c:a", "aac", "-b:a", "192k" });
		else
			arguments.push_back("-an");
		arguments.insert(arguments.end(), { "-movflags", "+faststart", outputFilename });

		// Start FFmpeg on the first frame. Beginning the read before the preview
		// renderer is ready can make the FIFO report EOF on some Android builds.
		androidArguments = std::move(arguments);
		return true;
	#else
		(void)outputFilename; (void)width; (void)height; (void)fps; (void)audioFilename; (void)audioStart; (void)audioDelay; (void)soundEffectsFilename; (void)duration;
		error = "MP4 export is currently supported on Windows only.";
		return false;
	#endif
#endif
	}

	bool FfmpegEncoder::writeFrame(const std::vector<uint8_t>& rgbaFrame)
	{
#ifdef _WIN32
		if (!inputHandle)
			return false;
		HANDLE pipe = static_cast<HANDLE>(inputHandle);
		size_t offset = 0;
		while (offset < rgbaFrame.size())
		{
			DWORD written = 0;
			const DWORD request = static_cast<DWORD>(std::min<size_t>(rgbaFrame.size() - offset, 1024 * 1024));
			if (!WriteFile(pipe, rgbaFrame.data() + offset, request, &written, nullptr) || !written)
				return false;
			offset += written;
		}
		return true;
#elif defined(MIKUMIKUWORLD_IOS)
		return IOSVideoWrite(processHandle, rgbaFrame);
#else
	#ifdef __ANDROID__
		if (androidSessionId <= 0)
		{
			if (androidPipePath.empty() || androidArguments.empty())
				return false;
			androidSessionId = AndroidFileDialog_StartFfmpeg(androidArguments);
			androidArguments.clear();
			if (androidSessionId <= 0)
			{
				__android_log_print(ANDROID_LOG_ERROR, "MMWFFmpegNative", "Could not start FFmpegKit for the first video frame");
				return false;
			}
		}
		if (androidPipeHandle < 0)
			return false;
		static bool loggedFirstFrame = false;
		if (!loggedFirstFrame)
		{
			loggedFirstFrame = true;
			__android_log_print(ANDROID_LOG_INFO, "MMWFFmpegNative", "First frame has %zu RGBA bytes", rgbaFrame.size());
		}
		size_t offset = 0;
		while (offset < rgbaFrame.size())
		{
			const size_t requested = std::min<size_t>(rgbaFrame.size() - offset, 1024 * 1024);
			const ssize_t written = write(androidPipeHandle, rgbaFrame.data() + offset, requested);
			if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                        {
                                if (errno != EINTR)
                                {
                                        struct pollfd pfd = { androidPipeHandle, POLLOUT, 0 };
                                        if (poll(&pfd, 1, 20) == 0 && AndroidFileDialog_GetFfmpegStatus(androidSessionId) != INT32_MAX)
                                        {
                                                __android_log_print(ANDROID_LOG_ERROR, "MMWFFmpegNative", "FFmpeg stopped while accepting a video frame");
                                                return false;
                                        }
                                }
                                continue;
                        }
			if (written <= 0)
			{
				__android_log_print(ANDROID_LOG_ERROR, "MMWFFmpegNative", "Frame pipe write failed after %zu/%zu bytes: errno=%d",
					offset, rgbaFrame.size(), errno);
				return false;
			}
			offset += static_cast<size_t>(written);
		}
		return true;
	#else
		(void)rgbaFrame;
		return false;
	#endif
#endif
	}

	bool FfmpegEncoder::finish(std::string& error)
	{
#ifdef _WIN32
		if (inputHandle)
		{
			CloseHandle(static_cast<HANDLE>(inputHandle));
			inputHandle = nullptr;
		}
		if (!processHandle)
			return false;

		HANDLE process = static_cast<HANDLE>(processHandle);
		WaitForSingleObject(process, INFINITE);
		DWORD exitCode = 1;
		GetExitCodeProcess(process, &exitCode);
		CloseHandle(process);
		processHandle = nullptr;
		if (exitCode != 0)
		{
			error = "FFmpeg could not encode the selected video. Check that the installed build includes the H.264 encoder.";
			return false;
		}
		return true;
#elif defined(MIKUMIKUWORLD_IOS)
		void* encoder = processHandle;
		processHandle = nullptr;
		return IOSVideoFinish(encoder, error);
#else
	#ifdef __ANDROID__
		if (androidPipeHandle >= 0)
		{
			close(androidPipeHandle);
			androidPipeHandle = -1;
		}
		if (androidSessionId <= 0)
			return false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
		int status = INT32_MAX;
		while (std::chrono::steady_clock::now() < deadline)
		{
			status = AndroidFileDialog_GetFfmpegStatus(androidSessionId);
			if (status != INT32_MAX)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		if (status == INT32_MAX)
		{
			AndroidFileDialog_CancelFfmpeg(androidSessionId);
			error = "Bundled FFmpeg did not finish the MP4 export in time.";
		}
		else if (status != 0)
		{
			__android_log_print(ANDROID_LOG_ERROR, "MMWFFmpegNative", "FFmpeg returned status %d", status);
			error = "Bundled FFmpeg could not encode the MP4. Check the selected audio files and available storage.";
		}
		else
		{
			std::error_code fileError;
			if (!std::filesystem::exists(std::filesystem::u8path(androidOutputPath), fileError))
				error = "The exported MP4 file could not be found.";
		}
		androidSessionId = 0;
		androidOutputPath.clear();
		if (!androidPipePath.empty())
		{
			unlink(androidPipePath.c_str());
			androidPipePath.clear();
		}
		return status == 0 && error.empty();
	#else
		error = "MP4 export is currently supported on Windows only.";
		return false;
	#endif
#endif
	}

	void FfmpegEncoder::cancel()
	{
#ifdef MIKUMIKUWORLD_IOS
		IOSVideoCancel(processHandle);
		processHandle = nullptr;
#elif defined(_WIN32)
		if (inputHandle)
		{
			CloseHandle(static_cast<HANDLE>(inputHandle));
			inputHandle = nullptr;
		}
		if (processHandle)
		{
			TerminateProcess(static_cast<HANDLE>(processHandle), 1);
			WaitForSingleObject(static_cast<HANDLE>(processHandle), 1000);
			CloseHandle(static_cast<HANDLE>(processHandle));
			processHandle = nullptr;
		}
#elif defined(__ANDROID__)
		if (androidPipeHandle >= 0)
		{
			close(androidPipeHandle);
			androidPipeHandle = -1;
		}
		if (androidSessionId > 0)
		{
			AndroidFileDialog_CancelFfmpeg(androidSessionId);
			androidSessionId = 0;
		}
		androidArguments.clear();
		if (!androidPipePath.empty())
		{
			unlink(androidPipePath.c_str());
			androidPipePath.clear();
		}
#endif
	}
}
