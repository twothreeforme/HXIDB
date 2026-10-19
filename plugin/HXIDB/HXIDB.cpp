// HXIDB - passive packet reader -> signed batches to the HXIDB backend.
//
// RELEASE build: no commands, no files except the status box position, nothing a player can change. The server address
// is compiled in; the reporter identity is derived from the character through the server without the server seeing it
// (core/oprf.h); events wait in memory only.
//
// TESTER build (-DHXIDB_DEV, see build.bat): adds /hxidb status|start|stop|probe, raw packet capture to JSONL and a
// devstatus.json heartbeat for scripted tests. Those write to disk and are never part of a release.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "Ashita.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
#include <fstream>
#include <string>
#include "core/builder.h"
#include "core/reporter.h"
#include "core/member_tags.h"

#ifndef HXIDB_BACKEND_URL
#define HXIDB_BACKEND_URL "http://127.0.0.1:8080"  // built in; set with build.bat <dev|release> <url>
#endif

/**
 * HXIDB - the Ashita plugin class (the "driver").
 *
 * Glue between the game client and the core library. It hands incoming packets to the EventBuilder, answers the
 * builder's questions about the party (who is in the zone, who is close enough to share gil, what Treasure Hunter we
 * have on), passes finished events to the Reporter and draws the small status box. All game memory reads happen on
 * the game thread; the network work lives in Reporter and MemberTags on their own threads.
 *
 * Nothing that identifies a player is stored or sent from here: ids are only compared in memory.
 */
class HXIDB final : public IPlugin
{
    // ---- layout of the game's party memory ----
    static constexpr uint32_t kAllianceSlots = 18;  // slots 0-5 own party, 6-11 and 12-17 the other two parties
    static constexpr uint32_t kPartySlots    = 6;
    static constexpr uint32_t kEquipSlots    = 16;

    // ---- tuning ----
    static constexpr float  kSplitRange     = 50.0f;  // the client only keeps members up to about this far from us up to date
    static constexpr uint32_t kRenderInRange = 0x200; // render flag bit set while a member is inside the update range
    static constexpr time_t kPartyRefreshSecs = 2;
    static constexpr time_t kPosSaveSecs      = 2;

    // ---- incoming packet ids we care about ----
    static constexpr uint16_t kPktZoneIn = 0x00A, kPktEntity = 0x00E, kPktAction = 0x028, kPktBattle = 0x029, kPktChest = 0x034, kPktPool = 0x0D2;

    IAshitaCore* m_core = nullptr;
    std::string m_configDir;  // <install>\config\HXIDB\ (built once in Initialize)
    float m_posX = 12.0f, m_posY = 12.0f;  // status box position: the only thing kept on disk
    time_t m_lastPosSave = 0;
    hxidb::EventBuilder m_builder;
    hxidb::MemberTags m_memberTags;
    hxidb::Reporter m_reporter;
    time_t m_lastParty = 0;
    uint32_t m_playerId = 0;  // the character's id, used only locally as the input of the identity derivation (never sent)
#ifdef HXIDB_DEV
    // DEV ONLY: state for the logger, the capture file, probe mode and the packet counters used by the heartbeat.
    ILogManager* m_log = nullptr;
    std::ofstream m_out;
    bool m_capturing = false, m_probe = false;
    uint64_t m_count = 0, m_inPkts = 0, m_outPkts = 0;
    ULONGLONG m_lastStatus = 0, m_lastNotice = 0;
#endif

public:
    const char* GetName(void) const override { return "HXIDB"; }
    const char* GetAuthor(void) const override { return "HXIDB"; }
    const char* GetDescription(void) const override { return "Drop data reporter."; }
    double GetVersion(void) const override { return 0.3; }
    uint32_t GetFlags(void) const override
    {
        uint32_t f = (uint32_t)(Ashita::PluginFlags::UsePackets | Ashita::PluginFlags::UseDirect3D);
#ifdef HXIDB_DEV
        // DEV ONLY: register the /hxidb command. Release has no commands, so nothing a player types reaches the plugin.
        f |= (uint32_t)Ashita::PluginFlags::UseCommands;
#endif
        return f;
    }

    /**
     * Called by Ashita when the plugin loads. Wires the builder callbacks and starts the two network threads.
     * @param core   Ashita core, used for all memory reads
     * @param logger Ashita log manager (only kept in the tester build)
     * @param id     plugin id given by Ashita (not used)
     * @return true so the plugin stays loaded
     */
    bool Initialize(IAshitaCore* core, ILogManager* logger, const uint32_t id) override
    {
        UNREFERENCED_PARAMETER(id);
        UNREFERENCED_PARAMETER(logger);
        m_core = core;
#ifdef HXIDB_DEV
        // DEV ONLY: keep the log manager so Say() can also write to the Ashita log.
        m_log = logger;
#endif
        m_configDir = BuildConfigDir();

        m_builder.SetMonotonicClock([]() { return (double)GetTickCount64() / 1000.0; });  // server-clock kill times, not PC time
        m_builder.SetEligibleFn([this](uint32_t mobId) { return EligibleForSplit(mobId); });
        m_builder.SetZoneMembersFn([this]() { return ZoneMembers(); });
        m_builder.SetSelfThFn([this]() { return SelfTh(); });
        m_builder.SetInAllianceFn([this]() { return InAlliance(); });
        m_builder.SetPartyTagsFn([this](std::vector<uint8_t>& self, std::vector<std::vector<uint8_t>>& all) { return PartyTags(self, all); });
        LoadPos();
        m_reporter.Start(HXIDB_BACKEND_URL);
        m_memberTags.Start(HXIDB_BACKEND_URL);
        //m_core->GetChatManager()->Writef(200, false, "[HXIDB] init done, url=%s", HXIDB_BACKEND_URL);
        return true;
    }

    /** Called by Ashita when the plugin unloads: saves the box position and stops the threads. */
    void Release(void) override
    {
        SavePos();
        m_memberTags.Stop();
        m_reporter.Stop();
#ifdef HXIDB_DEV
        // DEV ONLY: close the capture file if one is open.
        StopCapture();
#endif
    }

    // ---- status box position (the only file) ----

    /**
     * Builds (and creates, if needed) the plugin's config folder.
     * @return full path ending in a backslash, e.g. C:\Ashita\config\HXIDB\
     */
    std::string BuildConfigDir()
    {
        std::string base = m_core->GetInstallPath();
        if (!base.empty() && base.back() != '\\' && base.back() != '/') base += "\\";
        CreateDirectoryA((base + "config").c_str(), nullptr);
        CreateDirectoryA((base + "config\\HXIDB").c_str(), nullptr);
        return base + "config\\HXIDB\\";
    }

    /** Reads the saved box position. A missing file or values outside the screen range keep the defaults. */
    void LoadPos()
    {
        FILE* f = nullptr;
        if (fopen_s(&f, (m_configDir + "ui.ini").c_str(), "r") == 0 && f)
        {
            float x, y;
            if (fscanf_s(f, "%f %f", &x, &y) == 2 && x > -5000.0f && x < 10000.0f && y > -5000.0f && y < 10000.0f) { m_posX = x; m_posY = y; }
            fclose(f);
        }
    }

    /** Writes the box position. Failure to open the file is ignored. */
    void SavePos()
    {
        FILE* f = nullptr;
        if (fopen_s(&f, (m_configDir + "ui.ini").c_str(), "w") == 0 && f) { fprintf(f, "%.1f %.1f\n", m_posX, m_posY); fclose(f); }
    }

    // ---- party questions the builder asks us ----

    /** True when party slot `slot` is filled and the member is in the given zone. */
    static bool InZone(IParty* party, uint32_t slot, uint16_t zone)
    {
        return party->GetMemberIsActive(slot) && party->GetMemberZone(slot) == zone;
    }

    /**
     * Alliance members (slots 0-17) that share a mob's gil: active, same zone as us, strictly < 50 yalms from the mob.
     * The game splits among the whole alliance in the zone within 100 yalms (LandSandBoat DistributeGil, range marked as
     * unverified there); the client only sees about 50. Dead members count. Only positions and flags are read (no names/IDs).
     * Entity index = low 12 bits of the unique ID.
     * @param mobId unique id of the mob that died
     * @return number of members counted for the split (0 if we cannot see anybody)
     */
    uint32_t EligibleForSplit(uint32_t mobId)
    {
        auto mem = m_core->GetMemoryManager();
        auto party = mem->GetParty();
        auto ent = mem->GetEntity();
        uint32_t mobIdx = mobId & 0xFFF;
        uint16_t zone = party->GetMemberZone(0);
        float mobX = ent->GetLocalPositionX(mobIdx);  // does not change inside the loop
        float mobY = ent->GetLocalPositionY(mobIdx);
        uint32_t n = 0;
        for (uint32_t i = 0; i < kAllianceSlots; i++)  // the whole alliance shares
        {
            if (!InZone(party, i, zone)) continue;
            uint32_t idx = party->GetMemberTargetIndex(i);
            if (idx == 0 || ent->GetSpawnFlags(idx) == 0) continue;
            // A member beyond the game's update range (about 50 yalms from us) keeps a STALE record: same flags, frozen position.
            // Measured: the render flags read 0x200 while the member is in range and 0x4000 once they are not. We ourselves
            // (slot 0) are in range of the kill by definition.
            if (i != 0 && (ent->GetRenderFlags0(idx) & kRenderInRange) == 0) continue;
            float dx = ent->GetLocalPositionX(idx) - mobX;
            float dy = ent->GetLocalPositionY(idx) - mobY;
            if (dx * dx + dy * dy < kSplitRange * kSplitRange) n++;
        }
        //char dbg[64]; sprintf_s(dbg, "HXIDB: eligible=%u\n", n); OutputDebugStringA(dbg);
        return n;
    }

    /**
     * Alliance members (slots 0-17) in our zone whatever their distance: the most that can share a kill's gil. The server
     * shares with members up to about 100 yalms away, which the client cannot see beyond about 50, so the true split lies
     * between EligibleForSplit() and this number.
     * @return number of active members in our zone
     */
    uint32_t ZoneMembers()
    {
        auto party = m_core->GetMemoryManager()->GetParty();
        uint16_t zone = party->GetMemberZone(0);
        uint32_t n = 0;
        for (uint32_t i = 0; i < kAllianceSlots; i++)
            if (InZone(party, i, zone)) n++;
        return n;
    }

    /**
     * True when members of the other alliance parties (slots 6-17) are in OUR zone: only people physically together count.
     * An alliance party that is elsewhere does not make this kill an alliance kill. Zones only; no IDs.
     * @return true if another party of the alliance is in our zone
     */
    bool InAlliance()
    {
        auto party = m_core->GetMemoryManager()->GetParty();
        uint16_t zone = party->GetMemberZone(0);
        for (uint32_t i = kPartySlots; i < kAllianceSlots; i++)
            if (InZone(party, i, zone)) return true;
        return false;
    }

    /**
     * IDs of the active party members (slots 0-5) in our zone, ourselves included. Party members in another zone were not
     * at the kill, so they are left out of the tags: a party split over two zones gives two separate groups, each valid on
     * its own. Only ever compared and turned into anonymous tags; never stored or sent.
     * @return the ids (empty when we are not in a party yet)
     */
    std::vector<uint32_t> PartyMemberIds()
    {
        auto party = m_core->GetMemoryManager()->GetParty();
        uint16_t zone = party->GetMemberZone(0);
        std::vector<uint32_t> ids;
        ids.reserve(kPartySlots);
        for (uint32_t i = 0; i < kPartySlots; i++)
        {
            if (!InZone(party, i, zone)) continue;
            uint32_t id = party->GetMemberServerId(i);
            if (id) ids.push_back(id);
        }
        return ids;
    }

    /**
     * Anonymous party facts for an event.
     * @param self output: the tag of this player
     * @param all  output: the sorted tags of the whole party
     * @return false until every tag has been resolved by the server (the outputs are then left alone)
     */
    bool PartyTags(std::vector<uint8_t>& self, std::vector<std::vector<uint8_t>>& all)
    {
        auto ids = PartyMemberIds();
        if (ids.empty()) return false;
        uint32_t me = m_core->GetMemoryManager()->GetParty()->GetMemberServerId(0);
        hxidb::MemberTag selfTag{};
        std::vector<hxidb::MemberTag> sorted;
        if (!m_memberTags.Get(ids, me, selfTag, sorted)) return false;
        self.assign(selfTag.begin(), selfTag.end());
        all.clear();
        for (auto& t : sorted) all.emplace_back(t.begin(), t.end());
        return true;
    }

    /**
     * Treasure Hunter of the local player right now. Read at the moment one of the player's own actions on a monster
     * arrives, because equipment can be changed (and changed back) at any time and any job can add Treasure Hunter with a
     * single piece of gear. Item IDs only; nothing is stored or sent except the two numbers.
     * @return (job trait tier, bonus of the worn gear)
     */
    std::pair<uint32_t, uint32_t> SelfTh()
    {
        auto mem = m_core->GetMemoryManager();
        auto party = mem->GetParty();
        uint32_t trait = hxidb::ThTraitTier(party->GetMemberMainJob(0), party->GetMemberMainJobLevel(0),
                                            party->GetMemberSubJob(0), party->GetMemberSubJobLevel(0));
        uint32_t gear = 0;
        auto inv = mem->GetInventory();
        for (uint32_t slot = 0; slot < kEquipSlots; slot++)
        {
            auto eq = inv->GetEquippedItem(slot);
            if (!eq) continue;
            uint32_t idx = eq->Index & 0x00FF, cont = (eq->Index & 0xFF00) >> 8;
            if (idx == 0) continue;  // empty slot
            auto it = inv->GetContainerItem(cont, idx);
            if (it && it->Id) gear += hxidb::ThBonusOfItem(it->Id);
        }
        return {trait, gear};
    }

    // ---- per-frame work ----

    /**
     * Runs on the game thread once per frame: refreshes the party job/level list (no names/IDs) and the identity input
     * every couple of seconds, lets the builder emit finished kills and passes them on.
     */
    void Pump()
    {
        time_t now = time(nullptr);
        if (now - m_lastParty >= kPartyRefreshSecs)
        {
            m_lastParty = now;
            RefreshParty();
        }
        m_builder.Tick((double)now);
        DrainEvents();
#ifdef HXIDB_DEV
        // DEV ONLY: heartbeat (devstatus json), chat notices from notice.txt. Writes to disk, used by the scripted tests.
        DevStatus();
#endif
    }

    /** Copies the party's jobs/levels to the builder, asks for member tags and updates the identity input. */
    void RefreshParty()
    {
        auto party = m_core->GetMemoryManager()->GetParty();
        std::vector<hxidb::PartyMember> pm;
        pm.reserve(kAllianceSlots);
        for (uint32_t i = 0; i < kAllianceSlots; i++)
            if (party->GetMemberIsActive(i))
                pm.push_back({party->GetMemberMainJob(i), party->GetMemberMainJobLevel(i), party->GetMemberSubJob(i), party->GetMemberSubJobLevel(i), party->GetMemberServerId(i)});
        m_builder.SetParty(std::move(pm));
        m_memberTags.Request(PartyMemberIds());
        //OutputDebugStringA("HXIDB: party refreshed\n");

        // The character's id never leaves this PC: it is only the input of the blinded identity derivation.
        uint32_t pid = party->GetMemberServerId(0);
        if (pid != m_playerId)
        {
            m_playerId = pid;
            m_reporter.SetIdentityInput(pid ? "HorizonXI|player|" + std::to_string(pid) : "");
        }
    }

    /** Moves every event the builder has finished into the reporter's queue. */
    void DrainEvents()
    {
        hxidb::SourceEvent ev;
        while (m_builder.Pop(ev)) m_reporter.Enqueue(ev);
    }

    bool Direct3DInitialize(IDirect3DDevice8* device) override
    {
        UNREFERENCED_PARAMETER(device);
        return true;
    }

    /** Called by Ashita every frame. Does the per-frame work, then draws the status box. */
    void Direct3DPresent(const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) override
    {
        UNREFERENCED_PARAMETER(pSourceRect);
        UNREFERENCED_PARAMETER(pDestRect);
        UNREFERENCED_PARAMETER(hDestWindowOverride);
        UNREFERENCED_PARAMETER(pDirtyRegion);
        auto gui = m_core->GetGuiManager();
        if (gui == nullptr) return;
        Pump();
        DrawStatusBox(gui);
    }

    /**
     * Small rounded status box. Read-only: it shows that the plugin runs and what was sent; it has no controls.
     * @param gui Ashita's ImGui wrapper
     */
    void DrawStatusBox(IGuiManager* gui)
    {
        gui->PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
        gui->PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
        gui->SetNextWindowBgAlpha(0.75f);
        gui->SetNextWindowPos(ImVec2(m_posX, m_posY), ImGuiCond_Once);
        const int flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;
        if (gui->Begin("HXIDB##status", nullptr, flags))
        {
            // Connection dot: green connected, amber degraded, red offline, grey waiting for a character.
            using hxidb::Link;
            Link st = m_reporter.State();
            float pulse = 0.6f + 0.4f * (float)sin(gui->GetTime() * 3.0);
            ImVec4 col;
            const char* label;
            switch (st)
            {
            case Link::Connected: col = ImVec4(0.25f, 0.85f, 0.40f, pulse); label = "connected"; break;
            case Link::Degraded:  col = ImVec4(0.95f, 0.70f, 0.20f, 1.0f);  label = "degraded";  break;
            case Link::Offline:   col = ImVec4(0.90f, 0.25f, 0.25f, 1.0f);  label = "offline";   break;
            default:              col = ImVec4(0.60f, 0.60f, 0.60f, 1.0f);  label = "waiting for character"; break;
            }
            gui->TextColored(col, "(*)");
            gui->SameLine();
#ifdef HXIDB_DEV
            // DEV ONLY: mark the box so testers can tell which build is loaded.
            gui->Text("HXIDB %s (tester build)", label);
#else
            // release: plain label, no mention of tester features
            gui->Text("HXIDB %s", label);
#endif
            gui->Text("sent: %llu  queued: %zu", (unsigned long long)m_reporter.Sent(), m_reporter.Queued());
            // Exactly what was last transmitted (no player names or IDs exist in it).
            if (gui->CollapsingHeader("Last payload##hx"))
            {
                std::string lp = m_reporter.LastPayload();
                gui->TextUnformatted(lp.empty() ? "(nothing sent yet)" : lp.c_str());
            }
            RememberBoxPos(gui);
        }
        gui->End();
        gui->PopStyleVar(2);
    }

    /** Notes where the player dragged the box to and saves it at most every couple of seconds. */
    void RememberBoxPos(IGuiManager* gui)
    {
        ImVec2 wp = gui->GetWindowPos();
        if (wp.x == m_posX && wp.y == m_posY) return;
        m_posX = wp.x;
        m_posY = wp.y;
        time_t now = time(nullptr);
        if (now - m_lastPosSave >= kPosSaveSecs) { SavePos(); m_lastPosSave = now; }
    }

    // ---- packets ----

    bool HandleOutgoingPacket(uint16_t id, uint32_t size, const uint8_t* data, uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) override
    {
        UNREFERENCED_PARAMETER(id); UNREFERENCED_PARAMETER(size); UNREFERENCED_PARAMETER(data);
        UNREFERENCED_PARAMETER(modified); UNREFERENCED_PARAMETER(sizeChunk); UNREFERENCED_PARAMETER(dataChunk);
        UNREFERENCED_PARAMETER(injected); UNREFERENCED_PARAMETER(blocked);
#ifdef HXIDB_DEV
        // DEV ONLY: count outgoing packets and, in probe mode, log their ids/sizes to the capture file.
        m_outPkts++;
        if (m_capturing && m_probe && size <= 512) Probe(true, id, size, data);
#endif
        return false;
    }

    /** True for the packet ids the EventBuilder understands. */
    static bool IsBuilderPacket(uint16_t id)
    {
        return id == kPktZoneIn || id == kPktEntity || id == kPktAction || id == kPktBattle || id == kPktPool || id == kPktChest;
    }

    /**
     * Called by Ashita for every packet from the game server. Packets the builder knows are passed on; nothing is ever
     * blocked or changed.
     * @param id   packet id
     * @param size packet size in bytes
     * @param data packet bytes
     * @return always false (do not block the packet)
     */
    bool HandleIncomingPacket(uint16_t id, uint32_t size, const uint8_t* data, uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) override
    {
        UNREFERENCED_PARAMETER(modified); UNREFERENCED_PARAMETER(sizeChunk); UNREFERENCED_PARAMETER(dataChunk);
        UNREFERENCED_PARAMETER(injected); UNREFERENCED_PARAMETER(blocked);
#ifdef HXIDB_DEV
        // DEV ONLY: count incoming packets and, in probe mode, log their ids/sizes to the capture file.
        m_inPkts++;
        if (m_capturing && m_probe && size <= 512) Probe(false, id, size, data);
#endif
        if (size >= 8 && size <= 512 && IsBuilderPacket(id))
        {
            //printf("pkt 0x%03X size=%u\n", id, size);
            // Builder only keeps mob/chest entities and the fields in the approved packet scope.
            m_builder.OnPacket(id, data, size, m_core->GetMemoryManager()->GetParty()->GetMemberZone(0), (double)time(nullptr));
            DrainEvents();
        }
#ifdef HXIDB_DEV
        // DEV ONLY: write the scrubbed packet to the capture file for offline replay (test\replay.cpp).
        Capture(id, size, data);
#endif
        return false;  // never block or alter packets
    }

#ifdef HXIDB_DEV
    // ================= tester build only: commands, capture, heartbeat (all write to disk or print) =================
    // Purpose: lets testers record packets (scrubbed of names/ids), replay them offline with test/replay.cpp and check the plugin
    // state from scripts. None of this may exist in a release build; the release DLL must not contain these strings.

    /** Prints a line in the game chat and in the Ashita log. */
    void Say(const char* msg)
    {
        m_core->GetChatManager()->Writef(200, false, "[HXIDB] %s", msg);
        m_log->Logf((uint32_t)Ashita::LogLevel::Info, "HXIDB", "%s", msg);
    }

    /** Hex text of a byte range, lower case. */
    static std::string ToHex(const uint8_t* b, size_t n)
    {
        static const char* hex = "0123456789abcdef";
        std::string h;
        h.reserve(n * 2);
        for (size_t i = 0; i < n; i++) { h += hex[b[i] >> 4]; h += hex[b[i] & 15]; }
        return h;
    }

    /** Opens a new capture-<date>.jsonl file. Does nothing if a capture is already running. */
    void StartCapture()
    {
        if (m_capturing) return;
        char name[64];
        time_t t = time(nullptr);
        strftime(name, sizeof(name), "capture-%Y%m%d-%H%M%S.jsonl", localtime(&t));
        m_out.open(m_configDir + name, std::ios::out | std::ios::app);
        m_capturing = m_out.is_open();
        Say(m_capturing ? "capture started (writes to disk; tester build only)" : "could not open capture file");
    }

    /** Closes the capture file. */
    void StopCapture()
    {
        if (!m_capturing) return;
        m_out.close();
        m_capturing = false;
        Say("capture stopped");
    }

    /**
     * Handles /hxidb [probe|start|stop]; with no argument it prints the status line.
     * @param command the full command line typed by the player
     * @return true when the command was ours (so Ashita does not pass it on)
     */
    bool HandleCommand(int32_t mode, const char* command, bool injected) override
    {
        UNREFERENCED_PARAMETER(mode);
        UNREFERENCED_PARAMETER(injected);
        if (_strnicmp(command, "/hxidb", 6) != 0) return false;
        if (strstr(command, "probe"))
        {
            m_probe = !m_probe;
            if (m_probe && !m_capturing) StartCapture();
            Say(m_probe ? "probe ON (logs all packet ids/sizes to disk)" : "probe OFF");
        }
        else if (strstr(command, "start")) StartCapture();
        else if (strstr(command, "stop")) StopCapture();
        else
        {
            char b[200];
            sprintf_s(b, "tester build: capturing=%d packets=%llu sent=%llu queued=%zu url=%.40s", m_capturing ? 1 : 0, (unsigned long long)m_count,
                      (unsigned long long)m_reporter.Sent(), m_reporter.Queued(), HXIDB_BACKEND_URL);
            Say(b);
        }
        return true;
    }

    /**
     * Tester coordination: lines in Ashita\config\HXIDB\notice.txt are shown in the chat window as "[HXIDB] text" and the
     * file is deleted. DISPLAY ONLY: the text is printed, never interpreted or run as a command. At most 5 lines of 120
     * characters, printable ASCII only. Tester build only.
     */
    void Notices()
    {
        ULONGLONG t = GetTickCount64();
        if (t - m_lastNotice < 1000) return;
        m_lastNotice = t;
        std::string path = m_configDir + "notice.txt";
        std::ifstream in(path);
        if (!in) return;
        std::vector<std::string> lines;
        for (std::string l; std::getline(in, l) && lines.size() < 5;)
        {
            std::string clean;
            for (char c : l) if (c >= 32 && c < 127 && clean.size() < 120) clean += c;
            if (!clean.empty()) lines.push_back(clean);
        }
        in.close();
        DeleteFileA(path.c_str());
        for (auto& l : lines) m_core->GetChatManager()->Writef(200, false, "[HXIDB] %s", l.c_str());
    }

    /**
     * Per-member readout (tester build only): what the client knows about party members, to judge who is really in range.
     * @return the members of slots 1-5 as a comma separated list of JSON objects
     */
    std::string MemberReadout()
    {
        auto party = m_core->GetMemoryManager()->GetParty();
        auto ent = m_core->GetMemoryManager()->GetEntity();
        std::string mem;
        for (uint32_t i = 1; i < kPartySlots; i++)
        {
            if (!party->GetMemberIsActive(i)) continue;
            uint32_t idx = party->GetMemberTargetIndex(i);
            char b[300];
            snprintf(b, sizeof(b), "%s{\"slot\":%u,\"zone\":%u,\"tidx\":%u,\"spawn\":%u,\"render\":%u,\"dist\":%.1f,\"x\":%.1f,\"y\":%.1f,\"upd\":%u,\"hpp\":%u,\"flag\":%u,\"zone2\":%u,\"sid\":%u}",
                     mem.empty() ? "" : ",", i, party->GetMemberZone(i), idx, idx ? ent->GetSpawnFlags(idx) : 0, idx ? ent->GetRenderFlags0(idx) : 0,
                     idx ? sqrtf(ent->GetDistance(idx)) : -1.f, idx ? ent->GetLocalPositionX(idx) : 0.f, idx ? ent->GetLocalPositionY(idx) : 0.f,
                     party->GetMemberLastUpdatedTimestamp(i), party->GetMemberHPPercent(i), party->GetMemberFlagMask(i), party->GetMemberZone2(i), party->GetMemberServerId(i) != 0);
            mem += b;
        }
        return mem;
    }

    /** While Ashita\config\HXIDB\devstatus.enabled exists, devstatus-<id>.json is rewritten every 2 s (scripted tests only). */
    void DevStatus()
    {
        ULONGLONG t = GetTickCount64();
        if (t - m_lastStatus < 2000) return;
        m_lastStatus = t;
        Notices();
        if (GetFileAttributesA((m_configDir + "devstatus.enabled").c_str()) == INVALID_FILE_ATTRIBUTES) return;
        auto party = m_core->GetMemoryManager()->GetParty();
        uint32_t members = 0;
        for (uint32_t i = 0; i < kAllianceSlots; i++) if (party->GetMemberIsActive(i)) members++;
        auto th = SelfTh();
        std::ofstream st(m_configDir + "devstatus-" + std::to_string(m_playerId) + ".json", std::ios::trunc);
        st << "{\"id\":" << m_playerId << ",\"t\":" << time(nullptr) << ",\"zone\":" << party->GetMemberZone(0) << ",\"party\":" << members
           << ",\"in_packets\":" << m_inPkts << ",\"out_packets\":" << m_outPkts << ",\"capturing\":" << (m_capturing ? 1 : 0)
           << ",\"sent\":" << m_reporter.Sent() << ",\"queued\":" << m_reporter.Queued() << ",\"th_trait\":" << th.first << ",\"th_gear\":" << th.second
           << ",\"members\":[" << MemberReadout() << "]}\n";
    }

    /** Zero any run of >=3 printable ASCII letters (names) in [from, to). */
    static void ScrubText(uint8_t* b, size_t from, size_t to)
    {
        size_t i = from;
        while (i < to)
        {
            size_t j = i;
            while (j < to && ((b[j] >= 'A' && b[j] <= 'Z') || (b[j] >= 'a' && b[j] <= 'z'))) j++;
            if (j - i >= 3) memset(b + i, 0, j - i);
            i = (j == i) ? i + 1 : j;
        }
    }

    /** True for the packet ids whose bytes the probe records (scrubbed); other ids only get id and size logged. */
    static bool IsCandidate(bool outgoing, uint16_t id)
    {
        static const uint16_t in[]  = {0x01D, 0x01E, 0x01F, 0x020, 0x027, 0x029, 0x02A, 0x032, 0x033, 0x034, 0x036, 0x052, 0x05C, 0x0D3, 0x0D4, 0x04B};
        static const uint16_t out[] = {0x01A, 0x05A, 0x05B, 0x05C, 0x0C3};
        if (outgoing) { for (auto v : out) if (v == id) return true; }
        else { for (auto v : in) if (v == id) return true; }
        return false;
    }

    /**
     * Probe mode: logs every packet's id and size, and the scrubbed bytes of the candidate ids.
     * @param outgoing true for packets the client sends
     * @param id       packet id
     * @param size     packet size (caller keeps it <= 512)
     * @param data     packet bytes
     */
    void Probe(bool outgoing, uint16_t id, uint32_t size, const uint8_t* data)
    {
        uint16_t zone = m_core->GetMemoryManager()->GetParty()->GetMemberZone(0);
        char head[160];
        if (!IsCandidate(outgoing, id))
        {
            sprintf_s(head, "{\"t\":%llu,\"zone\":%u,\"probe\":1,\"dir\":\"%s\",\"id\":\"0x%03X\",\"size\":%u}\n", (unsigned long long)time(nullptr), zone, outgoing ? "out" : "in", id, size);
            m_out << head;
            return;
        }
        uint8_t buf[512];
        memcpy(buf, data, size);
        ScrubText(buf, 4, size);
        sprintf_s(head, "{\"t\":%llu,\"zone\":%u,\"probe\":1,\"dir\":\"%s\",\"id\":\"0x%03X\",\"size\":%u,\"hex\":\"", (unsigned long long)time(nullptr), zone, outgoing ? "out" : "in", id, size);
        m_out << head << ToHex(buf, size) << "\"}\n";
    }

    /**
     * Scrubbed packet capture (player ids and names blanked) for offline replay.
     * @param id   packet id
     * @param size packet size in bytes
     * @param data packet bytes
     */
    void Capture(uint16_t id, uint32_t size, const uint8_t* data)
    {
        if (!m_capturing || size < 8 || size > 512) return;
        if (id != 0x00A && id != 0x00E && id != 0x0D2 && id != 0x0DD && id != 0x029) return;
        uint8_t buf[512];
        memcpy(buf, data, size);
        if (id == 0x029)
        {
            if (size < 0x1C) return;
            uint16_t msg = *(uint16_t*)(buf + 0x18);
            if (msg != 6 && msg != 565) return;
            memset(buf + 4, 0, 4);                  // caster/actor ID
            if (msg == 565) memset(buf + 8, 0, 4);  // target is the player
        }
        else if (id == 0x00E)
        {
            uint16_t idx = *(uint16_t*)(buf + 0x08);
            if (idx >= 0x400) return;  // players / pets
        }
        else if (id == 0x00A) { memset(buf + 4, 0, 6); ScrubText(buf, 8, size); }
        else if (id == 0x0DD) { memset(buf + 4, 0, 4); ScrubText(buf, 8, size); }
        uint16_t zone = m_core->GetMemoryManager()->GetParty()->GetMemberZone(0);
        char head[128];
        sprintf_s(head, "{\"t\":%llu,\"zone\":%u,\"id\":\"0x%03X\",\"size\":%u,\"hex\":\"", (unsigned long long)time(nullptr), zone, id, size);
        m_out << head << ToHex(buf, size) << "\"}\n";
        m_out.flush();
        m_count++;
    }
#endif  // HXIDB_DEV (end of the tester-only section: commands, capture, probe, heartbeat)
};

#pragma comment(linker, "/EXPORT:expGetInterfaceVersion=_expGetInterfaceVersion@0")
#pragma comment(linker, "/EXPORT:expCreatePlugin=_expCreatePlugin@4")
extern "C"
{
    double __stdcall expGetInterfaceVersion(void) { return ASHITA_INTERFACE_VERSION; }
    IPlugin* __stdcall expCreatePlugin(const char* args)
    {
        UNREFERENCED_PARAMETER(args);
        return new HXIDB();
    }
}
