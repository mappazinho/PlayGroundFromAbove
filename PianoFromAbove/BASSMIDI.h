#pragma once

#include <windows.h>
#include "bassmidi/bass.h"
#include "bassmidi/bassmidi.h"
#include <mutex>
#include <string>
#include <vector>

class BASSMIDI
{
public:
	HSTREAM m_hsHandle;
 // static WAVEFORMATEX m_wfWaveFormatStatic;

	bool m_bCanSeek;
	long long m_llPosition;
	long long m_llLength;

	static void InitBASS(WAVEFORMATEX format);
	static void DisposeBASS() { BASS_Free(); }

	BASSMIDI(int voices, bool nofx);
	~BASSMIDI() { BASS_StreamFree(m_hsHandle); }

	static void FreeSoundfont();
	static void LoadSoundfont(const wchar_t* path);
	static std::wstring ResolveSoundfontPath(const std::wstring& path, const std::wstring& dir = L"");
	static std::vector<std::wstring> EnumerateSoundfonts(const std::wstring& dir);

	bool WriteBass(int buflen, unsigned long* progress);
	float* WriteFloatArray(int buflen, unsigned long* progress);
	int KShortMessage(int dwParm1, int sampleoffset);
	DWORD SendEvent(DWORD type, DWORD param, DWORD chan, DWORD tick, DWORD time)
	{
		BASS_MIDI_EVENT ev = {
			type = type,
			param = param,
			chan = chan,
			tick = tick,
			time = time << 3
		};
		DWORD mode = BASS_MIDI_EVENTS_TIME | BASS_MIDI_EVENTS_STRUCT;
		return BASS_MIDI_StreamEvents(m_hsHandle, mode, &ev, 1);
	}

	DWORD SendEventRaw(BYTE data[], DWORD length = 3)
	{
		return BASS_MIDI_StreamEvents(m_hsHandle, BASS_MIDI_EVENTS_RAW | BASS_MIDI_EVENTS_NORSTATUS, data, length);
	}

	void ResetChannels()
	{
		for (int ch = 0; ch < 16; ch++)
		{
			BASS_MIDI_EVENT reset = { MIDI_EVENT_SOUNDOFF, 0, (DWORD)ch, 0, 0 };
			BASS_MIDI_StreamEvents(m_hsHandle, BASS_MIDI_EVENTS_STRUCT, &reset, 1);
		}
	}

	DWORD BassStreamEvents(BASS_MIDI_EVENT events[]) {
		DWORD mode = BASS_MIDI_EVENTS_TIME | BASS_MIDI_EVENTS_STRUCT;
		return BASS_MIDI_StreamEvents(m_hsHandle, mode, events, sizeof(events) / sizeof(BASS_MIDI_EVENT));
	}

	DWORD Read(float buffer[], int offset, int count);
};