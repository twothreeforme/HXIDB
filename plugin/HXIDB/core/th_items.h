// Treasure Hunter bonus of equipment, by item ID. Generated from the reference database (item_mods, modifier 303); Horizon
// may differ for individual items, which is why the plugin reports the gear part and the trait part separately.
// Equipment can change at any moment, so the plugin reads what is worn when the player's own action on a monster arrives.
#pragma once
#include <cstdint>

namespace hxidb {

struct ThItem { uint16_t id; uint8_t bonus; };

static const ThItem kThItems[] = {
    {23358, 4}, {23537, 4},
    {21575, 3}, {23202, 3}, {26987, 3}, {27422, 3},
    {10695, 2}, {21574, 2}, {23717, 2}, {26986, 2}, {27421, 2},
    {25679, 1}, {50021, 1}, {14914, 1}, {15107, 1}, {11149, 1}, {20618, 1}, {21573, 1}, {22299, 1}, {23713, 1},
    {23721, 1}, {23725, 1}, {23729, 1}, {26197, 1}, {27585, 1}, {28450, 1},
};

inline uint32_t ThBonusOfItem(uint32_t itemId)
{
    for (const auto& t : kThItems) if (t.id == itemId) return t.bonus;
    return 0;
}

// Treasure Hunter from the Thief job trait: TH I from level 15, TH II from level 45 (Horizon). Applies to the main job at its
// level, and to the sub job at the sub job level.
inline uint32_t ThTraitTier(uint32_t mainJob, uint32_t mainLevel, uint32_t subJob, uint32_t subLevel)
{
    auto tier = [](uint32_t level) -> uint32_t { return level >= 45 ? 2u : level >= 15 ? 1u : 0u; };
    uint32_t t = 0;
    if (mainJob == 6) t = tier(mainLevel);
    if (subJob == 6) { uint32_t s = tier(subLevel); if (s > t) t = s; }
    return t;
}

} // namespace hxidb
