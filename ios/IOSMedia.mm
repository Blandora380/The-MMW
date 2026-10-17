#include "IOSPlatform.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <UIKit/UIKit.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <cmath>

namespace MikuMikuWorld {
namespace {
    NSURL* fileURL(const std::string& path) {
        return [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    }
    std::string mediaError(NSError* error, const char* fallback) {
        return error ? std::string(error.localizedDescription.UTF8String) : std::string(fallback);
    }
    struct Encoder {
        AVAssetWriter* writer;
        AVAssetWriterInput* input;
        AVAssetWriterInputPixelBufferAdaptor* adaptor;
        NSURL* temporary;
        NSURL* destination;
        std::string music, effects;
        float audioStart, audioDelay, duration;
        int width, height, fps;
        int64_t frame = 0;
        ~Encoder() { [NSFileManager.defaultManager removeItemAtURL:temporary error:nil]; }
    };
    bool waitForCompletion(const std::shared_ptr<std::atomic<bool>>& finished, double seconds) {
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
        while (!finished->load() && deadline.timeIntervalSinceNow > 0) IOSPumpEvents();
        return finished->load();
    }
    // iOS suspends OpenGL while inactive. Never issue another GL call until the
    // app returns; keep the run loop alive for native lifecycle and Files UI.
    void waitForForeground() {
        while (UIApplication.sharedApplication.applicationState != UIApplicationStateActive) IOSPumpEvents();
    }
}

void* IOSVideoStart(const std::string& output, int width, int height, int fps,
    const std::string& music, float audioStart, float audioDelay,
    const std::string& effects, float duration, std::string& error) {
    @autoreleasepool {
        if (width < 2 || height < 2 || width % 2 || height % 2 || fps < 1 || fps > 120 || duration <= 0) {
            error = "Use even video dimensions, a positive duration, and a frame rate between 1 and 120.";
            return nullptr;
        }
        auto encoder = std::make_unique<Encoder>();
        encoder->destination = fileURL(output);
        encoder->temporary = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:
            [NSUUID.UUID.UUIDString stringByAppendingPathExtension:@"mp4"]]];
        encoder->width = width; encoder->height = height; encoder->fps = fps;
        encoder->music = music; encoder->effects = effects;
        encoder->audioStart = std::max(0.0f, audioStart);
        encoder->audioDelay = std::max(0.0f, audioDelay);
        encoder->duration = duration;
        NSError* nativeError = nil;
        encoder->writer = [[AVAssetWriter alloc] initWithURL:encoder->temporary fileType:AVFileTypeMPEG4 error:&nativeError];
        if (!encoder->writer) { error = mediaError(nativeError, "Could not create video writer."); return nullptr; }
        NSDictionary* settings = @{
            AVVideoCodecKey: AVVideoCodecTypeH264,
            AVVideoWidthKey: @(width), AVVideoHeightKey: @(height),
            AVVideoCompressionPropertiesKey: @{
                AVVideoAverageBitRateKey: @(std::clamp<int64_t>((int64_t)width * height * fps / 7, 2000000, 50000000)),
                AVVideoExpectedSourceFrameRateKey: @(fps),
                AVVideoMaxKeyFrameIntervalKey: @(fps * 2),
                AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel
            }
        };
        encoder->input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:settings];
        encoder->input.expectsMediaDataInRealTime = NO;
        if (![encoder->writer canAddInput:encoder->input]) { error = "This device cannot encode the selected resolution."; return nullptr; }
        [encoder->writer addInput:encoder->input];
        encoder->adaptor = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:encoder->input
            sourcePixelBufferAttributes:@{ (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                (id)kCVPixelBufferWidthKey: @(width), (id)kCVPixelBufferHeightKey: @(height),
                (id)kCVPixelBufferIOSurfacePropertiesKey: @{} }];
        if (![encoder->writer startWriting]) { error = mediaError(encoder->writer.error, "Could not start video writer."); return nullptr; }
        [encoder->writer startSessionAtSourceTime:kCMTimeZero];
        return encoder.release();
    }
}

bool IOSVideoWrite(void* handle, const std::vector<uint8_t>& rgba) {
    if (!handle) return false;
    Encoder* encoder = static_cast<Encoder*>(handle);
    if (rgba.size() != (size_t)encoder->width * encoder->height * 4) return false;
    @autoreleasepool {
        waitForForeground();
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:30];
        while (!encoder->input.readyForMoreMediaData) {
            if (encoder->writer.status != AVAssetWriterStatusWriting || deadline.timeIntervalSinceNow <= 0) return false;
            IOSPumpEvents();
            waitForForeground();
        }
        CVPixelBufferRef buffer = nullptr;
        if (!encoder->adaptor.pixelBufferPool || CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,
            encoder->adaptor.pixelBufferPool, &buffer) != kCVReturnSuccess) return false;
        if (CVPixelBufferLockBaseAddress(buffer, 0) != kCVReturnSuccess) { CVPixelBufferRelease(buffer); return false; }
        uint8_t* destination = (uint8_t*)CVPixelBufferGetBaseAddress(buffer);
        size_t stride = CVPixelBufferGetBytesPerRow(buffer);
        for (int y = 0; y < encoder->height; ++y) {
            const uint8_t* source = rgba.data() + (size_t)(encoder->height - y - 1) * encoder->width * 4;
            uint8_t* target = destination + (size_t)y * stride;
            for (int x = 0; x < encoder->width; ++x) {
                target[x * 4] = source[x * 4 + 2];
                target[x * 4 + 1] = source[x * 4 + 1];
                target[x * 4 + 2] = source[x * 4];
                target[x * 4 + 3] = 255;
            }
        }
        CVPixelBufferUnlockBaseAddress(buffer, 0);
        bool success = [encoder->adaptor appendPixelBuffer:buffer withPresentationTime:CMTimeMake(encoder->frame, encoder->fps)];
        CVPixelBufferRelease(buffer);
        if (success) ++encoder->frame;
        IOSRestoreGraphics();
        return success;
    }
}

bool IOSVideoFinish(void* handle, std::string& error) {
    if (!handle) { error = "The video writer is not running."; return false; }
    std::unique_ptr<Encoder> encoder(static_cast<Encoder*>(handle));
    @autoreleasepool {
        [encoder->input markAsFinished];
        [encoder->writer endSessionAtSourceTime:CMTimeMake(encoder->frame, encoder->fps)];
        auto completed = std::make_shared<std::atomic<bool>>(false);
        [encoder->writer finishWritingWithCompletionHandler:^{ completed->store(true); }];
        if (!waitForCompletion(completed, 180) || encoder->writer.status != AVAssetWriterStatusCompleted) {
            error = mediaError(encoder->writer.error, "Video encoding did not complete in time.");
            [encoder->writer cancelWriting];
            return false;
        }
        NSFileManager* manager = NSFileManager.defaultManager;
        if (encoder->music.empty() && encoder->effects.empty()) {
            NSError* nativeError = nil;
            [manager removeItemAtURL:encoder->destination error:nil];
            if (![manager moveItemAtURL:encoder->temporary toURL:encoder->destination error:&nativeError]) {
                error = mediaError(nativeError, "Could not save the video."); return false;
            }
            return true;
        }
        AVMutableComposition* composition = [AVMutableComposition composition];
        AVURLAsset* video = [AVURLAsset URLAssetWithURL:encoder->temporary options:nil];
        AVAssetTrack* sourceVideo = [video tracksWithMediaType:AVMediaTypeVideo].firstObject;
        if (!sourceVideo) { error = "The encoded file has no video track."; return false; }
        AVMutableCompositionTrack* videoTrack = [composition addMutableTrackWithMediaType:AVMediaTypeVideo preferredTrackID:kCMPersistentTrackID_Invalid];
        CMTime length = CMTimeMake(encoder->frame, encoder->fps);
        NSError* nativeError = nil;
        if (![videoTrack insertTimeRange:CMTimeRangeMake(kCMTimeZero, length) ofTrack:sourceVideo atTime:kCMTimeZero error:&nativeError]) {
            error = mediaError(nativeError, "Could not add the video track."); return false;
        }
        // Independent composition tracks are mixed together by AVFoundation.
        auto addAudio = [&](const std::string& filename, double start, double delay) -> bool {
            if (filename.empty()) return true;
            AVURLAsset* asset = [AVURLAsset URLAssetWithURL:fileURL(filename) options:nil];
            AVAssetTrack* source = [asset tracksWithMediaType:AVMediaTypeAudio].firstObject;
            if (!source) { error = "Could not decode an audio track selected for export."; return false; }
            double available = CMTimeGetSeconds(asset.duration) - start;
            double remaining = CMTimeGetSeconds(length) - delay;
            double duration = std::min(available, remaining);
            if (!std::isfinite(duration) || duration <= 0) return true;
            AVMutableCompositionTrack* track = [composition addMutableTrackWithMediaType:AVMediaTypeAudio preferredTrackID:kCMPersistentTrackID_Invalid];
            NSError* audioError = nil;
            BOOL added = [track insertTimeRange:CMTimeRangeMake(CMTimeMakeWithSeconds(start, 60000), CMTimeMakeWithSeconds(duration, 60000))
                ofTrack:source atTime:CMTimeMakeWithSeconds(delay, 60000) error:&audioError];
            if (!added) error = mediaError(audioError, "Could not add audio to the MP4.");
            return added;
        };
        if (!addAudio(encoder->music, encoder->audioStart, encoder->audioDelay) || !addAudio(encoder->effects, 0, 0)) return false;
        AVAssetExportSession* exportSession = [[AVAssetExportSession alloc] initWithAsset:composition presetName:AVAssetExportPresetHighestQuality];
        if (!exportSession) { error = "Could not prepare the final MP4."; return false; }
        // Write to another temporary file so a failed final mix never removes an
        // existing user export. Replace the destination only after success.
        NSURL* mixedURL = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:
            [NSUUID.UUID.UUIDString stringByAppendingPathExtension:@"mp4"]]];
        exportSession.outputURL = mixedURL;
        exportSession.outputFileType = AVFileTypeMPEG4;
        exportSession.shouldOptimizeForNetworkUse = YES;
        exportSession.timeRange = CMTimeRangeMake(kCMTimeZero, length);
        completed = std::make_shared<std::atomic<bool>>(false);
        auto mixed = completed;
        [exportSession exportAsynchronouslyWithCompletionHandler:^{ mixed->store(true); }];
        if (!waitForCompletion(completed, 900) || exportSession.status != AVAssetExportSessionStatusCompleted) {
            error = mediaError(exportSession.error, "Could not finish the MP4 audio mix.");
            [exportSession cancelExport];
            [manager removeItemAtURL:mixedURL error:nil];
            return false;
        }
        [manager removeItemAtURL:encoder->destination error:nil];
        if (![manager moveItemAtURL:mixedURL toURL:encoder->destination error:&nativeError]) {
            error = mediaError(nativeError, "Could not save the finished MP4.");
            [manager removeItemAtURL:mixedURL error:nil];
            return false;
        }
        IOSRestoreGraphics();
        return true;
    }
}

void IOSVideoCancel(void* handle) {
    if (!handle) return;
    std::unique_ptr<Encoder> encoder(static_cast<Encoder*>(handle));
    [encoder->writer cancelWriting];
}

namespace {
    struct Reader {
        AVAssetImageGenerator* generator;
        double duration = 0;
        int64_t lastFrame = -1;
    };
}
void* IOSVideoReaderCreate(const std::string& filename) {
    @autoreleasepool {
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:fileURL(filename) options:nil];
        if (![asset tracksWithMediaType:AVMediaTypeVideo].count) return nullptr;
        auto reader = std::make_unique<Reader>();
        reader->duration = CMTimeGetSeconds(asset.duration);
        reader->generator = [[AVAssetImageGenerator alloc] initWithAsset:asset];
        reader->generator.appliesPreferredTrackTransform = YES;
        reader->generator.maximumSize = CGSizeMake(1920, 1080);
        reader->generator.requestedTimeToleranceBefore = kCMTimeZero;
        reader->generator.requestedTimeToleranceAfter = kCMTimeZero;
        return reader.release();
    }
}
bool IOSVideoReaderFrame(void* handle, double time, std::vector<uint8_t>& rgba) {
    if (!handle) return false;
    Reader* reader = static_cast<Reader*>(handle);
    time = std::max(0.0, std::min(time, std::max(0.0, reader->duration - 1.0 / 60)));
    int64_t frame = (int64_t)std::floor(time * 60);
    if (frame == reader->lastFrame) return false;
    @autoreleasepool {
        CGImageRef image = [reader->generator copyCGImageAtTime:CMTimeMakeWithSeconds(time, 60000) actualTime:nil error:nil];
        if (!image) return false;
        rgba.resize(1920ull * 1080 * 4);
        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        CGContextRef context = CGBitmapContextCreate(rgba.data(), 1920, 1080, 8, 1920 * 4, colorSpace,
            kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(colorSpace);
        if (!context) { CGImageRelease(image); return false; }
        double scale = std::max(1920.0 / CGImageGetWidth(image), 1080.0 / CGImageGetHeight(image));
        double width = CGImageGetWidth(image) * scale, height = CGImageGetHeight(image) * scale;
        CGContextTranslateCTM(context, 0, 1080);
        CGContextScaleCTM(context, 1, -1);
        CGContextDrawImage(context, CGRectMake((1920 - width) / 2, (1080 - height) / 2, width, height), image);
        CGContextRelease(context);
        CGImageRelease(image);
        reader->lastFrame = frame;
        return true;
    }
}
void IOSVideoReaderDestroy(void* handle) { delete static_cast<Reader*>(handle); }
}
