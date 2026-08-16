#pragma once

#include <Windows.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <algorithm>
#include <chrono>

#include "bassmidi/bass.h"
#include "bassmidi/bassmidi.h"
#include "BASSMIDI.h"
#include "Misc.h"
#include "resource.h"
#include "MIDI.h"

extern std::atomic<double> g_preVolume;

class MIDIAudio;

class AudioBufferStream {
public:
	bool m_bCanSeek;
	WAVEFORMAT m_wfWaveFormat;
	long long m_llPosition;
	long long m_llLength;

	AudioBufferStream(MIDIAudio* source);
	~AudioBufferStream() {};

	int Read(float* buffer, int offset, int count);
	int ReadLM(float* buffer, int offset, int count);
	void CopyRepeatTail(MIDIAudio* src, float* buffer, int offset, int count);

private:
	MIDIAudio* m_maAudioSource;
};

class MIDIAudio {
public:
	int m_iDefaultVoices = 4096;
	bool m_bDefaultNoFx = false;
	double m_dSimulatedLagScale = 0.01;
	double m_dStartTime = 0;
	double m_dFPS = 0.0;
	int m_iSkippingVelocity;
	float* m_fAudioBuffer;

	double m_dAttack = 1.0;
	double m_dRelease = 0.005;

	double m_iVelThreshLow = 0;
	int m_iVelThreshUpp = 127;

 // Playback resampling: the synth generates the canonical (1x) timeline and the read side scales the source hop by m_dReadSpeed, so a speed change takes effect immediately instead of restarting the generator. Pitch tracks speed (allowed). Reads must happen under m_maMtx.
	double m_dReadSpeed = 1.0;
	double m_dReadFraction = 0.0;

	void SetReadSpeed(double dSpeed);

	double m_dBufferSeconds = 0.0;
	double m_dPlayerTime = 0.0;

 // Underrun handling: when the ring runs out of generated audio, repeat the last chunk of audio instead of going silent, until >= a full chunk of new audio has been generated (m_bUnderrunRepeat). Chunk length is dynamic (m_iRepeatFrames), set from the UI (default 12000 frames = 0.25s @ 48kHz).
	bool m_bUnderrunRepeat = false;
	int m_iRepeatFrames = 12000; // repeat chunk length in frames (48kHz)
	bool m_bInUnderrunStall = false;
	int m_iRepeatOffset = 0; // position within the repeating chunk window
	int m_iStallAnchor = 0; // window END anchor: last generated frame at stall entry
 // Audio is the master clock: while a stall is active the game's song clock must NOT advance (read on the game thread, written by the audio thread), otherwise the visuals pull ahead during the repeat and the audio is permanently delayed after it recovers. Set/cleared inside Read().
	std::atomic_bool m_bStallActive = false;

 // Sync-extension: the prerender audio playhead is the master clock. When this option is enabled the game snaps the song clock to the audio position on every frame, so the visuals skip to exactly where the audio is - a wall (stalled tail-repeat) just holds both in place, and drift is re-absorbed next frame instead of accumulating into a permanent delay. Toggled from the prerender audio settings.
	bool m_bExtendVisualsOnSkip = true;

	static WAVEFORMATEX m_wfFormat;
	std::vector<MIDIChannelEvent> m_vEvents;

	bool m_bPaused = true;
	bool m_bRequestedAudioCancel = false;

	friend class AudioBufferStream;
	AudioBufferStream m_asAudioStream;

	std::thread* m_tGeneratorThread;
	std::chrono::steady_clock::time_point m_tGenStart;

	int GetSkippingVelocity()
	{
		return 0;
	}

	static void Init()
	{
		m_wfFormat.nSamplesPerSec = 48000;
		m_wfFormat.nChannels = 2;
		m_wfFormat.nBlockAlign = (2 * 8) / 8;
		m_wfFormat.wBitsPerSample = 8;
		m_wfFormat.nAvgBytesPerSec = m_wfFormat.nSamplesPerSec * (2 * 8) / 8;
		m_wfFormat.wFormatTag = WAVE_FORMAT_PCM;
		BASSMIDI::InitBASS(m_wfFormat);
	}

	MIDIAudio(int bufferLength);
	~MIDIAudio()
	{
		//Stop();
		KillLastGenerator();
		delete &m_asAudioStream;
	}

	void Start(double time, std::vector<MIDIChannelEvent>* events, double speed, int start = 0);
	void Stop();
	void Reset();
	void StartRender(long long llStartTime, bool force, std::vector<MIDIChannelEvent>* events, double speed = 1.0, long long iStartPos = 0);
 // Owner of the event pool the generator reads; set before Start/StartRender.
	const MIDI* m_pMIDI = nullptr;
	void LoadSoundfont(const wchar_t* path);

	void SyncPlayer(double time, double speed);
	void ResizeBuffer(int size);

	int m_iMaxAheadFrames = 48000 * 30;
	void SetMaxAheadMs(int ms);
	void EnsureBufferCapacity(int minFrames);

	double GetPlayerTime() { return m_dStartTime + m_iBufferReadPos / 48000.0; }
	double GetBufferSeconds() { return max(0, m_iBufferWritePos - m_iBufferReadPos) / 48000.0; }
	bool IsAudioStarted() { return m_bAudioStarted; }
 // Diagnostics for the pianoroll overlay. Thread-unsafe by design; read-ownly.
	unsigned long long GetBufferUnderruns() { return m_ullBufferUnderruns; }
	int GetBufferWritePos() { return m_iBufferWritePos; }
	int GetBufferReadPos() { return m_iBufferReadPos; }

private:
	int m_iBufferReadPos = 0;
	int m_iBufferWritePos = 0;
	int m_iBufferLength;
	bool m_bAudioStarted = false;
	unsigned long long m_ullBufferUnderruns = 0;

	bool m_bAwaitingReset = false;
	std::chrono::duration<long long, std::milli> lastReadTime = std::chrono::time_point_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now()
	).time_since_epoch();

	BASSMIDI* m_bBass = nullptr;

	static void WrappedCopy(float* src, int pos, int srcCount, float* dst, int pos2, int count);
	bool BassWriteWrapped(BASSMIDI* bass, int start, int count);
	bool WriteAudioChunked(BASSMIDI* bass, int frames);

	void GeneratorFunc(double speed, double time, std::vector<MIDIChannelEvent>* events, int start = 0);

	void KillLastGenerator();

	// Synth-death tracking: the BASS decode position must advance with every
	// GetData. If it stalls (or errors) the synth is dead even though the ring
	// keeps filling with zeros - the prerender death signature.
	QWORD m_qLastSynthPos = 0;
	int m_iFrozenFrames = 0;
};