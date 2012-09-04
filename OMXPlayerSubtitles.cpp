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
#include "Enforce.h"
#include "utils/log.h"

#include <boost/algorithm/string.hpp>
#include <utility>
#include <algorithm>
#include <typeinfo>
#include <cstdint>

constexpr int RENDER_LOOP_SLEEP = 100;

OMXPlayerSubtitles::OMXPlayerSubtitles() BOOST_NOEXCEPT
: m_paused(),
  m_visible(),
  m_use_external_subtitles(),
  m_active_index(),
  m_delay(),
  m_thread_stopped(),
  m_font_size(),
  m_centered(),
  m_av_clock(),
#ifndef NDEBUG
  m_open()
#endif
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

  m_subtitle_caches.resize(stream_count, boost::circular_buffer<Subtitle>(32));
  m_external_subtitles = std::move(external_subtitles);
  
  m_paused = false;
  m_visible = true;
  m_use_external_subtitles = true;
  m_active_index = 0;
  m_delay = 0;
  m_thread_stopped.store(false, std::memory_order_relaxed);

  m_font_path = font_path;
  m_font_size = font_size;
  m_centered = centered;
  m_av_clock = clock;

  if(!Create())
    return false;

  m_mailbox.send(Message::Flush{m_external_subtitles});

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
  }

  m_mailbox.clear();
  m_subtitle_caches.clear();

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

template <typename Iterator>
Iterator FindSubtitle(Iterator begin, Iterator end, int time)
{
  auto first = std::upper_bound(begin, end, time,
                                [](int a, const Subtitle& b)
                                {
                                  return a < b.start;
                                });
  if(first != begin)
    --first;
  return first;
}

void OMXPlayerSubtitles::
RenderLoop(const std::string& font_path, float font_size, bool centered, OMXClock* clock)
{
  SubtitleRenderer renderer(1,
                            font_path,
                            font_size,
                            0.1f, 0.06f,
                            centered,
                            0xCC,
                            0x80);

  std::vector<Subtitle> subtitles;

  size_t next_index{};
  bool exit{};
  bool paused{};
  bool have_next{};
  int current_stop{};
  bool showing{};
  bool expensive_op{};
  int delay{};

  auto GetCurrentTime = [&]
  {
    return static_cast<int>(clock->OMXMediaTime()/1000);
  };

  auto Seek = [&](int time)
  {
    renderer.unprepare();
    current_stop = INT_MIN;
    have_next = false;
    auto it = FindSubtitle(subtitles.begin(),
                           subtitles.end(),
                           time-delay);
    next_index = it - subtitles.begin();
    printf("next_index: %i\n", next_index);
  };

  for(;;)
  {
    std::chrono::milliseconds timeout;

    if(paused)
    {
      timeout = std::chrono::milliseconds(INT_MAX);
    }
    else if(expensive_op)
    {
      expensive_op = false;
      timeout = std::chrono::milliseconds(0);
    }
    else
    {
      timeout = std::chrono::milliseconds(RENDER_LOOP_SLEEP);
    }

    m_mailbox.receive_wait(timeout,
      [&](Message::Push&& args)
      {
        subtitles.push_back(std::move(args.subtitle));
      },
      [&](Message::Flush&& args)
      {
        subtitles = std::move(args.subtitles);
        Seek(GetCurrentTime());
      },
      [&](Message::Seek&& args)
      {
        printf("Current time: %i, Seek: %i\n", GetCurrentTime(), args.time);
        Seek(args.time);
      },
      [&](Message::SetPaused&& args)
      {
        paused = args.value;
      },
      [&](Message::SetDelay&& args)
      {
        delay = args.value;
        Seek(GetCurrentTime());
      },
      [&](Message::Stop&&)
      {
        exit = true;
      });

    if(exit) break;

    if(paused) continue;

    auto now = GetCurrentTime() - delay;

    if(have_next && subtitles[next_index].stop <= now)
    {
      renderer.unprepare();
      have_next = false;
      ++next_index;   
    }

    if(!have_next)
    {
      for(; next_index != subtitles.size(); ++next_index)
      {
        printf("Looking at %i, stop: %i, now: %i\n", next_index, subtitles[next_index].stop, now);
        if(subtitles[next_index].stop > now)
        {
          have_next = true;
          renderer.prepare(subtitles[next_index].text_lines);
          break;
        }
      }
    }

    if(have_next && subtitles[next_index].start <= now)
    {
      renderer.show_next();
      showing = true;

      current_stop = subtitles[next_index].stop;
      have_next = false;
      ++next_index;
      expensive_op = true;
    }
    else if(showing && current_stop <= now)
    {
      renderer.hide();
      showing = false;

      expensive_op = true;
    }
  }
}

void OMXPlayerSubtitles::FlushRenderer()
{
  assert(GetVisible());

  if(GetUseExternalSubtitles())
  {
    m_mailbox.send(Message::Flush{m_external_subtitles});
  }
  else
  {
    Message::Flush flush;
    assert(!m_subtitle_caches.empty());
    for(auto& s : m_subtitle_caches[m_active_index])
      flush.subtitles.push_back(s);
    m_mailbox.send(std::move(flush));
  }
}

void OMXPlayerSubtitles::Flush(double pts) BOOST_NOEXCEPT
{
  assert(m_open);

  for(auto& q : m_subtitle_caches)
    q.clear();

  if(GetVisible())
  {
    if(GetUseExternalSubtitles())
      m_mailbox.send(Message::Seek{static_cast<int>(pts/1000)});
    else
      m_mailbox.send(Message::Flush{});
  }
}

void OMXPlayerSubtitles::Resume() BOOST_NOEXCEPT
{
  assert(m_open);

  m_paused = false;
  m_mailbox.send(Message::SetPaused{false});
}

void OMXPlayerSubtitles::Pause() BOOST_NOEXCEPT
{
  assert(m_open);

  m_paused = true;
  m_mailbox.send(Message::SetPaused{true});
}

void OMXPlayerSubtitles::SetUseExternalSubtitles(bool use) BOOST_NOEXCEPT
{
  assert(m_open);
  assert(use || !m_subtitle_caches.empty());

  m_use_external_subtitles = use;
  if(GetVisible())
    FlushRenderer();
}

void OMXPlayerSubtitles::SetDelay(int value) BOOST_NOEXCEPT
{
  assert(m_open);

  m_delay = value;
  m_mailbox.send(Message::SetDelay{value});
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
    if(m_visible)
    {
      m_visible = false;
      m_mailbox.send(Message::Flush{});
    }
  }
}

void OMXPlayerSubtitles::SetActiveStream(size_t index) BOOST_NOEXCEPT
{
  assert(m_open);
  assert(index < m_subtitle_caches.size());

  m_active_index = index;
  if(!GetUseExternalSubtitles() && GetVisible())
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
  assert(stream_index < m_subtitle_caches.size());

  if(!pkt)
    return false;

  if(m_thread_stopped.load(std::memory_order_relaxed))
  {
    // Rendering thread has stopped, throw away the packet
    CLog::Log(LOGWARNING, "Subtitle rendering thread has stopped, discarding packet");
    OMXReader::FreePacket(pkt);
    return true;
  }

  // Center the actual display time on the PTS
  auto start = static_cast<int>(pkt->pts/1000) - RENDER_LOOP_SLEEP/2;
  auto stop = start + static_cast<int>(pkt->duration/1000);
  auto text_lines = GetTextLines(pkt);

  if (!m_subtitle_caches[stream_index].empty() &&
    start < m_subtitle_caches[stream_index].back().start)
  {
    start = m_subtitle_caches[stream_index].back().start;
  }

  m_subtitle_caches[stream_index].push_back(
    Subtitle(start, stop, std::vector<std::string>()));
  m_subtitle_caches[stream_index].back().text_lines = text_lines;

  if(!GetUseExternalSubtitles() &&
     GetVisible() &&
     stream_index == GetActiveStream())
  {
    m_mailbox.send(Message::Push{{start, stop, std::move(text_lines)}});
  }

  OMXReader::FreePacket(pkt);
  return true;
}
