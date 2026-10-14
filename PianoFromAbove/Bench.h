#pragma once
// Headless, deterministic benchmark / regression mode:  PGFA.exe -bench script.ini
//
// Loads one MIDI with default settings (the user's config is neither read nor written), drives the song clock
// with a fixed time step (no real time involved), optionally sweeps note zoom / speed and seeks, and writes
//   <out>\result.txt   timing statistics (frame time percentiles, per-phase averages)
//   <out>\hashes.txt   a 64-bit hash of selected frames
//   <out>\fNNNNN.png   (+ .bgra raw) selected frames, and when `baseline=<dir>` is given a pixel comparison
//                      against the raw frames of an earlier run (diff images + counts in result.txt).
// then exits. Script format: one `key=value` per line, `#` starts a comment. See BenchParseScript.
#include <Windows.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>

struct BenchJump { int frame; double songSec; };

struct BenchState
{
    bool active = false;
    // ---- script ----
    std::wstring midi, outDir, baselineDir;
    int renderer = 1;                 // 0 = DirectX 11, 1 = DirectX 12 (the app default)
    int width = 1280, height = 720;   // size of the render window's client area
    int fps = 60;                     // fixed time step: 1 / fps song seconds per frame (x speed)
    int frames = 300, warmup = 30;    // frames to run / frames excluded from the statistics
    double startSec = 0.0;            // song time of frame 0
    double nspeed0 = 100.0, nspeed1 = 100.0; // note zoom in percent, linear sweep over the run
    double speed0 = 100.0, speed1 = 100.0;   // playback speed in percent, linear sweep over the run
    int sliding = -1;                 // -1 = build default, 0 = off, 1 = on (the sliding note window)
    bool ui = false;                  // draw the menu / toolbar / stats (off keeps the frames deterministic)
    bool keys = true;                 // draw the keyboard
    bool dual = false, horizontal = false; // piano roll mode
    bool tick = false;                // tick based time
    double corrupt = 0.0;             // corruptor amount 0..1
    std::set<int> hide;               // tracks hidden from the start
    std::vector<std::pair<int,int>> hideAt; // (frame, track): hide a track mid-run
    std::vector<BenchJump> jumps;     // seek to song second at frame
    std::set<int> dump;               // frames saved as png + raw
    int hashEvery = 0;                // hash every Nth frame (0 = only the dumped ones)
    std::string label;
    // ---- runtime ----
    int frame = 0;
    bool started = false;
    bool finished = false;
    bool capturedThisFrame = false;
    std::chrono::steady_clock::time_point tFrameStart, tPrevFrameStart;
    std::vector<double> wallMs;       // frame-to-frame wall time (measured frames only)
    std::vector<uint64_t> frameRows;  // event rows scanned per frame (all frames)
    std::vector<double> frameWall;    // wall time per frame (all frames)
    std::vector<char> frameCap;       // frame included a screenshot
    uint64_t prevRows = 0;
    std::vector<std::string> hashLines, compareLines;
    int compareDiff = 0, compareSame = 0;
    uint64_t captureNs = 0;
};

inline BenchState g_Bench;
inline bool g_bBenchMode = false;

inline std::wstring BenchWide(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

inline std::string BenchNarrow(const std::wstring& w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// "a" or "a:b"
inline void BenchParseRange(const std::string& v, double& a, double& b)
{
    a = atof(v.c_str());
    const size_t c = v.find(':');
    b = c == std::string::npos ? a : atof(v.c_str() + c + 1);
}

inline bool BenchParseScript(const wchar_t* path, BenchState& b)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f)
        return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        text.append(buf, n);
    fclose(f);

    size_t pos = 0;
    while (pos < text.size())
    {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t s0 = line.find_first_not_of(" \t");
        if (s0 == std::string::npos || line[s0] == '#' || line[s0] == ';') continue;
        const size_t eq = line.find('=', s0);
        if (eq == std::string::npos) continue;
        std::string key = line.substr(s0, eq - s0), val = line.substr(eq + 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        for (char& c : key) c = (char)tolower((unsigned char)c);

        if (key == "midi") b.midi = BenchWide(val);
        else if (key == "out") b.outDir = BenchWide(val);
        else if (key == "baseline") b.baselineDir = BenchWide(val);
        else if (key == "renderer") b.renderer = (val == "dx12" || val == "12" || val == "DX12") ? 1 : 0;
        else if (key == "width") b.width = atoi(val.c_str());
        else if (key == "height") b.height = atoi(val.c_str());
        else if (key == "fps") b.fps = (std::max)(1, atoi(val.c_str()));
        else if (key == "frames") b.frames = (std::max)(1, atoi(val.c_str()));
        else if (key == "warmup") b.warmup = (std::max)(0, atoi(val.c_str()));
        else if (key == "start") b.startSec = atof(val.c_str());
        else if (key == "nspeed") BenchParseRange(val, b.nspeed0, b.nspeed1);
        else if (key == "speed") BenchParseRange(val, b.speed0, b.speed1);
        else if (key == "sliding") b.sliding = atoi(val.c_str());
        else if (key == "ui") b.ui = atoi(val.c_str()) != 0;
        else if (key == "keys") b.keys = atoi(val.c_str()) != 0;
        else if (key == "dual") b.dual = atoi(val.c_str()) != 0;
        else if (key == "tick") b.tick = atoi(val.c_str()) != 0;
        else if (key == "corrupt") b.corrupt = atof(val.c_str());
        else if (key == "horizontal") b.horizontal = atoi(val.c_str()) != 0;
        else if (key == "hideat")
        {
            const size_t c = val.find(':');
            if (c != std::string::npos) b.hideAt.push_back({ atoi(val.c_str()), atoi(val.c_str() + c + 1) });
        }
        else if (key == "hide")
        {
            const char* p = val.c_str();
            while (*p) { b.hide.insert(atoi(p)); const char* c = strchr(p, ','); if (!c) break; p = c + 1; }
        }
        else if (key == "label") b.label = val;
        else if (key == "hashevery") b.hashEvery = atoi(val.c_str());
        else if (key == "jump")
        {
            const size_t c = val.find(':');
            if (c != std::string::npos)
                b.jumps.push_back({ atoi(val.c_str()), atof(val.c_str() + c + 1) });
        }
        else if (key == "dump")
        {
            // comma separated frame numbers
            const char* p = val.c_str();
            while (*p)
            {
                b.dump.insert(atoi(p));
                const char* c = strchr(p, ',');
                if (!c) break;
                p = c + 1;
            }
        }
        else if (key == "dumpevery")
        {
            const int step = (std::max)(1, atoi(val.c_str()));
            for (int fr = 0; fr < b.frames; fr += step) b.dump.insert(fr);
        }
    }
    return !b.midi.empty() && !b.outDir.empty();
}

inline uint64_t BenchHash(const void* data, size_t n)
{
    // FNV-1a over 8-byte words (then the tail): fast enough for a few MB per frame
    const uint64_t* p = (const uint64_t*)data;
    uint64_t h = 1469598103934665603ull;
    const size_t words = n / 8;
    for (size_t i = 0; i < words; i++) { h ^= p[i]; h *= 1099511628211ull; }
    const uint8_t* t = (const uint8_t*)data + words * 8;
    for (size_t i = 0; i < n % 8; i++) { h ^= t[i]; h *= 1099511628211ull; }
    return h;
}

inline bool BenchSavePng(const std::wstring& path, const uint8_t* bgra, UINT w, UINT h, bool forceOpaque = true)
{
    static thread_local bool s_coInit = false;
    if (!s_coInit) { CoInitializeEx(nullptr, COINIT_MULTITHREADED); s_coInit = true; }
    std::vector<uint8_t> copy;
    const uint8_t* pixels = bgra;
    if (forceOpaque)
    {
        copy.assign(bgra, bgra + (size_t)w * h * 4);
        for (size_t i = 3; i < copy.size(); i += 4) copy[i] = 255;
        pixels = copy.data();
    }
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, &props)) &&
        SUCCEEDED(frame->Initialize(props)) &&
        SUCCEEDED(frame->SetSize(w, h)))
    {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        frame->SetPixelFormat(&fmt);
        ok = fmt == GUID_WICPixelFormat32bppBGRA &&
             SUCCEEDED(frame->WritePixels(h, w * 4, w * h * 4, (BYTE*)pixels)) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    if (props) props->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    return ok;
}

inline bool BenchWriteFile(const std::wstring& path, const void* data, size_t n)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    fwrite(data, 1, n, f);
    fclose(f);
    return true;
}

inline bool BenchReadFile(const std::wstring& path, std::vector<uint8_t>& out)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    const size_t got = n > 0 ? fread(out.data(), 1, (size_t)n, f) : 0;
    fclose(f);
    return got == (size_t)n;
}

inline std::wstring BenchFramePath(const std::wstring& dir, int frame, const wchar_t* ext)
{
    wchar_t name[64];
    swprintf_s(name, L"\\f%05d%s", frame, ext);
    return dir + name;
}

// Handles one frame that is to be captured: hash, optional png/raw dump, optional comparison with the baseline.
inline void BenchCaptureFrame(int frame, const uint8_t* bgra, int w, int h)
{
    BenchState& b = g_Bench;
    const size_t bytes = (size_t)w * h * 4;
    // alpha is irrelevant (the swap chain ignores it): hash and compare with it forced to 255
    std::vector<uint8_t> px(bgra, bgra + bytes);
    for (size_t i = 3; i < bytes; i += 4) px[i] = 255;
    char line[96];
    snprintf(line, sizeof(line), "%d %016llx", frame, (unsigned long long)BenchHash(px.data(), bytes));
    b.hashLines.push_back(line);

    if (b.dump.count(frame))
    {
        BenchSavePng(BenchFramePath(b.outDir, frame, L".png"), px.data(), (UINT)w, (UINT)h, false);
        BenchWriteFile(BenchFramePath(b.outDir, frame, L".bgra"), px.data(), bytes);
        if (!b.baselineDir.empty())
        {
            std::vector<uint8_t> base;
            if (!BenchReadFile(BenchFramePath(b.baselineDir, frame, L".bgra"), base) || base.size() != bytes)
            {
                snprintf(line, sizeof(line), "frame %d: no baseline frame to compare", frame);
                b.compareLines.push_back(line);
            }
            else
            {
                size_t diffPixels = 0;
                int maxDelta = 0;
                std::vector<uint8_t> diff(bytes, 0);
                for (size_t i = 0; i < bytes; i += 4)
                {
                    int d = 0;
                    for (int c = 0; c < 3; c++) d = (std::max)(d, abs((int)px[i + c] - (int)base[i + c]));
                    if (d)
                    {
                        diffPixels++;
                        maxDelta = (std::max)(maxDelta, d);
                        diff[i] = diff[i + 1] = diff[i + 2] = 255;
                    }
                    diff[i + 3] = 255;
                }
                if (diffPixels == 0) { b.compareSame++; snprintf(line, sizeof(line), "frame %d: IDENTICAL", frame); }
                else
                {
                    b.compareDiff++;
                    snprintf(line, sizeof(line), "frame %d: DIFFERENT pixels=%zu maxChannelDelta=%d", frame, diffPixels, maxDelta);
                    BenchSavePng(BenchFramePath(b.outDir, frame, L"_diff.png"), diff.data(), (UINT)w, (UINT)h, false);
                }
                b.compareLines.push_back(line);
            }
        }
    }
}
