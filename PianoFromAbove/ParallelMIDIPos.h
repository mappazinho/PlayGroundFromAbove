#pragma once
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

class ParallelMIDIPos
{
public:
    explicit ParallelMIDIPos(MIDI& midi)
        : m_MIDI(midi), m_heap(HeapLater{ this })
    {
        m_iCurrTick = m_iCurrMicroSec = 0;
        if (m_MIDI.m_Info.iDivision & 0x8000) {
  int iFramesPerSec = -((m_MIDI.m_Info.iDivision | static_cast<int>(0xFFFF0000)) >> 8) * 100;
  if (iFramesPerSec == 2900) iFramesPerSec = 2997;
  const int iTicksPerFrame = m_MIDI.m_Info.iDivision & 0xFF;
  m_bIsStandard = false;
  m_iTicksPerBeat = m_iMicroSecsPerBeat = 0;
  m_iTicksPerSecond = iTicksPerFrame * iFramesPerSec;
        } else {
  m_bIsStandard = true;
  m_iTicksPerSecond = 0;
  m_iTicksPerBeat = m_MIDI.m_Info.iDivision;
  m_iMicroSecsPerBeat = 500000;
        }

        BuildAndSort();
    }

    ~ParallelMIDIPos()
    {
        if (m_sortUiActive)
  g_LoadingProgress.sortWorkerCount.store(0, std::memory_order_release);
    }

    int GetNextEvent(int iMicroSecs, MIDIEvent** pOutMeta, MIDIChannelEvent* pOutRow)
    {
        if (!pOutMeta || !pOutRow)
  return 0;
        *pOutMeta = nullptr;
        *pOutRow = UINT32_MAX;

        if (m_sortUiActive) {
  g_LoadingProgress.sortWorkerCount.store(0, std::memory_order_release);
  m_sortUiActive = false;
        }

        if (m_heap.empty())
  return 0;

        const HeapNode node = m_heap.top();
        const Ref ref = node.ref;
        const int iNextT = Tick(ref);

        int iMaxTickAllowed = m_iCurrTick;
        if (m_bIsStandard) {
  if (m_iMicroSecsPerBeat != 0)
      iMaxTickAllowed += (int)((static_cast<long long>(m_iTicksPerBeat) *
          (m_iCurrMicroSec + iMicroSecs)) / m_iMicroSecsPerBeat);
        } else {
  iMaxTickAllowed += (int)((static_cast<long long>(m_iTicksPerSecond) *
      (m_iCurrMicroSec + iMicroSecs)) / 1000000);
        }

        if (iMicroSecs < 0 || iNextT <= iMaxTickAllowed) {
  int iSpan = iNextT - m_iCurrTick;
  if (m_bIsStandard)
      iSpan = (int)((static_cast<long long>(m_iMicroSecsPerBeat) * iSpan) /
          m_iTicksPerBeat - m_iCurrMicroSec);
  else
      iSpan = (int)((1000000LL * iSpan) / m_iTicksPerSecond - m_iCurrMicroSec);
  m_iCurrTick = iNextT;
  m_iCurrMicroSec = 0;

  m_heap.pop();
  Lane& lane = m_lanes[node.lane];
  ++lane.pos;
  if (lane.pos < lane.end)
      m_heap.push(HeapNode{ node.lane, m_refs[lane.pos] });

  if (IsMeta(ref)) {
      MIDIEvent* pMetaEvent = Meta(ref);
      *pOutMeta = pMetaEvent;
      if (pMetaEvent && pMetaEvent->GetEventType() == MIDIEvent::MetaEvent) {
          MIDIMetaEvent* pMeta = reinterpret_cast<MIDIMetaEvent*>(pMetaEvent);
          if (pMeta->GetMetaEventType() == MIDIMetaEvent::SetTempo && pMeta->GetDataLen() == 3)
              MIDI::Parse24Bit(pMeta->GetData(), 3, &m_iMicroSecsPerBeat);
      }
  } else {
      *pOutRow = (MIDIChannelEvent)ref;
  }
  return iSpan;
        }

        if (m_bIsStandard)
  m_iCurrMicroSec = iMicroSecs + m_iCurrMicroSec -
      (int)((static_cast<long long>(m_iMicroSecsPerBeat) *
          (iMaxTickAllowed - m_iCurrTick)) / m_iTicksPerBeat);
        else
  m_iCurrMicroSec = iMicroSecs + m_iCurrMicroSec -
      (int)((1000000LL * (iMaxTickAllowed - m_iCurrTick)) / m_iTicksPerSecond);
        m_iCurrTick = iMaxTickAllowed;
        return iMicroSecs;
    }

    bool IsStandard() const { return m_bIsStandard; }
    int GetTicksPerBeat() const { return (int)m_iTicksPerBeat; }
    int GetTicksPerSecond() const { return m_iTicksPerSecond; }
    int GetMicroSecsPerBeat() const { return (int)m_iMicroSecsPerBeat; }

private:
    using Ref = uint32_t;
    static constexpr Ref MetaFlag = 0x80000000u;
    static constexpr size_t SortBlock = 1u << 18;

    struct Lane {
        size_t begin = 0;
        size_t end = 0;
        size_t pos = 0;
    };
    struct HeapNode {
        unsigned lane = 0;
        Ref ref = 0;
    };
    struct HeapLater {
        const ParallelMIDIPos* owner = nullptr;
        bool operator()(const HeapNode& a, const HeapNode& b) const
        {
  if (owner->RefLess(b.ref, a.ref)) return true;
  if (owner->RefLess(a.ref, b.ref)) return false;
  return a.lane > b.lane;
        }
    };

    MIDI& m_MIDI;
    std::vector<Ref> m_refs;
    std::vector<MIDIEvent*> m_metaRefs;
    std::vector<Lane> m_lanes;
    std::priority_queue<HeapNode, std::vector<HeapNode>, HeapLater> m_heap;

    bool m_bIsStandard = true;
    uint32_t m_iTicksPerBeat = 0;
    uint32_t m_iMicroSecsPerBeat = 500000;
    int m_iTicksPerSecond = 0;
    int m_iCurrTick = 0;
    int m_iCurrMicroSec = 0;
    bool m_sortUiActive = false;

    static unsigned PhysicalCoreCount()
    {
        DWORD bytes = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
        if (bytes > 0) {
  std::vector<unsigned char> storage(bytes);
  auto* first = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data());
  if (GetLogicalProcessorInformationEx(RelationProcessorCore, first, &bytes)) {
      unsigned count = 0;
      unsigned char* p = storage.data();
      unsigned char* end = storage.data() + bytes;
      while (p < end) {
          auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(p);
          if (info->Relationship == RelationProcessorCore)
              ++count;
          if (info->Size == 0)
              break;
          p += info->Size;
      }
      if (count > 0)
          return count;
  }
        }
        const unsigned logical = std::thread::hardware_concurrency();
        return logical > 0 ? logical : 1u;
    }

    bool IsMeta(Ref ref) const { return (ref & MetaFlag) != 0; }
    size_t MetaIndex(Ref ref) const { return (size_t)(ref & ~MetaFlag); }
    MIDIEvent* Meta(Ref ref) const { return m_metaRefs[MetaIndex(ref)]; }

    int Tick(Ref ref) const
    {
        return IsMeta(ref) ? Meta(ref)->GetAbsT() : (int)m_MIDI.GetEventTicks((MIDIChannelEvent)ref);
    }

    int Track(Ref ref) const
    {
        return IsMeta(ref) ? Meta(ref)->GetTrack() : (int)m_MIDI.GetEventTrack((MIDIChannelEvent)ref);
    }

    int Kind(Ref ref) const
    {
        if (IsMeta(ref)) return 0;
        return m_MIDI.IsThinRow((MIDIChannelEvent)ref) ? 2 : 1;
    }

    uint32_t Ordinal(Ref ref) const
    {
        return IsMeta(ref) ? (uint32_t)MetaIndex(ref) : ref;
    }

    bool RefLess(Ref a, Ref b) const
    {
        const int at = Tick(a), bt = Tick(b);
        if (at != bt) return at < bt;
        const int atr = Track(a), btr = Track(b);
        if (atr != btr) return atr < btr;
        const int ak = Kind(a), bk = Kind(b);
        if (ak != bk) return ak < bk; // meta, pool, thin: matches MIDIPos tie order
        return Ordinal(a) < Ordinal(b);
    }

    uint64_t LaneTaskCount(size_t count) const
    {
        if (count == 0)
  return 0;
        const size_t blocks = (count + SortBlock - 1) / SortBlock;
        uint64_t tasks = blocks;
        for (size_t width = SortBlock; width < count; ) {
  tasks += (count + width * 2 - 1) / (width * 2);
  if (width > count / 2)
      break;
  width *= 2;
        }
        return (std::max)(tasks, 1ull);
    }

    void SortLane(unsigned laneIndex)
    {
        Lane& lane = m_lanes[laneIndex];
        const size_t count = lane.end - lane.begin;
        const uint64_t taskMax = LaneTaskCount(count);
        g_LoadingProgress.sortMax[laneIndex].store(taskMax, std::memory_order_release);
        g_LoadingProgress.sortProgress[laneIndex].store(0, std::memory_order_release);
        if (count == 0)
  return;

        auto less = [this](Ref a, Ref b) { return RefLess(a, b); };
        for (size_t left = lane.begin; left < lane.end; left += SortBlock) {
  const size_t right = (std::min)(lane.end, left + SortBlock);
  std::sort(m_refs.begin() + left, m_refs.begin() + right, less);
  g_LoadingProgress.sortProgress[laneIndex].fetch_add(1, std::memory_order_relaxed);
  g_LoadingProgress.progress.fetch_add(1, std::memory_order_relaxed);
        }

        for (size_t width = SortBlock; width < count; ) {
  const size_t pairWidth = width * 2;
  for (size_t rel = 0; rel < count; rel += pairWidth) {
      const size_t left = lane.begin + rel;
      const size_t mid = (std::min)(lane.end, left + width);
      const size_t right = (std::min)(lane.end, left + pairWidth);
      if (mid < right)
          std::inplace_merge(m_refs.begin() + left, m_refs.begin() + mid,
              m_refs.begin() + right, less);
      g_LoadingProgress.sortProgress[laneIndex].fetch_add(1, std::memory_order_relaxed);
      g_LoadingProgress.progress.fetch_add(1, std::memory_order_relaxed);
  }
  if (width > count / 2)
      break;
  width *= 2;
        }
    }

    void BuildAndSort()
    {
        const size_t tracks = m_MIDI.m_vTracks.size();
        std::vector<size_t> trackOffsets(tracks + 1, 0);
        std::vector<size_t> metaBase(tracks, 0);
        size_t metaCount = 0;
        for (size_t t = 0; t < tracks; ++t) {
  MIDITrack* track = m_MIDI.m_vTracks[t];
  trackOffsets[t + 1] = trackOffsets[t] + track->GetRowCount() +
      track->GetThinCount() + track->GetMetaCount();
  metaBase[t] = metaCount;
  metaCount += track->GetMetaCount();
        }

        const uint64_t channelRows = (uint64_t)m_MIDI.m_iFullRows + (uint64_t)m_MIDI.m_vThinTicks.size();
        if (channelRows >= MetaFlag || metaCount >= MetaFlag)
  throw std::runtime_error("MIDI event count exceeds compact parallel-sort reference range");

        m_metaRefs.resize(metaCount);
        for (size_t t = 0; t < tracks; ++t) {
  MIDITrack* track = m_MIDI.m_vTracks[t];
  for (size_t i = 0; i < track->m_vMetas.size(); ++i)
      m_metaRefs[metaBase[t] + i] = track->m_vMetas[i];
        }

        m_refs.resize(trackOffsets.back());
        for (size_t t = 0; t < tracks; ++t) {
  MIDITrack* track = m_MIDI.m_vTracks[t];
  size_t out = trackOffsets[t];
  for (size_t i = 0; i < track->GetRowCount(); ++i)
      m_refs[out++] = (Ref)(track->GetRowStart() + i);
  for (size_t i = 0; i < track->GetThinCount(); ++i)
      m_refs[out++] = (Ref)(m_MIDI.m_iFullRows + track->GetThinStart() + i);
  for (size_t i = 0; i < track->GetMetaCount(); ++i)
      m_refs[out++] = MetaFlag | (Ref)(metaBase[t] + i);
        }

        if (m_refs.empty()) {
  g_LoadingProgress.sortWorkerCount.store(0, std::memory_order_release);
  return;
        }

        unsigned workers = PhysicalCoreCount();
        workers = (std::min)(workers, (unsigned)MIDILoadingProgress::MaxSortWorkers);
        workers = (std::min)(workers, (unsigned)m_refs.size());
        workers = (std::max)(workers, 1u);

        m_lanes.resize(workers);
        uint64_t totalTasks = 0;
        for (unsigned i = 0; i < MIDILoadingProgress::MaxSortWorkers; ++i) {
  g_LoadingProgress.sortProgress[i].store(0, std::memory_order_relaxed);
  g_LoadingProgress.sortMax[i].store(0, std::memory_order_relaxed);
        }
        for (unsigned i = 0; i < workers; ++i) {
  Lane& lane = m_lanes[i];
  lane.begin = (m_refs.size() * (size_t)i) / workers;
  lane.end = (m_refs.size() * (size_t)(i + 1)) / workers;
  lane.pos = lane.begin;
  const uint64_t tasks = LaneTaskCount(lane.end - lane.begin);
  g_LoadingProgress.sortMax[i].store(tasks, std::memory_order_relaxed);
  totalTasks += tasks;
        }

        g_LoadingProgress.stage = MIDILoadingProgress::SortEvents;
        g_LoadingProgress.progress.store(0, std::memory_order_release);
        g_LoadingProgress.max = totalTasks;
        g_LoadingProgress.sortWorkerCount.store(workers, std::memory_order_release);
        m_sortUiActive = true;

        std::vector<std::thread> threads;
        threads.reserve(workers);
        for (unsigned i = 0; i < workers; ++i)
  threads.emplace_back([this, i]() { SortLane(i); });
        for (auto& thread : threads)
  thread.join();

        for (unsigned i = 0; i < workers; ++i) {
  Lane& lane = m_lanes[i];
  lane.pos = lane.begin;
  if (lane.pos < lane.end)
      m_heap.push(HeapNode{ i, m_refs[lane.pos] });
        }
    }
};
