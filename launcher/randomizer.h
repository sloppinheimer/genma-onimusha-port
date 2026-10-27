// The randomizer's generator (docs/randomizer.md). randomizer.dat (tools/rando/build_data.py, built into the launcher)
// holds the item locations, where each is patched in the game's files, and the logic; item names and counts come from
// the player's own copy of the game. A run is a folder randomizer\<name>\ next to the launcher:
//   run.ini       seed and options, as made
//   spoiler.txt   every location's item, and the order a playthrough can collect what unlocks progress
//   tables.txt    the Dark Realm's / Ogre Tower's floor items ("va item"), patched at start by the game (randomizer.c)
//   files\image\rdt\, rdt2\   the room files with their items changed, read before any mod (kernel.c)
//   saves\        the run's own saves (the game makes it)
// Placement: assumed fill. Each item that unlocks something goes, in random order, to a random empty location that is
// reachable with every such item not yet placed; so whatever a location needs is never placed behind it, and the
// goal is always reachable. The rest go anywhere. The finished seed is replayed from nothing to check it.
// Door warp (two-way): the doors randomizer.dat lists (D lines) are paired up at random, and each takes you to the
// front of the door it's paired with, which takes you back. The logic then runs on the new map; a layout no item
// placement can finish is thrown away and another tried.
// Simplifications: a seal's level counts as met once its orb is held (the souls to level it are assumed); Hard
// Mode's room files get the same items and doors as the normal ones.
#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../src/host/game.h"   // the game's addresses this reads from default.xbe, and the level items

namespace rando {

struct Location { int flag; std::string room, kind, cls; int item = 0, count = 1; std::vector<std::string> needs; int placed = -1, placed_count = 1; bool shuffled = false; };
struct Patch { int flag; std::string dir, file; long off, objoff; };
struct Exit { std::string from, to; std::vector<std::string> needs; bool door_only = false; };   // door_only: Y, made by shuffled doors
struct Door { int id; std::string room, to; std::vector<uint8_t> data; int partner; std::vector<std::string> needs; int paired = -1; };
struct DoorSite { int id; std::string dir, file; long off; };
struct Event { int n; std::string room; std::vector<std::string> needs; };
// The orb level items in a run's pool, two per orb (Thunder, Fire, Wind): picked up, each raises its orb and sword a
// level once the orb is held (the game side waits till then: input.c, orb_items_frame), so no seal needs souls ground
// out. The game names them by their orb's sword (Raizan, Enryuu, Shippuu).
static const int LEVEL_ITEMS[3][2] = {RANDO_LEVEL_ITEMS};   // game.h: the game side gives them their slots
static const char *const LEVEL_NAMES[3] = {"Thunder Orb level", "Fire Orb level", "Wind Orb level"};
struct Options { std::string seed, name; bool fluorite_sanity = false, long_challenges = false, doors = false, keep_consumables = false; };

struct Data {
    std::vector<Location> locs; std::vector<Patch> patches; std::vector<std::pair<int, unsigned>> tables;
    std::vector<Exit> exits; std::vector<Event> events; std::map<int, std::string> event_items;   // item -> room
    std::string goal_room; int goal_event = -1;
    std::vector<Door> doors; std::vector<DoorSite> door_sites;
    std::set<std::string> kaede_rooms; std::vector<DoorSite> kaede_locks;   // KR, KL: her parts, out of door-warp runs
};

static std::vector<std::string> split(const std::string &s)
{
    std::vector<std::string> t; size_t i = 0;
    while (i < s.size()) { while (i < s.size() && s[i] == ' ') i++; size_t j = i; while (j < s.size() && s[j] != ' ') j++; if (j > i) t.push_back(s.substr(i, j - i)); i = j; }
    return t;
}

static Data parse(const std::string &text)
{
    Data d; size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i); if (e == std::string::npos) e = text.size();
        std::string line = text.substr(i, e - i); i = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto t = split(line);
        if (t.empty() || t[0][0] == '#') continue;
        if (t[0] == "L" && t.size() >= 7) { Location l; l.flag = std::stoi(t[1]); l.room = t[2]; l.kind = t[3]; l.item = std::stoi(t[4], 0, 16); l.cls = t[6]; d.locs.push_back(l); }
        else if (t[0] == "R" && t.size() >= 2) { for (auto &l : d.locs) if (l.flag == std::stoi(t[1])) l.needs.assign(t.begin() + 2, t.end()); }
        else if (t[0] == "P" && t.size() >= 6) d.patches.push_back({std::stoi(t[1]), t[2], t[3], std::stol(t[4]), std::stol(t[5])});
        else if (t[0] == "T" && t.size() >= 3) d.tables.push_back({std::stoi(t[1]), (unsigned)std::stoul(t[2])});
        else if ((t[0] == "X" || t[0] == "Y") && t.size() >= 3) d.exits.push_back({t[1], t[2], std::vector<std::string>(t.begin() + 3, t.end()), t[0] == "Y"});
        else if (t[0] == "D" && t.size() >= 5) {
            Door dr; dr.id = std::stoi(t[1]); dr.room = t[2]; dr.to = t[3]; dr.partner = std::stoi(t[4]); dr.needs.assign(t.begin() + 5, t.end());
            d.doors.push_back(dr);
        }
        else if (t[0] == "KR" && t.size() >= 2) d.kaede_rooms.insert(t[1]);
        else if (t[0] == "KL" && t.size() >= 4) d.kaede_locks.push_back({-1, t[1], t[2], std::stol(t[3])});
        else if (t[0] == "Q" && t.size() >= 5) d.door_sites.push_back({std::stoi(t[1]), t[2], t[3], std::stol(t[4])});
        else if (t[0] == "E" && t.size() >= 3) d.events.push_back({std::stoi(t[1]), t[2], std::vector<std::string>(t.begin() + 3, t.end())});
        else if (t[0] == "I" && t.size() >= 3) d.event_items[std::stoi(t[1], 0, 16)] = t[2];
        else if (t[0] == "G" && t.size() >= 3) { d.goal_room = t[1]; d.goal_event = std::stoi(t[2].substr(1)); }
    }
    return d;
}

static bool read_file(const std::wstring &path, std::vector<uint8_t> &out)
{
    FILE *f = _wfopen(path.c_str(), L"rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(n); bool ok = fread(out.data(), 1, n, f) == (size_t)n; fclose(f); return ok;
}
// A run's files go into its own new folder, deleted whole if anything fails (main.cpp), so a plain write is enough,
// but every failure must reach the caller: a run missing tables.txt would play with floor items the logic moved.
static bool write_file(const std::wstring &path, const void *p, size_t n)
{
    FILE *f = _wfopen(path.c_str(), L"wb"); if (!f) return false;
    bool ok = fwrite(p, 1, n, f) == n;
    return !fclose(f) && ok;   // fclose flushes: a full disk shows up here
}

// The item names, from the game's executable: the item table (0x1fffa0, a name id per item) and the English
// "names and labels" message list (offsets at 0x203a68 into text at 0x202d80; a letter is 0xfa + (char - 0x20)).
struct Xbe {
    std::vector<uint8_t> raw; struct Sec { uint32_t va, vsize, raddr; }; std::vector<Sec> secs;
    bool load(const std::wstring &path)
    {
        if (!read_file(path, raw) || raw.size() < 0x200 || memcmp(raw.data(), "XBEH", 4)) return false;
        uint32_t base = *(uint32_t *)&raw[0x104], n = *(uint32_t *)&raw[0x11c], hdr = *(uint32_t *)&raw[0x120] - base;
        for (uint32_t k = 0; k < n && hdr + k * 56 + 20 <= raw.size(); k++) { auto *s = (uint32_t *)&raw[hdr + k * 56]; secs.push_back({s[1], s[2], s[3]}); }
        return true;
    }
    const uint8_t *at(uint32_t va) const
    {
        for (auto &s : secs) if (va >= s.va && va < s.va + s.vsize && s.raddr + (va - s.va) < raw.size()) return &raw[s.raddr + (va - s.va)];
        return nullptr;
    }
};
static std::string item_name(const Xbe &x, int id)
{
    const uint8_t *nid = id >= 0 && id < 0xa4 ? x.at(G_ITEM_TABLE + 8 * id) : nullptr, *tab = x.at(G_NAME_OFFS);
    if (!nid || !tab || !*nid) { char b[16]; snprintf(b, sizeof b, "item %02x", id); return b; }
    int k = *nid;
    const uint8_t *p = x.at(G_NAME_TEXT + ((const uint16_t *)tab)[k]), *end = k + 1 < G_NAMES ? x.at(G_NAME_TEXT + ((const uint16_t *)tab)[k + 1]) : tab;
    std::string s;
    for (; p && end && p < end; p++) {
        if (*p == 0xfa) { s += (char)(p[1] + 0x20); p++; }
        else if ((*p == 0 || *p == 0xf2) && !s.empty() && s.back() != ' ') s += ' ';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

struct Rng {   // splitmix64: the same seed and options always give the same run, on any machine
    uint64_t s;
    uint64_t next() { uint64_t z = (s += 0x9e3779b97f4a7c15ull); z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull; z = (z ^ (z >> 27)) * 0x94d049bb133111ebull; return z ^ (z >> 31); }
    int below(int n) { return (int)(next() % (uint64_t)n); }
    template <class T> void shuffle(std::vector<T> &v) { for (int i = (int)v.size() - 1; i > 0; i--) std::swap(v[i], v[below(i + 1)]); }
};

struct World {
    const Data &d; const Options &o;
    std::set<int> items, events; std::set<std::string> rooms; std::string start = "000";
    std::set<int> before;   // the items held when this sphere's rooms opened, before its pickups (step)
    World(const Data &d, const Options &o) : d(d), o(o) {}
    bool ok(const std::vector<std::string> &need) const
    {
        for (auto &t : need) {
            if (t[0] == 'i' && !items.count(std::stoi(t.substr(1), 0, 16))) return false;
            if (t[0] == 'e' && !events.count(std::stoi(t.substr(1)))) return false;
            if (t[0] == 'o') {   // a seal: its orb, and a level item of it per level above 1 (no souls assumed)
                int e = t[1] - '0', lv = t.size() > 2 ? t[2] - '0' : 1, have = 0;
                if (!items.count(0x11 + e)) return false;
                for (int k = 0; k < 2; k++) have += items.count(LEVEL_ITEMS[e][k]) ? 1 : 0;
                if (have < lv - 1) return false;
            }
        }
        return true;
    }
    bool can_take(const Location &l) const
    {
        if (!rooms.count(l.room) || !ok(l.needs)) return false;
        if (l.cls == "long" && !o.long_challenges) return false;   // off: nothing there counts (behind it gets filler)
        if (l.cls == "hidden" && !o.fluorite_sanity && !items.count(0x4f)) return false;   // Fluorite shows with the Vision Staff
        return true;
    }
    // One sphere: the rooms, events and event items reachable with what's held, then every placed item reachable
    // there. Returns whether anything new came.
    bool step(const std::vector<Location> &locs)
    {
        bool more = false;
        rooms.insert(start);
        for (bool grew = true; grew;) {
            grew = false;
            for (auto &x : d.exits) if (rooms.count(x.from) && !rooms.count(x.to) && ok(x.needs)) { rooms.insert(x.to); grew = more = true; }
            for (auto &e : d.events) if (!events.count(e.n) && rooms.count(e.room) && ok(e.needs)) { events.insert(e.n); grew = more = true; }
            for (auto &ei : d.event_items) if (!items.count(ei.first) && rooms.count(ei.second)) { items.insert(ei.first); grew = more = true; }
        }
        before = items;
        std::vector<int> got;
        for (auto &l : locs) if (l.placed >= 0 && !items.count(l.placed) && can_take(l)) got.push_back(l.placed);
        for (int i : got) more |= items.insert(i).second;
        return more;
    }
    void sweep(const std::vector<Location> &locs) { while (step(locs)) {} }
    // Can the start reach room r with only these items, and the events they lead to? (rooms and events only: no pickups)
    bool reaches(const std::string &r, const std::set<int> &with, int ban_event, const std::string &ban_room = "") const
    {
        World w(d, o);
        w.items = with; w.rooms.insert(start);
        for (bool grew = true; grew && !w.rooms.count(r);) {
            grew = false;
            for (auto &x : d.exits) if (w.rooms.count(x.from) && !w.rooms.count(x.to) && x.to != ban_room && w.ok(x.needs)) { w.rooms.insert(x.to); grew = true; }
            for (auto &e : d.events) if (e.n != ban_event && !w.events.count(e.n) && w.rooms.count(e.room) && w.ok(e.needs)) { w.events.insert(e.n); grew = true; }
            for (auto &ei : d.event_items) if (!w.items.count(ei.first) && w.rooms.count(ei.second)) { w.items.insert(ei.first); grew = true; }
        }
        return w.rooms.count(r) > 0;
    }
    // A room you could get stuck in: arriving there with the items held when its sphere opened, but only the events it
    // can't be reached without (or whose room is on every way to it), and picking up whatever can be reached from it,
    // the goal can't be reached. A player can get there having skipped a scene that sets the flag the way out needs
    // (playtest: shut in 207 behind its Red Key doors, the logic's way out was 235's portal, whose story 61 is set in a
    // room never visited). Items stay as held: the logic's map has ways round the game's own order that a player can't
    // take, and skipping by items across those made every seed fail. Each room is checked once, in the sphere it opens
    // in: arriving later with more only helps. The game's own one-way moves pass when the goal is still ahead of them.
    // "" if there's none.
    std::string trap(const std::vector<Location> &locs, std::set<std::string> &checked) const
    {
        std::set<int> used_events{d.goal_event};   // events a requirement names: the only ones that can matter
        auto note = [&](const std::vector<std::string> &n) { for (auto &t : n) if (t[0] == 'e') used_events.insert(std::stoi(t.substr(1))); };
        for (auto &x : d.exits) note(x.needs);
        for (auto &e : d.events) note(e.needs);
        for (auto &l : d.locs) note(l.needs);
        for (auto &r : rooms) {
            if (!checked.insert(r).second) continue;
            std::set<int> events_needed;
            for (int e : events) {
                if (!used_events.count(e)) continue;
                const Event *ev = nullptr;
                for (auto &x : d.events) if (x.n == e) ev = &x;
                if (!reaches(r, before, e) || (ev && ev->room != r && !reaches(r, before, -1, ev->room))) events_needed.insert(e);
            }
            World w(d, o);
            w.start = r; w.items = before; w.events = events_needed;
            w.sweep(locs);
            if (!w.beaten()) return r;
        }
        return "";
    }
    bool beaten() const { return rooms.count(d.goal_room) && events.count(d.goal_event); }
};

static std::string run_item_name(const Xbe &x, int id)   // the spoiler's name: a level item says which orb it's for
{
    for (int e = 0; e < 3; e++) for (int k = 0; k < 2; k++) if (LEVEL_ITEMS[e][k] == id) return LEVEL_NAMES[e];
    return item_name(x, id);
}
static uint64_t fnv(const std::string &s) { uint64_t h = 1469598103934665603ull; for (unsigned char c : s) h = (h ^ c) * 1099511628211ull; return h; }
// Herbs, Medicine, Talismans, Soul Absorbers, Power and Magic Jewels, arrows, bullets, Kunai: "keep consumables" leaves them be
static bool consumable(int item) { return (item >= 0x01 && item <= 0x06) || (item >= 0x17 && item <= 0x1a) || item == 0x1e; }
static bool floor_ok(int item) { return (item >= 0x01 && item <= 0x07) || (item >= 0x17 && item <= 0x1a) || item == 0x1e || item == 0x34 || item == 0x4f || item == 0x50; }

// Makes the run in run_dir from the game in game_dir. Returns "" or what went wrong.
// The placement and its check, without the game's files: what generate does between reading them and writing the
// run, so tests/selftest.cpp checks seeds (every one beatable, the same seed giving the same run) from randomizer.dat
// alone. d's counts and door bytes come from the room files when generate has read them; without them a location
// counts 1 and the doors have no arrival bytes, which the placement doesn't use. Fills each location's placed and
// spheres (the spoiler's playthrough; item names from xbe, or hex ids without one). "" or why it failed.
static std::string place(Data &d, const Options &o, const Xbe *xbe, std::string &spheres)
{
    auto name_of = [&](int id) { if (xbe) return run_item_name(*xbe, id); char b[8]; snprintf(b, sizeof b, "%02x", id); return std::string(b); };
    if (o.doors) {   // Kaede's parts stay out: no way into them (generate locks their doors)
        std::vector<Exit> kept;
        for (auto &x : d.exits) if (d.kaede_rooms.count(x.from) || !d.kaede_rooms.count(x.to)) kept.push_back(x);
        d.exits = kept;
    }
    for (auto &l : d.locs)
        l.shuffled = (l.cls != "long" || o.long_challenges) && !(o.keep_consumables && consumable(l.item));
    // what unlocks progress: every item a requirement names, the orbs, the Vision Staff
    std::set<int> prog;
    auto note = [&](const std::vector<std::string> &need) { for (auto &t : need) { if (t[0] == 'i') prog.insert(std::stoi(t.substr(1), 0, 16)); if (t[0] == 'o') prog.insert(0x11 + (t[1] - '0')); } };
    for (auto &x : d.exits) note(x.needs);
    for (auto &e : d.events) note(e.needs);
    for (auto &l : d.locs) note(l.needs);
    prog.insert(0x4f);
    for (auto &ei : d.event_items) prog.erase(ei.first);

    Rng rng{fnv(o.seed + (o.fluorite_sanity ? "|fs" : "") + (o.long_challenges ? "|lc" : "") + (o.doors ? "|dw" : "")
                + (o.keep_consumables ? "|kc" : ""))};
    std::vector<std::pair<int, int>> keys, rest;   // (item, count) taken from the shuffled locations
    for (auto &l : d.locs) {
        if (!l.shuffled) { l.placed = l.item; l.placed_count = l.count; continue; }

        (prog.count(l.item) ? keys : rest).push_back({l.item, l.count});
    }
    {   // the six level items take the places of six fillers: consumables first, then documents, then anything
        int taken = 0;
        for (int pass = 0; pass < 3 && taken < 6; pass++)
            for (size_t k = 0; k < rest.size() && taken < 6;) {
                int it = rest[k].first;
                if (pass == 2 || (pass == 0 ? consumable(it) : it >= 0x80 && it < 0xa4)) { rest.erase(rest.begin() + k); taken++; }
                else k++;
            }
        for (auto &e : LEVEL_ITEMS) for (int id : e) { keys.push_back({id, 1}); prog.insert(id); }
    }
    const std::vector<Exit> map_exits = d.exits;   // the fixed map; shuffled doors add theirs per layout
    std::map<int, Door *> door_by_id;
    for (auto &dr : d.doors) door_by_id[dr.id] = &dr;
    bool placed_all = false;
    std::string trapped;   // the last attempt's trap, for the error
    for (int attempt = 0; attempt < (o.doors ? 3200 : 50) && !placed_all; attempt++) {
        if (o.doors && attempt % 8 == 0) {   // a new door layout every 8 tries at the items
            d.exits.clear();
            for (auto &x : map_exits) if (!x.door_only) d.exits.push_back(x);
            std::vector<Door *> ds;
            for (auto &dr : d.doors) ds.push_back(&dr);
            rng.shuffle(ds);
            for (size_t k = 0; k + 1 < ds.size(); k += 2) {
                if (ds[k]->room == ds[k + 1]->room && k + 2 < ds.size()) std::swap(ds[k + 1], ds[k + 2]);   // not back into its own room
                ds[k]->paired = ds[k + 1]->id; ds[k + 1]->paired = ds[k]->id;
            }
            if (ds.size() % 2) ds.back()->paired = -1;   // an odd one out stays as it was
            for (auto *dr : ds) d.exits.push_back({dr->room, dr->paired < 0 ? dr->to : door_by_id[dr->paired]->room, dr->needs, false});
            World all(d, o);   // with every item: can the goal be reached on this map at all?
            for (int i : prog) all.items.insert(i);
            all.sweep(d.locs);
            if (!all.beaten()) { attempt += 7; continue; }
        }
        for (auto &l : d.locs) if (l.shuffled) l.placed = -1;
        rng.shuffle(keys);
        placed_all = true;
        for (size_t k = 0; k < keys.size() && placed_all; k++) {
            World w(d, o);
            for (size_t j = k + 1; j < keys.size(); j++) w.items.insert(keys[j].first);   // assumed: the ones still to place
            w.sweep(d.locs);
            std::vector<Location *> spots;
            for (auto &l : d.locs) if (l.shuffled && l.placed < 0 && w.can_take(l) && (l.kind != "floor" || floor_ok(keys[k].first))) spots.push_back(&l);
            if (spots.empty()) { placed_all = false; break; }
            Location *s = spots[rng.below((int)spots.size())];
            s->placed = keys[k].first; s->placed_count = keys[k].second;
        }
        if (placed_all) {   // no room you can get into without a way out, at any point of a playthrough
            World w(d, o);
            std::string t;
            std::set<std::string> checked;
            while (t.empty() && w.step(d.locs)) t = w.trap(d.locs, checked);
            if (!t.empty()) { placed_all = false; trapped = t; if (o.doors) attempt += 7 - attempt % 8; }
        }
    }
    if (!placed_all) return trapped.empty() ? "Couldn't place every key item with these options; try another seed."
                                            : "Every layout tried had a room you could get stuck in (last: room " + trapped + "); try another seed.";
    rng.shuffle(rest);
    std::vector<Location *> empty;
    for (auto &l : d.locs) if (l.shuffled && l.placed < 0) empty.push_back(&l);
    rng.shuffle(empty);
    // the floor locations first: they only take what a floor can hold
    std::stable_partition(empty.begin(), empty.end(), [](const Location *l) { return l->kind == "floor"; });
    for (Location *l : empty) {
        size_t k = 0;
        while (k < rest.size() && l->kind == "floor" && !floor_ok(rest[k].first)) k++;
        if (k == rest.size()) return "Couldn't fill the floor locations; try another seed.";
        l->placed = rest[k].first; l->placed_count = rest[k].second; rest.erase(rest.begin() + k);
    }
    // the check: a playthrough from nothing, sphere by sphere
    {
        World w(d, o);
        std::set<int> told;
        for (int sphere = 1; sphere < 300 && w.step(d.locs); sphere++) {
            std::string got;
            for (auto &l : d.locs)
                if (prog.count(l.placed) && !told.count(l.flag) && w.can_take(l)) { told.insert(l.flag); got += "    " + name_of(l.placed) + " (room " + l.room + ")\n"; }
            if (!got.empty()) spheres += "  Sphere " + std::to_string(sphere) + "\n" + got;
        }
        if (!w.beaten()) return "The seed failed its own check (the goal is out of reach); try another seed.";
    }
    return "";
}

static std::string generate(const std::string &dat, const std::wstring &game_dir, const std::wstring &run_dir, const Options &o)
{
    Data d = parse(dat);
    Xbe xbe;
    if (!xbe.load(game_dir + L"\\default.xbe")) return "The game's default.xbe couldn't be read.";
    std::map<std::string, std::vector<uint8_t>> files;   // "rdt\\room125.rdt" -> contents
    std::vector<std::string> need_files;
    for (auto &p : d.patches) need_files.push_back(p.dir + "\\" + p.file);
    if (o.doors) for (auto &q : d.door_sites) need_files.push_back(q.dir + "\\" + q.file);
    if (o.doors) for (auto &q : d.kaede_locks) need_files.push_back(q.dir + "\\" + q.file);
    for (auto &key : need_files)
        if (!files.count(key) && !read_file(game_dir + L"\\image\\" + std::wstring(key.begin(), key.end()), files[key])) return "A room file is missing: " + key;
    if (o.doors)   // each door's "where you arrive" bytes, from the normal room file
        for (auto &dr : d.doors)
            for (auto &q : d.door_sites)
                if (q.id == dr.id && q.dir == "rdt" && dr.data.empty()) {
                    auto &f = files[q.dir + "\\" + q.file];
                    if (q.off + 11 <= (long)f.size()) dr.data.assign(f.begin() + q.off, f.begin() + q.off + 11);
                }
    // the vanilla counts, from the room files (a floor item: one)
    for (auto &l : d.locs) {
        l.count = 1;
        for (auto &p : d.patches)
            if (p.flag == l.flag && p.dir == "rdt") { auto &f = files[p.dir + "\\" + p.file]; if (p.off + 4 <= (long)f.size()) l.count = *(uint16_t *)&f[p.off + 2]; break; }
    }
    std::string spheres, err = place(d, o, &xbe, spheres);
    if (!err.empty()) return err;
    std::map<int, Door *> door_by_id;   // for the door bytes below
    for (auto &dr : d.doors) door_by_id[dr.id] = &dr;

    // write the run
    CreateDirectoryW(run_dir.c_str(), 0);
    for (const wchar_t *sub : {L"\\files", L"\\files\\image", L"\\files\\image\\rdt", L"\\files\\image\\rdt2"}) CreateDirectoryW((run_dir + sub).c_str(), 0);
    for (auto &p : d.patches) {
        const Location *l = nullptr;
        for (auto &x : d.locs) if (x.flag == p.flag) l = &x;
        if (!l || !l->shuffled) continue;
        auto &f = files[p.dir + "\\" + p.file];
        if (p.off + 4 > (long)f.size()) continue;
        *(uint16_t *)&f[p.off] = (uint16_t)l->placed;
        *(uint16_t *)&f[p.off + 2] = (uint16_t)(l->placed_count ? l->placed_count : 1);
    }
    if (o.doors)   // each door takes you to the front of the one it's paired with: that door's partner's arrival
        for (auto &q : d.door_sites) {
            Door *dr = door_by_id.count(q.id) ? door_by_id[q.id] : nullptr;
            if (!dr || dr->paired < 0) continue;
            Door *front = door_by_id.count(door_by_id[dr->paired]->partner) ? door_by_id[door_by_id[dr->paired]->partner] : nullptr;
            auto &f = files[q.dir + "\\" + q.file];
            if (!front || front->data.size() != 11 || q.off + 11 > (long)f.size()) continue;
            memcpy(&f[q.off], front->data.data(), 11);
        }
    if (o.doors)   // the doors into Kaede's parts never open: lock bit 63 (nothing sets it) and the Shinobi Kit as key (only Kaede
        // carries it, and she never gets here): Samanosuke reads "The keyhole is broken. You can't open it with a key."
        for (auto &q : d.kaede_locks) {
            auto &f = files[q.dir + "\\" + q.file];
            if (q.off + 2 <= (long)f.size()) { f[q.off] = 0x80 | 63; f[q.off + 1] = 0x33; }
        }
    for (auto &kv : files)
        if (!write_file(run_dir + L"\\files\\image\\" + std::wstring(kv.first.begin(), kv.first.end()), kv.second.data(), kv.second.size()))
            return "Couldn't write the run's room files.";
    std::string tables;
    for (auto &t : d.tables) for (auto &l : d.locs) if (l.flag == t.first && l.shuffled) { char b[32]; snprintf(b, sizeof b, "%x %x\n", t.second, l.placed); tables += b; }
    if (!write_file(run_dir + L"\\tables.txt", tables.data(), tables.size())) return "Couldn't write the run's tables.txt.";
    auto b01 = [](bool v) { return v ? "1" : "0"; };
    std::string ini = "; A randomizer run (launcher Randomizer tab)\r\nname = " + o.name + "\r\nseed = " + o.seed + "\r\nfluorite_sanity = " + b01(o.fluorite_sanity)
        + "\r\nlong_challenges = " + b01(o.long_challenges) + "\r\ndoor_warp = " + b01(o.doors)
        + "\r\nkeep_consumables = " + b01(o.keep_consumables) + "\r\n";
    if (!write_file(run_dir + L"\\run.ini", ini.data(), ini.size())) return "Couldn't write the run's run.ini.";
    std::string sp = "Genma Onimusha randomizer: seed " + o.seed + (o.fluorite_sanity ? ", Fluorite-sanity" : "") + (o.long_challenges ? ", Dark Realm & Ogre Tower" : "")
        + (o.doors ? ", Door-sanity" : "") + (o.keep_consumables ? ", Don't Shuffle Consumables" : "")
        + "\n\nWhat unlocks progress, in the order a playthrough can get it:\n" + spheres;
    if (o.doors) {
        sp += "\nDoors (room: the door that led to room X now leads to room Y):\n";
        for (auto &dr : d.doors)
            if (dr.paired >= 0) sp += "  room " + dr.room + ": door to " + dr.to + " > " + door_by_id[dr.paired]->room + "\n";
    }
    sp += "\nEvery location (room, flag: was > now):\n";
    for (auto &l : d.locs) {
        char b[200];
        snprintf(b, sizeof b, "  room %s, flag %d (%s): %s > %s%s\n", l.room.c_str(), l.flag, l.kind.c_str(), item_name(xbe, l.item).c_str(),
                 run_item_name(xbe, l.placed).c_str(), l.shuffled ? "" : " (not shuffled)");
        sp += b;
    }
    if (!write_file(run_dir + L"\\spoiler.txt", sp.data(), sp.size())) return "Couldn't write the run's spoiler log.";
    return "";
}

}   // namespace rando
