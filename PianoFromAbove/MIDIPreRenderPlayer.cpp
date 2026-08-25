#include "MIDIPreRenderPlayer.h"
#include "Globals.h"

#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <windows.h>

MIDIAudio* PRE_MIDIAudio = nullptr;
std::atomic<float> g_fGameFPS = 1000.0f;
std::atomic<bool> g_bGenDead = false;

static SDL_AudioSpec s_wanted;
static SDL_AudioSpec s_obtained;

static double s_bufferSecs = 60.0;
static int s_bufferSize = 48000 * 2;
static int s_sampleRate = 48000;

static float* s_pRingBuffer = nullptr;

// Set by the SDL audio callback on every invocation. The game thread polls this via
// PRE_AudioStalled() to detect a stalled audio device (observed on this machine: the
// DirectSound waveout thread parks on a buffer event that never fires again after a
// display/GPU hiccup, leaving the app alive but silent).
static std::atomic<long long> s_llLastCallbackTick{ 0 };

static bool PRE_OpenDevice();

static CRITICAL_SECTION s_csLog;

void PRE_DbgLog(const char* format, ...)
{
	if (!g_bLoggingEnabled.load(std::memory_order_relaxed))
		return;
 // Lazy init: PRE_DbgLog may be called before PRE_InitAudio() (e.g. from the UpdatePreRenderAudio init branch). Magic-static guard is thread-safe. Also starts the log file fresh per process: PRE_InitAudio only runs when prerender audio is enabled, so without this the live-mode sessions appended to a stale log from an earlier (possibly different-build) run.
	static bool bLockInit = [] {
		InitializeCriticalSection(&s_csLog);
		wchar_t path[MAX_PATH] = {};
		GetTempPathW(_countof(path), path);
		wcscat_s(path, L"pfa_prerender.log");
		DeleteFileW(path);
		return true;
	}();
	(void)bLockInit;

	__int64 ms = GetTickCount64();
	va_list args;
	va_start(args, format);
	EnterCriticalSection(&s_csLog);
	FILE* f = nullptr;
	wchar_t path[MAX_PATH] = {};
	GetTempPathW(_countof(path), path);
	wcscat_s(path, L"pfa_prerender.log");
	_wfopen_s(&f, path, L"a");
	if (f)
	{
		fprintf(f, "[%6lld.%03d] ", (long long)(ms / 1000), (int)(ms % 1000));
		vfprintf(f, format, args);
		fprintf(f, "\n");
		fclose(f);
	}
	LeaveCriticalSection(&s_csLog);
	va_end(args);
}

void PRE_FillAudio(void* udata, Uint8* stream, int len)
{
	s_llLastCallbackTick.store((long long)SDL_GetTicks64(), std::memory_order_relaxed);
	SDL_memset(stream, 0, len);
	if (PRE_MIDIAudio && s_pRingBuffer)
	{
		PRE_MIDIAudio->m_asAudioStream.ReadLM(s_pRingBuffer, 0, len / sizeof(float));

  // Diagnostic: click detector. A waveform discontinuity (repeat-tail lap, cross-fade seam, or unwritten zero bed) shows up as a step change of >kClickThresh on consecutive samples. Count them per callback and report the peak once per second so crackles can be correlated with stall/repeat/snap events instead of guessed.
		const double kClickThresh = 0.35;
		static int s_framesSec = 0;
		static double s_peakDet = 0;
		static long long s_clickFrames = 0;
		static long long s_clickCallbacks = 0;
		static double s_rmsSumSq = 0;
		static int s_levelFrames = 0;
		static float s_levelPeak = 0;
		const int n = len / (int)sizeof(float);
		{
			float* p = s_pRingBuffer;
			double peak = 0;
			int clicks = 0;
			double sumSq = 0;
			float lvlPeak = 0;
			for (int i = 1; i < n; i++)
			{
				double det = (double)fabs((double)p[i] - (double)p[i - 1]);
				if (det > peak) peak = det;
				if (det > kClickThresh) clicks++;
			}
			for (int i = 0; i < n; i++)
			{
				float v = p[i];
				sumSq += (double)v * (double)v;
				float a = v < 0 ? -v : v;
				if (a > lvlPeak) lvlPeak = a;
			}
			if (clicks > 0)
			{
				s_clickFrames += clicks;
				s_clickCallbacks++;
			}
			if (peak > s_peakDet) s_peakDet = peak;
			s_rmsSumSq += sumSq;
			s_levelFrames += n;
			if (lvlPeak > s_levelPeak) s_levelPeak = lvlPeak;
		}
		s_framesSec += n / 2;
		if (s_framesSec >= 48000)
		{
			s_framesSec = 0;
			PRE_DbgLog("CLK peak=%.2f clickFrames=%lld inCallbacks=%lld",
				s_peakDet, s_clickFrames, s_clickCallbacks);
			s_peakDet = 0;
			s_clickFrames = 0;
			s_clickCallbacks = 0;

   // Diagnostic: log the ring state once per second from the audio thread, including the actual RMS level of the samples delivered. Level in dBFS distinguishes "callback running but the synth produced silence" (rms around -120dB) from real playback (roughly -45dB..-10dB), which the click detector cannot: healthy quiet music has no discontinuities either.
			double rms = sqrt(s_rmsSumSq / (double)max(1, s_levelFrames));
			double rmsDb = 20.0 * log10(rms + 1e-12);
			PRE_DbgLog("CB r=%d w=%d paused=%d bufSecs=%.2f rms=%.1fdB peak=%.3f",
				PRE_MIDIAudio->GetBufferReadPos(),
				PRE_MIDIAudio->GetBufferWritePos(),
				PRE_MIDIAudio->m_bPaused,
				PRE_MIDIAudio->GetBufferSeconds(),
				rmsDb, s_levelPeak);
			s_rmsSumSq = 0;
			s_levelFrames = 0;
			s_levelPeak = 0;
		}
	}
	SDL_memcpy(stream, (const Uint8*)s_pRingBuffer, len);
}

void PRE_Reset()
{
	if (s_pRingBuffer)
		memset(s_pRingBuffer, 0, (size_t)s_bufferSize * sizeof(float) * (size_t)s_bufferSecs);
}

void PRE_TouchAudio()
{
	s_llLastCallbackTick.store((long long)SDL_GetTicks64(), std::memory_order_relaxed);
}

bool PRE_AudioStalled()
{
	// A paused device is intentionally not producing callbacks. The game thread
	// touches the timestamp while paused, and PRE_OpenDevice() establishes a fresh
	// startup grace period before the first callback is expected.
	if (SDL_GetAudioStatus() != SDL_AUDIO_PLAYING)
		return false;
	const long long now = (long long)SDL_GetTicks64();
	const long long last = s_llLastCallbackTick.load(std::memory_order_relaxed);
	if (last <= 0) {
		PRE_TouchAudio();
		return false;
	}
	return (now - last) > 2500;
}

void PRE_RestartAudio()
{
	const long long last = s_llLastCallbackTick.load(std::memory_order_relaxed);
	PRE_DbgLog("RestartAudio: err='%s' lastCB=%lldms",
		SDL_GetError() ? SDL_GetError() : "(null)", last);
	SDL_CloseAudio();
	PRE_OpenDevice();
}

static bool PRE_OpenDevice()
{
	if (SDL_OpenAudio(&s_wanted, NULL) < 0)
	{
		PRE_DbgLog("PRE_InitAudio: SDL_OpenAudio failed: %s", SDL_GetError());
		return false;
	}
	SDL_PauseAudio(1);
	// Opening/reopening is not a stall. Give WASAPI time to dispatch its first
	// callback after the game thread resumes the device.
	PRE_TouchAudio();
	PRE_DbgLog("PRE_InitAudio: SDL audio driver='%s'", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "(null)");
	return true;
}

int PRE_InitAudio()
{
 // WASAPI over DirectSound: the directsound waveout thread can park forever on a
 // dead buffer event after a display/GPU hiccup (observed: callback stops, process
 // alive, silence). WASAPI's audio thread waits on a shutdown event too, so device
 // re-open from the game thread is safe and the backend self-heals on device loss.
	SDL_SetHint(SDL_HINT_AUDIODRIVER, "wasapi");

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
		return -1;

	if (!s_pRingBuffer)
	{
		s_pRingBuffer = (float*)malloc((size_t)s_bufferSize * sizeof(float) * (size_t)s_bufferSecs);
		memset(s_pRingBuffer, 0, (size_t)s_bufferSize * sizeof(float) * (size_t)s_bufferSecs);
	}

	s_wanted.freq = s_sampleRate;
	s_wanted.format = AUDIO_F32;
	s_wanted.channels = 2;
	s_wanted.samples = 2048;
	s_wanted.padding = 0;
	s_wanted.callback = PRE_FillAudio;
	s_wanted.userdata = NULL;

	s_obtained = s_wanted;

	if (!PRE_OpenDevice())
		return -1;
	return s_sampleRate;
}