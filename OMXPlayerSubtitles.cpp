/*
 * Author: Torarin Hals Bakke (2012)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "OMXPlayerSubtitles.h"
#include "OMXOverlayText.h"
#include "SubtitleRenderer.h"
#include "LockBlock.h"
#include "Enforce.h"
#include "utils/log.h"

#include <boost/algorithm/string.hpp>
#include <utility>
#include <algorithm>
#include <typeinfo>
#include <deque>
#include <cstdint>

constexpr int RENDER_LOOP_SLEEP = 100;

OMXPlayerSubtitles::OMXPlayerSubtitles() BOOST_NOEXCEPT
:
#ifndef NDEBUG
  m_open(),
#endif
  m_thread_stopped(true),
  m_active_index(), /////////////////////////////////////////////////////////////////////////////////7
  m_delay(),
  m_font_size(),
  m_centered(),
  m_av_clock()
{}

OMXPlayerSubtitles::~OMXPlayerSubtitles() BOOST_NOEXCEPT
{
  Close();
}

bool OMXPlayerSubtitles::Open(size_t stream_count,
                              std::vector<Subtitle>&& external_subtitles,
                              const std::string& font_path,
                              float font_size,
                              bool centered,
                              OMXClock* clock) BOOST_NOEXCEPT
{
  assert(!m_open);

  m_subtitle_queues.resize(stream_count, boost::circular_buffer<Subtitle>(32));
  m_visible = true;
  m_delay = 0;
  m_thread_stopped.store(false, std::memory_order_relaxed);

  m_external_subtitles = std::move(external_subtitles);
  m_font_path = font_path;
  m_font_size = font_size;
  m_centered = centered;
  m_av_clock  = clock;

  if(!Create())
    return false;

#ifndef NDEBUG
  m_open = true;
#endif

  return true;
}

void OMXPlayerSubtitles::Close() BOOST_NOEXCEPT
{
  if(Running())
  {
    m_mailbox.send(Message::Stop{});
    StopThread();
    m_mailbox.clear();
  }

  m_subtitle_queues.clear();

#ifndef NDEBUG
  m_open = false;
#endif
}

void OMXPlayerSubtitles::Process()
{
  try
  {
    RenderLoop(m_font_path, m_font_size, m_centered, m_av_clock);
  }
  catch(Enforce_error& e)
  {
    if(!e.user_friendly_what().empty())
      printf("Error: %s\n", e.user_friendly_what().c_str());
    CLog::Log(LOGERROR, "OMXPlayerSubtitles::RenderLoop threw %s (%s)",
              typeid(e).name(), e.what());
  }
  catch(std::exception& e)
  {
    CLog::Log(LOGERROR, "OMXPlayerSubtitles::RenderLoop threw %s (%s)",
              typeid(e).name(), e.what());
  }
  m_thread_stopped.store(true, std::memory_order_relaxed);
}

template <typename SubtitleInputRange>
void Step(int now,
          SubtitleRenderer& renderer,
          SubtitleInputRange& subtitles,
          Subtitle*& next_subtitle,
          int& current_stop,
          bool& showing,
          bool& expensive_op)
{
  if(next_subtitle && next_subtitle->stop <= now)
  {
    renderer.unprepare();
    next_subtitle = NULL;
    subtitles.pop_front();     
  }

  if(!next_subtitle)
  {
    for(; !subtitles.empty(); subtitles.pop_front())
    {
      if(subtitles.front().stop > now)
      {
        next_subtitle = &subtitles.front();
        renderer.prepare(next_subtitle->text_lines);
        break;
      }
    }
  }

  if(next_subtitle && next_subtitle->start <= now)
  {
    renderer.show_next();
    showing = true;

    current_stop = next_subtitle->stop;
    next_subtitle = NULL;
    subtitles.pop_front();
    expensive_op = true;
  }
  else if(showing && current_stop <= now)
  {
    renderer.hide();
    showing = false;

    expensive_op = true;
  }
}

template<typename Iterator>
class iterator_range {
public:
  iterator_range(Iterator begin, Iterator end)
  : begin_(begin),
    end_(end)
  {}

  bool empty() {
    return begin_ == end_;
  }

  typename Iterator::reference front() {
    assert(!empty());
    return *begin_;
  }

  void pop_front() {
    assert(!empty());
    ++begin_;
  }

private:
  Iterator begin_;
  Iterator end_;
};

void OMXPlayerSubtitles::
RenderLoop(const std::string& font_path, float font_size, bool centered, OMXClock* clock)
{
  bool use_external_subtitles = true;

  SubtitleRenderer renderer(1,
                            font_path,
                            font_size,
                            0.01f, 0.06f,
                            centered,
                            0xDD,
                            0x80);

  typedef iterator_range<std::vector<Subtitle>::iterator> SubtitleRange;
  SubtitleRange external_subtitles(m_external_subtitles.begin(),
                                   m_external_subtitles.end());

  printf("count: %i\n", m_external_subtitles.size());

  bool exit{};
  std::deque<Subtitle> subtitle_queue;
  Subtitle* next_subtitle{};
  int current_stop{};
  bool showing{};
  bool expensive_op{};
  int delay{};

  for(;;)
  {
    std::chrono::milliseconds timeout(RENDER_LOOP_SLEEP);

    if(expensive_op)
    {
      expensive_op = false;
      timeout = std::chrono::milliseconds(0);
    }

    m_mailbox.receive_wait(timeout,
      [&](Message::PushSubtitle&& push_subtitle)
      {
        assert(!use_external_subtitles);
        subtitle_queue.push_back(std::move(push_subtitle.subtitle));
      },
      [&](Message::Flush&& flush)
      {
        assert(!use_external_subtitles);

        renderer.unprepare();
        current_stop = INT_MIN;
        next_subtitle = NULL;
        subtitle_queue = std::move(flush.subtitles);
      },
      [&](Message::FlushExternal&& flush_external)
      {
        assert(use_external_subtitles);

        renderer.unprepare();
        current_stop = INT_MIN;
        next_subtitle = NULL;
        auto first =
          std::upper_bound(m_external_subtitles.begin(),
                           m_external_subtitles.end(),
                           flush_external.time,
                           [](const Subtitle& a, int b)
                           {
                             return a.start < b;
                           });
        if(first != m_external_subtitles.begin())
          --first;
        external_subtitles = SubtitleRange(first, m_external_subtitles.end());
      },
      [&](Message::Stop&&)
      {
        exit = true;
      });

    if(exit) break;
    
    auto now = static_cast<int>(clock->OMXMediaTime()/1000) - delay;

    if(external_subtitles)
    {
      Step(now, renderer, external_subtitles,
           next_subtitle, current_stop, showing, expensive_op);
    }
    else
    {
      Step(now, renderer, subtitle_queue,
           next_subtitle, current_stop, showing, expensive_op);
    }
  }
}

void OMXPlayerSubtitles::Flush() BOOST_NOEXCEPT
{
  assert(m_open);

  m_mailbox.send(Message::Flush{});

  for(auto& q : m_subtitle_queues)
    q.clear();
}

void OMXPlayerSubtitles::FlushRenderer()
{
  Message::Flush flush;
  for(auto& s : m_subtitle_queues[m_active_index])
  {
    flush.subtitles.push_back({s.start + m_delay,
                               s.stop + m_delay,
                               s.text_lines});
  }
  m_mailbox.send(std::move(flush));
}

void OMXPlayerSubtitles::SetDelay(int value) BOOST_NOEXCEPT
{
  assert(m_open);

  m_delay = value;
  FlushRenderer();
}

void OMXPlayerSubtitles::SetVisible(bool visible) BOOST_NOEXCEPT
{
  assert(m_open);

  if(visible)
  {
    if (!m_visible)
    {
      m_visible = true;
      FlushRenderer();
    }
  }
  else
  {
    if (m_visible)
    {
      m_visible = false;
      m_mailbox.send(Message::Flush{});
    }
  }
}

void OMXPlayerSubtitles::SetActiveStream(size_t index) BOOST_NOEXCEPT
{
  assert(m_open);
  assert(index < m_subtitle_queues.size());

  m_active_index = index;
  FlushRenderer();
}

std::vector<std::string> OMXPlayerSubtitles::GetTextLines(OMXPacket *pkt)
{
  assert(pkt);

  m_subtitle_codec.Open(pkt->hints);

  auto result = m_subtitle_codec.Decode(pkt->data, pkt->size, 0, 0);
  assert(result == OC_OVERLAY);

  auto overlay = m_subtitle_codec.GetOverlay();
  assert(overlay);

  std::vector<std::string> text_lines;

  auto e = ((COMXOverlayText*) overlay)->m_pHead;
  if(e && e->IsElementType(COMXOverlayText::ELEMENT_TYPE_TEXT))
  {
    boost::split(text_lines,
                 ((COMXOverlayText::CElementText*) e)->m_text,
                 boost::is_any_of("\r\n"),
                 boost::token_compress_on);
  }

  return text_lines;
}

bool OMXPlayerSubtitles::AddPacket(OMXPacket *pkt, size_t stream_index) BOOST_NOEXCEPT
{
  assert(m_open);
  assert(stream_index < m_subtitle_queues.size());

  if(!pkt)
    return false;

  // if(m_thread_stopped.load(std::memory_order_relaxed))
  // {
  //   // Rendering thread has stopped, throw away the packet
  //   CLog::Log(LOGWARNING, "Subtitle rendering thread has stopped, discarding packet");
  //   OMXReader::FreePacket(pkt);
  //   return true;
  // }

  // Center the presentation time on the requested timestamps
  auto start = static_cast<int>(pkt->pts - RENDER_LOOP_SLEEP*1000/2);
  auto stop = static_cast<int>(start + pkt->duration);
  auto text_lines = GetTextLines(pkt);

  m_subtitle_queues[stream_index].push_back({start, stop, {}});
  m_subtitle_queues[stream_index].back().text_lines = text_lines;

  if(!m_use_external_subtitles && m_visible && stream_index == m_active_index)
  {
    m_mailbox.send(Message::PushSubtitle{{start, stop, std::move(text_lines)}});
  }

  OMXReader::FreePacket(pkt);
  return true;
}
