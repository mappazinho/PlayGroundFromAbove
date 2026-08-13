#include "MIDIPreRenderPlayer.h"

#include <cstdarg>
#include <cstdio>
#include <windows.h>

MIDIAudio* PRE_MIDIAudio = nullptr;
std::atomic<float> g_fGameFPS = 1000.0f;

static SDL_AudioSpec s_wanted;
static SDL_AudioSpec s_obtained;

static double s_bufferSecs = 60.0;
static int s_bufferSize = 48000 * 2;
static int s_sampleRate = 48000;

static float* s_pRingBuffer = nullptr;

static CRITICAL_SECTION s_csLog;

void PRE_DbgLog(const char* format, ...)
{
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
		const int n = len / (int)sizeof(float);
		{
			float* p = s_pRingBuffer;
			double peak = 0;
			int clicks = 0;
			for (int i = 1; i < n; i++)
			{
				double det = (double)fabs((double)p[i] - (double)p[i - 1]);
				if (det > peak) peak = det;
				if (det > kClickThresh) clicks++;
			}
			if (clicks > 0)
			{
				s_clickFrames += clicks;
				s_clickCallbacks++;
			}
			if (peak > s_peakDet) s_peakDet = peak;
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

   // Diagnostic: log the ring state once per second from the audio thread.
			PRE_DbgLog("CB r=%d w=%d paused=%d bufSecs=%.2f",
				PRE_MIDIAudio->GetBufferReadPos(),
				PRE_MIDIAudio->GetBufferWritePos(),
				PRE_MIDIAudio->m_bPaused,
				PRE_MIDIAudio->GetBufferSeconds());
		}
	}
	SDL_memcpy(stream, (const Uint8*)s_pRingBuffer, len);
}

void PRE_Reset()
{
	if (s_pRingBuffer)
		memset(s_pRingBuffer, 0, (size_t)s_bufferSize * sizeof(float) * (size_t)s_bufferSecs);
}

int PRE_InitAudio()
{
 // Fresh diagnostics each run.
	wchar_t path[MAX_PATH] = {};
	GetTempPathW(_countof(path), path);
	wcscat_s(path, L"pfa_prerender.log");
	DeleteFileW(path);

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

	if (SDL_OpenAudio(&s_wanted, NULL) < 0)
		return -1;

	SDL_PauseAudio(1);
	return s_sampleRate;
}