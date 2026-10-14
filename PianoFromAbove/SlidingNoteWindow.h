#pragma once
// Sliding note window: a persistent, per-key list of the notes that are on screen (or sounding), kept up to date
// incrementally while the song plays, instead of rebuilding the visible notes from the event list every frame.
//
// Per key the notes are stored in NoteOn order (= event index order = start time order). Each frame only the notes
// that scrolled into view are appended, and the dead ones are trimmed from the front. The notes that actually get
// drawn are then found per key with binary searches over screen rows (see MainScreen::SnwRender), so the per-frame
// cost depends on the number of screen rows, not on how many notes are in view.
#include <vector>
#include <cstdint>
#include <algorithm>
#include <atomic>

struct SnwNote
{
    int64_t start;      // microseconds, exactly MIDI::GetEventTime of the NoteOn
    uint32_t length;    // microseconds, exactly MIDI::GetEventLength
    uint32_t evIdx;     // index of the NoteOn in the song's event list
    uint32_t sister;    // index of the matching NoteOff (the note is dead once that event has been played)
    uint16_t track;
    uint8_t channel;
    uint8_t key;        // MIDI key 0..127
};

struct SnwKey
{
    static const size_t BlockShift = 6;                 // 64 notes per block
    std::vector<SnwNote> notes;                         // [head, size) are live candidates
    size_t head = 0;
    std::vector<int64_t> blockMax;                      // max end time (start + length) per block
    std::vector<int64_t> blockPrefMax;                  // max end time over this and all older blocks

    void Clear()
    {
        notes.clear();
        blockMax.clear();
        blockPrefMax.clear();
        head = 0;
    }

    size_t Size() const { return notes.size() - head; }

    void Append(const SnwNote& n)
    {
        const size_t pos = notes.size();
        notes.push_back(n);
        const int64_t end = n.start + (int64_t)n.length;
        const size_t b = pos >> BlockShift;
        if (b >= blockMax.size())
        {
            blockMax.push_back(end);
            blockPrefMax.push_back(blockPrefMax.empty() ? end : (std::max)(blockPrefMax.back(), end));
        }
        else
        {
            blockMax[b] = (std::max)(blockMax[b], end);
            blockPrefMax[b] = (std::max)(blockPrefMax[b], end);
        }
    }

    // Removes the newest notes whose NoteOn index is above `hi` (the window shrank). The block maxima stay as they
    // were: an over-estimate only makes searches a little slower, never wrong.
    void PopNewerThan(uint32_t hi)
    {
        while (notes.size() > head && notes.back().evIdx > hi)
            notes.pop_back();
        const size_t blocks = (notes.size() + (((size_t)1 << BlockShift) - 1)) >> BlockShift;
        if (blockMax.size() > blocks) { blockMax.resize(blocks); blockPrefMax.resize(blocks); }
    }

    // Drops dead notes (their NoteOff has been played) from the front; compacts the storage now and then.
    void TrimFront(int64_t lo)
    {
        while (head < notes.size() && (int64_t)notes[head].sister < lo)
            head++;
        if (head > 4096 && head * 2 > notes.size())
        {
            notes.erase(notes.begin(), notes.begin() + (ptrdiff_t)head);
            head = 0;
            RebuildBlocks();
        }
    }

    void RebuildBlocks()
    {
        blockMax.clear();
        blockPrefMax.clear();
        for (size_t pos = 0; pos < notes.size(); pos++)
        {
            const int64_t end = notes[pos].start + (int64_t)notes[pos].length;
            const size_t b = pos >> BlockShift;
            if (b >= blockMax.size())
            {
                blockMax.push_back(end);
                blockPrefMax.push_back(blockPrefMax.empty() ? end : (std::max)(blockPrefMax.back(), end));
            }
            else
            {
                blockMax[b] = (std::max)(blockMax[b], end);
                blockPrefMax[b] = (std::max)(blockPrefMax[b], end);
            }
        }
    }
};

// Work counters (bench / tuning): rows searched, notes examined while looking for the owner, blocks skipped.
struct SnwStats { std::atomic<uint64_t> rows{0}, scanned{0}, blockSkips{0}, owners{0}, frames{0}; };
inline SnwStats g_SnwStats;

struct SlidingNoteWindow
{
    bool valid = false;         // the keys reflect the song up to builtHi
    int64_t builtHi = -1;       // every NoteOn with an event index <= builtHi has been appended
    SnwKey keys[128];

    void Invalidate() { valid = false; }
    void ReleaseMemory()
    {
        valid = false;
        builtHi = -1;
        for (SnwKey& k : keys)
        {
            std::vector<SnwNote>().swap(k.notes);
            std::vector<int64_t>().swap(k.blockMax);
            std::vector<int64_t>().swap(k.blockPrefMax);
            k.head = 0;
        }
    }
    void Reset()
    {
        valid = false;
        builtHi = -1;
        for (SnwKey& k : keys) k.Clear();
    }
};

// All NoteOns of events [lo, hi) in event order - the list the dual / horizontal roll samples from (newest first).
// Kept up to date incrementally like the per-key window, so a frame only has to copy the notes it actually draws.
struct SnwGlobal
{
    bool valid = false;
    int64_t hi = 0;                 // events below this index have been appended
    std::vector<SnwNote> notes;     // [head, size) are the notes with an event index in [lo, hi)
    size_t head = 0;

    void Invalidate() { valid = false; }
    void ReleaseMemory()
    {
        valid = false;
        hi = 0;
        head = 0;
        std::vector<SnwNote>().swap(notes);
    }
    size_t Size() const { return notes.size() - head; }
    void TrimFront(int64_t lo)
    {
        while (head < notes.size() && (int64_t)notes[head].evIdx < lo)
            head++;
        if (head > (1u << 20) && head * 2 > notes.size())
        {
            notes.erase(notes.begin(), notes.begin() + (ptrdiff_t)head);
            head = 0;
        }
    }
    void PopFrom(int64_t hiExcl)
    {
        while (notes.size() > head && (int64_t)notes.back().evIdx >= hiExcl)
            notes.pop_back();
    }
};
