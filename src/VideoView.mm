#include "VideoView.h"
#include <atomic>
#include <cmath>

#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>
#import <AppKit/AppKit.h>
#import <MediaToolbox/MediaToolbox.h>

//==============================================================================
// Non-ARC Objective-C++ (matches JUCE's macOS layer). Manual retain/release.
struct VideoViewImpl
{
    NSView*         view        = nil;   // layer-backed container view
    AVPlayer*       player      = nil;
    AVPlayerLayer*  playerLayer = nil;
    std::atomic<float> videoPeak { 0.0f };   // last block's peak from the audio tap (for the master meter)
    double          cachedFps   = 0.0;       // video frame rate (lazily read from the asset)
    std::unique_ptr<juce::NSViewComponent> nsViewComp;
};

//==============================================================================
// MTAudioProcessingTap: measure the video's audio peak into VideoViewImpl::videoPeak.
static void tapInit (MTAudioProcessingTapRef, void* clientInfo, void** storageOut) { *storageOut = clientInfo; }
static void tapFinalize (MTAudioProcessingTapRef) {}
static void tapPrepare (MTAudioProcessingTapRef, CMItemCount, const AudioStreamBasicDescription*) {}
static void tapUnprepare (MTAudioProcessingTapRef) {}
static void tapProcess (MTAudioProcessingTapRef tap, CMItemCount numberFrames, MTAudioProcessingTapFlags /*flags*/,
                        AudioBufferList* bufferListInOut, CMItemCount* numberFramesOut, MTAudioProcessingTapFlags* flagsOut)
{
    if (MTAudioProcessingTapGetSourceAudio (tap, numberFrames, bufferListInOut, flagsOut, nullptr, numberFramesOut) != noErr)
        return;
    auto* peak = static_cast<std::atomic<float>*> (MTAudioProcessingTapGetStorage (tap));
    if (peak == nullptr) return;
    float mx = 0.0f;
    for (UInt32 b = 0; b < bufferListInOut->mNumberBuffers; ++b)
    {
        const float* d = static_cast<const float*> (bufferListInOut->mBuffers[b].mData);
        if (d == nullptr) continue;
        const UInt32 n = bufferListInOut->mBuffers[b].mDataByteSize / sizeof (float);
        for (UInt32 k = 0; k < n; ++k) { const float a = std::fabs (d[k]); if (a > mx) mx = a; }
    }
    peak->store (mx);
}

static void attachAudioTap (AVPlayerItem* item, AVURLAsset* asset, void* peakStorage)
{
    NSArray<AVAssetTrack*>* atracks = [asset tracksWithMediaType: AVMediaTypeAudio];
    if (atracks.count == 0) return;

    MTAudioProcessingTapCallbacks cb;
    cb.version    = kMTAudioProcessingTapCallbacksVersion_0;
    cb.clientInfo = peakStorage;
    cb.init       = tapInit;
    cb.finalize   = tapFinalize;
    cb.prepare    = tapPrepare;
    cb.unprepare  = tapUnprepare;
    cb.process    = tapProcess;

    MTAudioProcessingTapRef tap = NULL;
    if (MTAudioProcessingTapCreate (kCFAllocatorDefault, &cb, kMTAudioProcessingTapCreationFlag_PostEffects, &tap) != noErr || tap == NULL)
        return;

    AVMutableAudioMixInputParameters* params = [AVMutableAudioMixInputParameters audioMixInputParametersWithTrack: atracks.firstObject];
    params.audioTapProcessor = tap;
    AVMutableAudioMix* mix = [AVMutableAudioMix audioMix];
    mix.inputParameters = @[ params ];
    item.audioMix = mix;
    CFRelease (tap);   // retained by params
}

//==============================================================================
VideoView::VideoView()
{
    auto* i = new VideoViewImpl();

    i->view = [[NSView alloc] initWithFrame: NSMakeRect (0, 0, 320, 180)];
    [i->view setWantsLayer: YES];
    i->view.layer.backgroundColor = [[NSColor blackColor] CGColor];

    i->nsViewComp = std::make_unique<juce::NSViewComponent>();
    i->nsViewComp->setView (i->view);     // JUCE retains the view while attached
    addAndMakeVisible (i->nsViewComp.get());

    impl = i;
}

VideoView::~VideoView()
{
    auto* i = static_cast<VideoViewImpl*> (impl);
    if (i != nullptr)
    {
        if (i->nsViewComp != nullptr)
            i->nsViewComp->setView (nullptr);

        if (i->playerLayer != nil) { [i->playerLayer removeFromSuperlayer]; [i->playerLayer release]; }
        if (i->player != nil)      { [i->player release]; }
        if (i->view != nil)        { [i->view release]; }

        delete i;
    }

    impl = nullptr;
}

//==============================================================================
bool VideoView::loadFile (const juce::File& file)
{
    auto* i = static_cast<VideoViewImpl*> (impl);
    if (i == nullptr)
        return false;

    NSString* path = [NSString stringWithUTF8String: file.getFullPathName().toRawUTF8()];
    NSURL* url = [NSURL fileURLWithPath: path];
    if (url == nil)
        return false;

    AVURLAsset* asset = [AVURLAsset URLAssetWithURL: url options: @{ AVURLAssetPreferPreciseDurationAndTimingKey : @YES }];
    AVPlayerItem* item = [AVPlayerItem playerItemWithAsset: asset];
    attachAudioTap (item, asset, &i->videoPeak);                          // meter the video's own audio
    AVPlayer* player = [[AVPlayer alloc] initWithPlayerItem: item];       // owned (alloc)
    player.actionAtItemEnd = AVPlayerActionAtItemEndPause;

    AVPlayerLayer* layer = [[AVPlayerLayer playerLayerWithPlayer: player] retain];  // owned (retain)
    layer.videoGravity = AVLayerVideoGravityResizeAspect;
    layer.frame = i->view.bounds;
    layer.autoresizingMask = (kCALayerWidthSizable | kCALayerHeightSizable);

    // Tear down any previous clip.
    if (i->playerLayer != nil) { [i->playerLayer removeFromSuperlayer]; [i->playerLayer release]; i->playerLayer = nil; }
    if (i->player != nil)      { [i->player release]; i->player = nil; }

    [i->view.layer addSublayer: layer];

    i->player      = player;
    i->playerLayer = layer;
    i->cachedFps   = 0.0;   // recompute the frame rate for this clip
    return true;
}

void VideoView::play()
{
    auto* i = static_cast<VideoViewImpl*> (impl);
    if (i != nullptr && i->player != nil)
        [i->player play];
}

void VideoView::pause()
{
    auto* i = static_cast<VideoViewImpl*> (impl);
    if (i != nullptr && i->player != nil)
    {
        [i->player pause];
        i->videoPeak.store (0.0f);   // let the meter fall when stopped
    }
}

void VideoView::setRate (double rate)
{
    auto* i = static_cast<VideoViewImpl*> (impl);
    if (i != nullptr && i->player != nil && i->player.rate != 0.0f)   // only nudge while already playing
        i->player.rate = (float) rate;
}

float VideoView::getAudioPeak() const
{
    auto* i = static_cast<VideoViewImpl*> (impl);
    return i != nullptr ? i->videoPeak.load() : 0.0f;
}

double VideoView::getFrameRate() const
{
    auto* i = static_cast<VideoViewImpl*> (impl);
    if (i == nullptr) return 0.0;
    if (i->cachedFps <= 0.0 && i->player != nil && i->player.currentItem != nil)
    {
        AVAsset* asset = i->player.currentItem.asset;
        NSArray<AVAssetTrack*>* vtracks = [asset tracksWithMediaType: AVMediaTypeVideo];
        if (vtracks.count > 0)
        {
            const float fps = vtracks.firstObject.nominalFrameRate;
            if (fps > 1.0f) i->cachedFps = (double) fps;
        }
    }
    return i->cachedFps;
}

bool VideoView::isPlaying() const
{
    auto* i = static_cast<VideoViewImpl*> (impl);
    if (i != nullptr && i->player != nil)
        return i->player.rate != 0.0f;
    return false;
}

double VideoView::getDurationSeconds() const
{
    auto* i = static_cast<VideoViewImpl*> (impl);
    if (i != nullptr && i->player != nil && i->player.currentItem != nil)
    {
        CMTime d = i->player.currentItem.duration;
        if (CMTIME_IS_NUMERIC (d))
            return CMTimeGetSeconds (d);
    }
    return 0.0;
}

double VideoView::getPositionSeconds() const
{
    auto* i = static_cast<VideoViewImpl*> (impl);
    if (i != nullptr && i->player != nil)
    {
        CMTime t = [i->player currentTime];
        if (CMTIME_IS_NUMERIC (t))
            return CMTimeGetSeconds (t);
    }
    return 0.0;
}

void VideoView::setPositionSeconds (double seconds)
{
    auto* i = static_cast<VideoViewImpl*> (impl);
    if (i != nullptr && i->player != nil)
    {
        CMTime t = CMTimeMakeWithSeconds (seconds, 600);   // 600 ticks/sec: clean for common frame rates
        [i->player seekToTime: t toleranceBefore: kCMTimeZero toleranceAfter: kCMTimeZero];
    }
}

double VideoView::probeDurationSeconds (const juce::File& file)
{
    NSString* path = [NSString stringWithUTF8String: file.getFullPathName().toRawUTF8()];
    NSURL* url = [NSURL fileURLWithPath: path];
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL: url
                                            options: @{ AVURLAssetPreferPreciseDurationAndTimingKey : @YES }];
    if (asset == nil) return 0.0;

    CMTime d = asset.duration;
    double secs = CMTIME_IS_NUMERIC (d) ? CMTimeGetSeconds (d) : 0.0;

    if (secs <= 0.0)   // fall back to the first video track's time range
    {
        NSArray<AVAssetTrack*>* vtracks = [asset tracksWithMediaType: AVMediaTypeVideo];
        if (vtracks.count > 0)
        {
            CMTime td = vtracks.firstObject.timeRange.duration;
            if (CMTIME_IS_NUMERIC (td)) secs = CMTimeGetSeconds (td);
        }
    }
    return secs;
}

void VideoView::exportVideoWithAudio (const juce::File& videoFile, const juce::File& audioFile,
                                      double lengthSeconds, const juce::File& outFile,
                                      std::function<void (bool, juce::String)> onDone)
{
    NSURL* vurl = [NSURL fileURLWithPath: [NSString stringWithUTF8String: videoFile.getFullPathName().toRawUTF8()]];
    NSURL* aurl = [NSURL fileURLWithPath: [NSString stringWithUTF8String: audioFile.getFullPathName().toRawUTF8()]];
    NSURL* ourl = [NSURL fileURLWithPath: [NSString stringWithUTF8String: outFile.getFullPathName().toRawUTF8()]];

    AVURLAsset* vAsset = [AVURLAsset URLAssetWithURL: vurl options: @{ AVURLAssetPreferPreciseDurationAndTimingKey : @YES }];
    AVURLAsset* aAsset = [AVURLAsset URLAssetWithURL: aurl options: nil];

    AVMutableComposition* comp = [AVMutableComposition composition];
    const CMTime len = CMTimeMakeWithSeconds (lengthSeconds, 600);

    NSArray<AVAssetTrack*>* vtracks = [vAsset tracksWithMediaType: AVMediaTypeVideo];
    if (vtracks.count == 0) { if (onDone) onDone (false, "No video track in source."); return; }

    AVAssetTrack* vTrack = vtracks.firstObject;
    AVMutableCompositionTrack* compV = [comp addMutableTrackWithMediaType: AVMediaTypeVideo
                                                        preferredTrackID: kCMPersistentTrackID_Invalid];
    NSError* err = nil;
    [compV insertTimeRange: CMTimeRangeMake (kCMTimeZero, CMTimeMinimum (len, vTrack.timeRange.duration))
                   ofTrack: vTrack atTime: kCMTimeZero error: &err];
    compV.preferredTransform = vTrack.preferredTransform;   // preserve orientation

    NSArray<AVAssetTrack*>* atracks = [aAsset tracksWithMediaType: AVMediaTypeAudio];
    if (atracks.count > 0)
    {
        AVAssetTrack* aTrack = atracks.firstObject;
        AVMutableCompositionTrack* compA = [comp addMutableTrackWithMediaType: AVMediaTypeAudio
                                                            preferredTrackID: kCMPersistentTrackID_Invalid];
        [compA insertTimeRange: CMTimeRangeMake (kCMTimeZero, CMTimeMinimum (len, aTrack.timeRange.duration))
                       ofTrack: aTrack atTime: kCMTimeZero error: &err];
    }

    [[NSFileManager defaultManager] removeItemAtURL: ourl error: nil];

    AVAssetExportSession* ex = [[AVAssetExportSession alloc] initWithAsset: comp
                                                               presetName: AVAssetExportPresetHighestQuality];
    if (ex == nil) { if (onDone) onDone (false, "Could not create export session."); return; }
    ex.outputURL = ourl;
    NSString* ext = [[ourl pathExtension] lowercaseString];
    if ([ext isEqualToString: @"mp4"])      ex.outputFileType = AVFileTypeMPEG4;
    else if ([ext isEqualToString: @"m4v"]) ex.outputFileType = AVFileTypeAppleM4V;
    else                                    ex.outputFileType = AVFileTypeQuickTimeMovie;
    ex.timeRange = CMTimeRangeMake (kCMTimeZero, len);

    std::function<void (bool, juce::String)> cb = onDone;
    [ex exportAsynchronouslyWithCompletionHandler: ^{
        const bool ok = (ex.status == AVAssetExportSessionStatusCompleted);
        NSString* e = ex.error != nil ? ex.error.localizedDescription : @"";
        juce::String msg = juce::String::fromUTF8 (e.UTF8String != nullptr ? e.UTF8String : "");
        juce::MessageManager::callAsync ([cb, ok, msg] { if (cb) cb (ok, msg); });
        [ex release];
    }];
}

void VideoView::setMuted (bool shouldBeMuted)
{
    auto* i = static_cast<VideoViewImpl*> (impl);
    if (i != nullptr && i->player != nil)
        i->player.muted = shouldBeMuted ? YES : NO;
}

void VideoView::resized()
{
    auto* i = static_cast<VideoViewImpl*> (impl);
    if (i == nullptr)
        return;

    if (i->nsViewComp != nullptr)
        i->nsViewComp->setBounds (getLocalBounds());

    if (i->playerLayer != nil && i->view != nil)
    {
        [CATransaction begin];
        [CATransaction setDisableActions: YES];   // no resize animation lag
        i->playerLayer.frame = i->view.bounds;
        [CATransaction commit];
    }
}
