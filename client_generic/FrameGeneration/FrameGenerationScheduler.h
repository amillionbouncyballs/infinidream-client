#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "IFrameInterpolator.h"

namespace FrameGeneration
{

class CFrameGenerationScheduler
{
  public:
    using FrameProvider = std::function<ContentDecoder::spCVideoFrame()>;

    ~CFrameGenerationScheduler();

    void Configure(bool enabled, double sourceFps, double targetFps,
                   spIFrameInterpolator interpolator);
    void Reset();

    bool Enabled() const { return m_enabled && m_interpolator != nullptr; }
    double SourceFps() const { return m_sourceFps; }
    double PresentationFps() const { return Enabled() ? m_targetFps : m_sourceFps; }
    std::string ModeName() const
    {
        return m_interpolator ? std::string(m_interpolator->Name()) : std::string("off");
    }
    spIFrameInterpolator GetInterpolator() const { return m_interpolator; }
    const ContentDecoder::spCVideoFrame& CurrentDisplayFrame() const { return m_displayFrame; }
    double CurrentDisplayPhase() const { return m_displayPhase; }
    uint64_t GeneratedFrameCount() const { return m_generatedFrameCount; }
    uint64_t RealFrameCount() const { return m_realFrameCount; }
    //	Synthetic slots filled with a repeated real frame because the worker had no
    //	result ready in time. A steadily climbing count means the interpolation
    //	pipeline is not keeping up — playback stays smooth, but less of it is
    //	interpolated than the target implies.
    uint64_t MissedSyntheticSlotCount() const { return m_missedSyntheticSlots; }
    double InterpolatorLastTimeMs() const;
    double InterpolatorAverageTimeMs() const;
    uint64_t InterpolatorFailureCount() const;
    bool InterpolatorFallingBack() const;

    bool Advance(const FrameProvider& frameProvider, std::string* reason = nullptr);

    //	Pause interpolation without tearing the scheduler down. Suspended, the
    //	synthetic slot in each gap is filled by repeating the current real frame,
    //	so the presentation cadence — and therefore playback speed — is unchanged,
    //	but no interpolator is called and no async job is submitted.
    //
    //	This exists for the crossfade: Configure(Off) would do the job too, but it
    //	joins the async worker (blocking the player thread for a whole in-flight
    //	inference), drops the buffered frames the outgoing clip is still showing,
    //	and changes PresentationFps mid-fade. Suspending costs an atomic store.
    void SetGenerationSuspended(bool _suspended) { m_generationSuspended = _suspended; }
    bool GenerationSuspended() const { return m_generationSuspended; }

  private:
    bool prepareNextRealFrame(const FrameProvider& frameProvider, std::string* reason);
    bool canGenerateBetween(const ContentDecoder::spCVideoFrame& previous,
                            const ContentDecoder::spCVideoFrame& next) const;

    // Async worker — pre-computes the next interpolated frame off the render thread.
    void startAsyncWorker();
    void stopAsyncWorker();
    void asyncWorkerLoop();
    bool submitAsyncJob(const ContentDecoder::spCVideoFrame& prev,
                        const ContentDecoder::spCVideoFrame& next,
                        const spIFrameInterpolator& interpolator);
    ContentDecoder::spCVideoFrame takeAsyncResult(
        const ContentDecoder::spCVideoFrame& expectedPrev,
        const ContentDecoder::spCVideoFrame& expectedNext);
    //	Keep two real frames prefetched and a job in flight for the gap *after* the
    //	one being displayed. One gap of lead is not enough: at 22.62 -> 35.98 fps a
    //	gap that produces no synthetic frame submits its successor's job on the same
    //	tick that consumes it, leaving a ~28ms lead against a 15-30ms inference —
    //	a coin flip, and every lost toss is a repeated frame.
    void topUpPrefetchAndSubmit(const FrameProvider& frameProvider);

    bool m_enabled = false;
    // Read on the player thread, written by the player thread at crossfade
    // boundaries; atomic so the flag can never be seen half-written.
    std::atomic<bool> m_generationSuspended{false};
    double m_sourceFps = 0.0;
    double m_targetFps = 0.0;
    spIFrameInterpolator m_interpolator;

    ContentDecoder::spCVideoFrame m_currentRealFrame;
    ContentDecoder::spCVideoFrame m_nextRealFrame;
    ContentDecoder::spCVideoFrame m_generatedFrame;
    ContentDecoder::spCVideoFrame m_displayFrame;
    bool m_displayGeneratedNext = false;
    double m_displayPhase = 0.0;
    double m_pendingSyntheticFrames = 0.0;
    uint64_t m_generatedFrameCount = 0;
    uint64_t m_missedSyntheticSlots = 0;
    uint64_t m_realFrameCount = 0;

    // Frames pre-fetched from the decoder ahead of when they are needed. The pair
    // (prefetchedNext, prefetchedAfterNext) is the gap after the one on screen,
    // and is what the worker is given, so its result is ready a full gap early.
    // They shift down the chain as real frames are consumed.
    ContentDecoder::spCVideoFrame m_prefetchedNextRealFrame;
    ContentDecoder::spCVideoFrame m_prefetchedAfterNextRealFrame;

    // Async worker thread state — all fields guarded by m_asyncMutex.
    std::thread m_asyncThread;
    std::mutex m_asyncMutex;
    std::condition_variable m_asyncWorkCv;
    std::condition_variable m_asyncResultCv;
    bool m_asyncStop = false;

    // Submitted job
    ContentDecoder::spCVideoFrame m_jobPrev;
    ContentDecoder::spCVideoFrame m_jobNext;
    spIFrameInterpolator m_jobInterpolator; // captured at submit time; safe against Configure()
    bool m_jobPending = false;              // submitted, not yet picked up by worker
    bool m_jobRunning = false;              // worker has started executing

    // Result from the most recently completed job
    ContentDecoder::spCVideoFrame m_asyncResult;
    ContentDecoder::spCVideoFrame m_resultForPrev; // frames the result was computed for
    ContentDecoder::spCVideoFrame m_resultForNext;
    bool m_resultReady = false;
};

} // namespace FrameGeneration
