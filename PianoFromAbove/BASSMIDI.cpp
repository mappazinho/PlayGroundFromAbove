#include "BASSMIDI.h"
#include "MIDIPreRenderPlayer.h"
#include "Config.h"
#include <algorithm>
#include <vector>
#include <sstream>

static WAVEFORMATEX m_wfWaveFormatStatic = WAVEFORMATEX{};
static std::vector<BASS_MIDI_FONTEX2> m_bmFonts; // one entry per contiguous channel run of each loaded font
static std::vector<HSOUNDFONT> m_bmFontHandles;  // one per loaded font
static std::mutex sfLock;
static std::mutex m_bmMutex;

extern bool g_bReproCustomAudio;

void BASSMIDI::InitBASS(WAVEFORMATEX format) {
	m_wfWaveFormatStatic = format;
	BASS_Free();
	if (!BASS_Init(0, m_wfWaveFormatStatic.nSamplesPerSec, BASS_DEVICE_NOSPEAKER, NULL, NULL))
		MessageBoxW(NULL, L"BASSMIDI failed to initialize, proceeding without audio.\0", L"BASSMIDI Error", MB_ICONERROR);
}

BASSMIDI::BASSMIDI(int voices, bool nofx = true) {
	m_hsHandle = BASS_MIDI_StreamCreate(16,
		BASS_SAMPLE_FLOAT |
		BASS_STREAM_DECODE |
		BASS_MIDI_SINCINTER |
		BASS_MIDI_NOTEOFF1 |
		0x800000,
		m_wfWaveFormatStatic.nSamplesPerSec);

	if (m_hsHandle == -1 || m_hsHandle == 0)
	{
		int err = BASS_ErrorGetCode();
		PRE_DbgLog("STREAMCREATE failed err=%d", err);
		m_bStreamDead = true;
	}

	BASS_ChannelSetAttribute(m_hsHandle, BASS_ATTRIB_MIDI_VOICES, voices);
	BASS_ChannelSetAttribute(m_hsHandle, BASS_ATTRIB_SRC, 3);
	BASS_ChannelSetAttribute(m_hsHandle, BASS_ATTRIB_MIDI_CHANS, 16);
	//BASS_SetConfig(BASS_CONFIG_FLOATDSP, TRUE);

	if (nofx) BASS_ChannelFlags(m_hsHandle, BASS_MIDI_NOFX, BASS_MIDI_NOFX);

	{
		sfLock.lock();
		if (!m_bmFonts.empty() &&
			!BASS_MIDI_StreamSetFonts(m_hsHandle, m_bmFonts.data(), (DWORD)m_bmFonts.size()))
		{
			// Older bassmidi without per-channel mappings: use the whole-stream font list instead.
			PRE_DbgLog("SETFONTS (per-channel) failed err=%d - falling back", BASS_ErrorGetCode());
			std::vector<BASS_MIDI_FONTEX> plain;
			for (const BASS_MIDI_FONTEX2& f : m_bmFonts)
				if (f.minchan == 0 && f.numchan == 16)
					plain.push_back(BASS_MIDI_FONTEX{ f.font, f.spreset, f.sbank, f.dpreset, f.dbank, f.dbanklsb });
			if (plain.empty())
				for (const BASS_MIDI_FONTEX2& f : m_bmFonts)
					plain.push_back(BASS_MIDI_FONTEX{ f.font, f.spreset, f.sbank, f.dpreset, f.dbank, f.dbanklsb });
			if (!BASS_MIDI_StreamSetFonts(m_hsHandle, plain.data(), (DWORD)plain.size()))
				PRE_DbgLog("SETFONTS fallback failed err=%d", BASS_ErrorGetCode());
		}
		sfLock.unlock();
	}
	
}

static bool IsSoundfontFile(const std::wstring& filename)
{
	if (filename.length() < 4) return false;
	std::wstring lower = filename;
	for (auto& c : lower) c = towlower(c);
	if (lower.substr(lower.length() - 4) == L".sf2") return true;
	if (lower.substr(lower.length() - 4) == L".sf3") return true;
	if (lower.substr(lower.length() - 4) == L".sfz") return true;
	if (lower.length() >= 8 && lower.substr(lower.length() - 8) == L".sf2pack") return true;
	return false;
}

std::vector<std::wstring> BASSMIDI::EnumerateSoundfonts(const std::wstring& dir)
{
	std::vector<std::wstring> results;
	if (dir.empty()) return results;
	std::wstring searchPattern = dir;
	if (searchPattern.back() != L'\\' && searchPattern.back() != L'/')
		searchPattern += L"\\";
	std::wstring pattern = searchPattern + L"*.*";

	WIN32_FIND_DATAW fd;
	HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
	if (hFind != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				if (IsSoundfontFile(fd.cFileName))
				{
					results.push_back(searchPattern + fd.cFileName);
				}
			}
		} while (FindNextFileW(hFind, &fd));
		FindClose(hFind);
	}
	return results;
}

static std::wstring FindFirstSoundfontInDir(const std::wstring& dir)
{
	std::vector<std::wstring> sfs = BASSMIDI::EnumerateSoundfonts(dir);
	if (!sfs.empty()) return sfs[0];
	return L"";
}

std::wstring BASSMIDI::ResolveSoundfontPath(const std::wstring& path, const std::wstring& dir)
{
 // 1. Check explicit file path if provided
	if (!path.empty())
	{
		DWORD attr = GetFileAttributesW(path.c_str());
		if (attr != INVALID_FILE_ATTRIBUTES)
		{
			if (attr & FILE_ATTRIBUTE_DIRECTORY)
			{
				std::wstring sfInDir = FindFirstSoundfontInDir(path);
				if (!sfInDir.empty()) return sfInDir;
			}
			else
			{
				return path; // Valid soundfont file path
			}
		}
	}

 // 2. Check explicit directory if provided
	if (!dir.empty())
	{
		std::wstring sfInDir = FindFirstSoundfontInDir(dir);
		if (!sfInDir.empty()) return sfInDir;
	}

 // 3. Auto-detect from ExeDir/Soundfonts/ or ExeDir/
	WCHAR wchExe[MAX_PATH];
	GetModuleFileNameW(NULL, wchExe, MAX_PATH);
	std::wstring exeDir = std::wstring(wchExe);
	size_t pos = exeDir.find_last_of(L"\\/");
	if (pos != std::wstring::npos)
		exeDir = exeDir.substr(0, pos + 1);

 // Check ExeDir/Soundfonts/
	std::wstring sfInSoundfontsDir = FindFirstSoundfontInDir(exeDir + L"Soundfonts");
	if (!sfInSoundfontsDir.empty()) return sfInSoundfontsDir;

 // Check ExeDir/
	std::wstring sfInExeDir = FindFirstSoundfontInDir(exeDir);
	if (!sfInExeDir.empty()) return sfInExeDir;

	return path;
}

void BASSMIDI::FreeSoundfont()
{
	for (HSOUNDFONT font : m_bmFontHandles)
		BASS_MIDI_FontFree(font);
	m_bmFontHandles.clear();
	m_bmFonts.clear();
}

struct SfMemState { const unsigned char* p; QWORD len; QWORD pos; };

static void CALLBACK SfMemClose(void* user) { ((SfMemState*)user)->pos = 0; }
static QWORD CALLBACK SfMemLen(void* user) { return ((SfMemState*)user)->len; }
static DWORD CALLBACK SfMemRead(void* buffer, DWORD length, void* user)
{
	SfMemState* s = (SfMemState*)user;
	QWORD avail = s->len - s->pos;
	DWORD n = (DWORD)((avail < (QWORD)length) ? avail : (QWORD)length);
	if (n) { memcpy(buffer, s->p + s->pos, n); s->pos += n; }
	return n;
}
static BOOL CALLBACK SfMemSeek(QWORD offset, void* user)
{
	SfMemState* s = (SfMemState*)user;
	if (offset > s->len) return FALSE;
	s->pos = offset;
	return TRUE;
}

static HSOUNDFONT LoadSoundfontMem(const std::wstring& path)
{
	HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return 0;
	LARGE_INTEGER liSize = {};
	GetFileSizeEx(hFile, &liSize);
	if (liSize.QuadPart <= 0 || liSize.QuadPart > (LONGLONG)0x7FFFFFFF) { CloseHandle(hFile); return 0; }
	unsigned char* pBuf = (unsigned char*)malloc((size_t)liSize.QuadPart);
	if (!pBuf) { CloseHandle(hFile); return 0; }
	DWORD dwRead = 0;
	BOOL bOk = ReadFile(hFile, pBuf, (DWORD)liSize.QuadPart, &dwRead, NULL);
	CloseHandle(hFile);
	if (!bOk || dwRead != (DWORD)liSize.QuadPart) { free(pBuf); return 0; }

	// Per-font state; like the buffer it stays allocated for the font's lifetime.
	SfMemState* state = new SfMemState{ pBuf, (QWORD)liSize.QuadPart, 0 };
	BASS_FILEPROCS procs = { SfMemClose, SfMemLen, SfMemRead, SfMemSeek };
	HSOUNDFONT font = BASS_MIDI_FontInitUser(&procs, state, 0);
	if (font == 0) { free(pBuf); delete state; }
	return font;
}

void BASSMIDI::LoadSoundfont(const wchar_t* path)
{
	LoadSoundfonts({ FontSpec{ path ? path : L"", 0xFFFF } });
}

void BASSMIDI::LoadSoundfonts(const std::vector<FontSpec>& specs)
{
	std::wstring errors;
	{
		std::lock_guard<std::mutex> lock(sfLock);
		FreeSoundfont();

		// No usable entries: fall back to auto-detecting a soundfont for every channel.
		std::vector<FontSpec> list;
		for (const FontSpec& s : specs)
			if (!s.path.empty() && (s.chanMask & 0xFFFF)) list.push_back(s);
		if (list.empty())
		{
			std::wstring autoPath = ResolveSoundfontPath(L"");
			if (!autoPath.empty()) list.push_back(FontSpec{ autoPath, 0xFFFF });
		}

		for (const FontSpec& s : list)
		{
			// Don't let a missing entry get silently replaced by an auto-detected font.
			HSOUNDFONT font = 0;
			std::wstring resolved = s.path;
			if (GetFileAttributesW(s.path.c_str()) != INVALID_FILE_ATTRIBUTES)
			{
				resolved = ResolveSoundfontPath(s.path);
				font = BASS_MIDI_FontInit(resolved.c_str(), 0);
				if (font == 0)
					font = LoadSoundfontMem(resolved);
			}
			if (font == 0)
			{
				errors += L"\n" + s.path + L"  (err " + std::to_wstring(BASS_ErrorGetCode()) + L")";
				continue;
			}

			BASS_MIDI_FontLoad(font, -1, -1);
			m_bmFontHandles.push_back(font);

			// One mapping per contiguous run of channels.
			const unsigned mask = s.chanMask & 0xFFFF;
			for (int ch = 0; ch < 16; )
			{
				if (!(mask & (1u << ch))) { ch++; continue; }
				int run = 0;
				while (ch + run < 16 && (mask & (1u << (ch + run)))) run++;
				BASS_MIDI_FONTEX2 fx = {};
				fx.font = font;
				fx.spreset = -1;
				fx.sbank = -1;
				fx.dpreset = -1;
				fx.dbank = 0;
				fx.dbanklsb = 0;
				fx.minchan = ch;
				fx.numchan = run;
				m_bmFonts.push_back(fx);
				ch += run;
			}
		}
	}
	if (!errors.empty())
		MessageBoxW(NULL, (L"Soundfont failed to load from:" + errors).c_str(), L"Soundfont Load Error", MB_ICONWARNING);
}

void BASSMIDI::LoadConfiguredSoundfonts()
{
	std::vector<FontSpec> specs;
	for (const SoundfontEntry& e : Config::GetConfig().GetAudioSettings().vSoundfonts)
		if (e.bEnabled)
			specs.push_back(FontSpec{ e.sPath, e.uChanMask });
	LoadSoundfonts(specs);
}

bool BASSMIDI::WriteBass(int buflen, unsigned long *progress)
{
	buflen <<= 3;
	unsigned char* buf;

	DWORD ret = BASS_ChannelGetData(m_hsHandle, &buf, buflen);
	if (ret > 0)
	{
		(*progress) += (unsigned int)ret;
		return true;
	}
	else
	{
		int err = BASS_ErrorGetCode();
		return false;
	}
}

float* BASSMIDI::WriteFloatArray(int buflen, unsigned long* progress)
{
	unsigned char* buf = (unsigned char*)malloc(buflen * 4 * sizeof(unsigned char));
	float* flt = (float*)malloc(buflen * sizeof(float));

	DWORD ret = BASS_ChannelGetData(m_hsHandle, &buf, buflen * 4);
	if (ret > 0) {
		(*progress) += (unsigned int)ret;
		memcpy(flt, buf, sizeof(buf));
		return flt;
	}
	else
	{
		int err = BASS_ErrorGetCode();
		return nullptr;
	}
}

int BASSMIDI::KShortMessage(int dwParam1, int sampleoffset)
{
	if ((unsigned char)dwParam1 == 0xFF) return 1;

	unsigned char cmd = (unsigned char)dwParam1;

	BASS_MIDI_EVENT ev;

	if (cmd < 0xA0)
	{

		ev.event = MIDI_EVENT_NOTE;
		ev.param = cmd < 0x90 ? (unsigned char)(dwParam1 >> 8) : (unsigned short)(dwParam1 >> 8);
		ev.chan = (int)dwParam1 & 0xF;
		ev.tick = 0;
		ev.pos = sampleoffset << 3;
	}
	else if (cmd < 0xB0)
	{
		ev.event = MIDI_EVENT_KEYPRES;
		ev.param = (unsigned short)dwParam1 >> 8;
		ev.chan = (int)dwParam1 & 0xF;
		ev.tick = 0;
		ev.pos = sampleoffset << 3;
	}
	else if (cmd < 0xC0)
	{
  // TODO
		return 0;
	}
	else if (cmd < 0xD0)
	{
		ev.event = MIDI_EVENT_PROGRAM;
		ev.param = (unsigned char)(dwParam1 >> 8);
		ev.chan = (int)dwParam1 & 0xF;
		ev.tick = 0;
		ev.pos = sampleoffset << 3;
	}
	else if (cmd < 0xE0)
	{
		ev.event = MIDI_EVENT_CHANPRES;
		ev.param = (unsigned char)(dwParam1 >> 8);
		ev.chan = (int)dwParam1 & 0xF;
		ev.tick = 0;
		ev.pos = sampleoffset << 3;
	}
	else if (cmd == 0xF0)
	{
		ev.event = MIDI_EVENT_PITCH;
		ev.param = (int)((unsigned char)(dwParam1 >> 16) | ((dwParam1 & 0x7F00) >> 1));
		ev.chan = (int)dwParam1 & 0xF;
		ev.tick = 0;
		ev.pos = sampleoffset << 3;
	}
	else return 0;

	BASS_MIDI_EVENT evs[1] = { ev };

	BassStreamEvents(evs);

	return 0;
}

DWORD BASSMIDI::Read(float* buffer, int offset, int count) {
	DWORD size = count * sizeof(float);
	DWORD ret = BASS_ChannelGetData(m_hsHandle, buffer + offset, size | BASS_DATA_FLOAT);

	if (ret != size)
	{
		static int sLogCount = 0;
		if (sLogCount < 30)
		{
			sLogCount++;
			PRE_DbgLog("GETDATA short ret=%u want=%u err=%d", ret, size, BASS_ErrorGetCode());
		}
		if (ret == (DWORD)-1 || ret == 0)
		{
			m_bStreamDead = true;
		}
	}
	return ret / 4;
}