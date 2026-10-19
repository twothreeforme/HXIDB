// Checks the action-packet (0x028) parsing that records which Thief acted on a mob. Packets are built bit by bit in the
// layout documented in core/builder.h (actor at 0x05, targets from bit 150, LSB first).
#include <cstdio>
#include <cstring>
#include <vector>
#include "../core/builder.h"

struct BitWriter
{
    std::vector<uint8_t> b;
    size_t pos = 0;
    void Put(uint32_t v, int n) { for (int i = 0; i < n; i++, pos++) { if ((pos >> 3) >= b.size()) b.resize((pos >> 3) + 1, 0); if ((v >> i) & 1u) b[pos >> 3] |= (uint8_t)(1u << (pos & 7)); } }
};

// One action packet: actor does `nActions` actions to each target; the first action of target 0 carries an added effect and a spike
// effect so the variable-length path is exercised.
static std::vector<uint8_t> ActionPacket(uint32_t actor, const std::vector<uint32_t>& targets)
{
    BitWriter w;
    w.b.assign(0x09, 0);
    memcpy(&w.b[0x05], &actor, 4);
    w.b[0] = 0x28; w.b[1] = 0;  // id, size filled later
    w.pos = 0x09 * 8;
    w.Put((uint32_t)targets.size(), 10); w.Put(1, 4); w.Put(0, 16); w.Put(0, 32);  // count, category, param, recast
    w.Put(0, 16);                                                                    // unknown -> bit 150
    for (size_t t = 0; t < targets.size(); t++)
    {
        w.Put(targets[t], 32);
        w.Put(2, 4);  // two actions on this target
        for (int a = 0; a < 2; a++)
        {
            w.Put(0x1F, 5); w.Put(0xABC, 12); w.Put(0x55, 7); w.Put(5, 3); w.Put(1234, 17); w.Put(110, 10); w.Put(0x7FFFFFFF, 31);
            bool add = (t == 0 && a == 0), spike = (t == 0 && a == 0);
            w.Put(add, 1);
            if (add) { w.Put(0x3FF, 10); w.Put(0xF, 4); w.Put(777, 17); w.Put(0x2AA, 10); }
            w.Put(spike, 1);
            if (spike) { w.Put(0x155, 10); w.Put(0x9, 4); w.Put(321, 14); w.Put(0x3FF, 10); }
        }
    }
    w.b.resize(((w.pos + 7) / 8 + 3) & ~3u, 0);
    w.b[1] = (uint8_t)(w.b.size() / 4 * 2);
    return w.b;
}

static std::vector<uint8_t> DefeatPacket(uint32_t mob)
{
    std::vector<uint8_t> p(0x20, 0);
    p[0] = 0x29;
    uint16_t msg = 6;
    memcpy(&p[0x08], &mob, 4);
    memcpy(&p[0x18], &msg, 2);
    return p;
}

static int bad = 0;
static void Check(const char* what, bool ok) { printf("%-66s %s\n", what, ok ? "ok" : "FAIL"); if (!ok) bad++; }

static hxidb::SourceEvent Kill(hxidb::EventBuilder& b, uint32_t mob, double now)
{
    auto p = DefeatPacket(mob);
    b.OnPacket(0x029, p.data(), (uint32_t)p.size(), 102, now);
    b.Tick(now + 10);
    hxidb::SourceEvent ev;
    return b.Pop(ev) ? ev : hxidb::SourceEvent{};
}

int main()
{
    const uint32_t me = 0x1000AA, thief = 0x1000BB, mobA = 0x01066010, mobB = 0x01066011, mobC = 0x01066012;
    auto setup = [&](hxidb::EventBuilder& b)
    {
        b.SetParty({{4, 99, 0, 0, me}, {6, 99, 5, 49, thief}});
    };

    {   // The Thief acts on mob A only (mob B is a second target of the same action).
        hxidb::EventBuilder b; setup(b);
        auto a = ActionPacket(thief, {mobA, mobB});
        b.OnPacket(0x028, a.data(), (uint32_t)a.size(), 102, 100.0);
        auto ev = Kill(b, mobA, 101.0);
        Check("Thief acted on the mob: thKnown, level 99", ev.thKnown && ev.thActed == 99);
        auto evB = Kill(b, mobB, 102.0);
        Check("second target of the same action is credited too", evB.thKnown && evB.thActed == 99);
        auto evC = Kill(b, mobC, 103.0);
        Check("mob nobody acted on: known, level 0", evC.thKnown && evC.thActed == 0);
    }
    {   // A non-Thief acts: no credit.
        hxidb::EventBuilder b; setup(b);
        auto a = ActionPacket(me, {mobA});
        b.OnPacket(0x028, a.data(), (uint32_t)a.size(), 102, 100.0);
        auto ev = Kill(b, mobA, 101.0);
        Check("non-Thief actor gives no Thief level", ev.thKnown && ev.thActed == 0);
    }
    {   // Not in the party: no credit. Sub-job Thief counts at the sub level.
        hxidb::EventBuilder b; setup(b);
        auto a = ActionPacket(0xDEAD, {mobA});
        b.OnPacket(0x028, a.data(), (uint32_t)a.size(), 102, 100.0);
        Check("actor outside the party gives no level", Kill(b, mobA, 101.0).thActed == 0);
        b.SetParty({{4, 99, 0, 0, me}, {1, 99, 6, 37, thief}});
        auto a2 = ActionPacket(thief, {mobB});
        b.OnPacket(0x028, a2.data(), (uint32_t)a2.size(), 102, 102.0);
        Check("sub-job Thief counts at the sub-job level", Kill(b, mobB, 103.0).thActed == 37);
    }
    {   // Without any action packet the result is "not measured".
        hxidb::EventBuilder b; setup(b);
        auto ev = Kill(b, mobA, 101.0);
        Check("no action packets seen: thKnown false", !ev.thKnown && ev.thActed == 0);
    }
    {   // The encoded event carries the fields; the party IDs are not in it.
        hxidb::EventBuilder b; setup(b);
        auto a = ActionPacket(thief, {mobA});
        b.OnPacket(0x028, a.data(), (uint32_t)a.size(), 102, 100.0);
        auto bytes = hxidb::Encode(Kill(b, mobA, 101.0));
        bool leaks = false;
        for (uint32_t id : {me, thief})
            for (size_t i = 0; i + 4 <= bytes.size(); i++) if (memcmp(&bytes[i], &id, 4) == 0) leaks = true;
        Check("encoded event contains no party member ID", !leaks);
    }
    {   // Our own action: the trait and the gear worn at that moment are read when the packet arrives.
        hxidb::EventBuilder b; setup(b);
        uint32_t gear = 1, trait = 0;
        b.SetSelfThFn([&]() { return std::make_pair(trait, gear); });
        auto a = ActionPacket(me, {mobA});  // "me" is a Black Mage here: no Thief level, but one TH+1 piece worn
        b.OnPacket(0x028, a.data(), (uint32_t)a.size(), 102, 100.0);
        gear = 0;                           // swapped back right after the action
        auto a2 = ActionPacket(me, {mobA});
        b.OnPacket(0x028, a2.data(), (uint32_t)a2.size(), 102, 101.0);
        auto ev = Kill(b, mobA, 102.0);
        Check("non-Thief with one TH piece: gear 1 is kept after swapping back", ev.thKnown && ev.thSelfGear == 1 && ev.thSelfTrait == 0 && ev.thActed == 0);
        trait = 2; gear = 2;
        auto a3 = ActionPacket(me, {mobB});
        b.OnPacket(0x028, a3.data(), (uint32_t)a3.size(), 102, 103.0);
        auto a4 = ActionPacket(thief, {mobB});
        b.OnPacket(0x028, a4.data(), (uint32_t)a4.size(), 102, 104.0);
        auto evB = Kill(b, mobB, 105.0);
        Check("own trait 2 + gear 2 and another Thief acting are all recorded", evB.thSelfTrait == 2 && evB.thSelfGear == 2 && evB.thActed == 99);
        auto evC = Kill(b, mobC, 106.0);
        Check("mob we did not act on: no self Treasure Hunter", evC.thSelfTrait == 0 && evC.thSelfGear == 0);
    }
    {   // Anonymous party facts: the tags of the whole party travel with the event; no game ID does.
        hxidb::EventBuilder b; setup(b);
        std::vector<uint8_t> selfTag = {9, 1, 1, 1, 1, 1, 1, 1};
        std::vector<std::vector<uint8_t>> all = {{1, 2, 3, 4, 5, 6, 7, 8}, selfTag};
        b.SetPartyTagsFn([&](std::vector<uint8_t>& s, std::vector<std::vector<uint8_t>>& a) { s = selfTag; a = all; return true; });
        b.SetInAllianceFn([] { return true; });
        auto ev = Kill(b, mobA, 100.0);
        Check("party facts: alliance flag is on the event", ev.partyInAlliance);
        Check("party facts: own tag and the sorted tags of the party are on the event", ev.partySelfTag == selfTag && ev.partyTags.size() == 2);
        auto bytes = hxidb::Encode(ev);
        bool carriesTags = false, leaksId = false;
        for (size_t i = 0; i + 8 <= bytes.size(); i++) if (memcmp(&bytes[i], selfTag.data(), 8) == 0) carriesTags = true;
        for (uint32_t id : {me, thief})
            for (size_t i = 0; i + 4 <= bytes.size(); i++) if (memcmp(&bytes[i], &id, 4) == 0) leaksId = true;
        Check("encoded event carries the tags and still no party member ID", carriesTags && !leaksId);
        hxidb::EventBuilder b2; setup(b2);
        b2.SetPartyTagsFn([&](std::vector<uint8_t>&, std::vector<std::vector<uint8_t>>&) { return false; });  // not resolved yet
        auto ev2 = Kill(b2, mobA, 100.0);
        Check("tags not resolved: the event simply carries no party facts", ev2.partyTags.empty() && ev2.partySelfTag.empty());
    }
    Check("item table and trait tiers: armlets and White Rarab Cap +1 are TH+1; Thief 45 is tier 2, 44 tier 1, 14 none; sub Thief 37 tier 1",
          hxidb::ThBonusOfItem(15107) == 1 && hxidb::ThBonusOfItem(25679) == 1 && hxidb::ThBonusOfItem(1) == 0 &&
          hxidb::ThTraitTier(6, 45, 0, 0) == 2 && hxidb::ThTraitTier(6, 44, 0, 0) == 1 && hxidb::ThTraitTier(6, 14, 0, 0) == 0 &&
          hxidb::ThTraitTier(5, 75, 6, 37) == 1 && hxidb::ThTraitTier(8, 75, 5, 37) == 0);
    printf(bad ? "FAILED\n" : "all ok\n");
    return bad ? 1 : 0;
}
