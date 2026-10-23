#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace MikuMikuWorld
{
	class FfmpegEncoder
	{
	private:
		void* processHandle{};
		void* inputHandle{};
		int androidPipeHandle{ -1 };
		int64_t androidSessionId{};
		std::string androidPipePath;
		std::string androidOutputPath;
	std::vector<std::string> androidArguments;

                bool writeFrameDirect(const std::vector<uint8_t>& rgbaFrame);
                void writerLoop();
                void stopWriter(bool discard);
                std::thread writerThread;
                std::mutex writerMutex;
                std::condition_variable writerCv;
                std::deque<std::vector<uint8_t>> writerQueue;
                std::vector<std::vector<uint8_t>> writerPool;
                std::atomic<bool> writerFailed{ false };
                bool writerStarted{ false };
                bool writerStop{ false };

	public:
		FfmpegEncoder() = default;
		FfmpegEncoder(const FfmpegEncoder&) = delete;
		FfmpegEncoder& operator=(const FfmpegEncoder&) = delete;
		~FfmpegEncoder();

		static bool isAvailable();
		bool start(const std::string& outputFilename, int width, int height, int fps,
			const std::string& audioFilename, float audioStart, float audioDelay,
			const std::string& soundEffectsFilename, float duration, std::string& error);
		bool writeFrame(const std::vector<uint8_t>& rgbaFrame);
		bool finish(std::string& error);
		void cancel();
	};
}
