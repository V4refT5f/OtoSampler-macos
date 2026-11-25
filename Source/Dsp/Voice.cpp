// SPDX-License-Identifier: AGPL-3.0-only
#include "Voice.h"
#include "Interp.h"

namespace oto
{
namespace
{
inline double wrap1 (double v) noexcept { return v - std::floor (v + 0.5); }
}

void Voice::start (const SampleData& d, int midiNote, float vel, float glideFromPitch,
                   const VoiceParams& p, std::uint64_t serial) noexcept
{
    note_ = midiNote;
    serial_ = serial;
    velocity = (1.f - p.velSens) + p.velSens * std::clamp (vel, 0.f, 1.f);
    gated = true; active = true; pedalHeld = false; preview = false;
    const int N = d.numSamples();
    // Start never goes beyond the loop start point while Sustain Loop is on
    const float startNorm = std::min (std::clamp (p.startNorm, 0.f, 1.f),
                                      maxStartNorm (d.source->map, N, p.loopOn, p.loopStartNorm, p.loopEndNorm));
    a = (double) startNorm * std::max (0, N - 1);
    glideOffset = (p.glideMs > 0.5f && glideFromPitch == glideFromPitch) ? glideFromPitch - (float) midiNote : 0.f;
    lfoPhase = 0.0; vibTime = 0.0; initDone = false; loopArmed = false; rPrev = 1.f;

    // the output fundamental starts in phase with the source, so that an unshifted note rebuilds the original waveform
    const auto& M = d.source->model;
    if (M.valid()) { const double t = M.thetaAt (a); theta = t - std::floor (t); }
    else theta = 0.0;

    adsr.setSampleRate (sr);
    adsr.setParams (p.attackMs, p.decayMs, p.sustain, p.releaseMs);
    adsr.kill();
    adsr.noteOn();
}

void Voice::evalAt (const AdditiveModel& M, const HarmonicChannel& hc, double pos, const EvalCfg& cfg,
                    const LoopInfo& L, float wn, PartialSet& out) noexcept
{
    if (wn > 0.f)
    {
        // Denoise: average with the previous / next pitch period (aperiodic parts cancel, the partials stay)
        const double T = 1.0 / std::max (1.0e-5, (double) M.f0At (pos));
        const double th = M.thetaAt (pos);
        const double pa = pos - T, pb = pos + T;
        evalPartials (M, hc, pos, cfg, scratch, tapC);
        evalPartials (M, hc, pa, cfg, scratch, tapA);
        evalPartials (M, hc, pb, cfg, scratch, tapB);
        averageSets (out, tapC, tapA, wrap1 (th - M.thetaAt (pa)), wn, tapB, wrap1 (th - M.thetaAt (pb)), wn);
    }
    else evalPartials (M, hc, pos, cfg, scratch, out);

    // loop cross-fade: blend with the same material one loop length earlier (phase aligned)
    if (L.active && pos >= L.e - L.xf)
    {
        const float w2 = crossW2 ((pos - (L.e - L.xf)) / L.xf);
        const double pos2 = pos - L.len;
        evalPartials (M, hc, pos2, cfg, scratch, tapA);
        blendSets (out, tapA, w2, wrap1 (M.thetaAt (pos) - M.thetaAt (pos2)));
    }
}

void Voice::render (const SampleData& d, const VoiceParams& p, float bendTarget,
                    float* outL, float* outR, int n) noexcept
{
    if (! active || d.numChannels() == 0) return;
    const int N = d.numSamples();
    if (N < 4 || ! d.hasAdditive()) { kill(); return; }

    const AdditiveModel& M = d.source->model;
    const AdditiveData& D = *d.add;
    const int nch = (int) std::min<size_t> (2, D.ch.size());
    const double srcSr = d.source->sampleRate;
    const double k = srcSr / sr;

    adsr.setParams (p.attackMs, p.decayMs, p.sustain, p.releaseMs);
    const float normGain = p.normalize ? 0.89f / std::max (1e-6f, d.peak) : 1.f;
    const float targetGain = p.gain * normGain * velocity;
    if (! initDone) { gainSm = targetGain; bendCur = bendTarget; initDone = true; }

    // ---- loop points (snapped to pitch marks => whole number of periods) ----
    // The loop stays active while the note is released, so the release tail fades out inside the loop
    // and the part of the sample after the loop is never played.
    LoopInfo L;
    {
        const auto lr = resolveLoop (d.source->map, N, p.loopStartNorm, p.loopEndNorm);
        if (lr.valid)
        {
            L.s = lr.s; L.e = lr.e; L.len = lr.e - lr.s;
            L.xf = std::max (1.0, std::min ((double) p.loopXfadeMs * 0.001 * srcSr, L.len * 0.5));
            L.active = p.loopOn;
        }
    }

    const float dn = (p.formantHold || p.flatten) ? std::clamp (p.denoise, 0.f, 1.f) : 0.f;
    const float wn = dn * (1.f / 3.f);
    const float residualGain = 1.f - dn;

    const float bendCoef = std::exp (-1.f / (0.005f * (float) sr));
    const float gainCoef = std::exp (-1.f / (0.010f * (float) sr));
    const float glideCoef = p.glideMs > 0.5f ? std::exp (-6.9f / (p.glideMs * 0.001f * (float) sr)) : 0.f;
    const double vibInc = (double) p.vibRate / sr;
    const float vibDepth = p.vibDepthCents * 0.01f;
    const double vibDelay = (double) p.vibDelayMs * 0.001 * sr, vibFade = 0.1 * sr;

    int done = 0;
    while (done < n && active)
    {
        const int want = std::min (kChunk, n - done);

        // ---- per-sample scalar parameters: pitch ratio, envelope, gain ----
        float amp[kChunk];
        double sumR = 0.0;
        int len = 0;
        bool ended = false;
        for (int i = 0; i < want; ++i)
        {
            bendCur = bendTarget + (bendCur - bendTarget) * bendCoef;
            gainSm  = targetGain + (gainSm - targetGain) * gainCoef;
            glideOffset *= glideCoef;
            if (std::abs (glideOffset) < 1e-4f) glideOffset = 0.f;

            float vib = 0.f;
            if (vibDepth > 0.f)
            {
                const double onset = std::clamp ((vibTime - vibDelay) / vibFade, 0.0, 1.0);
                vib = vibDepth * (float) onset * (float) std::sin (2.0 * kPi * lfoPhase);
                lfoPhase += vibInc; if (lfoPhase >= 1.0) lfoPhase -= std::floor (lfoPhase);
            }
            vibTime += 1.0;

            const float semis = (float) (note_ - p.rootKey) + p.coarseSemis + bendCur + vib + glideOffset;
            const double r = std::exp2 ((double) std::clamp (semis, -72.f, 72.f) / 12.0);

            const float env = adsr.next();
            if (! adsr.isActive()) { ended = true; break; }
            amp[i] = env * gainSm;
            sumR += r;
            ++len;
        }
        if (len == 0) { active = false; break; }
        const double rAvg = sumR / (double) len;
        rPrev = (float) rAvg;

        // ---- read position ----
        if (L.active && ! loopArmed && a < L.e) loopArmed = true;
        const bool lp = L.active && loopArmed;
        if (! lp && a >= (double) N) { active = false; break; }
        LoopInfo Lc = L; Lc.active = lp;

        const double da = k * (p.speedHold ? (double) len : sumR);      // source samples covered by this block
        const double aEnd = a + da;
        const double step = da / (double) len;

        // ---- pitch: phase advance of the output fundamental ----
        // Exact phase increment of the source along the read path (no integration drift), scaled by the pitch ratio when
        // the read position does not follow the pitch (Speed Hold). Pitch Flatten uses the constant median pitch instead.
        double dTheta;
        if (p.flatten) dTheta = (double) M.refF0 * k * sumR;
        else
        {
            const double dth = M.thetaAt (aEnd) - M.thetaAt (a);
            dTheta = p.speedHold ? dth * rAvg : dth;
        }
        const double fOut = dTheta / (double) len;                      // cycles per output sample
        const int hNyq = (int) std::min (1000.0, std::floor (0.5 / std::max (1.0e-6, fOut)));

        // output pitch / source pitch at both ends of the block (frequency in Hz = cycles per sample * sample rate)
        const double rhoS0 = fOut / std::max (1.0e-6, (double) M.f0At (a) * k);
        const double rhoE0 = fOut / std::max (1.0e-6, (double) M.f0At (aEnd) * k);

        // Pitch Flatten must never move the formants: the envelope is always read at the frequency that matches the
        // (flattened) pitch. Without Formant Hold only the transposition by the note / Pitch knob moves the formants,
        // so that part (the average pitch ratio of the block) is taken out of the ratio used to read the envelope.
        const bool envPath = p.formantHold || p.flatten;
        const double envDiv = p.formantHold ? 1.0 : std::max (1.0e-3, rAvg);
        const double rhoS = std::clamp (rhoS0 / envDiv, 1.0 / 64.0, 64.0);
        const double rhoE = std::clamp (rhoE0 / envDiv, 1.0 / 64.0, 64.0);

        EvalCfg cs, ce;
        cs.formantHold = ce.formantHold = envPath;
        int nOut;
        if (envPath)
        {
            cs.rho = (float) rhoS; ce.rho = (float) rhoE;
            cs.ampScale = std::sqrt (std::clamp (cs.rho, 0.25f, 4.f));   // keeps the level when fewer / more partials share the envelope
            ce.ampScale = std::sqrt (std::clamp (ce.rho, 0.25f, 4.f));
            nOut = std::min ({ kMaxOutPartials, hNyq, (int) ((double) M.stride / std::min (rhoS, rhoE)) });
        }
        else nOut = std::min ({ M.stride, hNyq });
        nOut = std::max (0, nOut);
        cs.n = ce.n = nOut;

        const double th0 = theta - std::floor (theta);
        float harm[2][kChunk];
        for (int c = 0; c < nch; ++c)
        {
            for (int i = 0; i < len; ++i) harm[c][i] = 0.f;
            if (nOut == 0) continue;
            const HarmonicChannel& hc = D.ch[(size_t) c];
            evalAt (M, hc, a, cs, Lc, wn, setA);
            evalAt (M, hc, aEnd, ce, Lc, wn, setB);

            for (int j = 0; j < nOut; ++j)
            {
                const double h = (double) (j + 1);
                const double fh = h * fOut;
                if (fh > 0.45)                                           // fade out towards Nyquist, no aliasing
                {
                    const float g = (float) std::clamp ((0.5 - fh) / 0.05, 0.0, 1.0);
                    setA.amp[j] *= g; setB.amp[j] *= g;
                }
                double ps = h * th0 + (double) setA.ph[j];
                ps -= std::floor (ps);
                pStart[j] = (float) ps;
                pInc[j] = (float) ((h * dTheta + (double) wrap1 ((double) setB.ph[j] - (double) setA.ph[j])) / (double) len);
            }
            float mx = 0.f;
            for (int j = 0; j < nOut; ++j) mx = std::max ({ mx, setA.amp[j], setB.amp[j] });
            bankAdd (setA.amp, setB.amp, pStart, pInc, nOut, len, harm[c], 1.0e-4f * mx);   // partials 80 dB below the strongest are skipped
        }

        // ---- residual (consonants, breath, anything the partials do not explain): read at the source speed ----
        const int nres = (int) std::min<size_t> (2, D.residual.size());
        for (int i = 0; i < len; ++i)
        {
            const double pos = a + step * (double) i;
            float res[2] = { 0.f, 0.f };
            for (int c = 0; c < nres; ++c)
            {
                const float* rd = D.residual[(size_t) c].data();
                float v = readSample (rd, N, pos);
                if (lp && pos >= L.e - L.xf)
                {
                    const float w2 = crossW2 ((pos - (L.e - L.xf)) / L.xf);
                    v = v * std::sqrt (1.f - w2) + readSample (rd, N, pos - L.len) * std::sqrt (w2);   // equal power for noise
                }
                res[c] = v * residualGain;
            }
            if (nres == 1) res[1] = res[0];
            float l = (nch > 0 ? harm[0][i] : 0.f) + res[0];
            float rr = (nch > 1 ? harm[1][i] : (nch > 0 ? harm[0][i] : 0.f)) + res[1];
            l *= amp[i]; rr *= amp[i];
            if (outR != nullptr) { outL[done + i] += l; outR[done + i] += rr; }
            else outL[done + i] += 0.5f * (l + rr);
        }

        // ---- advance ----
        double th1 = th0 + dTheta;
        a = aEnd;
        while (lp && a >= L.e)
        {
            // wrap: the phase of the output fundamental is shifted by the same amount the source phase jumps,
            // so the waveform stays continuous across the loop point
            const double before = a;
            a -= L.len;
            th1 -= wrap1 (M.thetaAt (before) - M.thetaAt (a));
        }
        theta = th1 - std::floor (th1);
        done += len;
        if (! lp && a >= (double) N) { active = false; break; }
        if (ended) { active = false; break; }
    }
}
}
