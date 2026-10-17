#pragma once
#include <cstdint>
#include <string>
#include <vector>

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
