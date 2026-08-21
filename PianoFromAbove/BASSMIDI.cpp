#include "BASSMIDI.h"
#include "MIDIPreRenderPlayer.h"
#include <algorithm>
#include <vector>
#include <sstream>

static WAVEFORMATEX m_wfWaveFormatStatic = WAVEFORMATEX{};
static BASS_MIDI_FONTEX* m_bmFontArr = nullptr;
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
		if (m_bmFontArr != NULL && m_bmFontArr[0].font != 0)
			BASS_MIDI_StreamSetFonts(m_hsHandle, m_bmFontArr, 1);
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

// only one soundfont because i'm lazy lmfao
void BASSMIDI::FreeSoundfont()
{
	if (m_bmFontArr != NULL)
	{
		if (m_bmFontArr[0].font != 0)
			BASS_MIDI_FontFree(m_bmFontArr[0].font);
		free(m_bmFontArr);
		m_bmFontArr = NULL;
	}
}

// ---- memory-backed soundfont loading ----------------------------------------
// Some bass.dll builds in the wild cannot open files by path (BASS_ERROR_FILEOPEN
// from BASS_MIDI_FontInit / BASS_StreamCreateFile even for valid files). Loading
// the sf2 into memory and handing it to BASS via BASS_MIDI_FontInitUser sidesteps
// that entirely and is identical in behaviour for BASSMIDI synthesis.
struct SfMemState { const unsigned char* p; QWORD len; QWORD pos; };
static SfMemState g_sfMem;

static void CALLBACK SfMemClose(void* user) { (void)user; g_sfMem.pos = 0; }
static QWORD CALLBACK SfMemLen(void* user) { (void)user; return g_sfMem.len; }
static DWORD CALLBACK SfMemRead(void* buffer, DWORD length, void* user)
{
	(void)user;
	QWORD avail = g_sfMem.len - g_sfMem.pos;
	DWORD n = (DWORD)((avail < (QWORD)length) ? avail : (QWORD)length);
	if (n) { memcpy(buffer, g_sfMem.p + g_sfMem.pos, n); g_sfMem.pos += n; }
	return n;
}
static BOOL CALLBACK SfMemSeek(QWORD offset, void* user)
{
	(void)user;
	if (offset > g_sfMem.len) return FALSE;
	g_sfMem.pos = offset;
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

	g_sfMem.p = pBuf;
	g_sfMem.len = liSize.QuadPart;
	g_sfMem.pos = 0;
	BASS_FILEPROCS procs = { SfMemClose, SfMemLen, SfMemRead, SfMemSeek };
	HSOUNDFONT font = BASS_MIDI_FontInitUser(&procs, NULL, 0);
	if (font == 0) free(pBuf);
	// BASS keeps the font data alive via the procs until FontFree; the buffer is
	// intentionally leaked (tiny: <1KB) rather than freed while BASS may still
	// touch it during teardown races.
	return font;
}

void BASSMIDI::LoadSoundfont(const wchar_t* path)
{
	std::wstring resolvedPath = ResolveSoundfontPath(path ? path : L"");
	{
		sfLock.lock();
		FreeSoundfont();
		HSOUNDFONT font = 0;
		if (!resolvedPath.empty())
		{
			font = BASS_MIDI_FontInit(resolvedPath.c_str(), 0);
			if (font == 0)
				font = LoadSoundfontMem(resolvedPath);
		}

		if (font != 0)
		{
			BASS_MIDI_FONTEX* fonts = (BASS_MIDI_FONTEX*)calloc(1, sizeof(BASS_MIDI_FONTEX));
			fonts[0].font = font;
			fonts[0].spreset = -1;
			fonts[0].sbank = -1;
			fonts[0].dpreset = -1;
			fonts[0].dbank = 0;
			fonts[0].dbanklsb = 0;

			BASS_MIDI_FontLoad(font, -1, -1);
			m_bmFontArr = fonts;
		}
		else
		{
			m_bmFontArr = NULL;
			if (!resolvedPath.empty())
			{
				std::wstringstream err;
				err << L"Soundfont failed to load from: " << resolvedPath << L"\nErr Code: " << BASS_ErrorGetCode() << L"\0";
				MessageBoxW(NULL, err.str().c_str(), L"Soundfont Load Error", MB_ICONWARNING);
			}
		}
		sfLock.unlock();
	}
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
		// Diagnostic: GetData must return exactly the requested bytes on a
		// decode stream. Short returns mean the stream has ended or stalled -
		// the generator silently zero-fills the remainder of the chunk.
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