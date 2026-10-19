// Event model + serialization to hxidb.v1.SourceEvent / Payload.
#pragma once
#include "pb_writer.h"

namespace hxidb {

enum SourceType : uint32_t { SRC_UNKNOWN = 0, SRC_MOB = 1, SRC_CHEST = 2, SRC_COFFER = 3, SRC_CRATE = 4, SRC_OTHER = 5 };
enum ChestKind : uint32_t { CHEST_NONE = 0, CHEST_ITEMS = 1, CHEST_TEMP_ITEMS = 2 };

struct Drop { uint32_t item = 0, qty = 0, slot = 0; };
// job/level (and sub job) are what gets reported. uid is the party member's game ID: it is only compared in memory with the
// actor of an action packet (to see whether a Thief acted on a mob) and is never stored, logged or sent (see Encode).
struct PartyMember { uint32_t job = 0, level = 0, subJob = 0, subLevel = 0, uid = 0; };

struct SourceEvent
{
    uint32_t zone = 0, sourceId = 0, model = 0;
    std::string name;
    SourceType type = SRC_UNKNOWN;
    uint32_t eventMinute = 0;
    std::vector<Drop> drops;
    std::vector<PartyMember> party;
    uint32_t gil = 0;
    uint32_t gilParty = 0;  // eligible split members at the kill (see proto gil_party)
    uint32_t gilPartyZone = 0;  // party members in the same zone at any distance: upper bound of the split (proto gil_party_zone)
    // Kill identity (see proto kill_tick / event_ms): the server-issued pool tick when there were items, and the kill
    // time on the GAME SERVER clock otherwise. clientEventId only makes a resend of this event recognisable.
    uint32_t killTick = 0;
    uint64_t eventMs = 0;
    bool anchored = false;
    uint64_t clientEventId = 0;
    bool defeated = false;
    int32_t px = 0, py = 0, pz = 0;
    uint32_t spawnMinute = 0;
    ChestKind chestKind = CHEST_NONE;
    // Highest Thief level (main job level, or sub job level) among the party members seen acting on this mob before it died;
    // 0 = none. thKnown says the plugin watched the action packets, so 0 means "no Thief acted" and not "not measured".
    uint32_t thActed = 0;
    bool thKnown = false;
    // Treasure Hunter of THIS player at the moment of the player's own actions on the monster, read from the job and the worn
    // equipment then (gear can be swapped at any time): the highest value seen, split into the job trait and the gear.
    uint32_t thSelfTrait = 0, thSelfGear = 0;
    // Anonymous party facts (see member_tags.h): this reporter's tag and the sorted tags of the whole party. Empty when not resolved.
    std::vector<uint8_t> partySelfTag;
    std::vector<std::vector<uint8_t>> partyTags;
    bool partyInAlliance = false;  // our party was in an alliance (another party present) when the event was built
};

inline Bytes Encode(const SourceEvent& e)
{
    PbWriter w;
    w.U(1, e.zone); w.U(2, e.sourceId); w.Str(3, e.name); w.U(4, e.model); w.U(5, e.type); w.U(6, e.eventMinute);
    for (auto& d : e.drops) { PbWriter m; m.U(1, d.item); m.U(2, d.qty); m.U(3, d.slot); w.Msg(7, m.Data()); }
    for (auto& p : e.party) { PbWriter m; m.U(1, p.job); m.U(2, p.level); w.Msg(8, m.Data()); }
    w.U(9, e.gil); w.B(10, e.defeated);
    w.S(11, e.px); w.S(12, e.py); w.S(13, e.pz);
    w.U(14, e.spawnMinute); w.U(15, e.chestKind); w.U(16, e.gilParty);
    w.U(17, e.killTick); w.U(18, e.eventMs); w.B(19, e.anchored); w.U(20, e.clientEventId);
    w.U(21, e.thActed); w.B(22, e.thKnown); w.U(23, e.gilPartyZone); w.U(24, e.thSelfTrait); w.U(25, e.thSelfGear);
    if (!e.partySelfTag.empty()) w.Raw(26, e.partySelfTag.data(), e.partySelfTag.size());
    for (auto& t : e.partyTags) w.Raw(27, t.data(), t.size());
    w.B(28, e.partyInAlliance);
    return w.Data();
}

// Payload: nonce(16) + timestamp + events. This exact byte string is what gets signed.
inline Bytes EncodePayloadRaw(const uint8_t nonce[16], uint64_t ts, const std::vector<Bytes>& encodedEvents)
{
    PbWriter w;
    w.Raw(1, nonce, 16); w.U(2, ts);
    for (auto& e : encodedEvents) w.Msg(3, e);
    return w.Data();
}

inline Bytes EncodePayload(const uint8_t nonce[16], uint64_t ts, const std::vector<SourceEvent>& evs)
{
    std::vector<Bytes> enc;
    for (auto& e : evs) enc.push_back(Encode(e));
    return EncodePayloadRaw(nonce, ts, enc);
}

inline Bytes EncodeEnvelope(const uint8_t pub[32], const Bytes& payload, const uint8_t sig[64])
{
    PbWriter w;
    w.U(1, 1); w.Raw(2, pub, 32); w.Raw(3, payload); w.Raw(4, sig, 64);
    return w.Data();
}

inline Bytes EncodeEnroll(const uint8_t pub[32])
{
    PbWriter w;
    w.U(1, 1); w.Raw(2, pub, 32);
    return w.Data();
}

} // namespace hxidb
