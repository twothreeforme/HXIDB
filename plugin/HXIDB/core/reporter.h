// Reporter: identity derived from the player (oblivious PRF, see oprf.h), signed batching, WinHTTP transport and a retry
// queue. EVERYTHING IS IN MEMORY: no key file, no queue file, nothing is written to the player's disk.
// Runs its own worker thread; the game thread only calls Enqueue()/SetIdentityInput()/status getters.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include "events.h"
#include "http.h"
#include "oprf.h"

extern "C" {
#include "../third_party/monocypher/monocypher.h"
#include "../third_party/monocypher/monocypher-ed25519.h"
}

namespace hxidb {

enum class Link { Waiting, Connected, Degraded, Offline };  // Waiting: no character in the game yet, so no identity

/**
 * Reporter - sends finished events to the backend.
 *
 * Enqueue() puts an event in an in-memory queue; the worker thread derives the identity (once a character is known),
 * enrolls it, signs batches and posts them with retries. The queue is capped at 2000 events and dropped on unload.
 * State() gives the colour of the status dot, Sent()/Queued() the numbers shown in the box.
 */
class Reporter
{
public:
    // baseUrl: the backend, compiled into the plugin. The identity input arrives later (SetIdentityInput), once a
    // character is in the game.
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
        crypto_wipe(m_secret, sizeof(m_secret));
    }

    // What the identity is derived from (never sent: only the blinded value goes out). A change of input means a different
    // player: the identity is derived again.
    void SetIdentityInput(const std::string& input)
    {
        { std::lock_guard<std::mutex> g(m_mu); if (input == m_input) return; m_input = input; m_haveIdentity = false; m_enrolled = false; }
        m_cv.notify_all();
    }

    void Enqueue(const SourceEvent& e)
    {
        char s[256];
        snprintf(s, sizeof(s), "%s zone=%u src=0x%08x drops=%zu gil=%u party=%zu", e.type == SRC_MOB ? "kill" : "chest", e.zone, e.sourceId, e.drops.size(), e.gil, e.party.size());
        { std::lock_guard<std::mutex> g(m_mu); m_q.push_back({Encode(e), s}); if (m_q.size() > 2000) m_q.pop_front(); }
        m_cv.notify_all();
    }

    Link State()
    {
        { std::lock_guard<std::mutex> g(m_mu); if (m_input.empty()) return Link::Waiting; }
        int f = m_fails;
        return f == 0 ? Link::Connected : (f < 3 ? Link::Degraded : Link::Offline);
    }
    size_t Queued() { std::lock_guard<std::mutex> g(m_mu); return m_q.size(); }
    uint64_t Sent() const { return m_sent; }
    // Last HTTP status of identity / enrollment / upload (0 = no answer); for diagnostics only.
    int LastHttp() const { return m_lastHttp; }
    std::string LastPayload() { std::lock_guard<std::mutex> g(m_mu); return m_last; }

private:
    struct Item { Bytes enc; std::string summary; };

    std::string m_url, m_input;
    uint8_t m_secret[64] = {}, m_pub[32] = {};
    bool m_haveIdentity = false;
    std::atomic<bool> m_enrolled{false};
    std::atomic<int> m_fails{0};
    std::atomic<uint64_t> m_sent{0};
    std::atomic<int> m_lastHttp{0};
    bool m_run = false;
    std::thread m_thread;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<Item> m_q;
    std::string m_last;

    bool Identify()
    {
        std::string input;
        { std::lock_guard<std::mutex> g(m_mu); input = m_input; }
        if (input.empty()) return false;
        uint8_t seed[32];
        int st = 0;
        bool ok = OprfDerive(m_url, input, seed, &st);
        m_lastHttp = st;
        if (!ok) return false;
        uint8_t secret[64], pub[32];
        crypto_ed25519_key_pair(secret, pub, seed);  // wipes seed
        std::lock_guard<std::mutex> g(m_mu);
        if (input != m_input) return false;  // the player changed while we were asking
        memcpy(m_secret, secret, 64);
        memcpy(m_pub, pub, 32);
        crypto_wipe(secret, sizeof(secret));
        m_haveIdentity = true;
        return true;
    }

    bool Enroll()
    {
        HttpResult r = HttpPost(m_url, "/v1/enroll", EncodeEnroll(m_pub));
        m_lastHttp = r.status;
        if (r.status == 200) { m_enrolled = true; return true; }
        return false;
    }

    // Returns true when the queue may advance (sent, or permanently rejected).
    bool SendBatch(std::vector<Item>& batch)
    {
        std::vector<Bytes> enc;
        for (auto& b : batch) enc.push_back(b.enc);
        uint8_t nonce[16];
        RandomBytes(nonce, 16);
        Bytes payload = EncodePayloadRaw(nonce, (uint64_t)time(nullptr), enc);
        uint8_t sig[64];
        crypto_ed25519_sign(sig, m_secret, payload.data(), payload.size());
        HttpResult r = HttpPost(m_url, "/v1/ingest", EncodeEnvelope(m_pub, payload, sig));
        m_lastHttp = r.status;
        if (r.status == 200) { m_sent += batch.size(); m_fails = 0; SetLast(batch, "sent"); return true; }
        if (r.status == 403) { m_enrolled = false; m_fails++; return false; }  // not enrolled / revoked: retry enrollment
        if (r.status == 429 || r.status == 503 || r.status == 0 || r.status >= 500) { m_fails++; return false; }
        m_fails++;
        SetLast(batch, "rejected");  // other 4xx: server will never accept this batch; drop it
        return true;
    }

    void SetLast(const std::vector<Item>& batch, const char* verdict)
    {
        std::string s = std::string("[") + verdict + "] " + std::to_string(batch.size()) + " event(s)\n";
        for (auto& b : batch) s += "  " + b.summary + "\n";
        std::lock_guard<std::mutex> g(m_mu);
        m_last = s;
    }

    void Loop()
    {
        int backoff = 2;
        for (;;)
        {
            std::vector<Item> batch;
            {
                std::unique_lock<std::mutex> lk(m_mu);
                m_cv.wait_for(lk, std::chrono::seconds(m_fails ? backoff : 5), [this] { return !m_run || (!m_input.empty() && (!m_haveIdentity || !m_q.empty())); });
                if (!m_run) return;
                if (m_input.empty()) continue;
                if (m_haveIdentity)
                {
                    if (m_q.empty()) continue;
                    // Brief settle so a kill's follow-up events batch together.
                    size_t n = (std::min)(m_q.size(), (size_t)50);
                    for (size_t i = 0; i < n; i++) batch.push_back(m_q[i]);
                }
            }
            bool have;
            { std::lock_guard<std::mutex> g(m_mu); have = m_haveIdentity; }
            if (!have)
            {
                if (!Identify()) { m_fails++; if (!Rest(backoff)) return; backoff = (std::min)(backoff * 2, 120); }
                else { m_fails = 0; backoff = 2; }
                continue;
            }
            if (batch.empty()) continue;
            if (!m_enrolled && !Enroll()) { m_fails++; if (!Rest(backoff)) return; backoff = (std::min)(backoff * 2, 120); continue; }
            if (SendBatch(batch))
            {
                std::lock_guard<std::mutex> g(m_mu);
                for (size_t i = 0; i < batch.size() && !m_q.empty(); i++) m_q.pop_front();
                backoff = 2;
            }
            else
            {
                if (!Rest(backoff)) return;  // wait before retrying, however fast the server answered
                backoff = (std::min)(backoff * 2, 120);
            }
        }
    }

    // Sleeps for the back-off period; returns false if the reporter is being stopped.
    bool Rest(int seconds)
    {
        std::unique_lock<std::mutex> lk(m_mu);
        m_cv.wait_for(lk, std::chrono::seconds(seconds), [this] { return !m_run; });
        return m_run;
    }
};

} // namespace hxidb
