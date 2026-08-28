#include "FrameGenerationScheduler.h"

#include "Log.h"
#include "RifeInterpolatorNcnn.h"
#include <chrono>

namespace FrameGeneration
{

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

CFrameGenerationScheduler::~CFrameGenerationScheduler()
{
    stopAsyncWorker();
}

void CFrameGenerationScheduler::Configure(bool enabled, double sourceFps,
                                          double targetFps,
                                          spIFrameInterpolator interpolator)
{
    // Stop any existing worker before swapping the interpolator so the worker
    // thread never races against a replaced m_interpolator pointer.
    stopAsyncWorker();

    m_enabled = enabled && sourceFps > 0.0 && targetFps > sourceFps &&
                targetFps <= sourceFps * 2.0 && interpolator != nullptr;
    m_sourceFps = sourceFps;
    m_targetFps = targetFps;
    m_interpolator = std::move(interpolator);
    Reset();

    // Start the background worker only for GPU-backed interpolators; CPU-only
    // ones (blend) are fast enough that async overhead is not worthwhile.
    if (m_enabled && m_interpolator && m_interpolator->IsGpuBacked())
        startAsyncWorker();
}

void CFrameGenerationScheduler::Reset()
{
    // Flush async state — any in-flight job will finish and its result will be
    // discarded by takeAsyncResult() when frame pointers no longer match.
    {
        std::lock_guard<std::mutex> lock(m_asyncMutex);
        m_resultReady = false;
        m_asyncResult.reset();
        m_resultForPrev.reset();
        m_resultForNext.reset();
        // Do NOT clear m_jobPending/m_jobRunning: the worker owns those frames
        // until it finishes; they'll be naturally superseded on the next job.
    }

    m_prefetchedNextRealFrame.reset();
    m_prefetchedAfterNextRealFrame.reset();
    m_currentRealFrame.reset();
    m_nextRealFrame.reset();
    m_generatedFrame.reset();
    m_displayFrame.reset();
    m_displayGeneratedNext = false;
    m_displayPhase = 0.0;
    m_pendingSyntheticFrames = 0.0;
    m_generatedFrameCount = 0;
    m_missedSyntheticSlots = 0;
    m_realFrameCount = 0;
}

// ---------------------------------------------------------------------------
// Async worker
// ---------------------------------------------------------------------------

void CFrameGenerationScheduler::startAsyncWorker()
{
    m_asyncStop = false;
    m_asyncThread = std::thread(&CFrameGenerationScheduler::asyncWorkerLoop, this);
}

void CFrameGenerationScheduler::stopAsyncWorker()
{
    if (!m_asyncThread.joinable())
        return;
    {
        std::lock_guard<std::mutex> lock(m_asyncMutex);
        m_asyncStop = true;
    }
    m_asyncWorkCv.notify_all();
    m_asyncThread.join();
}

void CFrameGenerationScheduler::asyncWorkerLoop()
{
    while (true)
    {
        ContentDecoder::spCVideoFrame prev, next;
        spIFrameInterpolator interpolator;
        {
            std::unique_lock<std::mutex> lock(m_asyncMutex);
            m_asyncWorkCv.wait(lock,
                [this] { return m_asyncStop || m_jobPending; });
            if (m_asyncStop)
                return;
            prev         = m_jobPrev;
            next         = m_jobNext;
            interpolator = m_jobInterpolator;
            m_jobPending = false;
            m_jobRunning = true;
        }

        auto result = interpolator->Interpolate(prev, next, 0.5f);

        {
            std::lock_guard<std::mutex> lock(m_asyncMutex);
            m_jobRunning   = false;
            m_asyncResult  = std::move(result);
            m_resultForPrev = std::move(prev);
            m_resultForNext = std::move(next);
            m_resultReady   = true;
        }
        m_asyncResultCv.notify_one();
    }
}

bool CFrameGenerationScheduler::submitAsyncJob(
    const ContentDecoder::spCVideoFrame& prev,
    const ContentDecoder::spCVideoFrame& next,
    const spIFrameInterpolator& interpolator)
{
    std::lock_guard<std::mutex> lock(m_asyncMutex);
    // Don't overwrite a job that is already in-flight or a result not yet consumed.
    if (m_jobPending || m_jobRunning || m_resultReady)
        return false;
    m_jobPrev        = prev;
    m_jobNext        = next;
    m_jobInterpolator = interpolator;
    m_jobPending     = true;
    m_asyncWorkCv.notify_one();
    return true;
}

ContentDecoder::spCVideoFrame CFrameGenerationScheduler::takeAsyncResult(
    const ContentDecoder::spCVideoFrame& expectedPrev,
    const ContentDecoder::spCVideoFrame& expectedNext)
{
    std::unique_lock<std::mutex> lock(m_asyncMutex);

    // Never wait. This used to block on m_asyncResultCv for up to 50ms when the
    // worker was mid-inference, which is 7 frames at 144Hz and was the single
    // largest source of visible hitching. If the result is not ready this instant,
    // the caller repeats a real frame for this slot and we try again next time.
    if (!m_resultReady || !m_asyncResult)
        return nullptr;

    // Validate that the result is for the frame pair we actually need right now.
    if (m_resultForPrev != expectedPrev || m_resultForNext != expectedNext)
    {
        m_resultReady = false;
        m_asyncResult.reset();
        return nullptr;
    }

    m_resultReady = false;
    return std::move(m_asyncResult);
}

void CFrameGenerationScheduler::topUpPrefetchAndSubmit(const FrameProvider& frameProvider)
{
    if (!frameProvider)
        return;

    // Refill the two-frame lookahead. frameProvider() returns null when the
    // decoder has nothing queued; that is normal and simply means we try again on
    // the next tick, so never treat it as an error.
    if (!m_prefetchedNextRealFrame)
        m_prefetchedNextRealFrame = frameProvider();
    if (m_prefetchedNextRealFrame && !m_prefetchedAfterNextRealFrame)
        m_prefetchedAfterNextRealFrame = frameProvider();

    if (!m_asyncThread.joinable() || !m_interpolator)
        return;

    // Prefetch still happens while suspended (the decoder pipeline should not
    // stall), but submit no work: a job started here would run an inference on the
    // shared RIFE mutex, which is exactly what suspending exists to avoid.
    if (m_generationSuspended)
        return;

    if (!m_prefetchedNextRealFrame || !m_prefetchedAfterNextRealFrame)
        return;

    if (!canGenerateBetween(m_prefetchedNextRealFrame, m_prefetchedAfterNextRealFrame))
        return;

    // Don't clobber a job in flight or a result not yet consumed.
    {
        std::lock_guard<std::mutex> lock(m_asyncMutex);
        if (m_jobPending || m_jobRunning || m_resultReady)
            return;
    }

    submitAsyncJob(m_prefetchedNextRealFrame, m_prefetchedAfterNextRealFrame,
                   m_interpolator);
}

// ---------------------------------------------------------------------------
// Stats forwarding (unchanged)
// ---------------------------------------------------------------------------

double CFrameGenerationScheduler::InterpolatorLastTimeMs() const
{
    auto rife = std::dynamic_pointer_cast<CRifeInterpolatorNcnn>(m_interpolator);
    return rife ? rife->LastInferenceMs() : 0.0;
}

double CFrameGenerationScheduler::InterpolatorAverageTimeMs() const
{
    auto rife = std::dynamic_pointer_cast<CRifeInterpolatorNcnn>(m_interpolator);
    return rife ? rife->AverageInferenceMs() : 0.0;
}

uint64_t CFrameGenerationScheduler::InterpolatorFailureCount() const
{
    auto rife = std::dynamic_pointer_cast<CRifeInterpolatorNcnn>(m_interpolator);
    return rife ? rife->FailedInferenceCount() : 0;
}

bool CFrameGenerationScheduler::InterpolatorFallingBack() const
{
    auto rife = std::dynamic_pointer_cast<CRifeInterpolatorNcnn>(m_interpolator);
    return rife ? rife->IsFallingBack() : false;
}

// ---------------------------------------------------------------------------
// Internal helpers (unchanged)
// ---------------------------------------------------------------------------

bool CFrameGenerationScheduler::prepareNextRealFrame(const FrameProvider& frameProvider,
                                                     std::string* reason)
{
    if (m_nextRealFrame)
        return true;

    if (!frameProvider)
    {
        if (reason)
            *reason = "No frame provider configured.";
        return false;
    }

    m_nextRealFrame = frameProvider();
    if (!m_nextRealFrame)
    {
        if (reason)
            *reason = "Decoder queue is empty.";
        return false;
    }

    return true;
}

bool CFrameGenerationScheduler::canGenerateBetween(
    const ContentDecoder::spCVideoFrame& previous,
    const ContentDecoder::spCVideoFrame& next) const
{
    if (!previous || !next)
        return false;

    if (previous->Width() != next->Width() || previous->Height() != next->Height())
        return false;

    const auto& prevMeta = previous->GetMetaData();
    const auto& nextMeta = next->GetMetaData();
    if (prevMeta.isSeam || nextMeta.isSeam)
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// Advance — drives the render thread one presentation step forward.
//
// The async path works as follows:
//   • When we show a real frame, we immediately pre-fetch the frame after it
//     from the decoder and submit RIFE(current, prefetched) to the worker.
//   • On the next call that needs a generated frame, takeAsyncResult() collects
//     the pre-computed result — typically already ready since RIFE (~25 ms) fits
//     inside the presentation interval (~28 ms at 36 fps).
//   • A sync fallback fires on cold start or when the result arrives late.
// ---------------------------------------------------------------------------

bool CFrameGenerationScheduler::Advance(const FrameProvider& frameProvider, std::string* reason)
{
    if (!Enabled())
    {
        if (reason)
            *reason = "Frame generation is disabled.";
        return false;
    }

    const double synthsPerGap =
        (m_sourceFps > 0.0) ? ((m_targetFps / m_sourceFps) - 1.0) : 0.0;

    // -----------------------------------------------------------------------
    // Bootstrap: grab the very first real frame.
    // -----------------------------------------------------------------------
    if (!m_currentRealFrame)
    {
        m_currentRealFrame = frameProvider ? frameProvider() : nullptr;
        if (!m_currentRealFrame)
        {
            if (reason)
                *reason = "No initial decoded frame available.";
            return false;
        }

        m_displayFrame = m_currentRealFrame;
        m_displayPhase = 0.0;
        m_displayGeneratedNext = false;
        ++m_realFrameCount;

        // Pre-fetch the frame after this one so the worker can start early.
        topUpPrefetchAndSubmit(frameProvider);
        return true;
    }

    // -----------------------------------------------------------------------
    // Not yet showing a generated frame: decide whether to generate one.
    // -----------------------------------------------------------------------
    if (!m_displayGeneratedNext)
    {
        // Use the pre-fetched frame if available, otherwise pop from decoder.
        if (m_prefetchedNextRealFrame)
        {
            m_nextRealFrame = std::move(m_prefetchedNextRealFrame);
            // Shift the lookahead down so prefetchedNext is always the frame
            // after m_nextRealFrame; topUpPrefetchAndSubmit() refills the tail.
            m_prefetchedNextRealFrame = std::move(m_prefetchedAfterNextRealFrame);
            m_prefetchedAfterNextRealFrame.reset();
        }
        else if (!prepareNextRealFrame(frameProvider, reason))
        {
            return false;
        }

        m_pendingSyntheticFrames += synthsPerGap;
        const bool shouldGenerateThisGap = m_pendingSyntheticFrames >= 1.0;

        // True when the slot below is filled with a repeated real frame rather than
        // an interpolated one, so the stats can tell the two apart.
        bool slotIsRepeat = false;

        if (shouldGenerateThisGap && canGenerateBetween(m_currentRealFrame, m_nextRealFrame))
        {
            if (m_generationSuspended)
            {
                slotIsRepeat = true;
                // Fill the synthetic slot by repeating the current real frame.
                // Keeping the slot filled is the point: dropping it instead would
                // advance real frames at the presentation rate and play the clip
                // targetFps/sourceFps too fast.
                m_generatedFrame = m_currentRealFrame;
            }
            else if (m_asyncThread.joinable())
            {
                // GPU-backed: take the pre-computed result if the worker has one
                // ready. If not, repeat the current real frame rather than running
                // the inference inline — a synchronous Interpolate() here costs
                // 15-30ms on the render thread, which is the hitch we are avoiding.
                m_generatedFrame = takeAsyncResult(m_currentRealFrame, m_nextRealFrame);
                if (!m_generatedFrame)
                {
                    m_generatedFrame = m_currentRealFrame;
                    slotIsRepeat = true;
                    ++m_missedSyntheticSlots;
                }
            }
            else
            {
                // CPU interpolators (blend) have no worker and are sub-millisecond,
                // so running them inline costs nothing worth pipelining.
                m_generatedFrame = m_interpolator->Interpolate(m_currentRealFrame, m_nextRealFrame, 0.5f);
            }
        }
        else
        {
            m_generatedFrame.reset();
        }

        if (m_generatedFrame)
        {
            m_pendingSyntheticFrames -= 1.0;
            m_displayFrame = m_generatedFrame;
            m_displayPhase = 0.5;
            m_displayGeneratedNext = true;
            // A repeated frame — suspended during a crossfade, or the worker not
            // being ready in time — is not an interpolated one. Counting it would
            // overstate what RIFE actually produced.
            if (!slotIsRepeat)
                ++m_generatedFrameCount;

            // While we display this generated frame, pre-fetch the frame after
            // m_nextRealFrame and start the next RIFE job in the background.
            topUpPrefetchAndSubmit(frameProvider);
            return true;
        }

        // No generated frame this gap — advance directly to the next real frame.
        m_currentRealFrame = m_nextRealFrame;
        m_nextRealFrame.reset();
        m_displayFrame = m_currentRealFrame;
        m_displayPhase = 0.0;
        m_displayGeneratedNext = false;
        ++m_realFrameCount;

        topUpPrefetchAndSubmit(frameProvider);
        return true;
    }

    // -----------------------------------------------------------------------
    // Just showed a generated frame: advance to the next real frame.
    // -----------------------------------------------------------------------
    if (!m_nextRealFrame)
    {
        if (reason)
            *reason = "No decoded frame available for real-frame step.";
        return false;
    }

    m_currentRealFrame = m_nextRealFrame;
    m_nextRealFrame.reset();
    m_generatedFrame.reset();
    m_displayFrame = m_currentRealFrame;
    m_displayPhase = 0.0;
    m_displayGeneratedNext = false;
    ++m_realFrameCount;

    // Pre-fetch and submit the next job while the render thread displays this real frame.
    topUpPrefetchAndSubmit(frameProvider);
    return true;
}

} // namespace FrameGeneration
