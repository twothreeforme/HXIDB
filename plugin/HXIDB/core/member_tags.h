// Anonymous party member tags. Every member's plugin turns the game ID of each party member into an 8-byte tag through the
// oblivious PRF (purpose 1, a server key that is different from the identity key). Members who see the same party therefore
// send the same set of tags, so the server knows from facts, not inference, which reports belong to one party. The server
// never sees an ID and cannot reverse a tag; the tags are only held in memory here. Resolution runs on its own thread because
// each tag needs a request to the server; unresolved members simply leave the party facts out of the event.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "oprf.h"

namespace hxidb {

using MemberTag = std::array<uint8_t, 8>;

/**
 * MemberTags - resolves party member ids to anonymous 8-byte tags on a background thread.
 *
 * Request() queues ids, Get() returns the tags once all of them are known. Ids and tags stay in memory; failed requests
 * are retried with a growing delay (2 s up to 2 min).
 */
class MemberTags
{
public:
    ~MemberTags() { Stop(); }

    void Start(const std::string& baseUrl)
    {
        m_url = baseUrl;
        m_run = true;
        m_thread = std::thread([this] { Loop(); });
    }

    void Stop()
    {
        { std::lock_guard<std::mutex> g(m_mu); m_run = false; }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    // Ask for the tags of these party member IDs (cheap: IDs already known or pending are skipped).
    void Request(const std::vector<uint32_t>& ids)
    {
        std::lock_guard<std::mutex> g(m_mu);
        for (uint32_t id : ids)
            if (id && !m_tags.count(id) && std::find(m_todo.begin(), m_todo.end(), id) == m_todo.end()) m_todo.push_back(id);
        if (!m_todo.empty()) m_cv.notify_all();
    }

    // The sorted tags of all these members and the tag of `self`, or false if any is not resolved yet.
    bool Get(const std::vector<uint32_t>& ids, uint32_t self, MemberTag& selfTag, std::vector<MemberTag>& sorted)
    {
        std::lock_guard<std::mutex> g(m_mu);
        sorted.clear();
        for (uint32_t id : ids)
        {
            auto it = m_tags.find(id);
            if (it == m_tags.end()) return false;
            sorted.push_back(it->second);
            if (id == self) selfTag = it->second;
        }
        auto it = m_tags.find(self);
        if (it == m_tags.end()) return false;
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        return true;
    }

private:
    void Loop()
    {
        int backoff = 2;
        for (;;)
        {
            uint32_t id = 0;
            {
                std::unique_lock<std::mutex> lk(m_mu);
                m_cv.wait_for(lk, std::chrono::seconds(backoff), [this] { return !m_run || !m_todo.empty(); });
                if (!m_run) return;
                if (m_todo.empty()) continue;
                id = m_todo.front();
            }
            uint8_t seed[32];
            int st = 0;
            bool ok = OprfDerive(m_url, "HorizonXI|member|" + std::to_string(id), seed, &st, 1);
            std::lock_guard<std::mutex> g(m_mu);
            if (ok)
            {
                MemberTag t;
                memcpy(t.data(), seed, 8);
                sodium_memzero(seed, sizeof(seed));
                m_tags[id] = t;
                m_todo.erase(std::remove(m_todo.begin(), m_todo.end(), id), m_todo.end());
                backoff = 2;
            }
            else backoff = (std::min)(backoff * 2, 120);  // rate limited or offline: try again later
        }
    }

    std::string m_url;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::thread m_thread;
    bool m_run = false;
    std::map<uint32_t, MemberTag> m_tags;   // in memory only
    std::vector<uint32_t> m_todo;
};

} // namespace hxidb
