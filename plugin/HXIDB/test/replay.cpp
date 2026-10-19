// Replays capture-*.jsonl through EventBuilder and prints the resulting events.
// Usage: replay.exe <capture.jsonl> [--party job:level,job:level] [--eligible N] [--post <baseUrl> <stateDir>]
//   --post     sends signed envelopes through the real Reporter (the second argument is ignored; nothing is stored)
//   --player N the character id the identity is derived from (default 1); different N = a different reporter
//   --party    simulates the party job/level list the loaded plugin reads from Ashita (job 6 = THF)
//   --eligible simulates the plugin's eligible-for-gil-split count (EligibleForSplit)
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include "../core/builder.h"
#include "../core/reporter.h"

using namespace hxidb;

static bool Field(const std::string& l, const char* key, std::string& out)
{
    std::string k = std::string("\"") + key + "\":";
    size_t p = l.find(k);
    if (p == std::string::npos) return false;
    p += k.size();
    bool q = l[p] == '"';
    if (q) p++;
    size_t e = l.find(q ? '"' : ',', p);
    if (e == std::string::npos) e = l.find('}', p);
    out = l.substr(p, e - p);
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: replay <capture.jsonl> [--bin out.pb]\n"); return 2; }
    std::string postUrl, postDir;
    std::vector<PartyMember> party;
    uint32_t eligible = 0, player = 1;
    for (int i = 2; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--post" && i + 2 < argc) { postUrl = argv[i + 1]; postDir = argv[i + 2]; i += 2; }
        else if (a == "--eligible" && i + 1 < argc) eligible = (uint32_t)std::stoul(argv[++i]);
        else if (a == "--player" && i + 1 < argc) player = (uint32_t)std::stoul(argv[++i]);
        else if (a == "--party" && i + 1 < argc)
        {
            std::string l = argv[++i];
            size_t p = 0;
            while (p < l.size())
            {
                size_t e = l.find(',', p);
                if (e == std::string::npos) e = l.size();
                size_t c = l.find(':', p);
                if (c != std::string::npos && c < e) party.push_back({(uint32_t)std::stoul(l.substr(p, c - p)), (uint32_t)std::stoul(l.substr(c + 1, e - c - 1))});
                p = e + 1;
            }
        }
    }
    std::ifstream in(argv[1]);
    EventBuilder b;
    if (!party.empty()) b.SetParty(party);
    if (eligible) b.SetEligibleFn([eligible](uint32_t) { return eligible; });
    std::vector<SourceEvent> all;
    std::string line;
    double last = 0;
    while (std::getline(in, line))
    {
        std::string t, zone, id, hex;
        if (line.find("\"probe\"") != std::string::npos && line.find("\"dir\":\"in\",\"id\":\"0x034\"") == std::string::npos) continue;  // probe lines only carry the chest packet
        if (!Field(line, "t", t) || !Field(line, "zone", zone) || !Field(line, "id", id) || !Field(line, "hex", hex)) continue;
        std::vector<uint8_t> d;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) d.push_back((uint8_t)std::stoi(hex.substr(i, 2), nullptr, 16));
        last = (double)std::stoull(t);
        b.OnPacket((uint16_t)std::stoi(id, nullptr, 16), d.data(), (uint32_t)d.size(), (uint32_t)std::stoul(zone), last);
        SourceEvent e;
        while (b.Pop(e)) all.push_back(e);
    }
    b.Tick(last + 10);
    SourceEvent e;
    while (b.Pop(e)) all.push_back(e);

    for (auto& ev : all)
    {
        printf("%s zone=%u id=0x%08x name=%s min=%u gil=%u P=%u party=%zu model=%u tick=%u ms=%llu%s kind=%u pos=(%d,%d) drops=[", ev.type == SRC_MOB ? "KILL " : "CHEST", ev.zone, ev.sourceId, ev.name.c_str(), ev.eventMinute, ev.gil, ev.gilParty, ev.party.size(), ev.model, ev.killTick, (unsigned long long)ev.eventMs, ev.anchored ? "(srv)" : "", ev.chestKind, ev.px, ev.py);
        for (auto& d : ev.drops) printf("%u x%u s%u ", d.item, d.qty, d.slot);
        printf("]\n");
    }
    printf("%zu events\n", all.size());

    if (!postUrl.empty())
    {
        Reporter r;
        r.Start(postUrl);
        r.SetIdentityInput("HorizonXI|player|" + std::to_string(player));
        for (auto& ev : all) r.Enqueue(ev);
        for (int i = 0; i < 300 && (r.Queued() > 0 || r.Sent() == 0); i++) { Sleep(100); if (r.Sent() == 0 && r.Queued() == 0 && i > 50) break; }
        printf("queued=%zu sent=%llu lastHttp=%d\n%s", r.Queued(), (unsigned long long)r.Sent(), r.LastHttp(), r.LastPayload().c_str());
        r.Stop();
    }
    return 0;
}

