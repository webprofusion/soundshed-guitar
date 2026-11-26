// Soundshed Nano Q for iOS and iPadOS: the app around the plugin.
//
// The app delegate owns the plugin (through ClapHost) and the audio (AudioIO) for the life of the
// process; the one view controller puts the plugin's editor in its view, inside the safe area.
// On iOS 13 and later a scene delegate makes the window (Info.plist's scene manifest); iOS 12
// has no scenes, and the app delegate makes it there.
//
// See docs/plans/nano-q-ios.md.

#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

#include "AppLog.h"
#include "AudioIO.h"
#include "ClapHost.h"

#include <memory>

using nanoq::ios::AppLog;

namespace
{
std::unique_ptr<nanoq::ios::ClapHost> gHost;
std::unique_ptr<nanoq::ios::AudioIO> gAudio;

// Where the plugin's state is kept between runs: Application Support, which is backed up and
// never purged, as clap-wrapper's desktop standalone keeps it in the user's settings.
NSString* StatePath()
{
    NSURL* support = [[NSFileManager defaultManager] URLsForDirectory:NSApplicationSupportDirectory
                                                            inDomains:NSUserDomainMask]
                         .firstObject;
    NSURL* dir = [support URLByAppendingPathComponent:@"Soundshed Nano Q" isDirectory:YES];
    [[NSFileManager defaultManager] createDirectoryAtURL:dir withIntermediateDirectories:YES attributes:nil error:nil];
    return [dir URLByAppendingPathComponent:@"plugin-state.bin"].path;
}

void SaveState()
{
    if (!gHost)
        return;
    if (gHost->SaveState(StatePath().fileSystemRepresentation))
        AppLog(@"Nano Q: the plugin's state was saved");
    else
        AppLog(@"Nano Q: the plugin's state was not saved");
}

// Behind the safe area's margins: the dark theme's bgPrimary (ThemeTokens.generated.h), so the
// strips around the editor match it. The light and warm themes keep these strips dark; the app
// does not hear about theme changes.
UIColor* BackdropColour()
{
    return [UIColor colorWithRed:0x11 / 255.0 green:0x11 / 255.0 blue:0x16 / 255.0 alpha:1.0];
}

// Posted by the editor's view once its first frame is on screen (nanoq/ios/elements_host/base_view.mm).
NSString* const kFirstFrameNotification = @"NanoQFirstFramePresented";

// The audio session and the plugin's activation take half a second of the main thread on an iPad
// mini 2, which the first frame used to wait behind. They start kAudioAfterFirstFrame after it,
// once the UI has taken in the engine's state (the first frame comes before most of it), or after
// kAudioStartLimit if the first frame is slow to come (its shaders compiling, or no view: a launch
// into the background), whichever is first.
constexpr double kAudioAfterFirstFrame = 1.0;
constexpr double kAudioStartLimit = 3.0;

void StartAudioOnce()
{
    static bool started = false;
    if (started || !gAudio)
        return;
    started = true;
    gAudio->Start();
}
} // namespace

// ── The view controller ──────────────────────────────────────────────────────────

@interface NanoQViewController : UIViewController
@end

@implementation NanoQViewController
{
    UIView* mEditorParent;
    UIActivityIndicatorView* mLoading;
    id mFirstFrameObserver;
    bool mAttached;
}

- (void)loadView
{
    UIView* root = [[UIView alloc] initWithFrame:UIScreen.mainScreen.bounds];
    root.backgroundColor = BackdropColour();
    mEditorParent = [[UIView alloc] initWithFrame:root.bounds];
    mEditorParent.backgroundColor = BackdropColour();
    [root addSubview:mEditorParent];

    // Over the editor until it has drawn: its first frame can take seconds when its shaders have
    // to be compiled again, and an empty window looks like a hang.
    UIActivityIndicatorViewStyle style;
    if (@available(iOS 13.0, *))
        style = UIActivityIndicatorViewStyleLarge;
    else
        style = UIActivityIndicatorViewStyleWhiteLarge;
    mLoading = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:style];
    mLoading.color = [UIColor colorWithWhite:0.75 alpha:1.0];
    mLoading.hidesWhenStopped = YES;
    mLoading.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin |
                                UIViewAutoresizingFlexibleTopMargin | UIViewAutoresizingFlexibleBottomMargin;
    mLoading.center = CGPointMake(CGRectGetMidX(root.bounds), CGRectGetMidY(root.bounds));
    [root addSubview:mLoading];
    [mLoading startAnimating];

    __weak NanoQViewController* weakSelf = self;
    mFirstFrameObserver =
        [[NSNotificationCenter defaultCenter] addObserverForName:kFirstFrameNotification
                                                          object:nil
                                                           queue:[NSOperationQueue mainQueue]
                                                      usingBlock:^(NSNotification*) {
                                                          [weakSelf firstFramePresented];
                                                      }];
    self.view = root;
}

- (void)firstFramePresented
{
    [mLoading stopAnimating];
    [mLoading removeFromSuperview];
    mLoading = nil;
    if (mFirstFrameObserver)
        [[NSNotificationCenter defaultCenter] removeObserver:mFirstFrameObserver];
    mFirstFrameObserver = nil;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(kAudioAfterFirstFrame * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
                       StartAudioOnce();
                   });
}

- (void)dealloc
{
    if (mFirstFrameObserver)
        [[NSNotificationCenter defaultCenter] removeObserver:mFirstFrameObserver];
}

// The editor fills the safe area: clear of the status bar, the home indicator and a phone's
// camera housing, and following them through rotation.
- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];
    const auto frame = UIEdgeInsetsInsetRect(self.view.bounds, self.view.safeAreaInsets);
    mEditorParent.frame = frame;
    if (!gHost)
        return;

    const auto width = uint32_t(frame.size.width);
    const auto height = uint32_t(frame.size.height);
    if (!mAttached)
    {
        mAttached = gHost->AttachView((__bridge void*)mEditorParent, width, height);
        if (!mAttached)
            AppLog(@"Nano Q: the plugin's editor did not open");
    }
    else
    {
        gHost->ResizeView(width, height);
    }
}

- (UIStatusBarStyle)preferredStatusBarStyle
{
    return UIStatusBarStyleLightContent;
}

- (BOOL)prefersHomeIndicatorAutoHidden
{
    return NO;
}
@end

// ── The scene delegate (iOS 13 and later) ────────────────────────────────────────

API_AVAILABLE(ios(13.0))
@interface NanoQSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow* window;
@end

@implementation NanoQSceneDelegate

- (void)scene:(UIScene*)scene
    willConnectToSession:(UISceneSession*)session
                 options:(UISceneConnectionOptions*)connectionOptions
{
    if (![scene isKindOfClass:[UIWindowScene class]])
        return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.rootViewController = [NanoQViewController new];
    [self.window makeKeyAndVisible];
}

- (void)sceneDidEnterBackground:(UIScene*)scene
{
    SaveState();
}
@end

// ── The app delegate ─────────────────────────────────────────────────────────────

@interface NanoQAppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow* window;
@end

@implementation NanoQAppDelegate

- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)launchOptions
{
    gHost = std::make_unique<nanoq::ios::ClapHost>();
    if (!gHost->Load())
    {
        AppLog(@"Nano Q: the plugin did not load");
        gHost.reset();
    }
    else
    {
        gHost->LoadState(StatePath().fileSystemRepresentation);
        gAudio = std::make_unique<nanoq::ios::AudioIO>(*gHost);
        // After the first frame (see StartAudioOnce); at once with no window to draw.
        if (application.applicationState == UIApplicationStateBackground)
            StartAudioOnce();
        else
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(kAudioStartLimit * NSEC_PER_SEC)),
                           dispatch_get_main_queue(), ^{
                               StartAudioOnce();
                           });
    }

    if (@available(iOS 13.0, *))
        return YES; // the scene delegate makes the window

    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [NanoQViewController new];
    [self.window makeKeyAndVisible];
    return YES;
}

- (UISceneConfiguration*)application:(UIApplication*)application
    configurationForConnectingSceneSession:(UISceneSession*)connectingSceneSession
                                   options:(UISceneConnectionOptions*)options API_AVAILABLE(ios(13.0))
{
    auto* config = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:connectingSceneSession.role];
    config.delegateClass = [NanoQSceneDelegate class];
    return config;
}

// iOS 12 (no scenes).
- (void)applicationDidEnterBackground:(UIApplication*)application
{
    SaveState();
}

- (void)applicationWillTerminate:(UIApplication*)application
{
    SaveState();
    if (gAudio)
        gAudio->Stop();
    gAudio.reset();
    if (gHost)
        gHost->Unload();
    gHost.reset();
}
@end

int main(int argc, char* argv[])
{
    @autoreleasepool
    {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([NanoQAppDelegate class]));
    }
}
