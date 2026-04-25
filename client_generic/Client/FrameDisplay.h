#ifndef _FRAMEDISPLAY_H_
#define _FRAMEDISPLAY_H_

#include "base.h"
#include "Rect.h"
#include "Settings.h"
#include "Renderer.h"
#include "TextureFlat.h"
#include "Vector4.h"
#include "Timer.h"
#include "ContentDecoder.h"

enum class AspectRatioMode : int32_t
{
    STRETCH   = 0,
    LETTERBOX = 1,
    CROP      = 2,
};

#ifdef MAC
#include <CoreVideo/CVPixelBuffer.h>
#endif
// #ifndef FRAME_DIAG
// #define FRAME_DIAG
// #endif

/**
    CFrameDisplay().
    Basic display handling, simply blits texture.
*/
class CFrameDisplay
{
    double m_LastTexMoveClock;
    float m_CurTexMoveOff;
    float m_CurTexMoveDir;
    const double TEX_MOVE_SECS = 60.f * 30.f; // 30 minutes

  protected:
    using spCTextureFlat = DisplayOutput::spCTextureFlat;
    ContentDecoder::sFrameMetadata m_MetaData;

    DisplayOutput::spCShader m_spShader;
    spCTextureFlat m_spVideoTexture;

    //	Dimensions of the display surface.
    Base::Math::CRect m_dispSize;

    //  texture Rect
    Base::Math::CRect m_texRect;
    Base::Math::CRect m_uvRect;
    Base::CTimer m_Timer;

    AspectRatioMode m_arMode;

    bool m_bValid;

  public:
    CFrameDisplay(DisplayOutput::spCRenderer _spRenderer)
    {
        m_bValid = true;
        const int32_t arModeInt = g_Settings()->Get("settings.player.ar_mode", -1);
        if (arModeInt < 0)
        {
            // Fall back to legacy bool: preserve_AR=true → LETTERBOX
            const bool legacy = g_Settings()->Get("settings.player.preserve_AR", false);
            m_arMode = legacy ? AspectRatioMode::LETTERBOX : AspectRatioMode::STRETCH;
        }
        else
        {
            m_arMode = static_cast<AspectRatioMode>(arModeInt);
            if (m_arMode < AspectRatioMode::STRETCH || m_arMode > AspectRatioMode::CROP)
                m_arMode = AspectRatioMode::STRETCH;
        }
        m_texRect = Base::Math::CRect(1, 1);
        m_uvRect = Base::Math::CRect(1, 1);
        m_LastTexMoveClock = -1;
        m_CurTexMoveOff = 0;
        m_CurTexMoveDir = 1.;
        m_spShader = _spRenderer->NewShader(
            "quadPassVertex", "drawDecodedFrameNoBlendingFragment");
    }

    bool Valid() { return m_bValid; };

    //
    void SetDisplaySize(const uint32_t _w, const uint32_t _h)
    {
        m_dispSize = Base::Math::CRect(_w, _h);
        m_CurTexMoveOff = 0.f;
    }

    virtual spCTextureFlat& RequestTargetTexture() { return m_spVideoTexture; }

    virtual uint32_t StartAtFrame() const { return 0; }

    //	Decode a frame, and render it.
    virtual bool Draw(DisplayOutput::spCRenderer _spRenderer, float _alpha,
                      [[maybe_unused]] double _interframeDelta)
    {
        if (!m_spVideoTexture)
            return false;

        _spRenderer->SetShader(m_spShader);
        //    Bind texture and render a quad covering the screen.
        _spRenderer->SetBlend("alphablend");
        _spRenderer->SetTexture(m_spVideoTexture, 0);
        _spRenderer->Apply();

        ScrollVideoForNonMatchingAspectRatio(m_spVideoTexture->GetRect());

        _spRenderer->DrawQuad(m_texRect, Base::Math::CVector4(1, 1, 1, _alpha),
                              m_uvRect);

        return true;
    }

    virtual double GetFps(double /*_decodeFps*/, double _displayFps)
    {
        return _displayFps;
    }

    // Virtual method for seamless transition frame inheritance
    virtual void InheritFramesFrom(CFrameDisplay* previous) {
        // Base implementation does nothing (for normal display mode)
    }

    virtual void
    ScrollVideoForNonMatchingAspectRatio([[maybe_unused]] const Base::Math::CRect& texDim)
    {
        m_texRect = Base::Math::CRect(1, 1);
        m_uvRect  = Base::Math::CRect(1, 1);

        if (m_arMode == AspectRatioMode::STRETCH)
            return;

        const float dispW = static_cast<float>(m_dispSize.Width());
        const float dispH = static_cast<float>(m_dispSize.Height());
        if (dispW <= 0.f || dispH <= 0.f)
            return;

        const float targetAspect  = 16.0f / 9.0f;
        const float displayAspect = dispW / dispH;

        if (m_arMode == AspectRatioMode::LETTERBOX)
        {
            // Shrink dest quad to fit content; cleared background shows as bars.
            if (displayAspect > targetAspect)
            {
                const float xPad = (1.f - targetAspect / displayAspect) * 0.5f;
                m_texRect.m_X0 = xPad;
                m_texRect.m_X1 = 1.f - xPad;
            }
            else if (displayAspect < targetAspect)
            {
                const float yPad = (1.f - displayAspect / targetAspect) * 0.5f;
                m_texRect.m_Y0 = yPad;
                m_texRect.m_Y1 = 1.f - yPad;
            }
        }
        else // CROP: fill entire screen, crop UV to maintain content aspect ratio
        {
            if (displayAspect > targetAspect)
            {
                // Display wider than content: crop top/bottom of texture
                const float yPad = (1.f - targetAspect / displayAspect) * 0.5f;
                m_uvRect.m_Y0 = yPad;
                m_uvRect.m_Y1 = 1.f - yPad;
            }
            else if (displayAspect < targetAspect)
            {
                // Display taller than content: crop left/right of texture
                const float xPad = (1.f - displayAspect / targetAspect) * 0.5f;
                m_uvRect.m_X0 = xPad;
                m_uvRect.m_X1 = 1.f - xPad;
            }
        }
    }
};

MakeSmartPointers(CFrameDisplay);

#endif
