// UIKit host for the shared editor. All UI and OpenGL work stays on the main thread.
#include "../MikuMikuWorld/Application.h"
#include "../MikuMikuWorld/Utilities.h"
#include "../MikuMikuWorld/AndroidImGuiBackend.h"
#include "../MikuMikuWorld/AndroidFileDialog.h"
#include "IOSPlatform.h"
#import <UIKit/UIKit.h>
#import <QuartzCore/CAEAGLLayer.h>
#import <QuartzCore/QuartzCore.h>
#import <OpenGLES/EAGL.h>
#import <AVFoundation/AVFoundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <dlfcn.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <vector>
#include <unordered_map>

using namespace MikuMikuWorld;
namespace {
    Application editor;
    bool ready = false, drawing = false, exportActive = false, exitRequested = false;
    int modalDepth = 0, screenWidth = 1, screenHeight = 1;
    int viewportWidth = 1, viewportHeight = 1, viewportLeft = 0, viewportTop = 0;
    float density = 1.0f, pinchDelta = 0.0f;
    GLuint screenFbo = 0, editorFbo = 0, colorRenderbuffer = 0, depthRenderbuffer = 0, editorTexture = 0;
    EAGLContext* graphicsContext;
    UIViewController* rootController;
    UITextField* inputField;
    CFTimeInterval lastFrame = 0;
    std::set<std::string> pendingExports;

    struct TouchRecord { int type; int touchId; float x; float y; double time; };
    std::vector<TouchRecord> touchQueue;
    std::unordered_map<const void*, int> touchIds;
    int nextTouchId = 1;

    NSString* ns(const std::string& text) { return [NSString stringWithUTF8String:text.c_str()] ?: @""; }
    NSString* documents() {
        return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    }
    void waitUntil(bool (^done)()) {
        while (!done()) @autoreleasepool { IOSPumpEvents(); }
        IOSRestoreGraphics();
    }
    // Keep the editor frame suspended while a native modal runs. A second display
    // link callback must never enter ImGui while the first frame is still active.
    struct NativeModal {
        NativeModal() {
            ++modalDepth;
            if (ImGui::GetCurrentContext()) {
                ImGui::GetIO().AddMouseButtonEvent(0, false);
                ImGui::GetIO().AddMouseButtonEvent(1, false);
            }
            [inputField resignFirstResponder];
        }
        ~NativeModal() { --modalDepth; IOSRestoreGraphics(); }
    };
    void releaseBuffers() {
        if (screenFbo) glDeleteFramebuffers(1, &screenFbo);
        if (editorFbo) glDeleteFramebuffers(1, &editorFbo);
        if (colorRenderbuffer) glDeleteRenderbuffers(1, &colorRenderbuffer);
        if (depthRenderbuffer) glDeleteRenderbuffers(1, &depthRenderbuffer);
        if (editorTexture) glDeleteTextures(1, &editorTexture);
        screenFbo = editorFbo = colorRenderbuffer = depthRenderbuffer = editorTexture = 0;
    }
}

@interface MMWPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) BOOL done;
@property(nonatomic, strong) NSURL* selectedURL;
@end
@implementation MMWPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    self.selectedURL = urls.firstObject;
    [controller dismissViewControllerAnimated:YES completion:^{ self.done = YES; }];
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
    [controller dismissViewControllerAnimated:YES completion:^{ self.done = YES; }];
}
@end

@interface MMWTextField : UITextField
@end
@implementation MMWTextField
- (void)deleteBackward {
    if (ImGui::GetCurrentContext()) {
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, true);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, false);
    }
    [super deleteBackward];
}
@end

@interface MMWEditorView : UIView <UITextFieldDelegate, UIGestureRecognizerDelegate>
@property(nonatomic, strong) CADisplayLink* displayLink;
- (void)drawFrame;
- (void)pauseEditor;
- (void)resumeEditor;
@end

@implementation MMWEditorView
+ (Class)layerClass { return [CAEAGLLayer class]; }
- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    self.multipleTouchEnabled = YES;
    self.contentScaleFactor = UIScreen.mainScreen.scale;
    CAEAGLLayer* layer = (CAEAGLLayer*)self.layer;
    layer.opaque = YES;
    layer.drawableProperties = @{ kEAGLDrawablePropertyRetainedBacking: @NO,
                                  kEAGLDrawablePropertyColorFormat: kEAGLColorFormatRGBA8 };
    graphicsContext = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
    if (!graphicsContext || ![EAGLContext setCurrentContext:graphicsContext] || !gladLoadGLLoader(IOSGetGLProc))
        return nil;

    inputField = [[MMWTextField alloc] initWithFrame:CGRectMake(1, 1, 1, 1)];
    inputField.alpha = 0.01;
    inputField.delegate = self;
    inputField.autocorrectionType = UITextAutocorrectionTypeNo;
    inputField.autocapitalizationType = UITextAutocapitalizationTypeNone;
    inputField.smartQuotesType = UITextSmartQuotesTypeNo;
    inputField.smartDashesType = UITextSmartDashesTypeNo;
    [inputField addTarget:self action:@selector(textChanged:) forControlEvents:UIControlEventEditingChanged];
    [self addSubview:inputField];
    UILongPressGestureRecognizer* hold = [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(held:)];
    hold.minimumPressDuration = 0.6;
    hold.allowableMovement = 10;
    hold.delegate = self;
    [self addGestureRecognizer:hold];
    UIPinchGestureRecognizer* pinch = [[UIPinchGestureRecognizer alloc] initWithTarget:self action:@selector(pinched:)];
    pinch.delegate = self;
    [self addGestureRecognizer:pinch];
    self.displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(drawFrame)];
    self.displayLink.preferredFramesPerSecond = 60;
    [self.displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    return self;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    if (drawing || modalDepth || UIApplication.sharedApplication.applicationState != UIApplicationStateActive) return;
    [EAGLContext setCurrentContext:graphicsContext];
    releaseBuffers();
    glGenFramebuffers(1, &screenFbo);
    glGenRenderbuffers(1, &colorRenderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, colorRenderbuffer);
    [graphicsContext renderbufferStorage:GL_RENDERBUFFER fromDrawable:(CAEAGLLayer*)self.layer];
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &screenWidth);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &screenHeight);
    glBindFramebuffer(GL_FRAMEBUFFER, screenFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorRenderbuffer);
    // Landscape editor fits the safe area at 16:9, independently of Retina scale.
    UIEdgeInsets safe = self.safeAreaInsets;
    int usableWidth = std::max(1, screenWidth - (int)((safe.left + safe.right) * self.contentScaleFactor));
    int usableHeight = std::max(1, screenHeight - (int)((safe.top + safe.bottom) * self.contentScaleFactor));
    AndroidImGui_SetDisplay(usableWidth, usableHeight, self.contentScaleFactor);
    viewportLeft += (int)(safe.left * self.contentScaleFactor);
    viewportTop += (int)(safe.top * self.contentScaleFactor);
    glGenFramebuffers(1, &editorFbo);
    glGenTextures(1, &editorTexture);
    glBindTexture(GL_TEXTURE_2D, editorTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, viewportWidth, viewportHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, editorFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, editorTexture, 0);
    glGenRenderbuffers(1, &depthRenderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRenderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, viewportWidth, viewportHeight);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthRenderbuffer);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        NSLog(@"Editor framebuffer is incomplete");
        return;
    }
    IOSRestoreGraphics();
}
- (void)drawFrame {
    if (drawing || modalDepth || exitRequested || !editorFbo ||
        UIApplication.sharedApplication.applicationState != UIApplicationStateActive) return;
    drawing = true;
    @autoreleasepool {
        IOSRestoreGraphics();
        try {
            if (!ready) {
                Result result = editor.initialize(std::string(NSBundle.mainBundle.resourcePath.UTF8String) + "/");
                if (!result.isOk()) throw std::runtime_error(result.getMessage());
                ready = true;
                ImGui::GetPlatformIO().Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
                    static std::string text;
                    text = UIPasteboard.generalPasteboard.string.UTF8String ?: "";
                    return text.c_str();
                };
                ImGui::GetPlatformIO().Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
                    UIPasteboard.generalPasteboard.string = [NSString stringWithUTF8String:text ?: ""];
                };
            }
            editor.update();
            {
                const bool playtestInput = gPlaytestTouchActive;
                gPlaytestTouchActive = false;
                for (UIGestureRecognizer* recognizer in self.gestureRecognizers) {
                    if (recognizer.enabled == playtestInput) recognizer.enabled = !playtestInput;
                }
            }
            AndroidFileDialog_UpdateTextInput(ImGui::GetIO().WantTextInput);
            glDisable(GL_SCISSOR_TEST);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, screenFbo);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, editorFbo);
            int bottom = screenHeight - viewportTop - viewportHeight;
            glBlitFramebuffer(0, 0, viewportWidth, viewportHeight, viewportLeft, bottom,
                viewportLeft + viewportWidth, bottom + viewportHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindRenderbuffer(GL_RENDERBUFFER, colorRenderbuffer);
            [graphicsContext presentRenderbuffer:GL_RENDERBUFFER];
        } catch (const std::exception& error) {
            exitRequested = true;
            IOSMessageBox("Editor error", error.what(), IO::MessageBoxButtons::Ok);
        }
    }
    drawing = false;
}
- (void)pauseEditor {
    self.displayLink.paused = YES;
    if (ready) editor.writeSettings();
    if (ImGui::GetCurrentContext()) ImGui::GetIO().ClearInputKeys();
    glFinish();
}
- (void)resumeEditor {
    lastFrame = 0;
    self.displayLink.paused = NO;
    [self setNeedsLayout];
}
- (void)mouseAt:(CGPoint)point {
    if (!ready || modalDepth) return;
    ImGui::GetIO().AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    ImGui::GetIO().AddMousePosEvent((point.x * self.contentScaleFactor - viewportLeft) / density,
                                  (point.y * self.contentScaleFactor - viewportTop) / density);
}
- (void)recordTouches:(NSSet<UITouch*>*)touches type:(int)type {
    for (UITouch* touch in touches) {
        const void* key = (__bridge const void*)touch;
        int touchId = 0;
        auto found = touchIds.find(key);
        if (found == touchIds.end()) {
            if (type != 0) continue;
            touchId = nextTouchId++;
            touchIds[key] = touchId;
        } else {
            touchId = found->second;
        }
        const CGPoint point = [touch locationInView:self];
        const float x = (point.x * self.contentScaleFactor - viewportLeft) / density;
        const float y = (point.y * self.contentScaleFactor - viewportTop) / density;
        bool merged = false;
        if (type == 1) {
            for (auto it = touchQueue.rbegin(); it != touchQueue.rend(); ++it) {
                if (it->touchId != touchId) continue;
                if (it->type == 1) {
                    it->x = x;
                    it->y = y;
                    it->time = touch.timestamp;
                    merged = true;
                }
                break;
            }
        }
        if (!merged) {
            if (touchQueue.size() >= 512) touchQueue.erase(touchQueue.begin(), touchQueue.begin() + 256);
            touchQueue.push_back({ type, touchId, x, y, touch.timestamp });
        }
        if (type == 2) touchIds.erase(key);
    }
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!ready || modalDepth) return;
    [self recordTouches:touches type:0];
    if (event.allTouches.count > 1) { ImGui::GetIO().AddMouseButtonEvent(0, false); return; }
    [self mouseAt:[touches.anyObject locationInView:self]];
    ImGui::GetIO().AddMouseButtonEvent(0, true);
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (ready && !modalDepth) [self recordTouches:touches type:1];
    if (event.allTouches.count == 1) [self mouseAt:[touches.anyObject locationInView:self]];
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!ready) return;
    [self recordTouches:touches type:2];
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    ImGui::GetIO().AddMouseButtonEvent(1, false);
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { [self touchesEnded:touches withEvent:event]; }
- (void)held:(UILongPressGestureRecognizer*)gesture {
    if (!ready || modalDepth) return;
    if (gesture.state == UIGestureRecognizerStateBegan) {
        [self mouseAt:[gesture locationInView:self]];
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        ImGui::GetIO().AddMouseButtonEvent(1, true);
    } else if (gesture.state == UIGestureRecognizerStateEnded || gesture.state == UIGestureRecognizerStateCancelled) {
        ImGui::GetIO().AddMouseButtonEvent(1, false);
    }
}
- (void)pinched:(UIPinchGestureRecognizer*)gesture {
    if (!ready || modalDepth) return;
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    [self mouseAt:[gesture locationInView:self]];
    if (gesture.state == UIGestureRecognizerStateChanged) {
        pinchDelta += std::log2(std::max(0.01, (double)gesture.scale));
        gesture.scale = 1.0;
    }
}
- (void)textChanged:(UITextField*)field {
    if (!field.markedTextRange && field.text.length) {
        AndroidImGui_QueueText(field.text.UTF8String);
        field.text = @"";
    }
}
- (BOOL)textFieldShouldReturn:(UITextField*)field {
    AndroidImGui_QueueKey(ImGuiKey_Enter, true);
    AndroidImGui_QueueKey(ImGuiKey_Enter, false);
    [field resignFirstResponder];
    return NO;
}
@end

@interface MMWViewController : UIViewController
@end
@implementation MMWViewController
- (void)loadView { self.view = [[MMWEditorView alloc] initWithFrame:UIScreen.mainScreen.bounds]; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
@end

@interface MMWAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@end
@implementation MMWAppDelegate
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)options {
    [[AVAudioSession sharedInstance] setCategory:AVAudioSessionCategoryPlayback error:nil];
    [[AVAudioSession sharedInstance] setActive:YES error:nil];
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    rootController = [[MMWViewController alloc] init];
    self.window.rootViewController = rootController;
    [self.window makeKeyAndVisible];
    return YES;
}
- (void)applicationWillResignActive:(UIApplication*)application { [(MMWEditorView*)rootController.view pauseEditor]; }
- (void)applicationDidBecomeActive:(UIApplication*)application { [(MMWEditorView*)rootController.view resumeEditor]; }
- (void)applicationWillTerminate:(UIApplication*)application { if (ready) editor.writeSettings(); }
@end

namespace MikuMikuWorld {
    void* IOSGetGLProc(const char* name) { return dlsym(RTLD_DEFAULT, name); }
    unsigned int IOSDefaultFramebuffer() { return editorFbo; }
    void IOSRestoreGraphics() {
        [EAGLContext setCurrentContext:graphicsContext];
        glBindFramebuffer(GL_FRAMEBUFFER, editorFbo);
        glViewport(0, 0, viewportWidth, viewportHeight);
    }
    void IOSPumpEvents() {
        do {
            [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
        } while (UIApplication.sharedApplication.applicationState != UIApplicationStateActive);
    }
    void AndroidImGui_SetDisplay(int width, int height, float scale) {
        density = std::clamp(scale, 1.0f, 1.5f);
        viewportWidth = std::min(width, (int)(height * (16.0 / 9.0)));
        viewportHeight = std::min(height, (int)(width * (9.0 / 16.0)));
        viewportLeft = (width - viewportWidth) / 2;
        viewportTop = (height - viewportHeight) / 2;
    }
    int AndroidImGui_GetViewportLeft() { return 0; }
    int AndroidImGui_GetViewportTop() { return 0; }
    int AndroidImGui_GetViewportWidth() { return viewportWidth; }
    int AndroidImGui_GetViewportHeight() { return viewportHeight; }
    float AndroidImGui_GetDensity() { return density; }
    float AndroidImGui_ConsumePinchZoom() { float result = pinchDelta; pinchDelta = 0; return result; }
    int AndroidImGui_ConsumeTouchEvents(AndroidTouchEvent* out, int maxCount) {
        const double now = CACurrentMediaTime();
        int count = 0;
        for (const auto& e : touchQueue) {
            if (count >= maxCount) break;
            out[count++] = { e.type, e.touchId, e.x, e.y, std::max(0.0, now - e.time) };
        }
        touchQueue.erase(touchQueue.begin(), touchQueue.begin() + count);
        return count;
    }
    void AndroidRequestExit() { exitRequested = true; }
    bool AndroidExitRequested() { return exitRequested; }
    void AndroidImGui_QueueText(const char* text) { if (ready && text) ImGui::GetIO().AddInputCharactersUTF8(text); }
    void AndroidImGui_QueueKey(int key, bool down) { if (ready) ImGui::GetIO().AddKeyEvent((ImGuiKey)key, down); }
    void AndroidImGui_BeginFrame() {
        ImGuiIO& io = ImGui::GetIO();
        CFTimeInterval now = CACurrentMediaTime();
        io.DeltaTime = lastFrame ? (float)std::clamp(now - lastFrame, 0.001, 0.1) : 1.0f / 60.0f;
        lastFrame = now;
        io.DisplaySize = ImVec2(viewportWidth / density, viewportHeight / density);
        io.DisplayFramebufferScale = ImVec2(density, density);
    }
    void AndroidFileDialog_UpdateTextInput(bool visible) {
        if (visible && !modalDepth && !inputField.isFirstResponder) [inputField becomeFirstResponder];
        if (!visible && inputField.isFirstResponder) [inputField resignFirstResponder];
    }
    void AndroidFileDialog_OpenUrl(const std::string& url) {
        NSURL* target = [NSURL URLWithString:ns(url)];
        if ([target.scheme.lowercaseString isEqualToString:@"https"] || [target.scheme.lowercaseString isEqualToString:@"http"])
            [UIApplication.sharedApplication openURL:target options:@{} completionHandler:nil];
    }
    void AndroidFileDialog_ShowPermissions() {
        [UIApplication.sharedApplication openURL:[NSURL URLWithString:UIApplicationOpenSettingsURLString] options:@{} completionHandler:nil];
    }
    void AndroidFileDialog_ShowExportVideoPlayer() { exportActive = true; UIApplication.sharedApplication.idleTimerDisabled = YES; }
    void AndroidFileDialog_HideExportVideoPlayer() { exportActive = false; UIApplication.sharedApplication.idleTimerDisabled = NO; }
    void AndroidFileDialog_UpdateExportProgress(uint64_t, uint64_t, bool) { }
    bool AndroidFileDialog_IsBackgroundExportActive() { return exportActive; }

    IO::MessageBoxResult IOSMessageBox(const std::string& title, const std::string& message, IO::MessageBoxButtons buttons) {
        NativeModal modal;
        __block bool done = false;
        __block IO::MessageBoxResult result = IO::MessageBoxResult::Cancel;
        UIAlertController* alert = [UIAlertController alertControllerWithTitle:ns(title) message:ns(message) preferredStyle:UIAlertControllerStyleAlert];
        void (^add)(NSString*, IO::MessageBoxResult, UIAlertActionStyle) = ^(NSString* label, IO::MessageBoxResult value, UIAlertActionStyle style) {
            [alert addAction:[UIAlertAction actionWithTitle:label style:style handler:^(UIAlertAction*) { result = value; done = true; }]];
        };
        if (buttons == IO::MessageBoxButtons::YesNo || buttons == IO::MessageBoxButtons::YesNoCancel) {
            add(@"Yes", IO::MessageBoxResult::Yes, UIAlertActionStyleDefault);
            add(@"No", IO::MessageBoxResult::No, UIAlertActionStyleDefault);
        } else add(@"OK", IO::MessageBoxResult::Ok, UIAlertActionStyleDefault);
        if (buttons == IO::MessageBoxButtons::OkCancel || buttons == IO::MessageBoxButtons::YesNoCancel)
            add(@"Cancel", IO::MessageBoxResult::Cancel, UIAlertActionStyleCancel);
        [rootController presentViewController:alert animated:YES completion:nil];
        waitUntil(^bool { return done && !alert.presentingViewController; });
        return result;
    }

    std::string AndroidFileDialog_Pick(bool save, const std::string& title,
        const std::vector<std::string>& patterns, const std::string& extension, const std::string& suggested) {
        NativeModal modal;
        if (save) {
            __block bool done = false;
            __block NSString* name = nil;
            UIAlertController* alert = [UIAlertController alertControllerWithTitle:ns(title) message:@"Choose a filename. After saving, choose its destination in Files." preferredStyle:UIAlertControllerStyleAlert];
            [alert addTextFieldWithConfigurationHandler:^(UITextField* field) {
                field.text = ns(suggested).lastPathComponent.length ? ns(suggested).lastPathComponent : @"Untitled";
                field.autocorrectionType = UITextAutocorrectionTypeNo;
            }];
            [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction*) { done = true; }]];
            [alert addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
                name = alert.textFields.firstObject.text.lastPathComponent; done = true;
            }]];
            [rootController presentViewController:alert animated:YES completion:nil];
            waitUntil(^bool { return done && !alert.presentingViewController; });
            if (!name.length || [name isEqualToString:@"."] || [name isEqualToString:@".."]) return {};
            NSString* ext = ns(extension);
            while ([ext hasPrefix:@"."]) ext = [ext substringFromIndex:1];
            if (!name.pathExtension.length && ext.length) name = [name stringByAppendingPathExtension:ext];
            NSString* folder = [documents() stringByAppendingPathComponent:@"Exports"];
            [NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
            std::string path = [folder stringByAppendingPathComponent:name].UTF8String;
            pendingExports.insert(path);
            return path;
        }
        NSMutableArray<UTType*>* types = [NSMutableArray array];
        for (const auto& filter : patterns) {
            for (const auto& pattern : IO::split(filter, ";")) {
                if (pattern.rfind("*.", 0) != 0 || pattern == "*.*") continue;
                UTType* type = [UTType typeWithFilenameExtension:ns(pattern.substr(2))];
                if (type) [types addObject:type];
            }
        }
        // Custom chart types may not be registered with iOS yet; allow all data
        // so .mmws and .sus files are selectable alongside known media types.
        [types addObject:UTTypeData];
        UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:YES];
        MMWPickerDelegate* delegate = [MMWPickerDelegate new];
        picker.delegate = delegate;
        picker.allowsMultipleSelection = NO;
        [rootController presentViewController:picker animated:YES completion:nil];
        waitUntil(^bool { return delegate.done && !picker.presentingViewController; });
        NSURL* url = delegate.selectedURL;
        if (!url) return {};
        BOOL scoped = [url startAccessingSecurityScopedResource];
        NSString* folder = [[documents() stringByAppendingPathComponent:@"Imports"] stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        [NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
        NSString* path = [folder stringByAppendingPathComponent:url.lastPathComponent];
        NSError* error = nil;
        BOOL copied = [NSFileManager.defaultManager copyItemAtURL:url toURL:[NSURL fileURLWithPath:path] error:&error];
        if (scoped) [url stopAccessingSecurityScopedResource];
        if (!copied) { NSLog(@"Import failed: %@", error); return {}; }
        return path.UTF8String;
    }
    bool AndroidFileDialog_Commit(const std::string& filename) {
        if (!pendingExports.count(filename)) return true;
        if (!std::filesystem::exists(std::filesystem::u8path(filename))) return false;
        NativeModal modal;
        UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForExportingURLs:@[[NSURL fileURLWithPath:ns(filename)]] asCopy:YES];
        MMWPickerDelegate* delegate = [MMWPickerDelegate new];
        picker.delegate = delegate;
        [rootController presentViewController:picker animated:YES completion:nil];
        waitUntil(^bool { return delegate.done && !picker.presentingViewController; });
        pendingExports.erase(filename);
        // The original remains in Documents/Exports even if the share is cancelled.
        return delegate.selectedURL != nil;
    }
}

int main(int argc, char* argv[]) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(MMWAppDelegate.class)); }
}
