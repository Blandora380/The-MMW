#ifdef __ANDROID__
#include "AndroidImGuiBackend.h"
#endif
#include <cstdio>
#include <chrono>
double gPv[8] = {};
static std::chrono::steady_clock::time_point gPvT;
#define PV_RESET() (gPvT = std::chrono::steady_clock::now())
#define PV_LAP(i) do { const auto n_ = std::chrono::steady_clock::now(); gPv[i] += std::chrono::duration<double, std::milli>(n_ - gPvT).count(); gPvT = n_; } while (0)
#include "Application.h"
#include "ScorePreview.h"
#include "PreviewEngine.h"
#include "Rendering/Camera.h"
#include "ResourceManager.h"
#include "Colors.h"
#include "ImageCrop.h"
#include "NoteSkin.h"
#include "ImGuiManager.h"
#include "Rendering/Texture.h"
#include "ApplicationConfiguration.h"
#include "Utilities.h"
#include "ImGui/imgui_impl_opengl3.h"
#include <glad/glad.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef MIKUMIKUWORLD_IOS
#include "../ios/IOSPlatform.h"
#endif
#ifdef __ANDROID__
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#endif
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace MikuMikuWorld
{
	static void renderExportOverlay(ImDrawList& drawList, const ImVec2& size)
	{
		// Export renders before ImGui::EndFrame() refreshes PlatformIO.Textures.
		// Text at export sizes can grow the font atlas during this very draw list,
		// so collect its actual texture dependencies after generating all text.
		ImVector<ImTextureData*> textures;
		for (const ImDrawCmd& command : drawList.CmdBuffer)
		{
			ImTextureData* texture = command.TexRef._TexData;
			if (texture && !textures.contains(texture))
				textures.push_back(texture);
		}

		ImDrawData drawData;
		drawData.Valid = true;
		drawData.DisplayPos = ImVec2(0.0f, 0.0f);
		drawData.DisplaySize = size;
		drawData.FramebufferScale = ImVec2(1.0f, 1.0f);
		drawData.Textures = &textures;
		drawData.AddDrawList(&drawList);
		ImGui_ImplOpenGL3_RenderDrawData(&drawData, false);
	}

	class ScorePreviewVideoDecoder
	{
		static constexpr int VIDEO_WIDTH = 1920;
		static constexpr int VIDEO_HEIGHT = 1080;
		static constexpr int VIDEO_FPS = 60;
		static constexpr size_t FRAME_BYTES = static_cast<size_t>(VIDEO_WIDTH) * VIDEO_HEIGHT * 4;

		std::string filename;
		double streamStartTime{};
		double lastRequestedTime{};
		uint64_t decodedFrames{};
		bool started{};
		GLuint texture{};
		std::vector<uint8_t> latestFrame;
#ifdef MIKUMIKUWORLD_IOS
		void* iosReader{};
#endif

#if defined(_WIN32) || defined(__ANDROID__)
		struct DecodedFrame
		{
			uint64_t index{};
			std::vector<uint8_t> pixels;
		};
		std::mutex frameMutex;
		std::condition_variable frameSpaceAvailable;
		std::condition_variable frameAvailable;
		std::deque<DecodedFrame> frameQueue;
		std::thread readerThread;
		bool readerStop{};
		bool readerFinished{};
		bool hasFrameForStream{};
		uint64_t currentFrameIndex{};
		uint64_t nextFrameIndex{};
#endif

#ifdef _WIN32
		HANDLE process{};
		HANDLE outputPipe{};
#elif defined(__ANDROID__)
		AVFormatContext* formatContext{};
		AVCodecContext* codecContext{};
		int videoStreamIndex{ -1 };
#endif

#ifdef _WIN32
		static std::wstring quoteArgument(const std::wstring& value)
		{
			std::wstring result = L"\"";
			size_t backslashes = 0;
			for (wchar_t ch : value)
			{
				if (ch == L'\\')
				{
					++backslashes;
					continue;
				}
				if (ch == L'\"')
				{
					result.append(backslashes * 2 + 1, L'\\');
					result.push_back(ch);
					backslashes = 0;
					continue;
				}
				result.append(backslashes, L'\\');
				backslashes = 0;
				result.push_back(ch);
			}
			result.append(backslashes * 2, L'\\');
			result.push_back(L'\"');
			return result;
		}

		static std::wstring findFfmpeg()
		{
			DWORD length = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, 0, nullptr, nullptr);
			if (!length)
				return {};
			std::wstring path(length, L'\0');
			DWORD written = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, length, path.data(), nullptr);
			if (!written || written >= length)
				return {};
			path.resize(written);
			return path;
		}

		bool startStream(const std::string& videoFile, double startTime)
		{
			stopStream();
			hasFrameForStream = false;
			currentFrameIndex = 0;
			const std::wstring executable = findFfmpeg();
			if (executable.empty() || !IO::File::exists(videoFile))
				return false;

			SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
			HANDLE childOutput{};
			if (!CreatePipe(&outputPipe, &childOutput, &security, 0))
				return false;
			SetHandleInformation(outputPipe, HANDLE_FLAG_INHERIT, 0);
			HANDLE nullOutput = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
				&security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (nullOutput == INVALID_HANDLE_VALUE)
			{
				CloseHandle(childOutput);
				CloseHandle(outputPipe);
				outputPipe = nullptr;
				return false;
			}

			const double clampedStartTime = std::max(0.0, startTime);
			const double fastSeekTime = std::max(0.0, clampedStartTime - 5.0);
			const double preciseSeekTime = clampedStartTime - fastSeekTime;
			std::wstring command = quoteArgument(executable);
			command += L" -hide_banner -loglevel error -nostdin -ss ";
			command += IO::mbToWideStr(IO::formatString("%.6f", fastSeekTime));
			command += L" -i ";
			command += quoteArgument(IO::mbToWideStr(videoFile));
			command += L" -ss ";
			command += IO::mbToWideStr(IO::formatString("%.6f", preciseSeekTime));
			command += L" -an -vf \"setpts=PTS-STARTPTS,scale=1920:1080:force_original_aspect_ratio=increase,crop=1920:1080,fps=60,format=rgba\" -f rawvideo -pix_fmt rgba pipe:1";

			STARTUPINFOW startup{};
			startup.cb = sizeof(startup);
			startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
			startup.wShowWindow = SW_HIDE;
			startup.hStdInput = nullOutput;
			startup.hStdOutput = childOutput;
			startup.hStdError = nullOutput;
			PROCESS_INFORMATION info{};
			const BOOL launched = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
				CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
			CloseHandle(childOutput);
			if (nullOutput != INVALID_HANDLE_VALUE)
				CloseHandle(nullOutput);
			if (!launched)
			{
				CloseHandle(outputPipe);
				outputPipe = nullptr;
				return false;
			}
			CloseHandle(info.hThread);
			process = info.hProcess;
			filename = videoFile;
			streamStartTime = clampedStartTime;
			decodedFrames = 0;
			hasFrameForStream = false;
			currentFrameIndex = 0;
			{
				std::lock_guard<std::mutex> lock(frameMutex);
				frameQueue.clear();
				readerStop = false;
				readerFinished = false;
				nextFrameIndex = 0;
			}
			started = true;
			readerThread = std::thread(&ScorePreviewVideoDecoder::readFrames, this);
			return true;
		}

		void readFrames()
		{
			while (true)
			{
				DecodedFrame frame{};
				frame.pixels.resize(FRAME_BYTES);
				size_t bytesReadTotal = 0;
				while (bytesReadTotal < FRAME_BYTES)
				{
					DWORD bytesRead = 0;
					const DWORD requested = static_cast<DWORD>(std::min<size_t>(FRAME_BYTES - bytesReadTotal, 1024 * 1024));
					if (!ReadFile(outputPipe, frame.pixels.data() + bytesReadTotal, requested, &bytesRead, nullptr) || bytesRead == 0)
					{
						{
							std::lock_guard<std::mutex> lock(frameMutex);
							readerFinished = true;
						}
						frameAvailable.notify_all();
						return;
					}
					bytesReadTotal += bytesRead;
				}

				std::unique_lock<std::mutex> lock(frameMutex);
				frameSpaceAvailable.wait(lock, [this]() { return readerStop || frameQueue.size() < 3; });
				if (readerStop)
					return;
				frame.index = nextFrameIndex++;
				frameQueue.push_back(std::move(frame));
				lock.unlock();
				frameAvailable.notify_one();
			}
		}

		void stopStream()
		{
			if (process)
			{
				TerminateProcess(process, 0);
				WaitForSingleObject(process, 1000);
			}
			{
				std::lock_guard<std::mutex> lock(frameMutex);
				readerStop = true;
				frameQueue.clear();
			}
			frameSpaceAvailable.notify_all();
			frameAvailable.notify_all();
			if (readerThread.joinable())
				readerThread.join();
			if (process)
			{
				CloseHandle(process);
				process = nullptr;
			}
			if (outputPipe)
			{
				CloseHandle(outputPipe);
				outputPipe = nullptr;
			}
			started = false;
		}
#elif defined(__ANDROID__)
		bool startStream(const std::string& videoFile, double startTime)
		{
			stopStream();
			if (videoFile.empty() || avformat_open_input(&formatContext, videoFile.c_str(), nullptr, nullptr) < 0)
			{
				stopStream();
				return false;
			}
			if (avformat_find_stream_info(formatContext, nullptr) < 0)
			{
				stopStream();
				return false;
			}

			const AVCodec* decoder = nullptr;
			videoStreamIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
			if (videoStreamIndex < 0 || !decoder)
			{
				stopStream();
				return false;
			}
			codecContext = avcodec_alloc_context3(decoder);
			if (!codecContext || avcodec_parameters_to_context(codecContext, formatContext->streams[videoStreamIndex]->codecpar) < 0 ||
				avcodec_open2(codecContext, decoder, nullptr) < 0)
			{
				stopStream();
				return false;
			}

			streamStartTime = std::max(0.0, startTime);
			lastRequestedTime = streamStartTime;
			const AVRational timeBase = formatContext->streams[videoStreamIndex]->time_base;
			const int64_t seekTimestamp = av_rescale_q(static_cast<int64_t>(streamStartTime * AV_TIME_BASE), AV_TIME_BASE_Q, timeBase);
			av_seek_frame(formatContext, videoStreamIndex, seekTimestamp, AVSEEK_FLAG_BACKWARD);
			avcodec_flush_buffers(codecContext);
			{
				std::lock_guard<std::mutex> lock(frameMutex);
				frameQueue.clear();
				readerStop = false;
				readerFinished = false;
				hasFrameForStream = false;
				currentFrameIndex = 0;
				nextFrameIndex = 0;
			}
			filename = videoFile;
			started = true;
			readerThread = std::thread(&ScorePreviewVideoDecoder::readFrames, this);
			return true;
		}

		void readFrames()
		{
			AVPacket* packet = av_packet_alloc();
			AVFrame* frame = av_frame_alloc();
			SwsContext* scaler = nullptr;
			const AVStream* stream = formatContext->streams[videoStreamIndex];
			while (packet && frame)
			{
				{
					std::lock_guard<std::mutex> lock(frameMutex);
					if (readerStop)
						break;
				}
				if (av_read_frame(formatContext, packet) < 0)
					break;
				if (packet->stream_index == videoStreamIndex && avcodec_send_packet(codecContext, packet) >= 0)
				{
					while (avcodec_receive_frame(codecContext, frame) >= 0)
					{
						const int64_t timestamp = frame->best_effort_timestamp;
						const double frameTime = timestamp == AV_NOPTS_VALUE ? streamStartTime : timestamp * av_q2d(stream->time_base);
						const int64_t frameIndex = static_cast<int64_t>(std::llround((frameTime - streamStartTime) * VIDEO_FPS));
						if (frameIndex >= 0)
						{
							DecodedFrame decoded{};
							decoded.index = static_cast<uint64_t>(frameIndex);
							decoded.pixels.resize(FRAME_BYTES);
							const auto pixelFormat = static_cast<AVPixelFormat>(frame->format);
							scaler = sws_getCachedContext(scaler, frame->width, frame->height, pixelFormat,
								VIDEO_WIDTH, VIDEO_HEIGHT, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
							if (scaler)
							{
								uint8_t* outputPlanes[] = { decoded.pixels.data(), nullptr, nullptr, nullptr };
								int outputLineSizes[] = { VIDEO_WIDTH * 4, 0, 0, 0 };
								if (sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, outputPlanes, outputLineSizes) > 0)
								{
									std::unique_lock<std::mutex> lock(frameMutex);
									frameSpaceAvailable.wait(lock, [this]() { return readerStop || frameQueue.size() < 3; });
									if (readerStop)
										break;
									frameQueue.push_back(std::move(decoded));
									lock.unlock();
									frameAvailable.notify_one();
								}
							}
						}
						av_frame_unref(frame);
					}
				}
				av_packet_unref(packet);
			}
			if (scaler)
				sws_freeContext(scaler);
			if (frame)
				av_frame_free(&frame);
			if (packet)
				av_packet_free(&packet);
			{
				std::lock_guard<std::mutex> lock(frameMutex);
				readerFinished = true;
			}
			frameAvailable.notify_all();
		}

		void stopStream()
		{
			{
				std::lock_guard<std::mutex> lock(frameMutex);
				readerStop = true;
				frameQueue.clear();
			}
			frameSpaceAvailable.notify_all();
			frameAvailable.notify_all();
			if (readerThread.joinable())
				readerThread.join();
			if (codecContext)
				avcodec_free_context(&codecContext);
			if (formatContext)
				avformat_close_input(&formatContext);
			videoStreamIndex = -1;
			started = false;
			hasFrameForStream = false;
		}
#endif

		void uploadLatestFrame()
		{
			if (latestFrame.size() != FRAME_BYTES)
				return;
			if (!texture)
			{
				glGenTextures(1, &texture);
				glBindTexture(GL_TEXTURE_2D, texture);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, VIDEO_WIDTH, VIDEO_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, latestFrame.data());
			}
			else
			{
				glBindTexture(GL_TEXTURE_2D, texture);
				glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
				glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VIDEO_WIDTH, VIDEO_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, latestFrame.data());
			}
			glBindTexture(GL_TEXTURE_2D, 0);
		}

	public:
		~ScorePreviewVideoDecoder()
		{
#ifdef MIKUMIKUWORLD_IOS
			IOSVideoReaderDestroy(iosReader);
#endif
#if defined(_WIN32) || defined(__ANDROID__)
			stopStream();
#endif
			if (texture)
				glDeleteTextures(1, &texture);
		}

		void update(const std::string& videoFile, double timelineTime, bool isPlaying, bool waitForTargetFrame)
		{
#ifdef MIKUMIKUWORLD_IOS
			if (videoFile != filename)
			{
				IOSVideoReaderDestroy(iosReader);
				iosReader = videoFile.empty() ? nullptr : IOSVideoReaderCreate(videoFile);
				filename = videoFile;
				started = false;
			}
			if (IOSVideoReaderFrame(iosReader, timelineTime, latestFrame))
			{
				uploadLatestFrame();
				started = true;
			}
			(void)isPlaying; (void)waitForTargetFrame;
#elif defined(_WIN32) || defined(__ANDROID__)
			const bool pathChanged = videoFile != filename;
			const bool jumped = started && (isPlaying
				? std::abs(timelineTime - lastRequestedTime) > 0.25
				: std::abs(timelineTime - lastRequestedTime) > 0.02);
			if (videoFile.empty())
			{
				stopStream();
				filename.clear();
				return;
			}
			if (pathChanged || !started || jumped)
			{
				filename.clear();
				startStream(videoFile, timelineTime);
			}
			lastRequestedTime = timelineTime;
			if (!started)
				return;

			const uint64_t targetFrame = static_cast<uint64_t>(std::max(0.0, std::floor((timelineTime - streamStartTime) * VIDEO_FPS)));
			bool changed = false;
			while (true)
			{
				std::unique_lock<std::mutex> lock(frameMutex);
				while (!frameQueue.empty() && frameQueue.front().index <= targetFrame)
				{
					currentFrameIndex = frameQueue.front().index;
					hasFrameForStream = true;
					latestFrame = std::move(frameQueue.front().pixels);
					frameQueue.pop_front();
					++decodedFrames;
					changed = true;
				}
				const bool targetAvailable = hasFrameForStream && currentFrameIndex >= targetFrame;
				if (!waitForTargetFrame || targetAvailable || readerFinished || readerStop)
					break;
				frameSpaceAvailable.notify_one();
				frameAvailable.wait(lock, [this]() { return readerFinished || readerStop || !frameQueue.empty(); });
			}
			frameSpaceAvailable.notify_one();
			if (changed)
				uploadLatestFrame();
#else
			(void)videoFile;
			(void)timelineTime;
			(void)isPlaying;
			(void)waitForTargetFrame;
#endif
		}

		GLuint getTexture() const { return texture; }
		bool isReady() const
		{
#ifdef MIKUMIKUWORLD_IOS
			return texture != 0 && started;
#elif defined(_WIN32) || defined(__ANDROID__)
			return texture != 0 && hasFrameForStream;
#else
			return false;
#endif
		}
	};

	struct PreviewPlaybackState
	{
		bool isPlaying{}, wasLastFramePlaying{};
	} playbackState;

	constexpr float EFFECTS_TARGET_ASPECT = 16.f / 9.f;

	namespace Utils
	{
		// Scale a rectangle with specified aspect ratio to be visible inside the target rectangle
		inline void fitRect(float target_width, float target_height, long double source_aspect_ratio, float& width, float& height)
		{
			const float target_aspect_ratio = target_width / target_height;
			width  = target_aspect_ratio > source_aspect_ratio ? source_aspect_ratio * target_height : target_width;
			height = target_aspect_ratio < source_aspect_ratio ? target_width / source_aspect_ratio : target_height;
			return;
		}

		// Scale a rectangle with specified aspect ratio so it fill the area of the target rectangle
		inline void fillRect(float target_width, float target_height, long double source_aspect_ratio, float& width, float& height)
		{
			const float target_aspect_ratio = target_width / target_height;
			width  = target_aspect_ratio < source_aspect_ratio ? source_aspect_ratio * target_height : target_width;
			height = target_aspect_ratio > source_aspect_ratio ? target_width / source_aspect_ratio : target_height;
			return;
		}
	};

	static const int NOTE_SIDE_WIDTH = 91;
	static const int NOTE_SIDE_PAD = 10;
	static const int MAX_FLICK_SPRITES = 6;
	static const int HOLD_XCUTOFF = 36;
	static const int GUIDE_XCUTOFF = 3;
	static const int GUIDE_Y_TOP_CUTOFF = -41;
	static const int GUIDE_Y_BOTTOM_CUTOFF = -12;
	static Color defaultTint { 1.f, 1.f, 1.f, 1.f };

	namespace
	{
		constexpr float COMBO_BASE_SCALE = 0.85f;
		constexpr float COMBO_DIGIT_STEP = 92.0f;
		constexpr float COMBO_GROUP_SCALE = 1.25f;
		constexpr float COMBO_GROUP_OFFSET_Y = -12.0f;
		constexpr float COMBO_GROUP_CENTER_X = 1634.0f;
		constexpr float COMBO_GROUP_CENTER_Y = 478.0f;
		constexpr float SCORE_BAR_FULL_WIDTH = 354.0f;
		constexpr float SCORE_ROOT_SCALE = 1.5f;
		constexpr float JUDGE_VISIBLE_WINDOW_SEC = 0.24f;
		constexpr float HUD_LIFE_Y = 11.0f;
		constexpr float HUD_LIFE_HEIGHT = 104.0f;
		constexpr float PREVIEW_TOOLBAR_HUD_PADDING = 16.0f;
		constexpr float HUD_PAUSE_BUTTON_CENTER_X = 1834.56f;
		constexpr float HUD_PAUSE_BUTTON_CENTER_Y = 67.71f;
		constexpr float HUD_PAUSE_BUTTON_RADIUS = 39.55f;

		float clamp01(float value)
		{
			return std::max(0.0f, std::min(1.0f, value));
		}

		ImTextureID toImTextureID(const Texture& texture)
		{
			return static_cast<ImTextureID>(texture.getID());
		}

		void drawComboImage(ImDrawList* drawList, const Texture& texture, float x, float y, float width, float height, float alpha)
		{
			if (!drawList || texture.getID() == 0 || width <= 0.1f || height <= 0.1f)
				return;

			const int value = static_cast<int>(std::lround(clamp01(alpha) * 255.0f));
			if (value <= 0)
				return;

			drawList->AddImage(
				toImTextureID(texture),
				ImVec2(x, y),
				ImVec2(x + width, y + height),
				ImVec2(0.5f / texture.getWidth(), 0.5f / texture.getHeight()),
                                ImVec2(1.0f - 0.5f / texture.getWidth(), 1.0f - 0.5f / texture.getHeight()),
                                IM_COL32(255, 255, 255, value)
			);
		}

		void drawHudImageClipX(ImDrawList* drawList, const Texture& texture, float x, float y, float width, float height, float ratio, float alpha)
		{
			if (!drawList || texture.getID() == 0 || width <= 0.1f || height <= 0.1f)
				return;

			const float clippedRatio = clamp01(ratio);
			if (clippedRatio <= 0.001f)
				return;

			const int value = static_cast<int>(std::lround(clamp01(alpha) * 255.0f));
			if (value <= 0)
				return;

			drawList->AddImage(
				toImTextureID(texture),
				ImVec2(x, y),
				ImVec2(x + width * clippedRatio, y + height),
				ImVec2(0.0f, 0.0f),
				ImVec2(clippedRatio, 1.0f),
				IM_COL32(255, 255, 255, value)
			);
		}

		const Texture* getEditorTexture(const std::string& filename)
		{
			const int textureIndex = ResourceManager::getTextureByFilename(Application::getAppDir() + "res/editor/" + filename);
			return textureIndex >= 0 ? &ResourceManager::textures[textureIndex] : nullptr;
		}

		const Texture* getComboTexture(const std::string& filename)
		{
			return getEditorTexture("combo/" + filename);
		}

		const Texture* getHudTexture(const std::string& filename)
		{
			return getEditorTexture("hud/" + filename);
		}

		std::string scoreDigitsText(int score)
		{
			std::string text = std::to_string(std::max(0, score));
			while (text.size() < 8)
				text.insert(text.begin(), 'n');

			return text;
		}

		float scoreDigitOpticalOffsetPx(char ch, bool shadow)
		{
			switch (ch)
			{
			case '0':
			case 'n':
				return shadow ? -0.024f : -0.036f;
			case '1':
				return shadow ? 3.089f : 3.166f;
			case '2':
				return shadow ? 0.498f : 0.472f;
			case '3':
				return shadow ? 1.294f : 2.255f;
			case '4':
				return shadow ? 0.436f : 0.966f;
			case '5':
				return shadow ? -0.421f : 0.086f;
			case '6':
				return shadow ? -0.071f : -0.876f;
			case '7':
				return shadow ? -0.891f : -0.689f;
			case '8':
				return shadow ? 0.216f : 0.203f;
			case '9':
				return shadow ? 0.067f : 0.868f;
			default:
				return 0.0f;
			}
		}
	}

	ScorePreviewBackground::ScorePreviewBackground() : backgroundFile(), jacketFile{}, frameBuffer{2048, 2048}, brightness(0.5f), init{false},
		videoDecoder(std::make_unique<ScorePreviewVideoDecoder>()),
		mvLineOverlay(std::make_unique<Texture>(Application::getAppDir() + "res/editor/mv_line.png"))
	{

	}

	ScorePreviewBackground::~ScorePreviewBackground()
	{
		frameBuffer.dispose();
		if (mvLineOverlay)
			mvLineOverlay->dispose();
	}

	void ScorePreviewBackground::setBrightness(float value)
	{
		brightness = value;
	}

	void ScorePreviewBackground::update(Renderer* renderer, const Jacket& jacket)
	{
		init = true;
		backgroundFile = config.backgroundImage;
		jacketFile = jacket.getFilename();
		brightness = config.pvBackgroundBrightness;
		bool useDefaultTexture = backgroundFile.empty() || !IO::File::exists(backgroundFile);
		Texture backgroundTex = { useDefaultTexture ? Application::getAppDir() + "res/editor/default.png" : backgroundFile};
		const float bgWidth = backgroundTex.getWidth(), bgHeight = backgroundTex.getHeight();
		if (bgWidth != frameBuffer.getWidth() || bgHeight != frameBuffer.getHeight())
			frameBuffer.resize(bgWidth, bgHeight);
		frameBuffer.bind();
		frameBuffer.clear(0, 0, 0, 0);
		int shaderId;
		if ((shaderId = ResourceManager::getShader("basic2d")) == -1) return;
		Shader* basicShader = ResourceManager::shaders[shaderId];
		int index = ResourceManager::getTexture("stage");
		if (index == -1)
			return;

		basicShader->use();

		// DirectXMath dies when projection size is too small
		const float projectionX{ std::max(bgWidth, 10.f) };
		const float projectionY{ std::max(bgHeight, 10.f) };
		basicShader->setMatrix4("projection", Camera::getOffCenterOrthographicProjection(0, projectionX, 0, projectionY));
		
		if (backgroundTex.getID() > 0)
		{
			renderer->beginBatch();
			renderer->drawRectangle({0, 0}, {bgWidth, bgHeight}, backgroundTex, 0, bgWidth, 0, bgHeight, defaultTint, 0);
			renderer->endBatch();
			if (useDefaultTexture && IO::File::exists(jacket.getFilename()))
				updateDrawDefaultJacket(renderer, jacket);
		}
		frameBuffer.unblind();
		backgroundTex.dispose();
	}

	void ScorePreviewBackground::updateDrawDefaultJacket(Renderer* renderer, const Jacket& jacket)
	{
		if (jacket.getFilename().empty()) return;
		int index = ResourceManager::getTexture("stage");
		if (index == -1) return;
		const Texture& stage = ResourceManager::textures[index];
		const Sprite* sprite;
		if (stage.sprites.size() < STAGE_SPR_COUNT) return;
		int shaderId;
		if ((shaderId = ResourceManager::getShader("basic2d")) == -1) return;
		Shader* basicShader = ResourceManager::shaders[shaderId];
		if ((shaderId = ResourceManager::getShader("masking")) == -1) return;
		Shader* maskShader = ResourceManager::shaders[shaderId];
		
		DirectX::XMFLOAT4 defCol = {defaultTint.r, defaultTint.g, defaultTint.b, defaultTint.a};
		DirectX::XMFLOAT4 mainCol = { 1, 1, 1, 0.65 };
		DirectX::XMFLOAT4 mirrorCol = { 1, 1, 1, 0.35 };
		auto mainLeftPos = Engine::quadvPos(602, 602 + 264, 816, 816 + 174);
		auto mainRightPos = Engine::quadvPos(1205, 1205 + 200, 629, 629 + 114);
		auto mirrorLeftPos = Engine::quadvPos(615, 615 + 256, 1170, 1170 + 162);
		auto mirrorRightPos = Engine::quadvPos(1186, 1186 + 196, 1387, 1387 + 105);
		auto mainLeftMask = Engine::quadUV(stage.sprites[SPR_JACKET_LEFT_MASK], stage);
		auto mainRightMask = Engine::quadUV(stage.sprites[SPR_JACKET_RIGHT_MASK], stage);
		auto mirrorLeftMask = Engine::quadUV(stage.sprites[SPR_MIRROR_JACKET_LEFT_MASK], stage);
		auto mirrorRightMask = Engine::quadUV(stage.sprites[SPR_MIRROR_JACKET_RIGHT_MASK], stage);

		maskShader->use();
		maskShader->setInt("baseTex", 0);
		maskShader->setInt("maskTex", 1);
		maskShader->setMatrix4("projection", Camera::getOffCenterOrthographicProjection(0, 2048, 0, 2048));
		renderer->beginBatch();
		renderer->pushQuadMasked(mainLeftPos, DefaultJacket::getLeftUV(), mainLeftMask, mainCol, jacket.getTexID(), stage.getID());
		renderer->pushQuadMasked(mainRightPos, DefaultJacket::getRightUV(), mainRightMask, mainCol, jacket.getTexID(), stage.getID());
		renderer->pushQuadMasked(mirrorLeftPos, DefaultJacket::getLeftMirrorUV(), mirrorLeftMask, mirrorCol, jacket.getTexID(), stage.getID());
		renderer->pushQuadMasked(mirrorRightPos, DefaultJacket::getRightMirrorUV(), mirrorRightMask, mirrorCol, jacket.getTexID(), stage.getID());
		renderer->endBatch();
		
		basicShader->use();
		basicShader->setMatrix4("projection", Camera::getOffCenterOrthographicProjection(0, 2048, 0, 2048));
		renderer->beginBatch();
		renderer->pushQuad(mainLeftPos, mainLeftMask, DirectX::XMMatrixIdentity(), defCol, stage.getID(), 0);
		renderer->pushQuad(mainRightPos, mainRightMask, DirectX::XMMatrixIdentity(), defCol, stage.getID(), 0);
		renderer->pushQuad(mirrorLeftPos, mirrorLeftMask, DirectX::XMMatrixIdentity(), defCol, stage.getID(), 0);
		renderer->pushQuad(mirrorRightPos, mirrorRightMask, DirectX::XMMatrixIdentity(), defCol, stage.getID(), 0);

		sprite = &stage.sprites[SPR_JACKET_WINDOW];
		renderer->drawRectangle({682, 497}, {686, 686}, stage, sprite->getX1(), sprite->getX2(), sprite->getY1(), sprite->getY2(), defaultTint, 1);
		sprite = &stage.sprites[SPR_MIRROR_JACKET_WINDOW];
		renderer->drawRectangle({699, 958}, {651, 650}, stage, sprite->getX1(), sprite->getX2(), sprite->getY1(), sprite->getY2(), defaultTint.scaleAlpha(0.6f), 1);
		renderer->endBatch();

		auto mainWindowPos = Engine::quadvPos(824, 824 + 400, 666, 666 + 384);
		auto mainWindowMask = Engine::quadUV(stage.sprites[SPR_JACKET_MASK], stage);
		auto mirrorWindowPos = Engine::quadvPos(834, 834 + 386, 1120, 1120 + 336);
		auto mirrorWindowMask = Engine::quadUV(stage.sprites[SPR_MIRROR_JACKET_MASK], stage);
		maskShader->use();
		renderer->beginBatch();
		renderer->pushQuadMasked(mainWindowPos, DefaultJacket::getCenterUV(), mainWindowMask, {1, 1, 1, 0.8}, jacket.getTexID(), stage.getID());
		renderer->pushQuadMasked(mirrorWindowPos, DefaultJacket::getMirrorCenterUV(), mirrorWindowMask, {1, 1, 1, 0.5}, jacket.getTexID(), stage.getID());
		renderer->endBatch();

		basicShader->use();
		renderer->beginBatch();
		renderer->pushQuad(mainWindowPos, mainWindowMask, DirectX::XMMatrixIdentity(), defCol, stage.getID(), 0);
		renderer->pushQuad(mirrorWindowPos, mirrorWindowMask, DirectX::XMMatrixIdentity(), defCol, stage.getID(), 0);
		sprite = &stage.sprites[SPR_SEKAI_FLOOR];
		renderer->drawRectangle({0, 1251}, {sprite->getWidth(), sprite->getHeight()}, stage, sprite->getX1(), sprite->getX2(), sprite->getY1(), sprite->getY2(), defaultTint.scaleAlpha(0.8f), 1);
		renderer->endBatch();
	}

	bool ScorePreviewBackground::shouldUpdate(const Jacket& jacket) const
	{
		return !init || backgroundFile != config.backgroundImage || jacketFile != jacket.getFilename();
	}

	void ScorePreviewBackground::draw(Renderer *renderer, float scrWidth, float scrHeight, float timelineTime, bool isPlaying,
		const std::string& mvFilename, float mvOffsetMs, bool waitForVideoFrame)
	{
		const bool useMvVideo = !mvFilename.empty();
		if (useMvVideo)
			videoDecoder->update(mvFilename, timelineTime - mvOffsetMs / 1000.0, isPlaying, waitForVideoFrame);
		else
			videoDecoder->update("", timelineTime, false, false);

		float bgScrWidth = useMvVideo ? 1920.0f : static_cast<float>(frameBuffer.getWidth());
		float bgScrHeight = useMvVideo ? 1080.0f : static_cast<float>(frameBuffer.getHeight());
		float targetWidth, targetHeight;
		if (useMvVideo || !backgroundFile.empty())
		{
			if (useMvVideo && config.pvSekai2DMVScaling)
				Utils::fitRect(scrWidth, scrHeight, bgScrWidth / bgScrHeight, bgScrWidth, bgScrHeight);
			else
				Utils::fillRect(scrWidth, scrHeight, bgScrWidth / bgScrHeight, bgScrWidth, bgScrHeight);
			targetWidth = Engine::STAGE_TARGET_WIDTH;
			targetHeight = Engine::STAGE_TARGET_HEIGHT;
		}
		else
		{
			bgScrWidth = Engine::BACKGROUND_SIZE;
			bgScrHeight = Engine::BACKGROUND_SIZE;
			targetWidth = Engine::STAGE_TARGET_WIDTH;
			targetHeight = Engine::STAGE_TARGET_HEIGHT;
		}
		
		const float bgWidth = bgScrWidth / (targetWidth * Engine::STAGE_WIDTH_RATIO);
		const float bgLeft = -bgWidth / 2;
		const float bgHeight = bgScrHeight / (targetHeight * Engine::STAGE_HEIGHT_RATIO);
		const float centerY = 0.5f / Engine::STAGE_HEIGHT_RATIO + Engine::STAGE_LANE_TOP / Engine::STAGE_LANE_HEIGHT;
		const float bgTop = centerY + -bgHeight / 2;
		auto vPos = Engine::quadvPos(bgLeft, bgLeft + bgWidth, bgTop, bgTop + bgHeight);
		auto uv = Engine::quadvPos(0, 1, 0, 1);
		const DirectX::XMMATRIX identity = DirectX::XMMatrixIdentity();
		if (useMvVideo)
		{
			if (videoDecoder->isReady())
				renderer->pushQuad(vPos, uv, identity, DirectX::XMFLOAT4(brightness, brightness, brightness, 1), videoDecoder->getTexture(), -10);
			else
				renderer->pushQuad(vPos, uv, identity, DirectX::XMFLOAT4(brightness, brightness, brightness, 1), frameBuffer.getTexture(), -10);
			if (config.pvDrawMvLineOverlay && mvLineOverlay && mvLineOverlay->getID() > 0)
				renderer->pushQuad(vPos, uv, identity, DirectX::XMFLOAT4(1, 1, 1, 0.5f), mvLineOverlay->getID(), -9);
		}
		else
			renderer->pushQuad(vPos, uv, identity, DirectX::XMFLOAT4(brightness, brightness, brightness, 1), frameBuffer.getTexture(), -10);
	}

	std::array<DirectX::XMFLOAT4, 4> ScorePreviewBackground::DefaultJacket::getLeftUV()
	{
		return {{	
			{  303.8 / 740, 504.8 / 740, 0, 0 },
			{  317.5 / 740, 297.7 / 740, 0, 0 },
			{    5.5 / 740, 278.3 / 740, 0, 0 },
			{     -8 / 740, 497.4 / 740, 0, 0 }
		}};
	}

	std::array<DirectX::XMFLOAT4, 4> ScorePreviewBackground::DefaultJacket::getRightUV()
	{
		return {{
			{ 749.5 / 740, 377.7 / 740, 0, 0 },
			{ 738.2 / 740, 188.1 / 740, 0, 0 },
			{ 415.0 / 740, 171.4 / 740, 0, 0 },
			{ 432.1 / 740, 363.9 / 740, 0, 0 },
		}};
	}

	std::array<DirectX::XMFLOAT4, 4> ScorePreviewBackground::DefaultJacket::getLeftMirrorUV()
	{
		return {{
			{ 292.761414 / 740, 247.401382 / 740, 0, 0 },
			{ 310.765869 / 740, 491.944763 / 740, 0, 0 },
			{   6.892246 / 740, 498.470642 / 740, 0, 0 },
			{  -6.246704 / 740, 258.264862 / 740, 0, 0 }
		}};
	}

	std::array<DirectX::XMFLOAT4, 4> ScorePreviewBackground::DefaultJacket::getRightMirrorUV()
	{
		return {{
			{ 733.444458 / 740, 183.954681 / 740, 0, 0 },	
			{ 743.541321 / 740, 355.960449 / 740, 0, 0 },
			{ 418.899414 / 740, 332.759491 / 740, 0, 0 },
			{ 410.746246 / 740, 155.907684 / 740, 0, 0 },
		}};
	}

	std::array<DirectX::XMFLOAT4, 4> ScorePreviewBackground::DefaultJacket::getCenterUV()
	{
		return {{
			{ 755.541687 / 740, 744.057861 / 740, 0, 0 },
			{ 739.961182 / 740,  -1.859504 / 740, 0, 0 },
			{   0.043696 / 740,  -1.859504 / 740, 0, 0 },
			{ -17.484388 / 740, 744.057861 / 740, 0, 0 }
		}};
	}

	std::array<DirectX::XMFLOAT4, 4> ScorePreviewBackground::DefaultJacket::getMirrorCenterUV()
	{
		return {{
			{ 747.697083 / 740,   2.164453 / 740, 0, 0 },
			{ 743.909424 / 740, 731.297241 / 740, 0, 0 },
			{  -1.864066 / 740, 731.297241 / 740, 0, 0 },
			{   3.837242 / 740,   2.164453 / 740, 0, 0 }
		}};
	}

	ScorePreviewWindow::ScorePreviewWindow() : previewBuffer{ 1920, 1080 }, background(), scaledAspectRatio(1)
	{
		noteEffectsCamera.setFov(50.f);
		noteEffectsCamera.setRotation(-90.f, 27.1f);
		noteEffectsCamera.setPosition({ 0, 5.32f, -5.86f, 0 });
		noteEffectsCamera.positionCamNormal();
	}

	ScorePreviewWindow::~ScorePreviewWindow()
	{
		for (auto& texture : sekaiDifficultyBackgrounds)
			if (texture)
				texture->dispose();
		if (sekaiCollabTag)
			sekaiCollabTag->dispose();
		if (sekaiIntroGradient)
			sekaiIntroGradient->dispose();
		if (sekaiMusicSheet)
			sekaiMusicSheet->dispose();
		if (sekaiPerson)
			sekaiPerson->dispose();
	}

	void ScorePreviewWindow::update(ScoreContext& context, Renderer* renderer)
	{
		bool isWindowActive =  !ImGui::IsWindowDocked() || ImGui::GetCurrentWindow()->TabId == ImGui::GetWindowDockNode()->SelectedTabId;
		if (!isWindowActive && exportWidth <= 0)
			// Don't draw anything if the window is not active.
			// SFX and music updates are handled in the timeline
			return;
		if (context.scorePreviewDrawData.noteSpeed != config.pvNoteSpeed)
			context.scorePreviewDrawData.calculateDrawData(context.score);
		ImVec2 displaySize = ImGui::GetContentRegionAvail() - ImVec2{ this->getScrollbarWidth(), 0 }; // Reserve space for the scrollbar
		ImVec2 position = ImGui::GetCursorScreenPos();
		ImVec2 size = exportWidth > 0 ? ImVec2{ (float)exportWidth, (float)exportHeight } : displaySize;
		ImRect boundaries = ImRect(position, position + displaySize);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(boundaries.Min, boundaries.Max, 0xff202020);
		
		if ((config.drawBackground || (exportWidth > 0 && (exportSekaiIntro || exportStageFadeIn < 1.0f)))
			&& background.shouldUpdate(context.workingData.jacket))
			background.update(renderer, context.workingData.jacket);

		if (!context.scorePreviewDrawData.effectView.isInitialized())
			context.scorePreviewDrawData.effectView.init();

		if ((playbackState.isPlaying || exportWidth > 0) && !(exportWidth > 0 && exportSekaiIntro))
		{
			context.scorePreviewDrawData.effectView.update(context);
		}
		else if (!playbackState.isPlaying && playbackState.wasLastFramePlaying)
		{
			context.scorePreviewDrawData.effectView.reset();
		}

#ifdef MIKUMIKUWORLD_SEKAI_LIVE_RENDERING
		static int shaderId = ResourceManager::getShader("live_note");
#else
		static int shaderId = ResourceManager::getShader("basic2d");
#endif
		static int pteShaderId = ResourceManager::getShader("particles");
		if (shaderId == -1 || pteShaderId == -1)
			return;

		Shader* shader = ResourceManager::shaders[shaderId];
		Shader* pteShader = ResourceManager::shaders[pteShaderId];
		shader->use();

		// Create a projection to the engine coordinate system
		float width  = size.x, height = size.y;
		float scaledWidth = Engine::STAGE_TARGET_WIDTH * Engine::STAGE_WIDTH_RATIO;
		float scaledHeight = Engine::STAGE_TARGET_HEIGHT * Engine::STAGE_HEIGHT_RATIO;
		float scrTop  = Engine::STAGE_TARGET_HEIGHT * Engine::STAGE_TOP_RATIO;
		Utils::fillRect(Engine::STAGE_TARGET_WIDTH, Engine::STAGE_TARGET_HEIGHT, size.x / size.y, width, height);
		
		float aspectRatio = width / height;
		scaledAspectRatio = scaledWidth / scaledHeight;

		auto view = DirectX::XMMatrixScaling(scaledWidth, scaledHeight, 1.f) * DirectX::XMMatrixTranslation(0.f, -scrTop, 0.f);
		auto projection = Camera::getOffCenterOrthographicProjection(-width / 2, width / 2, height / 2, -height / 2);
		auto viewProjection = view * projection;

		const auto pView = noteEffectsCamera.getViewMatrix();
		auto pProjection = noteEffectsCamera.getProjectionMatrix(aspectRatio, 0.3, 1000);
		float projectionScale = std::min(aspectRatio / EFFECTS_TARGET_ASPECT, 1.f);
		pProjection = DirectX::XMMatrixScaling(projectionScale, projectionScale, 1.f) * pProjection;
		
		shader->setMatrix4("projection", viewProjection);
		float currentTime = context.getTimeAtCurrentTick();

		if (previewBuffer.getWidth() != size.x || previewBuffer.getHeight() != size.y)
			previewBuffer.resize(size.x, size.y);
		previewBuffer.bind();
                PV_RESET();
		previewBuffer.clear();

		if (exportWidth > 0 && exportSekaiIntro)
		{
			ImDrawList introDrawList(ImGui::GetDrawListSharedData());
			introDrawList._ResetForNewFrame();
			introDrawList.PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(static_cast<float>(exportWidth), static_cast<float>(exportHeight)));
			introDrawList.PushTexture(ImGui::GetIO().Fonts->TexRef);
			drawSekaiIntro(context, &introDrawList, ImVec2(static_cast<float>(exportWidth), static_cast<float>(exportHeight)), sekaiIntroElapsed);
			introDrawList.PopTexture();
			introDrawList.PopClipRect();
			introDrawList._PopUnusedDrawCmd();
			renderExportOverlay(introDrawList, ImVec2(static_cast<float>(exportWidth), static_cast<float>(exportHeight)));
			previewBuffer.unblind();
			drawList->AddImage((ImTextureID)(size_t)previewBuffer.getTexture(), position, position + displaySize, { 0, 1 }, { 1, 0 });
			return;
		}

		renderer->beginBatch();
		if (config.drawBackground)
		{
			background.setBrightness(config.pvBackgroundBrightness);
			background.draw(renderer, width, height, currentTime, playbackState.isPlaying,
				context.workingData.mvFilename, context.workingData.mvOffset, exportWidth > 0);
		}
		renderer->endBatch();

		shader->use();
		shader->setMatrix4("projection", viewProjection);
		renderer->beginBatch();
		PV_LAP(0);
                drawStage(renderer);
		renderer->endBatch();

		PV_LAP(7);
                context.scorePreviewDrawData.effectView.updateEffects(context, noteEffectsCamera, currentTime);

		shader->use();
		shader->setMatrix4("projection", viewProjection);
		renderer->beginBatch();
		PV_LAP(1);
                drawLines(context, renderer);
		drawHoldCurves(context, renderer);
		if (config.pvStageCover != 0) {
			drawStageCoverMask(renderer);
			renderer->endBatchWithDepthTest(GL_LEQUAL); // using depth test to cull the notes drawn
		}
		else
			renderer->endBatch();

		pteShader->use();
		pteShader->setMatrix4("projection", pProjection);
		pteShader->setMatrix4("view", pView);
		renderer->beginBatch();
		PV_LAP(2);
                context.scorePreviewDrawData.effectView.drawUnderNoteEffects(renderer, currentTime);
		renderer->endBatchWithBlending(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

		shader->use();
		shader->setMatrix4("projection", viewProjection);
		renderer->beginBatch();

		PV_LAP(3);
                drawHoldTicks(context, renderer);
		drawNotes(context, renderer);
		if (config.pvStageCover != 0) {
			drawStageCoverMask(renderer);
			drawStageCover(renderer);
			drawStageCoverDecoration(renderer);
			renderer->endBatchWithDepthTest(GL_LEQUAL); // using depth test to cull the notes drawn
		}
		else
			renderer->endBatch();

		pteShader->use();
		pteShader->setMatrix4("projection", pProjection);
		pteShader->setMatrix4("view", pView);
		renderer->beginBatch();
		PV_LAP(4);
                context.scorePreviewDrawData.effectView.drawEffects(renderer, currentTime);
		renderer->endBatchWithBlending(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

		PV_LAP(5);
                if (exportWidth > 0 && exportHeight > 0 && (exportIncludeUI || exportStageFadeIn < 1.0f))
		{
			ImDrawList hudDrawList(ImGui::GetDrawListSharedData());
			hudDrawList._ResetForNewFrame();
			hudDrawList.PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(static_cast<float>(exportWidth), static_cast<float>(exportHeight)));
			hudDrawList.PushTexture(ImGui::GetIO().Fonts->TexRef);
			if (exportIncludeUI)
				drawHudOverlay(context, &hudDrawList, ImVec2(0.0f, 0.0f), ImVec2(static_cast<float>(exportWidth), static_cast<float>(exportHeight)));
			if (exportStageFadeIn < 1.0f)
			{
				drawSekaiIntroBackdrop(&hudDrawList, ImVec2(static_cast<float>(exportWidth), static_cast<float>(exportHeight)),
					1.0f - exportStageFadeIn, false);
			}
			hudDrawList.PopTexture();
			hudDrawList.PopClipRect();
			hudDrawList._PopUnusedDrawCmd();

			renderExportOverlay(hudDrawList, ImVec2(static_cast<float>(exportWidth), static_cast<float>(exportHeight)));
		}

		previewBuffer.unblind();
		PV_LAP(6);
                drawList->AddImage((ImTextureID)(size_t)previewBuffer.getTexture(), position, position + displaySize, {0, 1}, {1, 0});
		if (exportWidth <= 0)
                {
                        updatePlaytest(context, position, displaySize.x / width, width, scaledWidth);
                        drawHudOverlay(context, drawList, position, displaySize);
                }
	}

	void ScorePreviewWindow::updatePlaytest(ScoreContext& context, ImVec2 position, float sx, float layoutWidth, float laneWidth)
        {
                auto& data = context.scorePreviewDrawData;
                (void)position;
                (void)sx;
                (void)layoutWidth;
                (void)laneWidth;
#ifdef __ANDROID__
                AndroidTouchEvent events[64];
                const int eventCount = AndroidImGui_ConsumeTouchEvents(events, 64);
#endif
                if (!playtestOn)
                {
                        if (data.playtestActive)
                                data.calculateDrawData(context.score);
                        playtestWasPlaying = false;
                        playtestPointers.clear();
                        return;
                }

                const double now = context.getTimeAtCurrentTick();
                const bool started = playbackState.isPlaying && !playtestWasPlaying;
                playtestWasPlaying = playbackState.isPlaying;
                if (started || !data.playtestActive)
                {
                        data.beginPlaytest(context.score, now);
                        playtestPointers.clear();
                        std::fill(std::begin(playtestLaneMoveAt), std::end(playtestLaneMoveAt), -1.0);
                }
                if (!playbackState.isPlaying)
                {
                        playtestPointers.clear();
                        return;
                }

#ifdef __ANDROID__
                auto laneAt = [&](float x)
                {
                        const float laneF = ((x - position.x) / sx - layoutWidth * 0.5f) / laneWidth + 6.0f;
                        const int lane = static_cast<int>(std::floor(laneF));
                        if (lane < 0 || lane > 11)
                                return -1;
                        return config.pvMirrorScore ? 11 - lane : lane;
                };
                auto withSounds = [&](auto&& action)
                {
                        const size_t before = data.playtestHits.size();
                        action();
                        for (size_t k = before; k < data.playtestHits.size(); ++k)
                        {
                                const auto noteIt = context.score.notes.find(data.playtestHits[k].first);
                                if (noteIt == context.score.notes.end())
                                        continue;
                                const std::string_view se = getNoteSE(noteIt->second, context.score);
                                if (!se.empty())
                                        context.audio.playOneShotSound(se);
                        }
                };
                const float flickDistance = laneWidth * sx * 0.7f;
                const float moveThreshold = laneWidth * sx * 0.08f;
                unsigned int touchMask = 0;

                for (int i = 0; i < eventCount; ++i)
                {
                        const AndroidTouchEvent& e = events[i];
                        const double t = now - e.ageSec;
                        if (e.type == 0)
                        {
                                if (e.ageSec > 0.5)
                                        continue;
                                playtestPointers.push_back({ e.id, e.x, e.y, t, e.x, e.y, 0.0 });
                                const int lane = laneAt(e.x);
                                if (lane >= 0)
                                {
                                        touchMask |= 1u << lane;
                                        withSounds([&] { data.playtestTouch(e.id, lane, t); });
                                }
                        }
                        else if (e.type == 1)
                        {
                                auto it = std::find_if(playtestPointers.begin(), playtestPointers.end(), [&](const PlaytestPointer& p) { return p.id == e.id; });
                                if (it == playtestPointers.end())
                                        continue;

                                const int curLane = laneAt(e.x);
                                if (curLane >= 0)
                                        touchMask |= 1u << curLane;
                                const float mdx = e.x - it->px;
                                const float mdy = e.y - it->py;
                                if (mdx * mdx + mdy * mdy >= moveThreshold * moveThreshold)
                                {
                                        const int prevLane = laneAt(it->px);
                                        if (curLane >= 0)
                                                playtestLaneMoveAt[curLane] = t;
                                        if (prevLane >= 0)
                                                playtestLaneMoveAt[prevLane] = t;
                                }
                                it->px = e.x;
                                it->py = e.y;

                                if (t < it->lockUntil || t - it->at > 0.25)
                                {
                                        it->ax = e.x;
                                        it->ay = e.y;
                                        it->at = t;
                                        continue;
                                }
                                const float dx = e.x - it->ax;
                                const float dy = e.y - it->ay;
                                if (dx * dx + dy * dy >= flickDistance * flickDistance)
                                {
                                        const int anchorLane = laneAt(it->ax);
                                        withSounds([&] { data.playtestFlick(curLane, anchorLane, t); });
                                        it->ax = e.x;
                                        it->ay = e.y;
                                        it->at = t;
                                        it->lockUntil = t + 0.15;
                                }
                        }
                        else
                        {
                                withSounds([&] { data.playtestRelease(e.id, t); });
                                playtestPointers.erase(std::remove_if(playtestPointers.begin(), playtestPointers.end(), [&](const PlaytestPointer& p) { return p.id == e.id; }), playtestPointers.end());
                        }
                }

                for (const auto& p : playtestPointers)
                {
                        const int lane = laneAt(p.px);
                        if (lane >= 0)
                                touchMask |= 1u << lane;
                }

                withSounds([&]
                {
                        data.playtestTrace(touchMask, playtestLaneMoveAt, now);
                        data.playtestAdvance(now);
                });
#else
                data.playtestAdvance(now);
#endif
        }

        void ScorePreviewWindow::updateUI(ScoreEditorTimeline& timeline, ScoreContext& context)
	{
		updateHudControls(timeline, context);
		updateToolbar(timeline, context);
		ImGuiIO io = ImGui::GetIO();
		float mouseWheel = io.MouseWheel * 1;
		if (!timeline.isPlaying() && ImGui::IsWindowHovered() && mouseWheel != 0)
		{
			context.currentTick += std::max(mouseWheel * TICKS_PER_BEAT / 2, (float)-context.currentTick);
			//timeline.focusCursor(context.currentTick, mouseWheel > 0 ? Direction::Up : Direction::Down);
		}
		updateScrollbar(timeline, context);

		playbackState.wasLastFramePlaying = playbackState.isPlaying;
		playbackState.isPlaying = timeline.isPlaying();

		if (isFullWindow())
		{
			if (ImGui::BeginPopupContextWindow("preview_context_menu", ImGuiPopupFlags_NoReopen | ImGuiPopupFlags_MouseButtonDefault_))
			{
				bool _fullWindow = fullWindow;
				if (ImGui::MenuItem(getString("fullscreen_preview"), ToShortcutString(config.input.togglePreviewFullWindow), &_fullWindow))
					setFullWindow(_fullWindow);

				ImGui::MenuItem(getString("preview_draw_toolbar"), NULL, &config.pvDrawToolbar);
				ImGui::MenuItem(getString("preview_draw_score_hud"), NULL, &config.pvDrawScoreHud);
				ImGui::MenuItem(getString("preview_draw_score_add_animation"), NULL, &config.pvDrawScorePopup);
				ImGui::MenuItem(getString("preview_draw_life_hud"), NULL, &config.pvDrawLifeHud);
				ImGui::MenuItem(getString("preview_draw_combo"), NULL, &config.pvDrawCombo);
				ImGui::MenuItem(getString("preview_draw_perfect"), NULL, &config.pvDrawPerfect);
				ImGui::MenuItem(getString("return_to_last_tick"), NULL, &config.returnToLastSelectedTickOnPause);
                                ImGui::MenuItem("Playtest", NULL, &playtestOn);
				ImGui::EndPopup();
			}
		}
	}

	void ScorePreviewWindow::updateHudControls(ScoreEditorTimeline& timeline, ScoreContext& context) const
	{
		if (!config.pvDrawLifeHud)
			return;

		ImGuiIO& io = ImGui::GetIO();
		const ImVec2 previewPosition = ImGui::GetCursorScreenPos();
		const ImVec2 previewSize = ImGui::GetContentRegionAvail() - ImVec2{ this->getScrollbarWidth(), 0.0f };
		if (previewSize.x <= 1.0f || previewSize.y <= 1.0f)
			return;

		const float uiScale = std::min(previewSize.x / 1920.0f, previewSize.y / 1080.0f);
		const float offsetX = (previewSize.x - 1920.0f * uiScale) * 0.5f;
		const ImVec2 buttonCenter{
			previewPosition.x + offsetX + HUD_PAUSE_BUTTON_CENTER_X * uiScale,
			previewPosition.y + HUD_PAUSE_BUTTON_CENTER_Y * uiScale
		};
		const float radius = HUD_PAUSE_BUTTON_RADIUS * uiScale;
		const ImVec2 delta = io.MousePos - buttonCenter;
		const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
			delta.x * delta.x + delta.y * delta.y <= radius * radius;

		if (!hovered)
			return;

		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			timeline.setPlaying(context, !timeline.isPlaying());
	}

	void ScorePreviewWindow::setFullWindow(bool _fullWindow)
	{
		fullWindow = _fullWindow;
	}

	void ScorePreviewWindow::setExportResolution(int width, int height, bool includeUI)
	{
		exportWidth = width;
		exportHeight = height;
		exportIncludeUI = includeUI;
	}

	void ScorePreviewWindow::setExportSekaiIntro(bool enabled, float elapsed, float duration, const std::string& title,
		const std::string& designer, const std::string& lyricist, const std::string& composer,
		const std::string& arrangement, const std::string& vocal, const std::string& collabTag, int difficulty,
		const std::string& chartId)
	{
		exportSekaiIntro = enabled;
		sekaiIntroElapsed = elapsed;
		sekaiIntroDuration = duration;
		introTitle = title;
		introDesigner = designer;
		introLyricist = lyricist;
		introComposer = composer;
		introArrangement = arrangement;
		introVocal = vocal;
		introCollabTag = collabTag;
		introChartId = chartId;
		introDifficulty = std::clamp(difficulty, 0, 6);
		if (!enabled)
			return;

		const std::string assets = Application::getAppDir() + "res/editor/sekai_intro/";
		if (!sekaiCollabTag)
			sekaiCollabTag = std::make_unique<Texture>(assets + "collab-tag.png");
		if (!sekaiIntroGradient)
			sekaiIntroGradient = std::make_unique<Texture>(assets + "start_grad.png");
		if (!sekaiMusicSheet)
			sekaiMusicSheet = std::make_unique<Texture>(assets + "music-sheet.png");
		if (!sekaiPerson)
			sekaiPerson = std::make_unique<Texture>(assets + "person.png");
		static constexpr const char* difficultyFiles[] = {
			"easy_bg.png", "normal_bg.png", "hard_bg.png", "expert_bg.png", "master_bg.png", "append_bg.png", "eternal_bg.png"
		};
		if (!sekaiDifficultyBackgrounds[introDifficulty])
			sekaiDifficultyBackgrounds[introDifficulty] = std::make_unique<Texture>(assets + difficultyFiles[introDifficulty]);
	}

	void selectScorePreviewTab()
        {
                ImGuiWindow* window = ImGui::FindWindowByName("###score_preview");
                if (window && window->DockNode && window->DockNode->TabBar)
                        window->DockNode->TabBar->NextSelectedTabId = window->TabId;
        }

        void ScorePreviewWindow::clearExportResolution()
	{
		exportWidth = exportHeight = 0;
		exportIncludeUI = false;
		exportSekaiIntro = false;
		sekaiIntroElapsed = sekaiIntroDuration = 0.0f;
		exportStageFadeIn = 1.0f;
	}

	void ScorePreviewWindow::readPreviewPixels(std::vector<uint8_t>& pixels) const
        {
                previewBuffer.readPixels(pixels);
        }

        bool ScorePreviewWindow::readPreviewPixelsPipelined(std::vector<uint8_t>& pixels)
        {
                return previewBuffer.readPixelsPipelined(pixels);
        }

        bool ScorePreviewWindow::flushPreviewPixelsPipelined(std::vector<uint8_t>& pixels)
        {
                return previewBuffer.flushPipelined(pixels);
        }

        void ScorePreviewWindow::resetPreviewPixelsPipeline()
        {
                previewBuffer.resetPipelined();
        }

	void ScorePreviewWindow::drawSekaiIntroBackdrop(ImDrawList* drawList, ImVec2 size, float opacity, bool tinted)
	{
		if (!drawList || size.x <= 0.0f || size.y <= 0.0f || opacity <= 0.0f)
			return;
		opacity = clamp01(opacity);
		auto alphaByte = [](float alpha) { return static_cast<int>(std::lround(clamp01(alpha) * 255.0f)); };
		if (background.getTexture() && background.getWidth() && background.getHeight())
		{
			// Template zoom: 120% at 16:9, 80% at 4:3. Keep the same crop
			// at other resolutions, filling any extra area without stretching.
			const float aspect = size.x / size.y;
			float zoom = size.y / 1080.0f * std::clamp(0.9f * aspect - 0.4f, 0.8f, 1.2f);
			zoom = std::max(zoom, std::max(size.x / background.getWidth(), size.y / background.getHeight()));
			const ImVec2 dimensions(background.getWidth() * zoom, background.getHeight() * zoom);
			const ImVec2 minimum = (size - dimensions) * 0.5f;
			// The slate layer ends with the card at frame 299. Gameplay layers
			// then fade in over the original, untinted background for 96 frames.
			const float tint = tinted ? 0.8f : 0.0f;
			const float imageAlpha = opacity * (1.0f - tint) / (1.0f - opacity * tint);
			drawList->AddImage(static_cast<ImTextureID>(background.getTexture()), minimum, minimum + dimensions,
				// This cache uses a Y-up projection and stores the source's top
				// row at v=0. It does not use previewBuffer's flipped UV mapping.
				{ 0.0f, 0.0f }, { 1.0f, 1.0f }, IM_COL32(255, 255, 255, alphaByte(imageAlpha)));
			if (tinted)
				drawList->AddRectFilled({ 0.0f, 0.0f }, size, IM_COL32(104, 104, 156, alphaByte(opacity * tint)));
		}
		else
			drawList->AddRectFilled({ 0.0f, 0.0f }, size, IM_COL32(104, 104, 156, alphaByte(opacity)));
	}

	void ScorePreviewWindow::drawSekaiIntro(const ScoreContext& context, ImDrawList* drawList, ImVec2 size, float elapsed)
	{
		if (!drawList || size.x <= 0.0f || size.y <= 0.0f)
			return;

		// Port of pjsekai-overlay-APPEND's main2_16-9_1920x1080.object
		// and the quadratic Lua easing in @加減速TRA.tra (param=2).
		// Reference frames are 0..299 at 60 fps, independent of export fps.
		const float frame = std::clamp(elapsed * 300.0f / std::max(sekaiIntroDuration, 0.001f), 0.0f, 300.0f);
		const float scale = std::min(size.x / 1920.0f, size.y / 1080.0f);
		const float offsetX = (size.x - 1920.0f * scale) * 0.5f;
		// The 4:3 template scales the group to 75% and moves it down 135px:
		// its composition remains attached to the bottom of the frame.
		const float offsetY = size.y - 1080.0f * scale;
		auto px = [&](float x) { return offsetX + x * scale; };
		auto py = [&](float y) { return offsetY + y * scale; };
		auto ps = [&](float value) { return value * scale; };
		auto white = [](float alpha) { return IM_COL32(255, 255, 255, static_cast<int>(std::lround(clamp01(alpha) * 255.0f))); };
		auto easeOut = [](float progress) { progress = clamp01(progress); return 1.0f - (1.0f - progress) * (1.0f - progress); };
		const float contentAlpha = 1.0f - clamp01((frame - 270.0f) / 29.0f);
		auto sprite = [&](const Texture* texture, float centerX, float centerY, float zoomX, float zoomY, float alpha)
		{
			if (!texture || !texture->getID() || alpha <= 0.0f)
				return;
			const ImVec2 halfSize(ps(texture->getWidth() * zoomX) * 0.5f, ps(texture->getHeight() * zoomY) * 0.5f);
			const ImVec2 center(px(centerX), py(centerY));
			drawList->AddImage(static_cast<ImTextureID>(texture->getID()), center - halfSize, center + halfSize,
				{ 0.0f, 0.0f }, { 1.0f, 1.0f }, white(alpha));
		};
		ImFont* titleFont = ImGuiManager::getSekaiDifficultyFont();
		ImFont* detailsFont = ImGuiManager::getSekaiDetailsFont();
		if (!titleFont) titleFont = ImGui::GetFont();
		if (!detailsFont) detailsFont = ImGui::GetFont();

		// AviUtl's auto-resize fits a single text row in X only. Fit before
		// drawing so long text isn't clipped, then restore its original height.
		auto text = [&](ImFont* font, float fontSize, float x, float y, float maxWidth,
			float tracking, const std::string& value)
		{
			if (value.empty() || contentAlpha <= 0.0f)
				return;
			std::string line = value;
			std::replace(line.begin(), line.end(), '\n', ' ');
			std::replace(line.begin(), line.end(), '\r', ' ');
			int characters = 0;
			for (unsigned char byte : line)
				if ((byte & 0xc0) != 0x80) ++characters;
			const float originalSize = ps(fontSize);
			const float naturalWidth = font->CalcTextSizeA(originalSize, FLT_MAX, 0.0f, line.c_str()).x
				+ ps(tracking) * std::max(0, characters - 1);
			const float fit = std::min(1.0f, ps(maxWidth) / std::max(naturalWidth, 1.0f));
			const float drawSize = originalSize * fit;
			const float startY = py(y);
			const int firstVertex = drawList->VtxBuffer.Size;
			float cursor = px(x);
			const char* end = line.c_str() + line.size();
			for (const char* current = line.c_str(); current < end;)
			{
				unsigned int codepoint;
				const int bytes = ImTextCharFromUtf8(&codepoint, current, end);
				if (bytes <= 0) break;
				drawList->AddText(font, drawSize, { cursor, startY }, white(contentAlpha), current, current + bytes);
				cursor += font->GetFontBaked(drawSize)->GetCharAdvance(static_cast<ImWchar>(codepoint)) + ps(tracking) * fit;
				current += bytes;
			}
			if (fit < 1.0f)
				for (int i = firstVertex; i < drawList->VtxBuffer.Size; ++i)
					drawList->VtxBuffer[i].pos.y = startY + (drawList->VtxBuffer[i].pos.y - startY) / fit;
		};
		auto credit = [](const std::string& value) -> std::string { return value.empty() ? "-" : value; };

		drawSekaiIntroBackdrop(drawList, size, 1.0f);
		// The original gradient image sweeps upward twice, at frames 60 and 180.
		for (float start : { 60.0f, 180.0f })
			if (frame >= start && frame < start + 120.0f)
			{
				const float centerY = 540.0f + 1500.0f * (1.0f - easeOut((frame - start) / 119.0f));
				sprite(sekaiIntroGradient.get(), 960.0f, centerY, 1.5f, 1.5f, 0.1f);
			}

		// Only the panel, difficulty text and its icon move; the jacket stays put.
		const float revealOffset = 43.0f * (1.0f - easeOut(frame / 111.0f));
		const int difficulty = std::clamp(introDifficulty, 0, 6);
		sprite(sekaiDifficultyBackgrounds[difficulty].get(), 298.5f + revealOffset, 828.0f - revealOffset,
			0.3926f, 0.3926f, contentAlpha);
		static constexpr const char* difficultyNames[] = { "EASY", "NORMAL", "HARD", "EXPERT", "MASTER", "APPEND", "ETERNAL" };
		if (contentAlpha > 0.0f)
		{
			// AviUtl sizes and aligns the rendered capital letters. ImGui's
			// line height includes font padding, so normalize to the visible
			// ink bounds to keep the difficulty wording on the icon's baseline.
			const float capHeight = ps(84.0f * 0.38f);
			float fontSize = capHeight;
			const ImFontGlyph* capital = titleFont->GetFontBaked(fontSize)->FindGlyph('H');
			if (capital && capital->Y1 > capital->Y0)
				fontSize *= capHeight / (capital->Y1 - capital->Y0);
			const ImVec2 anchor(px(149.0f + revealOffset), py(1008.29f - revealOffset) - capHeight * 0.5f);
			const int firstVertex = drawList->VtxBuffer.Size;
			drawList->AddText(titleFont, fontSize, anchor, white(contentAlpha), difficultyNames[difficulty]);
			ImVec2 inkMin(FLT_MAX, FLT_MAX), inkMax(-FLT_MAX, -FLT_MAX);
			for (int i = firstVertex; i < drawList->VtxBuffer.Size; ++i)
			{
				const ImVec2 point = drawList->VtxBuffer[i].pos;
				inkMin.x = std::min(inkMin.x, point.x);
				inkMin.y = std::min(inkMin.y, point.y);
				inkMax.x = std::max(inkMax.x, point.x);
				inkMax.y = std::max(inkMax.y, point.y);
			}
			if (inkMax.y > inkMin.y && inkMax.x > inkMin.x)
			{
				const float heightScale = capHeight / (inkMax.y - inkMin.y);
				const float widthScale = std::min(heightScale, ps(350.0f) / (inkMax.x - inkMin.x));
				// Normal Layout has no chart icon: center the difficulty in the
				// exposed gradient strip. Custom Layout keeps the icon/text row.
				const float left = config.pvCustomChartLayout ? anchor.x :
					px(298.5f + revealOffset) - (inkMax.x - inkMin.x) * widthScale * 0.5f;
				for (int i = firstVertex; i < drawList->VtxBuffer.Size; ++i)
				{
					ImVec2& point = drawList->VtxBuffer[i].pos;
					point.x = left + (point.x - inkMin.x) * widthScale;
					point.y = anchor.y + (point.y - inkMin.y) * heightScale;
				}
			}
		}
		if (config.pvCustomChartLayout)
			sprite(sekaiMusicSheet.get(), 125.19f + revealOffset, 1008.29f - revealOffset, 0.65137f, 0.65137f, contentAlpha);

		const float metadataOffsetY = 24.0f;
		if (!introCollabTag.empty())
		{
			sprite(sekaiCollabTag.get(), 794.0f, 648.0f + metadataOffsetY, 2.0f, 1.0f, contentAlpha);
			text(detailsFont, 72.0f * 0.38f, 580.0f, 631.0f + metadataOffsetY, 420.0f, -0.152f, introCollabTag);
		}
		text(titleFont, 96.0f * 0.4f, 582.5f, 705.0f + metadataOffsetY, 3350.0f * 0.4f, 12.1f * 0.4f,
			introTitle.empty() ? "Untitled" : introTitle);
		const bool japanese = config.language.rfind("ja", 0) == 0 ||
			(config.language == "auto" && Utilities::getSystemLocale().rfind("ja", 0) == 0);
		const std::string musicCredits = japanese
			? std::string(u8"作詞：") + credit(introLyricist) + u8"    作曲：" + credit(introComposer) + u8"    編曲：" + credit(introArrangement)
			: "Lyrics: " + credit(introLyricist) + "    Music: " + credit(introComposer) + "    Arrangement: " + credit(introArrangement);
		text(detailsFont, 72.0f * 0.38f, 580.0f, 795.5f + metadataOffsetY, 3350.0f * 0.38f, -0.152f, musicCredits);
		text(detailsFont, 72.0f * 0.38f, 580.0f, 844.5f + metadataOffsetY, 3350.0f * 0.38f, -0.152f,
			(japanese ? std::string(u8"Vo：") : std::string("Vocals: ")) + credit(introVocal));

		if (config.pvCustomChartLayout)
		{
			// Additional rows for fanmade chart videos, controlled separately
			// from the song information used by the normal layout.
			drawList->AddRectFilled({ px(581.25f), py(902.4f + metadataOffsetY) }, { px(1858.75f), py(905.6f + metadataOffsetY) }, white(contentAlpha));
			sprite(sekaiMusicSheet.get(), 605.3f, 951.0f + metadataOffsetY, 0.65137f, 0.65137f, contentAlpha);
			sprite(sekaiPerson.get(), 605.3f, 1010.0f + metadataOffsetY, 1.2f, 1.2f, contentAlpha);
			text(detailsFont, 72.0f * 0.38f, 637.0f, 932.5f + metadataOffsetY, 1220.0f, -0.152f,
				(japanese ? std::string(u8"譜面ID：") : std::string("Chart ID: ")) + credit(introChartId));
			text(detailsFont, 72.0f * 0.38f, 637.0f, 993.0f + metadataOffsetY, 1220.0f, -0.152f, credit(introDesigner));
		}

		// Last in the source layer stack: an exactly 512px jacket at 78.75%.
		const ImVec2 jacketMin(px(140.4f), py(583.4f));
		const ImVec2 jacketMax(px(543.6f), py(986.6f));
		if (context.workingData.jacket.getTexID())
			drawList->AddImage(static_cast<ImTextureID>(context.workingData.jacket.getTexID()), jacketMin, jacketMax,
				{ 0.0f, 0.0f }, { 1.0f, 1.0f }, white(contentAlpha));
		else
		{
			drawList->AddRectFilled(jacketMin, jacketMax, IM_COL32(50, 50, 72, static_cast<int>(contentAlpha * 255.0f)));
			text(detailsFont, 24.0f, 194.0f, 770.0f, 300.0f, 0.0f, "No jacket image");
		}
	}

	void ScorePreviewWindow::loadNoteEffects(Effect::EffectView& effectView)
	{
		const std::string effectsDir = Application::getAppDir() + "res/effect/" + std::to_string(config.pvEffectsProfile) + "/";
		size_t effectCount = arrayLength(Effect::effectNames);

		// Cleanup. We don't want all profiles and their resources loaded in memory
		ResourceManager::removeAllParticleEffects();
		int texIndex = ResourceManager::getTexture("tex_note_common_all_v2");
		if (texIndex > -1)
			ResourceManager::disposeTexture(ResourceManager::textures[texIndex].getID());

		ResourceManager::loadTexture(effectsDir + "tex_note_common_all_v2.png");

		std::vector<std::string> failedParticleFiles;
		for (size_t i = 0; i < effectCount; i++)
		{
			const std::string filename{ effectsDir + Effect::effectNames[i] + ".json" };
			int particleId = ResourceManager::loadParticleEffect(filename);

			if (particleId == -1)
				failedParticleFiles.push_back(filename);
		}

		if (!failedParticleFiles.empty())
		{
			std::string fullErrorMessage = "Failed to load the following note effects: \n\n";
			for (const auto& error : failedParticleFiles)
				fullErrorMessage.append(error).append("\n");

			IO::messageBox(
				APP_NAME,
				fullErrorMessage,
				IO::MessageBoxButtons::Ok,
				IO::MessageBoxIcon::Warning,
				Application::windowState.windowHandle
			);
		}

		effectView.reset();
		effectView.init();
	}

	void ScorePreviewWindow::drawHudOverlay(const ScoreContext& context, ImDrawList* drawList, ImVec2 position, ImVec2 size)
	{
		if (!drawList || size.x <= 1.0f || size.y <= 1.0f)
			return;

		if (context.score.tempoChanges.empty())
			return;

		const auto& drawData = context.scorePreviewDrawData;
		const auto& comboTimes = drawData.comboTimes;
		const float chartTimeSec = static_cast<float>(context.getTimeAtCurrentTick());
		int latestComboIndex = -1;
		if (!comboTimes.empty())
		{
			const auto comboIt = std::upper_bound(comboTimes.begin(), comboTimes.end(), chartTimeSec + 0.0001f);
			latestComboIndex = static_cast<int>(std::distance(comboTimes.begin(), comboIt)) - 1;
		}

		const int combo = latestComboIndex >= 0 && latestComboIndex < static_cast<int>(drawData.hudComboValues.size())
                    ? drawData.hudComboValues[latestComboIndex] : latestComboIndex + 1;
		const int hudScore = latestComboIndex >= 0 && latestComboIndex < static_cast<int>(drawData.hudScores.size()) ? drawData.hudScores[latestComboIndex] : 0;
		const char hudRank = latestComboIndex >= 0 && latestComboIndex < static_cast<int>(drawData.hudRanks.size()) ? drawData.hudRanks[latestComboIndex] : 'd';
		const float hudScoreBar = latestComboIndex >= 0 && latestComboIndex < static_cast<int>(drawData.hudScoreBars.size()) ? drawData.hudScoreBars[latestComboIndex] : 0.0f;
		const float scaleX = size.x / Engine::STAGE_TARGET_WIDTH;
		const float scaleY = size.y / Engine::STAGE_TARGET_HEIGHT;
		const float uiScale = std::min(scaleX, scaleY);

		// Position anchors follow the actual frame dimensions so corner HUDs
		// remain attached to the playfield on portrait, ultrawide, and custom
		// aspect ratios. A uniform size scale keeps HUD textures undistorted.
		auto px = [&](float x) { return position.x + x * scaleX; };
		auto py = [&](float y) { return position.y + y * scaleY; };
		auto topPy = [&](float y) { return py(y); };
		auto ps = [&](float v) { return v * uiScale; };
		auto comboPx = [&](float x) { return position.x + (size.x - Engine::STAGE_TARGET_WIDTH * uiScale) * 0.5f + x * uiScale; };
		auto comboGroupX = [&](float x) {
			return COMBO_GROUP_CENTER_X + (x - COMBO_GROUP_CENTER_X) * COMBO_GROUP_SCALE;
		};
		auto comboGroupY = [&](float y) {
			return COMBO_GROUP_CENTER_Y + (y - COMBO_GROUP_CENTER_Y) * COMBO_GROUP_SCALE + COMBO_GROUP_OFFSET_Y;
		};
		auto comboGroupS = [&](float v) { return v * COMBO_GROUP_SCALE; };

		auto drawImage = [&](const Texture* texture, float x, float y, float width, float height, float alpha = 1.0f)
		{
			if (texture)
				drawComboImage(drawList, *texture, px(x), py(y), ps(width), ps(height), alpha);
		};
		auto drawTopImage = [&](const Texture* texture, float x, float y, float width, float height, float alpha = 1.0f)
		{
			if (texture)
				drawComboImage(drawList, *texture, px(x), topPy(y), ps(width), ps(height), alpha);
		};
		auto drawTopImageClipX = [&](const Texture* texture, float x, float y, float width, float height, float ratio, float alpha = 1.0f)
		{
			if (texture)
				drawHudImageClipX(drawList, *texture, px(x), topPy(y), ps(width), ps(height), ratio, alpha);
		};

		// Keep the score HUD's internal spacing on the same uniform scale as its
		// textures. Using px() here stretches the spacing on ultrawide previews,
		// while ps() keeps the artwork uniform, making the bar and rank markers
		// drift apart at non-16:9 resolutions.
		const float scoreAnchorShift = (size.x - Engine::STAGE_TARGET_WIDTH * uiScale) * 0.5f * 0.5f;
		auto scoreX = [&](float v) { return position.x + scoreAnchorShift + (36.0f + v * SCORE_ROOT_SCALE) * uiScale; };
		auto scoreY = [&](float v) { return position.y + (-3.0f + v * SCORE_ROOT_SCALE) * uiScale; };
		auto scoreS = [&](float v) { return v * SCORE_ROOT_SCALE * uiScale; };
		auto drawScoreImage = [&](const Texture* texture, float x, float y, float width, float height)
		{
			if (texture)
				drawComboImage(drawList, *texture, scoreX(x), scoreY(y), scoreS(width), scoreS(height), 1.0f);
		};
		auto drawScoreImageClipX = [&](const Texture* texture, float x, float y, float width, float height, float ratio)
		{
			if (texture)
				drawHudImageClipX(drawList, *texture, scoreX(x), scoreY(y), scoreS(width), scoreS(height), ratio, 1.0f);
		};

		if (config.pvDrawScoreHud || config.pvDrawScorePopup)
		{
			if (config.pvDrawScoreHud)
			{
				drawScoreImage(getHudTexture("score/bg.png"), 0.0f, 0.0f, 444.0f, 96.0f);
				drawScoreImageClipX(
				getHudTexture("score/bar.png"),
					79.0f,
					37.0f,
					SCORE_BAR_FULL_WIDTH,
					16.0f,
				hudScoreBar
			);
				drawScoreImage(getHudTexture("score/fg.png"), 0.0f, 0.0f, 444.0f, 96.0f);
				drawScoreImage(
				getHudTexture(std::string("score/rank/chr/") + hudRank + ".png"),
					10.0f,
					13.0f,
					49.0f,
					58.0f
			);
				drawScoreImage(
				getHudTexture(std::string("score/rank/txt/en/") + hudRank + ".png"),
					6.0f,
					77.0f,
					60.0f,
					8.0f
			);

			const std::string scoreText = scoreDigitsText(hudScore);
			for (size_t i = 0; i < scoreText.size(); ++i)
			{
				const char ch = scoreText[i];
				const Texture* shadow = getHudTexture(std::string("score/digit/s") + ch + ".png");
				const Texture* main = getHudTexture(std::string("score/digit/") + ch + ".png");
				if (!shadow || !main)
					continue;

					const float slotCenterX = scoreX(82.0f + static_cast<float>(i) * 22.0f + 11.0f);
					const float slotY = scoreY(60.0f);
					const float shadowHeight = scoreS(36.0f);
					const float mainHeight = scoreS(29.0f);
				const float shadowWidth = shadowHeight * (static_cast<float>(shadow->getWidth()) / static_cast<float>(shadow->getHeight()));
				const float mainWidth = mainHeight * (static_cast<float>(main->getWidth()) / static_cast<float>(main->getHeight()));
				const float shadowScaleX = shadowWidth / static_cast<float>(shadow->getWidth());
				const float mainScaleX = mainWidth / static_cast<float>(main->getWidth());
				const float shadowCenterX = slotCenterX - scoreDigitOpticalOffsetPx(ch, true) * shadowScaleX;
				const float mainCenterX = slotCenterX - scoreDigitOpticalOffsetPx(ch, false) * mainScaleX;
					drawComboImage(drawList, *shadow, shadowCenterX - shadowWidth * 0.5f, slotY - scoreS(4.0f), shadowWidth, shadowHeight, 1.0f);
					drawComboImage(drawList, *main, mainCenterX - mainWidth * 0.5f, slotY, mainWidth, mainHeight, 1.0f);
			}

			}

			if (config.pvDrawScorePopup && latestComboIndex >= 0 && latestComboIndex < static_cast<int>(drawData.hudScoreGains.size()))
			{
				const float eventTime = comboTimes[latestComboIndex];
				const float popupElapsed = std::max(0.0f, chartTimeSec - eventTime);
				constexpr float popupDuration = 0.72f;
				if (popupElapsed < popupDuration)
				{
					int firstSameTimeEvent = latestComboIndex;
					while (firstSameTimeEvent > 0 && std::abs(comboTimes[firstSameTimeEvent - 1] - eventTime) < 0.0001f)
						--firstSameTimeEvent;
					int scoreGain = 0;
					for (int i = firstSameTimeEvent; i <= latestComboIndex; ++i)
						scoreGain += drawData.hudScoreGains[i];

					if (scoreGain > 0)
					{
						const float popupSizeScale = std::clamp(config.pvScorePopupSize, 50.0f, 200.0f) / 100.0f * 0.92f;
						const float slideProgress = clamp01(popupElapsed / 0.12f);
						const float popupX = (position.x + scoreAnchorShift + ps(450.0f + std::clamp(config.pvScorePopupOffsetX, -400.0f, 400.0f)))
							- ps(30.0f) * popupSizeScale * (1.0f - slideProgress);
						const float popupY = position.y + ps(108.0f + std::clamp(config.pvScorePopupOffsetY, -250.0f, 250.0f));
						const float fadeOut = clamp01((popupDuration - popupElapsed) / 0.18f);
						const float alpha = (0.25f + 0.75f * slideProgress) * fadeOut;
						const float digitHeight = scoreS(22.0f) * popupSizeScale;
						const float digitGap = scoreS(16.0f) * popupSizeScale;
						const float plusHeight = digitHeight * 0.58f;
						const float plusX = popupX;
						const float plusY = popupY + (digitHeight - plusHeight) * 0.5f;
						const float plusAlpha = alpha;
						if (const Texture* plus = getHudTexture("score/digit/plus.png"))
						{
							const float plusWidth = plusHeight * (static_cast<float>(plus->getWidth()) / static_cast<float>(plus->getHeight()));
							drawComboImage(drawList, *plus, plusX, plusY, plusWidth, plusHeight, plusAlpha);
						}

						const std::string gainDigits = std::to_string(scoreGain);
						const Texture* plus = getHudTexture("score/digit/plus.png");
						const float plusWidth = plus && plus->getHeight() > 0
							? plusHeight * (static_cast<float>(plus->getWidth()) / static_cast<float>(plus->getHeight()))
							: plusHeight;
						const float firstDigitX = plusX + plusWidth + scoreS(5.0f) * popupSizeScale;
						for (size_t i = 0; i < gainDigits.size(); ++i)
						{
							const float digitWidth = digitHeight;
							const float digitX = firstDigitX + static_cast<float>(i) * digitGap;
							const float digitY = popupY;
							const char digit = gainDigits[i];
							const Texture* shadow = getHudTexture(std::string("score/digit/s") + digit + ".png");
							const Texture* main = getHudTexture(std::string("score/digit/") + digit + ".png");
							if (!shadow || !main)
								continue;
							const float shadowWidth = digitWidth * (static_cast<float>(shadow->getWidth()) / static_cast<float>(shadow->getHeight()));
							const float mainWidth = digitWidth * (static_cast<float>(main->getWidth()) / static_cast<float>(main->getHeight()));
							drawComboImage(drawList, *shadow, digitX + ps(1.5f) * popupSizeScale, digitY + ps(2.0f) * popupSizeScale, shadowWidth, digitWidth, alpha * 0.8f);
							drawComboImage(drawList, *main, digitX, digitY, mainWidth, digitWidth, alpha);
						}
					}
				}
			}
		}

		if (config.pvDrawLifeHud)
		{
			const float lifeRatio = latestComboIndex >= 0 && latestComboIndex < static_cast<int>(drawData.hudLifeValues.size())
                        ? clamp01(drawData.hudLifeValues[latestComboIndex] / 1000.0f) : 1.0f;
			drawTopImage(getHudTexture("life/bg.png"), 1442.0f, HUD_LIFE_Y, 444.0f, HUD_LIFE_HEIGHT);
			drawTopImageClipX(getHudTexture("life/normal.png"), 1442.0f, HUD_LIFE_Y, 444.0f, HUD_LIFE_HEIGHT, lifeRatio);

			const std::string lifeText = std::to_string(static_cast<int>(std::lround(1000.0f * lifeRatio)));
			for (size_t i = 0; i < lifeText.size(); ++i)
			{
				const char ch = lifeText[lifeText.size() - 1 - i];
				const Texture* shadow = getHudTexture(std::string("life/digit/s") + ch + ".png");
				const Texture* main = getHudTexture(std::string("life/digit/") + ch + ".png");
				if (!shadow || !main)
					continue;

				const float slotX = 305.0f - static_cast<float>(i) * 22.0f;
				const float slotY = 21.0f;
				const float shadowHeight = ps(37.0f);
				const float mainHeight = ps(34.0f);
				const float shadowWidth = shadowHeight * (static_cast<float>(shadow->getWidth()) / static_cast<float>(shadow->getHeight()));
				const float mainWidth = mainHeight * (static_cast<float>(main->getWidth()) / static_cast<float>(main->getHeight()));
				const float centerX = px(1442.0f) + ps(slotX + 13.0f);
				drawComboImage(drawList, *shadow, centerX - shadowWidth * 0.5f, topPy(slotY - 2.0f), shadowWidth, shadowHeight, 1.0f);
				drawComboImage(drawList, *main, centerX - mainWidth * 0.5f, topPy(slotY), mainWidth, mainHeight, 1.0f);
			}
		}

		if (config.pvDrawCombo && combo > 0)
		{
			const float comboElapsedSec = std::max(0.0f, chartTimeSec - comboTimes[latestComboIndex]);
			constexpr float AP_PULSE_ANGULAR = 3.14159265359f * (4.0f / 3.0f);
			const float pulseAlpha = clamp01((std::sin(chartTimeSec * AP_PULSE_ANGULAR) + 1.0f) * 0.5f);
			const Texture* comboTag = getComboTexture("pt.png");
			const Texture* comboTagGlow = getComboTexture("pe.png");
			if (comboTag && comboTagGlow)
			{
				constexpr float comboTagWidth = 127.0f;
				constexpr float comboTagHeight = 42.0f;
				const float comboTagGlowWidth = comboTagWidth * (static_cast<float>(comboTagGlow->getWidth()) / static_cast<float>(comboTag->getWidth()));
				const float comboTagGlowHeight = comboTagHeight * (static_cast<float>(comboTagGlow->getHeight()) / static_cast<float>(comboTag->getHeight()));
                                const float tagCenterY = py(comboGroupY(478.0f)) - ps(comboGroupS(67.0f));
				drawComboImage(
					drawList,
					*comboTagGlow,
					comboPx(comboGroupX(1634.0f)) - ps(comboGroupS(comboTagGlowWidth * 0.5f)),
					tagCenterY - ps(comboGroupS(3.0f)) - ps(comboGroupS(comboTagGlowHeight)) * 0.5f,
					ps(comboGroupS(comboTagGlowWidth)),
					ps(comboGroupS(comboTagGlowHeight)),
					pulseAlpha
				);
				drawComboImage(
					drawList,
					*comboTag,
					comboPx(comboGroupX(1634.0f)) - ps(comboGroupS(comboTagWidth * 0.5f)),
					tagCenterY - ps(comboGroupS(comboTagHeight)) * 0.5f,
					ps(comboGroupS(comboTagWidth)),
					ps(comboGroupS(comboTagHeight)),
					1.0f
				);
			}

			const float numberRate = clamp01(comboElapsedSec / 0.1f);
			const float numberScale = COMBO_BASE_SCALE * (numberRate * 0.4f + 0.6f);
			const float effectRate = clamp01((comboElapsedSec - 0.08f) / 0.16f);
			const float effectScale = comboElapsedSec >= 0.08f ? COMBO_BASE_SCALE * (effectRate * 0.45f + 1.0f) : 0.0f;
			const float effectAlpha = comboElapsedSec >= 0.08f ? (1.0f - effectRate) * 0.8f : 0.0f;
			const std::string comboText = std::to_string(combo);
			const float mid = static_cast<float>(comboText.size()) / 2.0f;
			constexpr float comboCenterYOffset = 18.0f;

			for (size_t i = 0; i < comboText.size(); ++i)
			{
				const char ch = comboText[i];
				const Texture* glow = getComboTexture(std::string("b") + ch + ".png");
				const Texture* main = getComboTexture(std::string("p") + ch + ".png");
				if (!main)
					continue;

				auto digitCenter = [&](float layerScale)
				{
					const float left = (static_cast<float>(i) - mid + 0.5f) * COMBO_DIGIT_STEP * layerScale;
					const float centerX = comboPx(comboGroupX(1634.0f)) + ps(comboGroupS(left));
					const float centerY = py(comboGroupY(478.0f)) + ps(comboGroupS(comboCenterYOffset * layerScale));
					return ImVec2(centerX, centerY);
				};

				auto drawDigitLayer = [&](const Texture& texture, float layerScale, float alpha)
				{
					if (layerScale <= 0.001f || alpha <= 0.001f)
						return;

					const ImVec2 center = digitCenter(layerScale);
					const float height = ps(comboGroupS(134.0f * layerScale));
					const float width = height * (static_cast<float>(texture.getWidth()) / static_cast<float>(texture.getHeight()));
					drawComboImage(drawList, texture, center.x - width * 0.5f, center.y - height * 0.5f, width, height, alpha);
				};

				if (glow && effectAlpha > 0.001f)
				{
					const ImVec2 center = digitCenter(effectScale);
					const float glowBaseHeight = 134.0f * (static_cast<float>(glow->getHeight()) / static_cast<float>(main->getHeight()));
					const float glowHeight = ps(comboGroupS(glowBaseHeight * effectScale));
					const float glowWidth = glowHeight * (static_cast<float>(glow->getWidth()) / static_cast<float>(glow->getHeight()));
					drawComboImage(drawList, *glow, center.x - glowWidth * 0.5f, center.y - glowHeight * 0.5f, glowWidth, glowHeight, effectAlpha);
				}
				drawDigitLayer(*main, effectScale, effectAlpha * 0.7f);

				if (glow && pulseAlpha > 0.001f)
				{
					const ImVec2 center = digitCenter(numberScale);
					const float glowBaseHeight = 134.0f * (static_cast<float>(glow->getHeight()) / static_cast<float>(main->getHeight()));
					const float glowHeight = ps(comboGroupS(glowBaseHeight * numberScale));
					const float glowWidth = glowHeight * (static_cast<float>(glow->getWidth()) / static_cast<float>(glow->getHeight()));
					drawComboImage(drawList, *glow, center.x - glowWidth * 0.5f, center.y - glowHeight * 0.5f, glowWidth, glowHeight, pulseAlpha);
				}
				drawDigitLayer(*main, numberScale, 1.0f);
			}
		}

		const auto& judgeTimes = drawData.hudJudgeTimes;
		if (config.pvDrawPerfect && !judgeTimes.empty())
		{
			const auto judgeIt = std::upper_bound(judgeTimes.begin(), judgeTimes.end(), chartTimeSec + 0.0001f);
			const int latestJudgeIndex = static_cast<int>(std::distance(judgeTimes.begin(), judgeIt)) - 1;
			if (latestJudgeIndex >= 0)
			{
				const float judgeElapsedSec = chartTimeSec - judgeTimes[latestJudgeIndex];
				if (judgeElapsedSec >= 0.0f && judgeElapsedSec <= JUDGE_VISIBLE_WINDOW_SEC)
				{
					const float progressFrames = judgeElapsedSec * 60.0f;
					float alpha = 1.0f;
					float rawScale = 2.0f / 3.0f;
					if (progressFrames < 2.0f)
						alpha = 0.0f;
					else if (progressFrames < 5.0f)
						rawScale = (2.0f / 3.0f) - std::pow(-1.45f + progressFrames / 4.0f, 4.0f) * (2.0f / 3.0f);

					const float scale = std::max(0.01f, rawScale * 1.5f);
					const Texture* judgeTexture = getHudTexture(std::string("judge/") + drawData.judgeNameAt(latestJudgeIndex) + ".png");
                                        const float judgeWidth = (judgeTexture ? static_cast<float>(judgeTexture->getWidth()) : 310.0f) * scale;
                                        const float judgeHeight = (judgeTexture ? static_cast<float>(judgeTexture->getHeight()) : 81.0f) * scale;
                                        drawImage(
                                                judgeTexture,
						960.0f - judgeWidth * 0.5f,
						667.5f - judgeHeight * 0.5f,
						judgeWidth,
						judgeHeight,
						alpha
					);
				}
			}
		}
	}

	const Texture &ScorePreviewWindow::getNoteTexture()
	{
		return ResourceManager::textures[noteSkins.getItemIndex(NoteSkinItem::Notes)];
	}

	void ScorePreviewWindow::drawStage(Renderer* renderer)
	{
		int index = ResourceManager::getTexture("stage");
		if (index == -1)
			return;
		const Texture& stage = ResourceManager::textures[index];
		if (!isArrayIndexInBounds(SPR_SEKAI_STAGE, stage.sprites))
			return;
		const Sprite& stageSprite = stage.sprites[SPR_SEKAI_STAGE];
		constexpr float stageWidth = (Engine::STAGE_TEX_WIDTH / Engine::STAGE_LANE_WIDTH) * Engine::STAGE_NUM_LANES;
		constexpr float stageLeft  = -stageWidth / 2;
		constexpr float stageTop = Engine::STAGE_LANE_TOP / Engine::STAGE_LANE_HEIGHT;
		constexpr float stageHeight = Engine::STAGE_TEX_HEIGHT / Engine::STAGE_LANE_HEIGHT;
		renderer->drawRectangle({stageLeft, stageTop}, {stageWidth, stageHeight}, stage, stageSprite.getX1(), stageSprite.getX2(), stageSprite.getY1(), stageSprite.getY2(), defaultTint.scaleAlpha(config.pvStageOpacity), -1);
	}

	void ScorePreviewWindow::drawStageCoverMask(Renderer* renderer)
	{
		int index = ResourceManager::getTexture("stage");
		if (index == -1)
			return;
		const Texture& stage = ResourceManager::textures[index];
		if (!isArrayIndexInBounds(SPR_SEKAI_STAGE, stage.sprites))
			return;
		const Sprite& stageSprite = stage.sprites[SPR_SEKAI_STAGE];
		constexpr float stageWidth = (Engine::STAGE_TEX_WIDTH / Engine::STAGE_LANE_WIDTH) * Engine::STAGE_NUM_LANES;
		constexpr float stageLeft = -stageWidth / 2, stageRight = stageWidth / 2;

		constexpr float stageTop = Engine::STAGE_LANE_TOP / Engine::STAGE_LANE_HEIGHT;
		const float stageHeight = config.pvStageCover * (1 - stageTop);
		const float spriteHeight = config.pvStageCover * (Engine::STAGE_LANE_HEIGHT - Engine::STAGE_LANE_TOP);

		static auto model = DirectX::XMMatrixTranslation(0, 0, 1);
		renderer->drawQuad(
			Engine::quadvPos(stageLeft, stageRight, stageTop + stageHeight, 0), model, stage,
			0, 0, 1, 1, defaultTint.scaleAlpha(0), 0
		);
	}

	void ScorePreviewWindow::drawStageCover(Renderer* renderer)
	{
		int index = ResourceManager::getTexture("stage");
		if (index == -1)
			return;
		const Texture& stage = ResourceManager::textures[index];
		if (!isArrayIndexInBounds(SPR_SEKAI_STAGE, stage.sprites))
			return;
		const Sprite& stageSprite = stage.sprites[SPR_SEKAI_STAGE];
		constexpr float stageWidth = (Engine::STAGE_TEX_WIDTH / Engine::STAGE_LANE_WIDTH) * Engine::STAGE_NUM_LANES;
		const float stageLeft = -stageWidth / 2, stageRight = stageWidth / 2;

		constexpr float stageTop = Engine::STAGE_LANE_TOP / Engine::STAGE_LANE_HEIGHT;
		const float stageHeight = config.pvStageCover * (1 - stageTop);
		const float spriteHeight = config.pvStageCover * (Engine::STAGE_LANE_HEIGHT - Engine::STAGE_LANE_TOP);

		static auto model = DirectX::XMMatrixTranslation(0, 0, 1);
		renderer->drawQuad(
			Engine::quadvPos(stageLeft, stageRight, stageTop + stageHeight, stageTop), model, stage,
			stageSprite.getX1(), stageSprite.getX2(), stageSprite.getY1(), stageSprite.getY1() + spriteHeight,
			Color{0.f, 0.f, 0.f, config.pvStageOpacity}, 0
		);
	}

	void MikuMikuWorld::ScorePreviewWindow::drawStageCoverDecoration(Renderer *renderer)
	{
		constexpr float stageTop = Engine::STAGE_LANE_TOP / Engine::STAGE_LANE_HEIGHT;
		const Texture& noteTex = getNoteTexture();
		size_t sprIndex = SPR_SIMULTANEOUS_CONNECTION;
		size_t transIndex = static_cast<size_t>(SpriteType::SimultaneousLine);
		if (!isArrayIndexInBounds(sprIndex, noteTex.sprites)) return;
		if (!isArrayIndexInBounds(transIndex, ResourceManager::spriteTransforms)) return;
		const SpriteTransform& lineTransform = ResourceManager::spriteTransforms[transIndex];
		const Sprite& sprite = noteTex.sprites[sprIndex];
		float x = 0.12 * (1 - config.pvStageCover);
		auto vPos = lineTransform.apply(Engine::perspectiveQuadvPos(-6 - x, 6 + x, 1. + Engine::getNoteHeight(), 1. - Engine::getNoteHeight()));
		float y = stageTop + config.pvStageCover * (1 - stageTop);
		auto model = DirectX::XMMatrixScaling(y, y, 1.f);
		renderer->drawQuad(vPos, model, noteTex,
			sprite.getX1(), sprite.getX2(), sprite.getY1(), sprite.getY2(),
			defaultTint.scaleAlpha(config.pvStageOpacity), -1
		);
	}

	void ScorePreviewWindow::drawNotes(const ScoreContext& context, Renderer *renderer)
	{
		double current_tm = accumulateDuration(context.currentTick, TICKS_PER_BEAT, context.score.tempoChanges);
		double scaled_tm = accumulateScaledDuration(context.currentTick, TICKS_PER_BEAT, context.score.tempoChanges, context.score.hiSpeedChanges);
		const auto& drawData = context.scorePreviewDrawData;
	
		for (auto& note : drawData.drawingNotes)
		{
			const Note& noteData = context.score.notes.at(note.refID);
                   double upperTime = note.visualTime.max;
                   if (drawData.playtestActive && noteData.getType() == NoteType::Tap)
                   {
                           upperTime += (note.visualTime.max - note.visualTime.min) * 0.12;
                           const auto hitIt = drawData.playtestHitTime.find(note.refID);
                           if (hitIt != drawData.playtestHitTime.end() && current_tm >= hitIt->second)
                                   continue;
                   }
                   if (scaled_tm < note.visualTime.min || scaled_tm > upperTime)
                           continue;
			double y = scaled_tm > note.visualTime.max
                           ? std::pow(1.06, 45.0 * (scaled_tm - note.visualTime.max) / (note.visualTime.max - note.visualTime.min))
                           : Engine::approach(note.visualTime.min, note.visualTime.max, scaled_tm);
                   float l = Engine::laneToLeft(noteData.lane), r = Engine::laneToLeft(noteData.lane) + noteData.width;
			drawNoteBase(renderer, noteData, l, r, y);
			if (noteData.friction)
				drawTraceDiamond(renderer, noteData, l, r, y);
			if (noteData.isFlick()) 
				drawFlickArrow(renderer, noteData, y, current_tm);
		}
	}

	void ScorePreviewWindow::drawLines(const ScoreContext& context, Renderer* renderer)
	{
		if (!config.pvSimultaneousLine)
			return;

		if (noteSkins.getItemIndex(NoteSkinItem::Notes) == -1)
			return;
		double scaled_tm = accumulateScaledDuration(context.currentTick, TICKS_PER_BEAT, context.score.tempoChanges, context.score.hiSpeedChanges);
		const auto& drawData = context.scorePreviewDrawData.drawingLines;

		const Texture& texture = getNoteTexture();
		size_t sprIndex = SPR_SIMULTANEOUS_CONNECTION;
		if (!isArrayIndexInBounds(sprIndex, texture.sprites))
			return;
		const Sprite& sprite = texture.sprites[sprIndex];
		size_t transIndex = static_cast<size_t>(SpriteType::SimultaneousLine);
		if (!isArrayIndexInBounds(transIndex, ResourceManager::spriteTransforms))
			return;
		const SpriteTransform& lineTransform = ResourceManager::spriteTransforms[transIndex];
		const float noteTop = 1. + Engine::getNoteHeight(), noteBottom = 1. - Engine::getNoteHeight();

		for (auto& line : drawData)
		{
			if (scaled_tm < line.visualTime.min || scaled_tm > line.visualTime.max)
				continue;
			float noteLeft = line.xPos.min, noteRight = line.xPos.max;
			if (config.pvMirrorScore)
				std::swap(noteLeft *= -1, noteRight *= -1);
			float y = Engine::approach(line.visualTime.min, line.visualTime.max, scaled_tm);
			auto vPos = lineTransform.apply(Engine::perspectiveQuadvPos(noteLeft, noteRight, noteTop, noteBottom));
			auto model = DirectX::XMMatrixScaling(y, y, 1.f);
			renderer->drawQuad(vPos, model, texture, sprite.getX1(), sprite.getX2(), sprite.getY1(), sprite.getY2(), defaultTint, Engine::getZIndex(SpriteLayer::UNDER_NOTE_EFFECT, 0, y));
		}
	}

	void ScorePreviewWindow::drawHoldTicks(const ScoreContext &context, Renderer *renderer)
	{
		if (noteSkins.getItemIndex(NoteSkinItem::Notes) == -1)
			return;
		double scaled_tm = accumulateScaledDuration(context.currentTick, TICKS_PER_BEAT, context.score.tempoChanges, context.score.hiSpeedChanges);
		const float notesHeight = Engine::getNoteHeight() * 1.3f;
		const float w = notesHeight / scaledAspectRatio;
		const float noteTop = 1. + notesHeight, noteBottom = 1. - notesHeight;
		const Texture& texture = getNoteTexture();

		for (auto& tick : context.scorePreviewDrawData.drawingHoldTicks)
		{
			if (scaled_tm < tick.visualTime.min || scaled_tm > tick.visualTime.max)
				continue;
			int sprIndex = getNoteSpriteIndex(context.score.notes.at(tick.refID));
			if (!isArrayIndexInBounds(sprIndex, texture.sprites))
				continue;
			const Sprite& sprite = texture.sprites[sprIndex];
			size_t transIndex = static_cast<size_t>(SpriteType::HoldTick);
			if (!isArrayIndexInBounds(transIndex, ResourceManager::spriteTransforms))
				return;
			const SpriteTransform& transform = ResourceManager::spriteTransforms[transIndex];
			float y = Engine::approach(tick.visualTime.min, tick.visualTime.max, scaled_tm);
			const float tickCenter = tick.center * (config.pvMirrorScore ? -1 : 1);
			
			auto vPos = transform.apply(Engine::quadvPos(
				tickCenter - w, tickCenter + w,
				noteTop, noteBottom
			));
			auto model = DirectX::XMMatrixScaling(y, y, 1.f);
			int zIndex = Engine::getZIndex(SpriteLayer::DIAMOND, tickCenter, y);
			renderer->drawQuad(vPos, model, texture, sprite.getX1(), sprite.getX2(), sprite.getY1(), sprite.getY2(), defaultTint, zIndex);
		}
	}

	void ScorePreviewWindow::drawHoldCurves(const ScoreContext& context, Renderer* renderer)
	{
		const float total_tm = accumulateDuration(context.scorePreviewDrawData.maxTicks, TICKS_PER_BEAT, context.score.tempoChanges);
		const double current_tm = accumulateDuration(context.currentTick, TICKS_PER_BEAT, context.score.tempoChanges);
		const double current_stm = accumulateScaledDuration(context.currentTick, TICKS_PER_BEAT, context.score.tempoChanges, context.score.hiSpeedChanges);
		const float mirror = config.pvMirrorScore ? -1 : 1;
		const auto& drawData = context.scorePreviewDrawData;

		for (auto& segment : drawData.drawingHoldSegments)
		{
			const double visible_stm = current_stm + segment.visibleDuration;
			if ((std::min(segment.headTime, segment.tailTime) > visible_stm && segment.startTime > current_tm) || current_tm >= segment.endTime)
				continue;

			const Note& holdEnd = context.score.notes.at(segment.endID);
			const Note& holdStart = context.score.notes.at(holdEnd.parentID);
			float holdStartCenter = Engine::getNoteCenter(holdStart) * mirror;
			bool isHoldActivated = current_tm >= segment.activeTime;
			bool isSegmentActivated = current_tm >= segment.startTime;

			int textureID = noteSkins.getItemIndex(segment.isGuide ? NoteSkinItem::TouchLine : NoteSkinItem::LongNote);
			if (textureID == -1)
				continue;
			const Texture& texture = ResourceManager::textures[textureID];
			const int sprIndex = (!holdStart.critical ? 1 : 3);
			if (!isArrayIndexInBounds(sprIndex, texture.sprites))
				continue;
			const Sprite& segmentSprite = texture.sprites[sprIndex];

			double segmentHead_stm = std::min(segment.headTime, segment.tailTime);
			double segmentTail_stm = std::max(segment.headTime, segment.tailTime);
			// Clamp the segment to be within the visible stage
			double segmentStart_stm = std::max(segmentHead_stm, current_stm);
			double segmentEnd_stm = std::min(segmentTail_stm, visible_stm);
			double segmentStartProgress, segmentEndProgress, holdStartProgress, holdEndProgress;

			if (!isSegmentActivated)
			{
				segmentStartProgress = 0;
				segmentEndProgress = unlerpD(segmentHead_stm, segmentTail_stm, segmentEnd_stm);
			}
			else
			{
				segmentStartProgress = unlerpD(segment.startTime, segment.endTime, current_tm);
				segmentEndProgress = lerpD(segmentStartProgress, 1.0, unlerpD(current_stm, segmentTail_stm, segmentEnd_stm));
			}

			const int steps = (segment.ease == EaseType::Linear ? 10 : 15)
				+ static_cast<int>(std::log(std::max((segmentEnd_stm - segmentStart_stm) / segment.visibleDuration, 4.5399e-5)) + 0.5); // Reduce steps if the segment is relatively small
			const auto ease = getEaseFunction(segment.ease);
			float startLeft = segment.headLeft;
			float startRight = segment.headRight;
			float endLeft = segment.tailLeft;
			float endRight = segment.tailRight;

			if (isSegmentActivated && context.score.holdNotes.at(holdStart.ID).startType == HoldNoteType::Normal)
			{
				float l = ease(startLeft, endLeft, segmentStartProgress), r = ease(startRight, endRight, segmentStartProgress);
				drawNoteBase(renderer, holdStart, l, r, 1, segment.activeTime / total_tm);
				if (holdStart.friction)
					drawTraceDiamond(renderer, holdStart, l, r, 1);
			}

			if (config.pvMirrorScore)
			{
				std::swap(startLeft *= -1, startRight *= -1);
				std::swap(endLeft *= -1, endRight *= -1);
			}

			if (segment.isGuide)
			{
				const HoldNote& hold = context.score.holdNotes.at(holdStart.ID);
				// We'll assume guide don't have HoldStepType::Skip
				double totalJoints = 1 + hold.steps.size();
				double headProgress = segment.tailStepIndex / totalJoints;
				double tailProgress = (segment.tailStepIndex + 1) / totalJoints;

				if (!isSegmentActivated)
				{
					holdStartProgress = headProgress;
					holdEndProgress = lerpD(headProgress, tailProgress, unlerpD(segmentHead_stm, segmentTail_stm, segmentEnd_stm));
				}
				else
				{
					holdStartProgress = lerpD(headProgress, tailProgress, unlerp(segment.startTime, segment.endTime, current_tm));
					holdEndProgress = lerpD(holdStartProgress, tailProgress, unlerpD(current_stm, segment.tailTime, segmentEnd_stm));
				}
			}

			double from_percentage = 0;
			double stepStart_stm = segmentStart_stm;
			double stepTop = Engine::approach(stepStart_stm - segment.visibleDuration, stepStart_stm, current_stm);
			double stepStartProgress = segmentStartProgress;

			auto model = DirectX::XMMatrixIdentity();
			float alpha = segment.isGuide ? config.pvGuideAlpha : config.pvHoldAlpha;
			int zIndex = Engine::getZIndex(segment.isGuide ? SpriteLayer::GUIDE_PATH : SpriteLayer::HOLD_PATH, holdStartCenter, segment.activeTime / total_tm);

			for (int i = 0; i < steps; i++)
			{
				double to_percentage = double(i + 1) / steps;
				double stepEnd_stm = lerpD(segmentStart_stm, segmentEnd_stm, to_percentage);
				double stepBottom = Engine::approach(stepEnd_stm - segment.visibleDuration, stepEnd_stm, current_stm);
				double stepEndProgress = lerpD(segmentStartProgress, segmentEndProgress, to_percentage);

				float stepStartLeft = ease(startLeft, endLeft, stepStartProgress);
				float   stepEndLeft = ease(startLeft, endLeft, stepEndProgress);
				float stepStartRight = ease(startRight, endRight, stepStartProgress);
				float   stepEndRight = ease(startRight, endRight, stepEndProgress);

				auto vPos = Engine::perspectiveQuadvPos(stepStartLeft, stepEndLeft, stepStartRight, stepEndRight, stepTop, stepBottom);

				float spr_x1, spr_x2, spr_y1, spr_y2;
				if (segment.isGuide)
				{
					spr_x1 = segmentSprite.getX1() + GUIDE_XCUTOFF;
					spr_x2 = segmentSprite.getX2() - GUIDE_XCUTOFF;
					spr_y1 = lerp(segmentSprite.getY2() - GUIDE_Y_BOTTOM_CUTOFF, segmentSprite.getY1() + GUIDE_Y_TOP_CUTOFF, lerpD(holdStartProgress, holdEndProgress, from_percentage));
					spr_y2 = lerp(segmentSprite.getY2() - GUIDE_Y_BOTTOM_CUTOFF, segmentSprite.getY1() + GUIDE_Y_TOP_CUTOFF, lerpD(holdStartProgress, holdEndProgress, to_percentage));
				}
				else
				{
					spr_x1 = segmentSprite.getX1() + HOLD_XCUTOFF;
					spr_x2 = segmentSprite.getX2() - HOLD_XCUTOFF;
					spr_y1 = segmentSprite.getY1();
					spr_y2 = segmentSprite.getY2();
				}

				if (config.pvHoldAnimation && isHoldActivated && isArrayIndexInBounds(sprIndex - 1, texture.sprites))
				{
					const Sprite& activeSprite = texture.sprites[sprIndex - 1];
					const int norm2ActiveOffset = activeSprite.getY1() - segmentSprite.getY1();
					double delta_tm = current_tm - segment.activeTime;
					float normalAplha = (std::cos(delta_tm * NUM_PI * 2) + 2) / 3.;

					renderer->drawQuad(vPos, model, texture, spr_x1, spr_x2, spr_y1, spr_y2, defaultTint.scaleAlpha(alpha * normalAplha), zIndex);
					renderer->drawQuad(vPos, model, texture, spr_x1, spr_x2, spr_y1 + norm2ActiveOffset, spr_y2 + norm2ActiveOffset, defaultTint.scaleAlpha(alpha * (1.f - normalAplha)), zIndex);
				}
				else
					renderer->drawQuad(vPos, model, texture, spr_x1, spr_x2, spr_y1, spr_y2, defaultTint.scaleAlpha(alpha), zIndex);

				from_percentage = to_percentage;
				stepStart_stm = stepEnd_stm;
				stepTop = stepBottom;
				stepStartProgress = stepEndProgress;
			}
		}
	}

	void ScorePreviewWindow::drawNoteBase(Renderer* renderer, const Note& note, float noteLeft, float noteRight, float y, float zScalar)
	{
		if (noteSkins.getItemIndex(NoteSkinItem::Notes) == -1)
			return;

		const Texture& texture = getNoteTexture();
		const int sprIndex = getNoteSpriteIndex(note);
		if (!isArrayIndexInBounds(sprIndex, texture.sprites))
			return;
		const Sprite& sprite = texture.sprites[sprIndex];

		size_t transIndex = static_cast<size_t>(SpriteType::NoteMiddle);
		if (!isArrayIndexInBounds(transIndex, ResourceManager::spriteTransforms))
			return;
		const SpriteTransform& mTransform = ResourceManager::spriteTransforms[transIndex];

		transIndex = static_cast<size_t>(SpriteType::NoteLeft);
		if (!isArrayIndexInBounds(transIndex, ResourceManager::spriteTransforms))
			return;
		const SpriteTransform& lTransform = ResourceManager::spriteTransforms[transIndex];

		transIndex = static_cast<size_t>(SpriteType::NoteRight);
		if (!isArrayIndexInBounds(transIndex, ResourceManager::spriteTransforms))
			return;
		const SpriteTransform& rTransform = ResourceManager::spriteTransforms[transIndex];

		const float noteHeight = Engine::getNoteHeight();
		const float noteTop = 1. - noteHeight, noteBottom = 1. + noteHeight;
		if (config.pvMirrorScore)
			std::swap(noteLeft *= -1, noteRight *= -1);
		// Inset each edge in lane units so adjacent heads have a small gap.
		// Apply before perspective/lean projection so it scales with distance
		// and export resolution. Limit the inset for narrow hold endpoints.
		const float margin = std::min(std::clamp(config.pvNoteMargin, 0.0f, 0.1f),
			std::max(0.0f, noteRight - noteLeft) * 0.1f);
		noteLeft += margin;
		noteRight -= margin;
		int zIndex = Engine::getZIndex(
			!note.friction ? SpriteLayer::BASE_NOTE : SpriteLayer::TICK_NOTE,
			noteLeft + (noteRight - noteLeft) / 2.f, y * zScalar
		);

		auto model = DirectX::XMMatrixScaling(y, y, 1.f);
		auto drawSlice = [&](std::array<DirectX::XMFLOAT4, 4> vertices, float x1, float x2)
		{
#ifdef MIKUMIKUWORLD_SEKAI_LIVE_RENDERING
			if (config.pvLiveNoteRendering && config.pvLiveNoteLean > 0.0f)
			{
				const float lean = std::clamp(config.pvLiveNoteLean, 0.0f, 1.0f);
				for (auto& vertex : vertices)
				{
					// Tilt the surface toward the camera around its center (y=1).
					// Lane position, approach timing and hit-line center stay fixed.
					// One homography across all slices keeps their edges joined.
					const float depth = vertex.y - 1.0f;
					const float q = 1.0f / std::max(0.25f, 1.0f + depth * 0.8f * lean);
					vertex.x *= q;
					vertex.y = 1.0f + depth * (1.0f + lean * 0.16f) * q;
					vertex.w = q;
				}
				renderer->drawProjectedQuad(vertices, model, texture, x1, x2, sprite.getY1(), sprite.getY2(), defaultTint, zIndex);
				return;
			}
#endif
			renderer->drawQuad(vertices, model, texture, x1, x2, sprite.getY1(), sprite.getY2(), defaultTint, zIndex);
		};
		std::array<DirectX::XMFLOAT4, 4> vPos;
		// Middle
		vPos = mTransform.apply(Engine::perspectiveQuadvPos(noteLeft + 0.25f, noteRight - 0.3f, noteTop, noteBottom));
		drawSlice(vPos, sprite.getX1() + NOTE_SIDE_WIDTH, sprite.getX2() - NOTE_SIDE_WIDTH);

		// Left slice
		vPos = lTransform.apply(Engine::perspectiveQuadvPos(noteLeft, noteLeft + 0.25f, noteTop, noteBottom));
		drawSlice(vPos, sprite.getX1() + NOTE_SIDE_PAD, sprite.getX1() + NOTE_SIDE_WIDTH);
		
		// Right slice
		vPos = rTransform.apply(Engine::perspectiveQuadvPos(noteRight - 0.3, noteRight, noteTop, noteBottom));
		drawSlice(vPos, sprite.getX2() - NOTE_SIDE_WIDTH, sprite.getX2() - NOTE_SIDE_PAD);
	}

	void ScorePreviewWindow::drawTraceDiamond(Renderer *renderer, const Note &note, float noteLeft, float noteRight, float y)
	{
		if (noteSkins.getItemIndex(NoteSkinItem::Notes) == -1)
			return;
			
		const Texture& texture = getNoteTexture();
		int frictionSprIndex = getFrictionSpriteIndex(note);
		if (!isArrayIndexInBounds(frictionSprIndex, texture.sprites))
			return;
		const Sprite& frictionSpr = texture.sprites[frictionSprIndex];
		size_t transIndex = static_cast<size_t>(SpriteType::TraceDiamond);
		if (!isArrayIndexInBounds(transIndex, ResourceManager::spriteTransforms))
			return;
		const SpriteTransform& transform = ResourceManager::spriteTransforms[transIndex];

		const float w = Engine::getNoteHeight() / scaledAspectRatio;
		const float noteTop = 1. + Engine::getNoteHeight(), noteBottom = 1. - Engine::getNoteHeight();
		if (config.pvMirrorScore)
			std::swap(noteLeft *= -1, noteRight *= -1);
		const float noteCenter = noteLeft + (noteRight - noteLeft) / 2;
		int zIndex = Engine::getZIndex(SpriteLayer::DIAMOND, noteCenter, y);

		auto vPos = transform.apply(Engine::quadvPos(
			noteCenter - w,
			noteCenter + w,
			noteTop, noteBottom)
		);
		auto model = DirectX::XMMatrixScaling(y, y, 1.f);
		
		renderer->drawQuad(vPos, model, texture, frictionSpr.getX1(), frictionSpr.getX2(), frictionSpr.getY1(), frictionSpr.getY2(), defaultTint, zIndex);
	}

	void ScorePreviewWindow::drawFlickArrow(Renderer *renderer, const Note &note, float y, double time)
	{
		if (noteSkins.getItemIndex(NoteSkinItem::Notes) == -1)
			return;

		const Texture& texture = getNoteTexture();
		const int sprIndex = getFlickArrowSpriteIndex(note);
		if (!isArrayIndexInBounds(sprIndex, texture.sprites))
			return;
		const Sprite& arrowSprite = texture.sprites[sprIndex];
		size_t flickTransformIdx = std::clamp(note.width, 1, MAX_FLICK_SPRITES) - 1 + static_cast<int>((note.flick == FlickType::Left || note.flick == FlickType::Right) ? SpriteType::FlickArrowLeft : SpriteType::FlickArrowUp);
		if (!isArrayIndexInBounds(flickTransformIdx, ResourceManager::spriteTransforms))
			return;
		const SpriteTransform& transform = ResourceManager::spriteTransforms[flickTransformIdx];

		const int mirror = config.pvMirrorScore ? -1 : 1;
		const int flickDirection = mirror * (note.flick == FlickType::Left ? -1 : (note.flick == FlickType::Right ? 1 : 0));
		const float center = Engine::getNoteCenter(note) * mirror;
		const float w = std::clamp(note.width, 0, MAX_FLICK_SPRITES) * (note.flick == FlickType::Right ? -1 : 1) * mirror / 4.f;
		
		auto vPos = transform.apply(
			Engine::quadvPos(
				center - w,
				center + w,
				1, 1 - 2 * std::abs(w) * scaledAspectRatio
			)
		);
		int zIndex = Engine::getZIndex(SpriteLayer::FLICK_ARROW, center, y);
		
		if (config.pvFlickAnimation)
		{
			double t = std::fmod(time, 0.5) / 0.5;
			const auto cubicEaseIn = [](double t) { return t * t * t; };
			auto animationVector = DirectX::XMVectorScale(DirectX::XMVectorSet(flickDirection, -2 * scaledAspectRatio, 0.f, 0.f), t);
			auto model = DirectX::XMMatrixTranslationFromVector(animationVector) * DirectX::XMMatrixScaling(y, y, 1.f);
			
			renderer->drawQuad(vPos, model, texture,
				arrowSprite.getX1(), arrowSprite.getX2(), arrowSprite.getY1(), arrowSprite.getY2(),
				defaultTint.scaleAlpha(1 - cubicEaseIn(t)), zIndex
			);
		}
		else
		{
			auto model = DirectX::XMMatrixScaling(y, y, 1.f);

			renderer->drawQuad(vPos, model, texture,
				arrowSprite.getX1(), arrowSprite.getX2(), arrowSprite.getY1(), arrowSprite.getY2(),
				defaultTint, zIndex
			);
		}
	}

	void ScorePreviewWindow::updateToolbar(ScoreEditorTimeline &timeline, ScoreContext &context) const
	{
		static float lastHoveredTime = -1;
		constexpr float MAX_NO_HOVER_TIME = 1.5f;
		static float toolBarWidth = UI::btnNormal.x * 2;
		if (!config.pvDrawToolbar)
			return;
		ImGuiIO io = ImGui::GetIO();
		ImGuiStyle& style = ImGui::GetStyle();
		const ImVec2 previewSize = ImGui::GetContentRegionAvail() - ImVec2{ this->getScrollbarWidth(), 0.0f };
		const float previewUiScale = std::clamp(std::min(previewSize.x / 1920.0f, previewSize.y / 1080.0f), 0.65f, 1.0f);
		const ImVec2 toolbarButtonSize = UI::btnNormal * previewUiScale;
		const ImVec2 toolbarWindowPadding = style.WindowPadding * previewUiScale;
		const ImVec2 toolbarFramePadding = style.FramePadding * previewUiScale;
		const ImVec2 toolbarItemSpacing = style.ItemSpacing * previewUiScale;
		const ImVec2 toolbarInnerSpacing = style.ItemInnerSpacing * previewUiScale;
		const float toolbarHudAvoidanceY = config.pvDrawLifeHud ? (HUD_LIFE_Y + HUD_LIFE_HEIGHT + PREVIEW_TOOLBAR_HUD_PADDING) * previewUiScale : 0.0f;
		ImGui::SetNextWindowPos(ImGui::GetWindowPos() + ImVec2{
			ImGui::GetContentRegionAvail().x - toolbarWindowPadding.x * 4 - toolBarWidth,
			toolbarWindowPadding.y * 5 + toolbarHudAvoidanceY
		});
		ImGui::SetNextWindowSizeConstraints({48.0f * previewUiScale, 0}, {120.0f * previewUiScale, FLT_MAX}, NULL);
		float childBgAlpha = std::clamp(easeInCubic(unlerp(MAX_NO_HOVER_TIME, 0, lastHoveredTime)), 0.25f, 1.f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, toolbarWindowPadding);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, toolbarFramePadding);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, toolbarItemSpacing);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, toolbarInnerSpacing);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.f);
		
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetColorU32(ImGuiCol_WindowBg, childBgAlpha));
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0.0f, 0.0f, 0.0f, 0.0f });
		
		ImGui::Begin("###preview_toolbar", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_ChildWindow);
		ImGui::SetWindowFontScale(previewUiScale);
		toolBarWidth = ImGui::GetWindowWidth();
		float centeredXBtn = toolBarWidth / 2 - toolbarButtonSize.x / 2;
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
			lastHoveredTime = 0;
		else
			lastHoveredTime = std::min(io.DeltaTime + lastHoveredTime, MAX_NO_HOVER_TIME);

		ImGui::SetCursorPosX(centeredXBtn);
		if (UI::transparentButton(ICON_FA_ANGLE_DOUBLE_UP, toolbarButtonSize, true, context.currentTick < context.scorePreviewDrawData.maxTicks + TICKS_PER_BEAT))
		{
			if (timeline.isPlaying()) timeline.setPlaying(context, false);
			context.currentTick = timeline.roundTickDown(context.currentTick, timeline.getDivision()) + (TICKS_PER_BEAT / (timeline.getDivision() / 4));
		}

		ImGui::SetCursorPosX(centeredXBtn);
		if (UI::transparentButton(ICON_FA_ANGLE_UP, toolbarButtonSize, true, context.currentTick < context.scorePreviewDrawData.maxTicks + TICKS_PER_BEAT))
		{
			if (timeline.isPlaying()) timeline.setPlaying(context, false);
			context.currentTick++;
		}

		ImGui::SetCursorPosX(centeredXBtn);
		if (UI::transparentButton(ICON_FA_STOP, toolbarButtonSize, false))
			timeline.stop(context);

		ImGui::SetCursorPosX(centeredXBtn);
		if (UI::transparentButton(timeline.isPlaying() ? ICON_FA_PAUSE : ICON_FA_PLAY, toolbarButtonSize))
			timeline.setPlaying(context, !timeline.isPlaying());

		ImGui::SetCursorPosX(centeredXBtn);
		if (UI::transparentButton(ICON_FA_ANGLE_DOWN, toolbarButtonSize, true, context.currentTick > 0))
		{
			if (timeline.isPlaying()) timeline.setPlaying(context, false);
			context.currentTick--;
		}

		ImGui::SetCursorPosX(centeredXBtn);
		if (UI::transparentButton(ICON_FA_ANGLE_DOUBLE_DOWN, toolbarButtonSize, true, context.currentTick > 0))
		{
			if (timeline.isPlaying()) timeline.setPlaying(context, false);
			context.currentTick = std::max(timeline.roundTickDown(context.currentTick, timeline.getDivision()) - (TICKS_PER_BEAT / (timeline.getDivision() / 4)), 0);
		}

		ImGui::SetCursorPosX(centeredXBtn);
		if (UI::transparentButton(isFullWindow() ? ICON_FA_COMPRESS : ICON_FA_EXPAND))
			fullWindow = !isFullWindow();

		ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal);

		ImGui::SetCursorPosX(centeredXBtn);
		if (UI::transparentButton(ICON_FA_MINUS, toolbarButtonSize, false, timeline.getPlaybackSpeed() > 0.25f))
			timeline.setPlaybackSpeed(context, timeline.getPlaybackSpeed() - 0.25f);

		const float playbackStrWidth = ImGui::CalcTextSize("0000%").x;
		ImGui::SetCursorPosX(toolBarWidth / 2 - playbackStrWidth / 2);
		UI::transparentButton(IO::formatString("%.0f%%", timeline.getPlaybackSpeed() * 100).c_str(), ImVec2{playbackStrWidth, toolbarButtonSize.y }, false, false);

		ImGui::SetCursorPosX(centeredXBtn);
		if (UI::transparentButton(ICON_FA_PLUS, toolbarButtonSize, false, timeline.getPlaybackSpeed() < 1.0f))
			timeline.setPlaybackSpeed(context, timeline.getPlaybackSpeed() + 0.25f);

		ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal);

		float currentTm = accumulateDuration(context.currentTick, TICKS_PER_BEAT, context.score.tempoChanges);
		double currentScaledTm = accumulateScaledDuration(context.currentTick, TICKS_PER_BEAT, context.score.tempoChanges, context.score.hiSpeedChanges);
		int currentMeasure = accumulateMeasures(context.currentTick, TICKS_PER_BEAT, context.score.timeSignatures);
		const TimeSignature& ts = context.score.timeSignatures[findTimeSignature(currentMeasure, context.score.timeSignatures)];
		const Tempo& tempo = getTempoAt(context.currentTick, context.score.tempoChanges);
		int hiSpeedIdx = findHighSpeedChange(context.currentTick, context.score.hiSpeedChanges);
		float speed = (hiSpeedIdx == -1 ? 1.0f : context.score.hiSpeedChanges[hiSpeedIdx].speed);
		int seVolumeIdx = findSEVolumeChange(context.currentTick, context.score.seVolumeChanges);
		float seVolume = (seVolumeIdx == -1 ? 1.0f : context.score.seVolumeChanges[seVolumeIdx].volume);

		char rhythmString[256];
		snprintf(rhythmString, sizeof(rhythmString), "%02d:%02d:%02d|%.2fs|%d/%d|%g BPM|%sx|SE %s",
			static_cast<int>(currentTm / 60), static_cast<int>(std::fmod(currentTm, 60.f)), static_cast<int>(std::fmod(currentTm * 100, 100.f)),
			currentScaledTm,
			ts.numerator, ts.denominator,
			tempo.bpm,
			IO::formatFixedFloatTrimmed(speed).c_str(),
			IO::formatFixedFloatTrimmed(seVolume).c_str()
		);
		char* str = strtok(rhythmString, "|");
		ImGui::SetCursorPosX(toolBarWidth / 2 - ImGui::CalcTextSize(str).x / 2);
		ImGui::Text(str);
		for (auto&& col : {feverColor, timeColor, tempoColor, speedColor, seVolumeColor})
		{
			str = strtok(NULL, "|");
			ImGui::SetCursorPosX(toolBarWidth / 2 - ImGui::CalcTextSize(str).x / 2);
			ImGui::TextColored(ImColor(col), str);
		}
		ImGui::SetWindowFontScale(1.0f);
		ImGui::EndChild();
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(5);
	}

	float ScorePreviewWindow::getScrollbarWidth() const
	{
		ImGuiStyle& style = ImGui::GetStyle();
		return style.ScrollbarSize + 4;
	}

    void ScorePreviewWindow::updateScrollbar(ScoreEditorTimeline &timeline, ScoreContext &context) const
	{
		constexpr float scrollpadY = 30.f;
		ImGuiIO& io = ImGui::GetIO();
		ImGuiStyle& style = ImGui::GetStyle();
		ImVec2 contentSize = ImGui::GetWindowContentRegionMax();
		ImVec2 cursorBegPos = ImGui::GetCursorStartPos();
		ImVec2 scrollbarSize = { getScrollbarWidth(), contentSize.y - cursorBegPos.y };
		
		ImGui::SetCursorPos(cursorBegPos + ImVec2{ contentSize.x - scrollbarSize.x - style.WindowPadding.x / 2, 0 });
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_ScrollbarBg));
		ImGui::BeginChild("###scrollbar", scrollbarSize, false, ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
		ImGui::PopStyleColor();

		ImVec2 scrollContentSize = ImGui::GetContentRegionAvail();
		ImVec2 scrollMaxSize = ImGui::GetWindowContentRegionMax();
		int maxTicks = std::max(context.scorePreviewDrawData.maxTicks, 1);
		float scrollRatio = std::min(Engine::getNoteDuration(config.pvNoteSpeed) / accumulateDuration(context.scorePreviewDrawData.maxTicks, TICKS_PER_BEAT, context.score.tempoChanges), 1.f);
		float progress = 1.f - std::min(float(context.currentTick) / maxTicks, 1.f);
		float handleHeight = std::max(20.f, scrollContentSize.y * scrollRatio);
		
		bool scrollbarActive = false;
		ImGui::BeginDisabled(timeline.isPlaying());
		ImGui::SetCursorPos(ImGui::GetCursorStartPos());
		ImGui::InvisibleButton("##scroll_bg", contentSize, ImGuiButtonFlags_NoNavFocus);
		scrollbarActive |= ImGui::IsItemActive();

		ImVec2 handleSize = {style.ScrollbarSize, handleHeight};
		ImVec2 handlePos = {scrollMaxSize.x / 2 - handleSize.x / 2, lerp(0, scrollMaxSize.y - handleHeight, progress)};
		ImVec2 absHandlePos = ImGui::GetWindowPos() + handlePos;

		ImGui::SetCursorPos(handlePos);
		ImGui::InvisibleButton("##scroll_handle", handleSize);
		scrollbarActive |= ImGui::IsItemActive();

		ImGuiCol_ handleColBase = scrollbarActive ? ImGuiCol_ScrollbarGrabActive : ImGui::IsItemHovered() ? ImGuiCol_ScrollbarGrabHovered : ImGuiCol_ScrollbarGrab;
		
		ImGui::RenderFrame(absHandlePos, absHandlePos + ImGui::GetItemRectSize(), ImGui::GetColorU32(handleColBase), true, 3.f);
		ImGui::EndDisabled();

		if (scrollbarActive)
		{
			float absScrollStart = ImGui::GetWindowPos().y + handleSize.y / 2;
			float absScrollEnd = ImGui::GetWindowPos().y + scrollMaxSize.y - handleSize.y / 2;
			float mouseProgress = 1.f - std::clamp(unlerp(absScrollStart, absScrollEnd, io.MousePos.y), 0.f, 1.f);
			context.currentTick = std::round(lerp(0, maxTicks, mouseProgress));	
		}
		
		ImGui::EndChild();
	}
}
