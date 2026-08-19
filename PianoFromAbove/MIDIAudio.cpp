#include <Windows.h>
#include "MIDIAudio.h"
#include "MIDIPreRenderPlayer.h"
#include "Config.h"
#include <functional>
#include <future>
#include <fstream>

// global mutable variables... o.o
static std::atomic_bool stopGenerator = false;
std::mutex m_maMtx;
std::atomic<double> g_preVolume = 1.0;

// The ring holds 120s of audio, but the generator must never render the whole ring ahead: a slow synth stretch would silently drain the entire safety margin (the visual clock holds the whole time) and the eventual recovery is one giant backfill burst. Frame counters, NOT the float-counted m_iBufferLength (m_iBufferLength/2 happens to be the whole ring).

// Render in bounded chunks so a single BASS synthesis call can never stall the generator (or a KillLastGenerator join waiting on it) for long, and stopGenerator is honored between chunks.
static const int kGenWriteChunkFrames = 16384;

AudioBufferStream::AudioBufferStream(MIDIAudio* source)
{
	m_maAudioSource = source;
}

// ------- loudmax parameters -------
static const bool reduceHighPitch = false;
static const double strength = 1.0;
static const double minThresh = 0.4;
static const double velocityThresh = 1.0;

int AudioBufferStream::Read(float* buffer, int offset, int count)
{
	{
		m_maMtx.lock();
		if (m_maAudioSource->m_bPaused || m_maAudioSource->m_bAwaitingReset)
		{
			for (int i = 0; i < count; i++)
			{
				buffer[i + offset] = 0;
			}
			m_maAudioSource->m_bStallActive.store(false);
			m_maMtx.unlock();
			return count;
		}

  // --- Stutter handling ------------------------------------------------ Repeat the last chunk of generated audio instead of dropping out when the generator has not kept up (underrun). The read position does NOT advance while stuttering, so playback is simply held in place.
		const int kRepeatFrames = m_maAudioSource->m_iRepeatFrames;

		if (m_maAudioSource->m_bUnderrunRepeat)
		{
   // Enter the stall the moment a callback can no longer be served fully (nAvail < count/2), NOT only at nAvail <= 0. The normal path zero-fills any shortfall, so with the late trigger the player kept outputting slivers of audio + silence for ~1.5s before the stall finally engaged - that alternating audio/silence is the crackle. The stall guard keeps us from re-resetting m_iRepeatOffset every callback (else we'd never walk through the repeat window).
			bool bStarving = (long long)m_maAudioSource->m_iBufferWritePos - m_maAudioSource->m_iBufferReadPos < count / 2;
			if (bStarving && !m_maAudioSource->m_bInUnderrunStall)
			{
				m_maAudioSource->m_bInUnderrunStall = true;
				m_maAudioSource->m_iRepeatOffset = 0;
    // Anchor the repeat window's END at the LAST GENERATED frame. During an underrun the generator is behind, so the region between writePos and readPos is unwritten silence; anchoring here guarantees we only ever repeat real audio - never the unwritten gap (which caused the clicks).
				m_maAudioSource->m_iStallAnchor = m_maAudioSource->m_iBufferWritePos;
				m_maAudioSource->m_ullBufferUnderruns++;
				PRE_DbgLog("STL+ r=%d w=%d anchor=%d", m_maAudioSource->m_iBufferReadPos, m_maAudioSource->m_iBufferWritePos, m_maAudioSource->m_iStallAnchor);
			}
			else if (m_maAudioSource->m_bInUnderrunStall &&
				(long long)m_maAudioSource->m_iBufferWritePos - m_maAudioSource->m_iBufferReadPos >= kRepeatFrames)
			{
    // Exit once a full chunk of playable audio is available again. Because the game's song clock was frozen while the stall was active (m_bStallActive), the read position resumes exactly where the visuals are - a seamless hand-off, no catch-up jump needed. The old behaviour snapped the read head toward the song clock instead, which left it behind the visuals' position whenever the generator was running slower than real time - that is the "audio is delayed after the stutters" complaint.
				m_maAudioSource->m_bInUnderrunStall = false;
				PRE_DbgLog("STL- r=%d w=%d anchor=%d off=%d", m_maAudioSource->m_iBufferReadPos, m_maAudioSource->m_iBufferWritePos, m_maAudioSource->m_iStallAnchor, m_maAudioSource->m_iRepeatOffset);
			}
		}
  // Publish stall state so the game thread freezes the song clock while audio is held (audio is the master clock - see m_bStallActive). The freeze only happens when "extend visuals on skip" is enabled: it adds the stall duration to the visuals so audio and visuals resume in the same place, whereas without it the visuals keep running and the audio comes back delayed.
		m_maAudioSource->m_bStallActive.store(m_maAudioSource->m_bExtendVisualsOnSkip && m_maAudioSource->m_bInUnderrunStall);
		if (m_maAudioSource->m_bInUnderrunStall)
		{
			CopyRepeatTail(m_maAudioSource, buffer, offset, count);
   // Diagnostic: log every time the repeat walk completes a full chunk
			// lap (raw counter grows across laps; wrap is done at read time).
			static int sLastLap = 0;
			int lap = m_maAudioSource->m_iRepeatOffset / kRepeatFrames;
			if (lap != sLastLap)
			{
				sLastLap = lap;
				PRE_DbgLog("STL~ lap=%d totalOff=%d r=%d w=%d", lap, m_maAudioSource->m_iRepeatOffset, m_maAudioSource->m_iBufferReadPos, m_maAudioSource->m_iBufferWritePos);
			}
			m_maMtx.unlock();
			return count;
		}

		int frames = count / 2;
		const int half = m_maAudioSource->m_iBufferLength / 2;
		const double speed = m_maAudioSource->m_dReadSpeed;
		const float* src = m_maAudioSource->m_fAudioBuffer;

		if (fabs(speed - 1.0) < 1e-9)
		{
   // Fast path (unchanged behavior): copy whole interleaved frames.
			int readpos = m_maAudioSource->m_iBufferReadPos % half;
			if (m_maAudioSource->m_iBufferReadPos + frames > m_maAudioSource->m_iBufferWritePos)
			{
				int copyCount = m_maAudioSource->m_iBufferReadPos - (m_maAudioSource->m_iBufferWritePos + frames);
				if (copyCount > frames) copyCount = frames;
				if (copyCount > 0) MIDIAudio::WrappedCopy(m_maAudioSource->m_fAudioBuffer, readpos * 2, m_maAudioSource->m_iBufferLength, buffer, offset, copyCount * 2);
				else
				{
					copyCount = 0;
				}
				for (int i = copyCount * 2; i < count; i++)
				{
					buffer[i + offset] = 0;
				}
    // The SDL callback caught up with the generator: count it so the nerd-stats overlay can surface crackle sources. Only counted here when underrun-repeat handling is off; when it's on the stall path above is used.
				if (!m_maAudioSource->m_bUnderrunRepeat)
					m_maAudioSource->m_ullBufferUnderruns++;
			}
			else
			{
				MIDIAudio::WrappedCopy(m_maAudioSource->m_fAudioBuffer, readpos * 2, m_maAudioSource->m_iBufferLength, buffer, offset, count);
			}
			m_maAudioSource->m_iBufferReadPos += frames;
		}
		else
		{
   // Resampled path: hop the virtual playhead over the canonical source by `speed` frames per output frame (linear interp). A speed change applies on the very next callback with no generator restart.
			double consumed = (double)frames * speed;
			long long need = (long long)floor(consumed) + 2;
			int gen = frames;
			if (m_maAudioSource->m_iBufferReadPos + need > m_maAudioSource->m_iBufferWritePos)
			{
				long long avail = m_maAudioSource->m_iBufferWritePos - m_maAudioSource->m_iBufferReadPos;
				if (avail < 0) avail = 0;
				gen = (int)((double)avail / speed);
				if (gen > frames) gen = frames;
				if (gen < 0) gen = 0;
				consumed = (double)gen * speed;
				if (!m_maAudioSource->m_bUnderrunRepeat)
					m_maAudioSource->m_ullBufferUnderruns++;
			}
			for (int o = 0; o < gen; o++)
			{
				double p = m_maAudioSource->m_iBufferReadPos + (double)o * speed;
				int i0 = (int)floor(p);
				double w = p - (double)i0;
				int a = (i0 % half) * 2;
				int b = ((i0 + 1) % half) * 2;
				buffer[offset + o * 2 + 0] = (float)(src[a + 0] * (1.0 - w) + src[b + 0] * w);
				buffer[offset + o * 2 + 1] = (float)(src[a + 1] * (1.0 - w) + src[b + 1] * w);
			}
			for (int i = gen * 2; i < count; i++)
			{
				buffer[i + offset] = 0;
			}
			double adv = consumed + m_maAudioSource->m_dReadFraction;
			int advInt = (int)floor(adv);
			m_maAudioSource->m_dReadFraction = adv - advInt;
			m_maAudioSource->m_iBufferReadPos += advInt;
		}

		m_maAudioSource->lastReadTime = std::chrono::time_point_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now()
		).time_since_epoch();
		m_maMtx.unlock();
	}
	return count;
}

// Fills the output with the last chunk of GENERATED audio ending at src->m_iStallAnchor (the generator front at stall entry; during an underrun that front sits at/below the read position, and anchoring there guarantees we only ever repeat real audio - never the unwritten silence gap ahead of it). Each successive call advances src->m_iRepeatOffset so the FULL repeat window plays through before wrapping back to the window start. Does NOT modify the read position. Caller holds m_maMtx.
void AudioBufferStream::CopyRepeatTail(MIDIAudio* src, float* buffer, int offset, int count)
{
	const int kFrames = src->m_iRepeatFrames;
 // Lap-seam crossfade: the repeating window loops; a hard cut back to the window start when the ends don't meet a zero crossing is a click. Fade the first few frames of each pass over the equivalent frames at the window's end, so the loop is seamless.
	const int kSeamFadeFrames = 64; // ~1.3ms
	int written = 0; // floats written so far
	while (written < count)
	{
		int endAbs = src->m_iStallAnchor; // absolute frame of window end
		int startAbs = endAbs - kFrames;  // absolute frame of window start
		int winFrames = kFrames;
		if (startAbs < 0)
		{
			startAbs = 0;
			winFrames = endAbs;
		}
		if (winFrames <= 0)
		{
   // Nothing playable yet (startup); keep the output silence-clean.
			memset(&buffer[offset + written], 0, (size_t)(count - written) * sizeof(float));
			break;
		}
		int pos = src->m_iRepeatOffset % winFrames; // where in the window we are
		int takeFrames = winFrames - pos;
		if (takeFrames * 2 > count - written) takeFrames = (count - written) / 2;
		if (takeFrames <= 0) break;
		int srcFloatPos = ((startAbs + pos) % (src->m_iBufferLength / 2)) * 2;
		MIDIAudio::WrappedCopy(src->m_fAudioBuffer, srcFloatPos, src->m_iBufferLength, buffer, offset + written, takeFrames * 2);
  // Seam blend on the pass that starts at the window start: cross-fade window[start + i] against the tail of the window (which is what the previous pass just played), read from the ring so it also works when the seam splits across two callbacks.
		if (pos == 0 && takeFrames >= kSeamFadeFrames && winFrames >= kSeamFadeFrames)
		{
			int endPhi = (startAbs + winFrames - kSeamFadeFrames) % (src->m_iBufferLength / 2);
			int startPhi = startAbs % (src->m_iBufferLength / 2);
			for (int i = 0; i < kSeamFadeFrames; i++)
			{
				int ef = (endPhi + i) % (src->m_iBufferLength / 2);
				int sf = (startPhi + i) % (src->m_iBufferLength / 2);
				float w = (float)i / (float)(kSeamFadeFrames - 1); // 0..1
				int efl = ef * 2, sfl = sf * 2;
				float aL = src->m_fAudioBuffer[efl + 0];
				float aR = src->m_fAudioBuffer[efl + 1];
				float bL = src->m_fAudioBuffer[sfl + 0];
				float bR = src->m_fAudioBuffer[sfl + 1];
				buffer[offset + written + i * 2 + 0] = aL * (1.0f - w) + bL * w;
				buffer[offset + written + i * 2 + 1] = aR * (1.0f - w) + bR * w;
			}
		}
		written += takeFrames * 2;
		src->m_iRepeatOffset += takeFrames;
	}
}
// read with loudmax
int AudioBufferStream::ReadLM(float* buffer, int offset, int count)
{
	int read = Read(buffer, offset, count);
	m_maAudioSource->ApplyLoudMax(buffer + offset, read / 2, m_maAudioSource->m_liveLimiter);
	return read;
}

// LoudMax-style level processor: normalizes by a smoothed loudness envelope
// (attack/release from the pre-render audio settings), plus an optional
// high-velocity rolloff and the playback volume. Runs independently on the ring chunks the
// output device pulls (real-time) AND on the render's WAV capture with separate state instances,
// so live playback and video rendering never clobber each other.
void MIDIAudio::ApplyLoudMax(float* buffer, int frames, LoudMaxState& state)
{
	double attack = 48000.0 * m_dAttack;
	double falloff = 48000.0 * m_dRelease;

	for (int i = 0; i < frames; i++)
	{
		int o = i * 2;
		double l = (double)fabs(buffer[o]);
		double r = (double)fabs(buffer[o + 1]);

		if (state.loudnessL > l) state.loudnessL = (state.loudnessL * falloff + l) / (falloff + 1.0);
		else state.loudnessL = (state.loudnessL * attack + l) / (attack + 1.0);

		if (state.loudnessR > r) state.loudnessR = (state.loudnessR * falloff + r) / (falloff + 1.0);
		else state.loudnessR = (state.loudnessR * attack + r) / (attack + 1.0);

		if (state.loudnessL < minThresh) state.loudnessL = minThresh;
		if (state.loudnessR < minThresh) state.loudnessR = minThresh;

		double nl = buffer[o] / (state.loudnessL * strength + 2.0 * (1.0 - strength)) / 2.0;
		double nr = buffer[o + 1] / (state.loudnessR * strength + 2.0 * (1.0 - strength)) / 2.0;

		if (!state.firstChunk || i != 0)
		{
			double dl = std::abs((double)buffer[o] - nl);
			double dr = std::abs((double)buffer[o + 1] - nr);

			if (state.velocityL > dl)
				state.velocityL = (state.velocityL * falloff + dl) / (falloff + 1.0);
			else
				state.velocityL = (state.velocityL * attack + dl) / (attack + 1.0);

			if (state.velocityR > dr)
				state.velocityR = (state.velocityR * falloff + dr) / (falloff + 1.0);
			else
				state.velocityR = (state.velocityR * attack + dr) / (attack + 1.0);
		}

		if (reduceHighPitch)
		{
			if (state.velocityL > velocityThresh)
				nl = nl / state.velocityL * velocityThresh;
			if (state.velocityR > velocityThresh)
				nr = nr / state.velocityR * velocityThresh;
		}

		buffer[o] = (float)(nl * g_preVolume.load());
		buffer[o + 1] = (float)(nr * g_preVolume.load());
	}
	state.firstChunk = false;
}

void MIDIAudio::WriteWavChunk(const float* rawSrc, int frames)
{
	if (!m_pWavFile || frames <= 0) return;
	std::vector<float> wavBuf((size_t)frames * 2);
	memcpy(wavBuf.data(), rawSrc, (size_t)frames * 2 * sizeof(float));
	ApplyLoudMax(wavBuf.data(), frames, m_wavLimiter);
	fwrite(wavBuf.data(), sizeof(float), (size_t)(frames * 2), m_pWavFile);
	m_lWavDataBytes += (long)(frames * 2) * (long)sizeof(float);
}

void MIDIAudio::WrappedCopy(float* src, int pos, int srcCount, float *dst, int pos2, int count)
{
	if (pos + count > srcCount)
	{
		memcpy(dst + pos2, src + pos, (srcCount - pos) * sizeof(float));
		count -= (srcCount - pos);
		pos = 0;
	}
	memcpy(dst + pos2, src + pos, count * sizeof(float));
}

MIDIAudio::MIDIAudio(int bufferLength) : m_asAudioStream(this) 
{
	m_fAudioBuffer = (float*)malloc(bufferLength * 2 * sizeof(float));
	memset(m_fAudioBuffer, 0, bufferLength * 2 * sizeof(float));
	m_iBufferLength = bufferLength * 2;
	m_asAudioStream = AudioBufferStream(this);
	m_tGeneratorThread = nullptr;
}

void MIDIAudio::LoadSoundfont(const wchar_t* path)
{
	m_bBass->LoadSoundfont(path);
}

void MIDIAudio::Reset()
{
	memset(m_fAudioBuffer, 0, m_iBufferLength * sizeof(float));
	m_iBufferWritePos = 0;
	m_iBufferReadPos = 0;
	m_dReadFraction = 0.0;
	m_liveLimiter.Reset();
}

void MIDIAudio::SetReadSpeed(double dSpeed)
{
	m_maMtx.lock();
	m_dReadSpeed = dSpeed;
	m_maMtx.unlock();
}

bool MIDIAudio::BassWriteWrapped(BASSMIDI* bass, int start, int count)
{
	if (bass->IsStreamDead())
	{
		g_bGenDead = true;
		PRE_DbgLog("SYNTHDEAD detected before write (stream dead)");
		return false;
	}
	start = (start * 2) % m_iBufferLength;
	count *= 2;
	if (start + count > m_iBufferLength)
	{
		int part1 = m_iBufferLength - start;
		bass->Read(m_fAudioBuffer, start, part1);
		if (m_pWavFile)
		{
			WriteWavChunk(m_fAudioBuffer + start, part1 / 2);
		}
		count -= part1;
		bass->Read(m_fAudioBuffer, 0, count);
		if (m_pWavFile)
		{
			WriteWavChunk(m_fAudioBuffer, count / 2);
		}
	}
	else
	{
		bass->Read(m_fAudioBuffer, start, count);
		if (m_pWavFile)
		{
			WriteWavChunk(m_fAudioBuffer + start, count / 2);
		}
	}

	// Diagnostic: RMS of the chunk just pulled from the synth. A chunk that
	// comes back silent while the generator keeps sending events means the
	// synth is no longer sounding the notes (the prerender death signature).
	// Only logged after real audio has been heard (skips the silent pre-roll).
	static bool sHeardAudio = false;
	static int sSilentLogs = 0;
	double dSum = 0.0;
	int n = 0;
	for (int i = start; i < min(start + count, m_iBufferLength); i += 2)
	{
		float f = m_fAudioBuffer[i];
		dSum += (double)f * f;
		n++;
	}
	double dRms = (n > 0) ? sqrt(dSum / n) : 0.0;
	if (dRms > 0.005) sHeardAudio = true;
	if (sHeardAudio && dRms < 0.0005 && sSilentLogs < 30)
	{
		sSilentLogs++;
		PRE_DbgLog("SILENTCHUNK posF=%.1f frames=%d rms=%.5f r=%d",
			(double)(start / 2) / 48000.0, count / 2, dRms, m_iBufferReadPos);
	}

	// Synth-death detection: the decode position must advance with every write.
	// A frozen or errored position while events keep flowing means the synth is
	// dead (the prerender death) - the ring fills with zeros forever otherwise.
	// (An RMS-based "silent output" heuristic was removed: quiet passages in real
	// songs trip it and burned all rebuilds mid-song.)
	QWORD qSynthPos = BASS_ChannelGetPosition(bass->m_hsHandle, BASS_POS_BYTE);
	if (qSynthPos == (QWORD)-1)
	{
		bass->MarkStreamDead();
		g_bGenDead = true;
		PRE_DbgLog("SYNTHDEAD GetPosition=-1 w=%d r=%d", m_iBufferWritePos, m_iBufferReadPos);
		return false;
	}
	if (qSynthPos <= m_qLastSynthPos)
	{
		m_iFrozenFrames += count / 2;
		if (m_iFrozenFrames > 48000 * 2)
		{
			bass->MarkStreamDead();
			g_bGenDead = true;
			PRE_DbgLog("SYNTHDEAD position frozen %d frames pos=%llu w=%d r=%d",
				m_iFrozenFrames, (unsigned long long)qSynthPos, m_iBufferWritePos, m_iBufferReadPos);
			return false;
		}
	}
	else
	{
		m_iFrozenFrames = 0;
		m_qLastSynthPos = qSynthPos;
	}
	return true;
}

// Renders `frames` frames of audio into the ring in kGenWriteChunkFrames chunks, advancing m_iBufferWritePos. Returns false if stopGenerator was set mid-write (caller tears down); true when everything was written.
bool MIDIAudio::WriteAudioChunked(BASSMIDI* bass, int frames)
{
	while (frames > 0)
	{
		int chunk = min(frames, kGenWriteChunkFrames);
		if (!BassWriteWrapped(bass, m_iBufferWritePos, chunk))
		{
			return false;
		}
		m_iBufferWritePos += chunk;
		frames -= chunk;
		if (stopGenerator)
		{
			return false;
		}
	}
	return true;
}

// this was *sorta* copied from kiva
void MIDIAudio::GeneratorFunc(double speed, double time, std::vector<MIDIChannelEvent>* events, int start)
{
	BASSMIDI* bass = new BASSMIDI(m_iDefaultVoices, m_bDefaultNoFx);
	m_iBufferWritePos = 0;
	m_iBufferReadPos = 0;

	PRE_DbgLog("GEN start t=%.3f startidx=%d nEvents=%d speed=%.2f", time, start, (int)events->size(), speed);
	if (bass->IsStreamDead())
	{
		g_bGenDead = true;
		PRE_DbgLog("GEN abort: stream create failed, no audio this pass");
		bass->~BASSMIDI();
		return;
	}
	int dBgSent = 0;
	m_tGenStart = std::chrono::steady_clock::now();

 // Diagnostic: generator throughput vs. wall clock, SendEventRaw cost, and the note-on density per audio second the synth actually faced.
	using clock_t = std::chrono::steady_clock;
	auto tGenStart = clock_t::now();
	auto tLastProg = tGenStart;
	long long llSent = 0, llProgSent = 0;
	long long llSynthUs = 0, llProgSynthUs = 0;
	int iDensSec = -1, iDensOns = 0, iDensPeak = 0;
	double dDensPeakT = 0.0;

 // Events before the seek point: only non-note events still matter (they set BASS program/controller state). Note events before `start` are skipped wholesale - iterating tens of millions of them on every seek rebuild is what made the audio stall for seconds after a seek.
	size_t iSkip = min((size_t)max(start, 0), events->size());
	auto itPre = events->begin();
	while (itPre != events->begin() + iSkip)
	{
		unsigned char iCode = m_pMIDI->GetEventCode(*itPre);
		if ((iCode >> 4 != 0x8) && (iCode >> 4 != 0x9))
		{
			BYTE ev[3] = { iCode, m_pMIDI->GetEventParam1(*itPre), m_pMIDI->GetEventParam2(*itPre) };
			bass->SendEventRaw(ev, 3);
			llSent++;
		}
		if (stopGenerator)
		{
			PRE_DbgLog("GEN stopped mid-loop");
			break;
		}
		++itPre;
	}

	int iConsecEvents = 0;
	for (std::vector<MIDIChannelEvent>::iterator e = itPre; e != events->end(); ++e)
	{
  // everything before the seek point was handled by the pre-loop above
		if (e - events->begin() < (long long)iSkip) continue;

		if (stopGenerator)
		{
			PRE_DbgLog("GEN stopped mid-loop");
			break;
		}

		if (m_iBufferWritePos < m_iBufferReadPos)
		{
   // Generator fell behind the callback (underrun). Snap the write head to the read head and KEEP processing events in order (reference behavior). Events now behind the front compute samples <= 0 - no audio write, but the event is still sent to the synth, so note state stays consistent. The old fast-forward loop silently dropped note events here: unmatched NoteOffs left notes stuck across the skip window and dropped NoteOns made repeated notes never sound - "repeated notes get cut off" during long playback.
			m_iBufferWritePos = m_iBufferReadPos;
		}

		double evTime = m_pMIDI->GetEventTime(*e) / 1e6;

  // Diagnostic: note-on density per audio second (the load the synth faced) and a 1 Hz throughput/progress line while the generator runs.
		{
			int iDensCode = m_pMIDI->GetEventCode(*e);
			if ((iDensCode >> 4) == 0x9 && m_pMIDI->GetEventParam2(*e) > 0)
			{
				int s = (int)floor(evTime);
				if (s != iDensSec)
				{
					if (iDensOns > iDensPeak)
					{
						iDensPeak = iDensOns;
						dDensPeakT = (double)iDensSec;
					}
					iDensSec = s;
					iDensOns = 0;
				}
				iDensOns++;
			}
			auto tNow = clock_t::now();
			if (std::chrono::duration_cast<std::chrono::milliseconds>(tNow - tLastProg).count() >= 1000)
			{
				double dElapsed = std::chrono::duration<double, std::milli>(tNow - tLastProg).count();
				double dAudioFront = m_dStartTime + m_iBufferWritePos / 48000.0;
				QWORD qSynthPos = BASS_ChannelGetPosition(bass->m_hsHandle, BASS_POS_BYTE);
				PRE_DbgLog("GENPROG ev=%zu/%zu sent/s=%.0f synthMs/s=%.1f r=%d w=%d aheadSec=%.3f synthPos=%.3f",
					(size_t)(e - events->begin()), events->size(),
					(double)(llSent - llProgSent) * 1000.0 / dElapsed,
					(double)(llSynthUs - llProgSynthUs) / 1000.0,
					m_iBufferReadPos, m_iBufferWritePos, evTime - dAudioFront,
					(qSynthPos == -1 ? -1.0 : (double)(qSynthPos / 8)));
				llProgSent = llSent;
				llProgSynthUs = llSynthUs;
				tLastProg = tNow;
			}
		}

  // quantizes events to the nearest "frame"
		if (m_dFPS != 0.0)
		{
			evTime = floor(evTime * m_dFPS) / m_dFPS;
		}
		
		double offset = evTime - m_dStartTime;
		int samples = (int)(48000 * offset) - m_iBufferWritePos;
		if (samples > 0)
		{
   // Never run more than m_iMaxAheadFrames ahead of the device read position: it keeps a huge underrun cushion while bounding stall drain and the recovery backfill that caused the multi-second freezes (and the joined main thread during stop/restart).
			while (!stopGenerator && m_iBufferWritePos + samples > m_iBufferReadPos + m_iMaxAheadFrames)
			{
				auto spare = (m_iBufferReadPos + m_iMaxAheadFrames) - m_iBufferWritePos;
				if (spare > 0)
				{
					if (spare > samples) spare = samples;
					if (spare != 0)
					{
						if (!WriteAudioChunked(bass, spare))
						{
							samples = 0;
							break;
						}
						samples -= spare;
					}
					if (samples == 0) break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			if (samples != 0 && !stopGenerator)
			{
				if (!WriteAudioChunked(bass, samples))
				{
					break;
				}
			}
			iConsecEvents = 0;
		}

  // skipping velocity
		if ((m_pMIDI->GetEventCode(*e) >> 4) == 0x9 && m_pMIDI->GetEventParam2(*e) > 0 && m_pMIDI->GetEventParam2(*e) < GetSkippingVelocity()) continue;
		//if ((m_pMIDI->GetEventCode(*e) >> 4) != 0x9 && (m_pMIDI->GetEventCode(*e) >> 4) != 0x8) continue;

  // skip notes with velocity lower than this value
		if ((m_pMIDI->GetEventCode(*e) >> 4) == 0x9 && m_pMIDI->GetEventParam2(*e) > 0 && \
			(m_pMIDI->GetEventParam2(*e) <= m_iVelThreshLow || m_pMIDI->GetEventParam2(*e) > m_iVelThreshUpp)) continue;

		BYTE ev[3] = { m_pMIDI->GetEventCode(*e), m_pMIDI->GetEventParam1(*e), m_pMIDI->GetEventParam2(*e) };

		auto tSend0 = clock_t::now();
		int err = 1;
		err = bass->SendEventRaw(ev, 3);
		if (err <= 0)
		{
			// Diagnostic: events rejected by BASS (queue full / stream state).
			static int sLogErr = 0;
			if (sLogErr < 30)
			{
				sLogErr++;
				PRE_DbgLog("EVFAIL err=%d code=0x%02X p1=%d p2=%d evT=%.3f w=%d r=%d",
					err, m_pMIDI->GetEventCode(*e), m_pMIDI->GetEventParam1(*e), m_pMIDI->GetEventParam2(*e),
					m_pMIDI->GetEventTime(*e) / 1e6, m_iBufferWritePos, m_iBufferReadPos);
			}
		}
		{
			// Diagnostic: poison-event scan - any status byte >= 0xF0 sent as raw
			// MIDI starts a SysEx/real-time sequence in BASS and eats following
			// events, permanently corrupting the stream. Also dump every 5000th
			// event so the stream around the death boundary is visible.
			int iCode = m_pMIDI->GetEventCode(*e);
			if ((iCode & 0xF0) == 0xF0)
			{
				static int sPoison = 0;
				if (sPoison < 50)
				{
					sPoison++;
					PRE_DbgLog("POISON code=0x%02X p1=%d p2=%d absT=%.6f",
						iCode, m_pMIDI->GetEventParam1(*e), m_pMIDI->GetEventParam2(*e),
						m_pMIDI->GetEventTime(*e) / 1e6);
				}
			}
			if ((llSent % 5000) == 0)
			{
				PRE_DbgLog("EVSTREAM sent=%llu code=0x%02X p1=%d p2=%d absT=%.6f w=%d",
					(unsigned long long)llSent, iCode,
					m_pMIDI->GetEventParam1(*e), m_pMIDI->GetEventParam2(*e),
					m_pMIDI->GetEventTime(*e) / 1e6, m_iBufferWritePos);
			}
		}
		llSynthUs += std::chrono::duration_cast<std::chrono::microseconds>(clock_t::now() - tSend0).count();
		llSent++;
		iConsecEvents++;
		if (dBgSent < 8 && (m_pMIDI->GetEventCode(*e) >> 4) == 0x9 && m_pMIDI->GetEventParam2(*e) > 0)
		{
			PRE_DbgLog("GEN  ev[%d] absT=%.3f ch=%d note=%d vel=%d", dBgSent, m_pMIDI->GetEventTime(*e) / 1e6, m_pMIDI->GetEventChannel(*e), m_pMIDI->GetEventParam1(*e), m_pMIDI->GetEventParam2(*e));
			dBgSent++;
		}
		if ((m_pMIDI->GetEventTime(*e) / 1e6) < 8.0 && (llSent % 10) == 0 && llSent < 4000)
		{
			PRE_DbgLog("EVFULL sent=%llu code=0x%02X p1=%d p2=%d absT=%.4f",
				(unsigned long long)llSent, m_pMIDI->GetEventCode(*e),
				m_pMIDI->GetEventParam1(*e), m_pMIDI->GetEventParam2(*e),
				m_pMIDI->GetEventTime(*e) / 1e6);
		}
		if (stopGenerator)
		{
			PRE_DbgLog("GEN stopped mid-loop");
			break;
		}
	}

	while (!stopGenerator)
	{
		auto spare = (m_iBufferReadPos + m_iMaxAheadFrames) - m_iBufferWritePos;
		if (spare > 0 && spare != 0)
		{
			if (!WriteAudioChunked(bass, spare))
			{
				break;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}

	PRE_DbgLog("GEN exit (t=%.3f)", time);

	if (iDensOns > iDensPeak)
	{
		iDensPeak = iDensOns;
		dDensPeakT = (double)iDensSec;
	}
	PRE_DbgLog("GENPROG done sent=%lld synthMs=%.1f wallMs=%.1f peakOnsPerSec=%d peakT=%.1f",
		llSent, (double)llSynthUs / 1000.0,
		std::chrono::duration<double, std::milli>(clock_t::now() - tGenStart).count(),
		iDensPeak, dDensPeakT);

	bass->~BASSMIDI();
}

void MIDIAudio::KillLastGenerator()
{
	memset(m_fAudioBuffer, 0, m_iBufferLength * sizeof(float));
	stopGenerator = true;
	if (m_tGeneratorThread != nullptr)
	{
		m_tGeneratorThread->join();
		m_tGeneratorThread = nullptr;
	}
}

void MIDIAudio::Start(double time, std::vector<MIDIChannelEvent>* events, double speed, int start)
{
	KillLastGenerator();
	stopGenerator = false;
	g_bGenDead = false;
	m_qLastSynthPos = 0;
	m_iFrozenFrames = 0;
	m_dStartTime = time;
	m_liveLimiter.Reset();
	m_pMIDI = events ? m_pMIDI : nullptr;
	m_tGeneratorThread = new std::thread([this, speed, time, events, start] { GeneratorFunc(speed, time, events, start); });
	m_bAudioStarted = true;
	m_bAwaitingReset = false;
}

void MIDIAudio::Stop()
{
	KillLastGenerator();
	m_bPaused = true;
	m_iBufferWritePos = 0;
	m_iBufferReadPos = 0;
}

void MIDIAudio::StartWavRecording(const wchar_t* path)
{
	StopWavRecording();
	m_wavLimiter.Reset();
	m_pWavFile = _wfopen(path, L"wb");
	if (!m_pWavFile)
	{
		PRE_DbgLog("WAV: open failed: %ls", path);
		return;
	}
	m_lWavDataBytes = 0;
	unsigned char hdr[44] = {};
	memcpy(hdr, "RIFF", 4);
	memcpy(hdr + 8, "WAVE", 4);
	memcpy(hdr + 12, "fmt ", 4);
	hdr[16] = 16; // fmt chunk size
	hdr[20] = 3; hdr[21] = 0;  // WAVE_FORMAT_IEEE_FLOAT
	hdr[22] = 2; hdr[23] = 0;  // 2 channels
	hdr[24] = 0x80; hdr[25] = 0xBB; hdr[26] = 0; hdr[27] = 0; // 48000 Hz
	unsigned int uiByteRate = 48000 * 2 * 4;
	memcpy(hdr + 28, &uiByteRate, 4);
	hdr[32] = 8; hdr[33] = 0;  // block align (2ch * 4 bytes)
	hdr[34] = 32; hdr[35] = 0; // 32 bits per sample
	memcpy(hdr + 36, "data", 4);
	fwrite(hdr, 1, 44, m_pWavFile);
	PRE_DbgLog("WAV: recording started: %ls", path);
}

void MIDIAudio::StopWavRecording()
{
	if (!m_pWavFile)
		return;
	fflush(m_pWavFile);
	unsigned int uiDataBytes = (unsigned int)m_lWavDataBytes;
	fseek(m_pWavFile, 40, SEEK_SET);
	fwrite(&uiDataBytes, 4, 1, m_pWavFile);
	unsigned int uiFileBytes = uiDataBytes + 36;
	fseek(m_pWavFile, 4, SEEK_SET);
	fwrite(&uiFileBytes, 4, 1, m_pWavFile);
	fclose(m_pWavFile);
	m_pWavFile = nullptr;
	PRE_DbgLog("WAV: recording stopped (%u bytes)", uiDataBytes);
}

void MIDIAudio::SyncPlayer(double time, double speed)
{
	{
		m_maMtx.lock();
  // Publish the speed-adjusted song clock (kept for diagnostics near the ring, e.g. StartRender's force-check). Audio is the master clock, so the read position is deliberately NOT touched here: the fork nudges m_iBufferReadPos toward the song clock whenever it drifts >30ms, but doing that every game frame fires a cross-fade jump per frame even in otherwise-clean playback, which reads as crackle. Stall recovery owns its position entirely (see m_bStallActive) - normal playback just consumes the ring in real time, matching the fork's steady state.
		m_maMtx.unlock();
	}
}

void MIDIAudio::StartRender(long long llStartTime, bool force, std::vector<MIDIChannelEvent>* events, double speed, long long iStartPos)
{
	double time = (double)llStartTime / 1000000;
	if (!force)
	{
		if (time + 0.1 > GetPlayerTime() + GetBufferSeconds() || time + 0.01 < GetPlayerTime())
		{
			force = true;
		}
	}
	if (force)
	{
		m_pMIDI = events ? m_pMIDI : nullptr;
		Start(time, events, speed, iStartPos);
	}
	else
	{
		SyncPlayer(time, speed);
	}
}

void MIDIAudio::SetMaxAheadMs(int ms)
{
	if (ms < 1000) ms = 1000;
	int frames = (int)((long long)ms * 48);
	m_iMaxAheadFrames = frames;
	EnsureBufferCapacity(frames * 2);
}

void MIDIAudio::EnsureBufferCapacity(int minFrames)
{
	int minFloats = minFrames * 2;
	if (m_iBufferLength < minFloats)
	{
		m_maMtx.lock();
		int oldLength = m_iBufferLength;
		int newLength = minFloats;
		float* pNewBuffer = (float*)realloc(m_fAudioBuffer, (size_t)newLength * sizeof(float));
		if (pNewBuffer)
		{
			memset(pNewBuffer + oldLength, 0, (size_t)(newLength - oldLength) * sizeof(float));
			m_fAudioBuffer = pNewBuffer;
			m_iBufferLength = newLength;
		}
		m_maMtx.unlock();
	}
}