// Turns incoming packets into SourceEvents. Host-independent (no Ashita), so it is testable by replay.
// Privacy: reads only mob/chest entities (index < 0x400); never stores player names or IDs.
#pragma once
#include <cstring>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <random>
#include <unordered_map>
#include "events.h"
#include "th_items.h"

namespace hxidb {

/**
 * EventBuilder - turns the raw packet stream into SourceEvents (one per kill or chest).
 *
 * Feed it every packet of interest through OnPacket(), call Tick() now and then, and collect finished events with Pop().
 * A kill is held for kPoolWindow seconds so the pool items and gil that arrive right after the defeat message can be
 * attached to it. The host (the plugin) supplies the things only the game client knows through the Set...Fn callbacks.
 * No Ashita types are used here, which is why the replay tool can run it on captured packets.
 */
class EventBuilder
{
public:
    static constexpr double kPoolWindow = 3.0;     // seconds after a defeat to collect pool items / gil
    static constexpr double kDefeatDedup = 5.0;    // msg 6 arrives twice per kill
    static constexpr double kChestResend = 300.0;  // suppress identical chest presentations

    void SetParty(std::vector<PartyMember> p) { m_party = std::move(p); }
    // Host callback: number of party members eligible for the gil split for the mob with this unique ID.
    void SetEligibleFn(std::function<uint32_t(uint32_t)> f) { m_eligible = std::move(f); }
    // Host callback: active party members in the same zone as us, at any distance.
    void SetZoneMembersFn(std::function<uint32_t()> f) { m_zoneMembers = std::move(f); }
    // Host callback: the local player's Treasure Hunter right now, as (job trait tier, worn gear bonus). Called when an action of
    // the local player on a monster arrives, because gear can be swapped at any time.
    void SetSelfThFn(std::function<std::pair<uint32_t, uint32_t>()> f) { m_selfTh = std::move(f); }
    // Host callback: anonymous party facts at the kill (self tag, sorted tags of the whole party); false when not resolved yet.
    void SetInAllianceFn(std::function<bool()> f) { m_inAlliance = std::move(f); }
    void SetPartyTagsFn(std::function<bool(std::vector<uint8_t>&, std::vector<std::vector<uint8_t>>&)> f) { m_partyTags = std::move(f); }
    // Monotonic clock in seconds (host: GetTickCount64()/1000). Used with the server time in the zone-in packet to
    // timestamp kills on the game server's clock instead of the PC's. Without it the "now" argument is used.
    void SetMonotonicClock(std::function<double()> f) { m_mono = std::move(f); }

    /**
     * Handles one incoming packet.
     * @param id           packet id (0x00A, 0x00E, 0x028, 0x029, 0x034 or 0x0D2; others are ignored)
     * @param d            packet bytes
     * @param size         packet size in bytes
     * @param zoneFromHost zone id read from game memory, used until the zone-in packet has been seen
     * @param now          current time in seconds (PC clock)
     */
    void OnPacket(uint16_t id, const uint8_t* d, uint32_t size, uint32_t zoneFromHost, double now)
    {
        if (m_zone == 0) m_zone = zoneFromHost;  // capture started after zone-in
        m_nowMono = m_mono ? m_mono() : now;
        switch (id)
        {
        case 0x00A:
            if (size >= 0x32) SetZone(U16(d, 0x30));
            Anchor(d, size);
            break;
        case 0x00E: Entity(d, size, now); break;
        case 0x028: Action(d, size, now); break;
        case 0x029: Battle(d, size, now); break;
        case 0x0D2: Pool(d, size, now); break;
        case 0x034: Chest(d, size, now); break;
        default: break;
        }
        Flush(now);
    }

    // Call periodically so pending kills are emitted even when no packets arrive.
    void Tick(double now) { Flush(now); }

    /**
     * Takes the oldest finished event.
     * @param out receives the event
     * @return false when nothing is ready
     */
    bool Pop(SourceEvent& out)
    {
        if (m_ready.empty()) return false;
        out = std::move(m_ready.front());
        m_ready.pop_front();
        return true;
    }

private:
    struct Ent
    {
        std::string name;
        float x = 0, y = 0, z = 0;
        bool hasPos = false;
        double firstSeen = 0;
        uint32_t model = 0;  // first 4 model bytes, big-endian (matches HEX(SUBSTR(mob_pools.modelid,1,4)))
    };
    struct Pending { SourceEvent ev; double t; };
    struct ChestSent { std::vector<uint32_t> slots; double t = 0; };

    uint32_t m_zone = 0;
    std::vector<PartyMember> m_party;
    std::function<uint32_t(uint32_t)> m_eligible;
    std::function<uint32_t()> m_zoneMembers;
    std::function<std::pair<uint32_t, uint32_t>()> m_selfTh;
    std::function<bool(std::vector<uint8_t>&, std::vector<std::vector<uint8_t>>&)> m_partyTags;
    std::function<bool()> m_inAlliance;
    std::unordered_map<uint32_t, Ent> m_ents;
    std::vector<Pending> m_pending;
    std::unordered_map<uint32_t, double> m_recentDefeat;
    struct ThAct { uint32_t level = 0; uint32_t selfTrait = 0, selfGear = 0; double t = 0; };
    std::unordered_map<uint32_t, ThAct> m_thActed;  // mob id -> highest Thief level seen acting on it (in memory only)
    bool m_sawAction = false;
    std::unordered_map<uint64_t, ChestSent> m_chestSent;
    std::deque<SourceEvent> m_ready;
    std::function<double()> m_mono;
    double m_nowMono = 0;
    bool m_anchored = false;
    double m_anchorServer = 0, m_anchorMono = 0;  // server unix seconds at the zone-in packet, and the monotonic clock then
    std::mt19937_64 m_rng{std::random_device{}() ^ (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count()};

    static uint16_t U16(const uint8_t* d, size_t o) { uint16_t v; memcpy(&v, d + o, 2); return v; }
    static uint32_t U32(const uint8_t* d, size_t o) { uint32_t v; memcpy(&v, d + o, 4); return v; }
    static float F32(const uint8_t* d, size_t o) { float v; memcpy(&v, d + o, 4); return v; }
    static uint32_t Minute(double now) { return (uint32_t)(now / 60.0); }

    void SetZone(uint32_t z)
    {
        if (z == m_zone) return;
        m_zone = z;
        m_ents.clear();
        m_pending.clear();
        m_recentDefeat.clear();
        m_chestSent.clear();
        m_thActed.clear();
    }

    static SourceType ChestType(const std::string& n)
    {
        if (n.rfind("Treasure_Casket", 0) == 0 || n.rfind("Treasure_Chest", 0) == 0) return SRC_CHEST;
        if (n.rfind("Treasure_Coffer", 0) == 0) return SRC_COFFER;
        if (n.rfind("Armoury_Crate", 0) == 0) return SRC_CRATE;
        return SRC_UNKNOWN;
    }

    void Entity(const uint8_t* d, uint32_t size, double now)
    {
        if (size < 0x20) return;
        uint32_t idx = U16(d, 0x08);
        if (idx >= 0x400) return;  // players / pets never touched
        uint32_t uid = U32(d, 0x04);
        bool fresh = m_ents.find(uid) == m_ents.end();
        Ent& e = m_ents[uid];
        if (fresh) e.firstSeen = now;
        if (size >= 0x44)
        {
            char nm[17] = {};
            memcpy(nm, d + 0x34, 16);
            if (nm[0]) e.name = nm;
            e.model = ((uint32_t)d[0x30] << 24) | ((uint32_t)d[0x31] << 16) | ((uint32_t)d[0x32] << 8) | d[0x33];
            float x = F32(d, 0x0C), z = F32(d, 0x10), y = F32(d, 0x14);
            // A chest slot reused at a new position is a new spawn.
            if (e.hasPos && (std::fabs(x - e.x) > 1.0f || std::fabs(y - e.y) > 1.0f)) e.firstSeen = now;
            e.x = x; e.y = y; e.z = z; e.hasPos = true;
        }
    }

    // The zone-in packet carries the game server's unix time (seconds) at 0x38. It is the same for every client of that
    // server whatever the PC clock says. The value is a whole second, so the true time lies in [t, t+1): add 0.5.
    void Anchor(const uint8_t* d, uint32_t size)
    {
        if (size < 0x3C) return;
        uint32_t t = U32(d, 0x38);
        if (t < 1500000000u || t > 4000000000u) return;
        m_anchored = true;
        m_anchorServer = (double)t + 0.5;
        m_anchorMono = m_nowMono;
    }


    // Level at which a party member's Thief job counts toward Treasure Hunter: main job level when the main job is Thief,
    // else the sub job level when the sub job is Thief. 0 when the member is not a Thief.
    static constexpr uint32_t kJobTHF = 6;
    static uint32_t ThiefLevel(const PartyMember& m)
    {
        uint32_t l = 0;
        if (m.job == kJobTHF) l = m.level;
        if (m.subJob == kJobTHF && m.subLevel > l) l = m.subLevel;
        return l;
    }

    // Action packet 0x028: who did something to whom. The server applies Treasure Hunter to a mob when a player performs a
    // direct action on it (an attack, spell, weapon skill or ability), whatever the order, and keeps the highest level. The
    // client is never told the level, so we record the Thief level of every party member whose action had the mob as a target.
    // Layout: actor ID at 0x05, target list starting at bit 150, bit-packed least significant bit first; per target
    // 32-bit ID, 4-bit action count, then per action 85 bits + a flag (+41 bits) + a flag (+38 bits).
    void Action(const uint8_t* d, uint32_t size, double now)
    {
        m_sawAction = true;
        if (size < 0x20 || m_party.empty()) return;
        uint32_t actor = U32(d, 0x05);
        uint32_t level = 0;
        bool self = false;
        for (size_t i = 0; i < m_party.size(); i++)
            if (m_party[i].uid != 0 && m_party[i].uid == actor) { level = ThiefLevel(m_party[i]); self = (i == 0); break; }
        // Our own action: read job and worn gear now. Any job can add Treasure Hunter with a piece of gear, so level 0 is not "no TH".
        uint32_t selfTrait = 0, selfGear = 0;
        if (self && m_selfTh) { auto p = m_selfTh(); selfTrait = p.first; selfGear = p.second; }
        if (level == 0 && selfTrait + selfGear == 0) return;
        uint32_t nTargets = (U16(d, 0x09) & 0x3FF);
        if (nTargets == 0 || nTargets > 32) return;
        size_t nbits = (size_t)size * 8, pos = 150;
        auto bits = [&](int n, bool& ok) -> uint32_t
        {
            uint32_t v = 0;
            if (pos + n > nbits) { ok = false; return 0; }
            for (int i = 0; i < n; i++, pos++) v |= (uint32_t)((d[pos >> 3] >> (pos & 7)) & 1u) << i;
            return v;
        };
        bool ok = true;
        for (uint32_t t = 0; t < nTargets && ok; t++)
        {
            uint32_t id = bits(32, ok);
            uint32_t nAct = bits(4, ok);
            if (!ok) break;
            if ((id & 0xFFF) < 0x400 && id != actor)
            {
                auto& a = m_thActed[id];
                if (level > a.level) a.level = level;
                if (selfTrait + selfGear > a.selfTrait + a.selfGear) { a.selfTrait = selfTrait; a.selfGear = selfGear; }
                a.t = now;
            }
            for (uint32_t k = 0; k < nAct && ok; k++)
            {
                pos += 85;
                if (bits(1, ok)) pos += 41;
                if (ok && bits(1, ok)) pos += 38;
            }
        }
        if (m_thActed.size() > 2000)  // old fights whose mob never died in sight
            for (auto it = m_thActed.begin(); it != m_thActed.end();) it = (now - it->second.t > 900.0) ? m_thActed.erase(it) : std::next(it);
    }

    void Battle(const uint8_t* d, uint32_t size, double now)
    {
        if (size < 0x1C) return;
        uint16_t msg = U16(d, 0x18);
        if (msg == 6)
        {
            uint32_t tgt = U32(d, 0x08);
            auto r = m_recentDefeat.find(tgt);
            if (r != m_recentDefeat.end() && now - r->second < kDefeatDedup) return;
            m_recentDefeat[tgt] = now;
            SourceEvent ev;
            ev.zone = m_zone; ev.sourceId = tgt; ev.type = SRC_MOB; ev.defeated = true;
            ev.eventMinute = Minute(now); ev.party = m_party;
            if (m_anchored)
            {
                ev.eventMs = (uint64_t)((m_anchorServer + (m_nowMono - m_anchorMono)) * 1000.0);
                ev.anchored = true;
                ev.eventMinute = (uint32_t)(ev.eventMs / 60000);
            }
            do { ev.clientEventId = m_rng(); } while (ev.clientEventId == 0);
            if (m_eligible) ev.gilParty = m_eligible(tgt);
            if (m_zoneMembers) ev.gilPartyZone = m_zoneMembers();
            if (m_partyTags && !m_partyTags(ev.partySelfTag, ev.partyTags)) { ev.partySelfTag.clear(); ev.partyTags.clear(); }
            if (m_inAlliance) ev.partyInAlliance = m_inAlliance();
            if (m_sawAction)
            {
                ev.thKnown = true;
                auto th = m_thActed.find(tgt);
                if (th != m_thActed.end() && now - th->second.t < 900.0)
                {
                    ev.thActed = th->second.level;
                    ev.thSelfTrait = th->second.selfTrait;
                    ev.thSelfGear = th->second.selfGear;
                }
                if (th != m_thActed.end()) m_thActed.erase(th);
            }
            auto it = m_ents.find(tgt);
            if (it != m_ents.end()) { ev.name = it->second.name; ev.model = it->second.model; }
            m_pending.push_back({std::move(ev), now});
        }
        else if (msg == 565 && !m_pending.empty())
        {
            uint32_t gil = U32(d, 0x0C);
            if (gil && gil <= 1000000 && now - m_pending.back().t < kPoolWindow && m_pending.back().ev.gil == 0)
                m_pending.back().ev.gil = gil;
        }
    }

    void Pool(const uint8_t* d, uint32_t size, double now)
    {
        if (size < 0x14) return;
        uint32_t src = U32(d, 0x08), item = U16(d, 0x10), qty = U32(d, 0x04);
        if (src == 0 || item == 0) return;  // zone-in pool-clear packets
        for (auto& p : m_pending)
            if (p.ev.sourceId == src && now - p.t < kPoolWindow)
            {
                p.ev.drops.push_back({item, (qty && qty < 100) ? qty : 1, 0});
                // The pool tick (server ms) is the same on every client; items of one kill differ by a few ticks: keep the lowest.
                uint32_t tick = size >= 0x1C ? U32(d, 0x18) : 0;
                if (tick && (p.ev.killTick == 0 || tick < p.ev.killTick)) p.ev.killTick = tick;
                return;
            }
    }

    void Chest(const uint8_t* d, uint32_t size, double now)
    {
        if (size < 0x30) return;
        uint32_t uid = U32(d, 0x04);
        auto it = m_ents.find(uid);
        if (it == m_ents.end()) return;  // only whitelisted chests we have seen via 0x00E
        const Ent& ent = it->second;
        SourceType st = ChestType(ent.name);
        if (st == SRC_UNKNOWN || !ent.hasPos) return;
        uint32_t evNo = U16(d, 0x2C);
        if (evNo < 1000) return;
        uint32_t kind = (evNo - 1000) % 3;  // 0 items, 1 temp items, 2 locked (no loot)
        if (kind == 2) return;

        SourceEvent ev;
        ev.zone = m_zone; ev.sourceId = uid; ev.name = ent.name; ev.type = st;
        ev.chestKind = kind == 0 ? CHEST_ITEMS : CHEST_TEMP_ITEMS;
        ev.eventMinute = Minute(now); ev.spawnMinute = Minute(ent.firstSeen);
        ev.px = (int32_t)std::lround(ent.x * 10.0f); ev.py = (int32_t)std::lround(ent.y * 10.0f); ev.pz = (int32_t)std::lround(ent.z * 10.0f);
        ev.party = m_party;
        std::vector<uint32_t> slots(4, 0);
        bool any = false;
        for (uint32_t s = 0; s < 4; s++)
        {
            uint32_t item = U32(d, 0x08 + s * 4);
            if (item > 0xFFFF) return;  // not an item list; refuse
            slots[s] = item;
            if (item) { ev.drops.push_back({item, 1, s}); any = true; }
        }
        if (!any) return;

        // Plugin-side suppression: re-send only if the list gained an item we have not sent for this spawn.
        uint64_t key = (uint64_t)uid << 32 | Minute(ent.firstSeen);
        auto& sent = m_chestSent[key];
        bool expired = sent.slots.empty() || now - sent.t > kChestResend;
        bool news = expired;
        if (!expired)
            for (int s = 0; s < 4; s++) if (slots[s] && sent.slots[s] == 0) news = true;
        if (!news) return;
        if (expired) sent.slots = slots;
        else for (int s = 0; s < 4; s++) if (slots[s]) sent.slots[s] = slots[s];
        sent.t = now;
        m_ready.push_back(std::move(ev));
    }

    void Flush(double now)
    {
        for (size_t i = 0; i < m_pending.size();)
        {
            if (now - m_pending[i].t >= kPoolWindow)
            {
                m_ready.push_back(std::move(m_pending[i].ev));
                m_pending.erase(m_pending.begin() + i);
            }
            else i++;
        }
    }
};

} // namespace hxidb
