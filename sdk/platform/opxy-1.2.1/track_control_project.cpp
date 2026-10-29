// Track control's data kept with each project (docs/projects.md, open-xy.control):
// what each machine keeps, and which machine holds each aux track it took over
// with the stock role's controls kept aside (aux_tracks.cpp). Part of the
// control service, so only builds with a track control machine link it.
// The companion writer calls sdk_control_kept_write, through startup.cpp, with the
// companion open, at the start of its sections.
//
// Stock loads every project through one call of read(project, path) (8d599b10, from
// 8d58913e in 8d5890b0). After a read that succeeds, track control machines get back
// what they kept. A new project reads no file: stock clears the project (8d588114,
// from 8d572f70) or clears it at random (8d588988, from 8d572ff8). Both do nothing
// while a project is loading (the word at 840a0528); otherwise machines then get
// nothing back, as after loading a project without their data.
#include "project_files.hpp"
#include "project_companion.hpp"
#include "track_control.hpp"

extern "C" opxy::u32 sdk_control_kept_write(void* file) __asm__("sdk_control_kept_write");
extern "C" opxy::u32 sdk_project_read(void* project, const char* path) __asm__("sdk_project_read");
extern "C" opxy::u32 sdk_project_clear(opxy::u32, opxy::u32, opxy::u32) __asm__("sdk_project_clear");
extern "C" opxy::u32 sdk_project_clear_random(opxy::u32, opxy::u32, opxy::u32) __asm__("sdk_project_clear_random");
extern "C" {
const char* sdk_aux_holder_id(opxy::u32 track) __asm__("sdk_aux_holder_id");
const opxy::i32* sdk_aux_kept_controls(opxy::u32 track) __asm__("sdk_aux_kept_controls");
void sdk_aux_restore(opxy::u32 track, const char* id, opxy::u32 id_length, const opxy::i32* controls)
    __asm__("sdk_aux_restore");
void sdk_aux_forget() __asm__("sdk_aux_forget");
}

namespace {
u32 read(void* project, const char* path) { return stock<u32(void*, const char*)>(0x8d599b10)(project, path); }
const u32 kProjectLoading = 0x840a0528;

// The format's 64 KiB, less what a reader never needs.
const u32 kReadBytes = 32768;
char project_text[kReadBytes];
// One machine's kept bytes, as it saved them or as read back.
unsigned char kept[opxy::control::saved_capacity];
const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Writes through a small buffer straight to the open companion.
class Stream {
public:
    explicit Stream(File* file) : file(file) {}
    ~Stream() { flush(); }
    void put(const char* s) { while (*s) byte(*s++); }
    void byte(char c) {
        if (count == sizeof buffer) flush();
        buffer[count++] = c;
    }
    void base64(const unsigned char* data, u32 bytes) {
        for (u32 i = 0; i < bytes; i += 3) {
            const u32 n = bytes - i, v = data[i] << 16 | (n > 1 ? data[i + 1] << 8 : 0) | (n > 2 ? data[i + 2] : 0);
            byte(kBase64[v >> 18]);
            byte(kBase64[v >> 12 & 63]);
            byte(n > 1 ? kBase64[v >> 6 & 63] : '=');
            byte(n > 2 ? kBase64[v & 63] : '=');
        }
    }
private:
    void flush() {
        if (count) file_write(buffer, count, 1, file);
        count = 0;
    }
    File* file;
    char buffer[256];
    u32 count = 0;
};

// A small reader for companions any writer made: the span [at, to) of a value.
struct Reader {
    u32 length;
    u32 space(u32 i) const {
        while (i < length && (project_text[i] == ' ' || project_text[i] == '\n' || project_text[i] == '\r' || project_text[i] == '\t')) ++i;
        return i;
    }
    // The end of the value at i, or 0 when it is malformed.
    u32 end(u32 i) const {
        if (i >= length) return 0;
        if (project_text[i] == '"') {
            for (++i; i < length; ++i) {
                if (project_text[i] == '\\') ++i;
                else if (project_text[i] == '"') return i + 1;
            }
            return 0;
        }
        if (project_text[i] == '{' || project_text[i] == '[') {
            u32 depth = 0;
            for (; i < length; ++i) {
                if (project_text[i] == '"') {
                    const u32 after = end(i);
                    if (!after) return 0;
                    i = after - 1;
                } else if (project_text[i] == '{' || project_text[i] == '[') {
                    ++depth;
                } else if (project_text[i] == '}' || project_text[i] == ']') {
                    if (!--depth) return i + 1;
                }
            }
            return 0;
        }
        while (i < length && project_text[i] != ',' && project_text[i] != '}' && project_text[i] != ']' && project_text[i] != ' ' &&
               project_text[i] != '\n' && project_text[i] != '\r' && project_text[i] != '\t')
            ++i;
        return i;
    }
    bool is(u32 at, u32 to, const char* key) const {
        if (to - at < 2 || project_text[at] != '"') return false;
        u32 i = at + 1;
        for (; *key && i < to - 1; ++i, ++key)
            if (project_text[i] != *key) return false;
        return !*key && i == to - 1;
    }
    // The member `key` of the object [object, object_end): its value's span.
    bool member(u32 object, u32 object_end, const char* key, u32& at, u32& to) const {
        if (object >= object_end || project_text[object] != '{') return false;
        for (u32 i = space(object + 1); i < object_end && project_text[i] == '"';) {
            const u32 key_end = end(i);
            const u32 colon = key_end ? space(key_end) : object_end;
            if (colon >= object_end || project_text[colon] != ':') return false;
            const u32 value = space(colon + 1), value_end = end(value);
            if (!value_end || value_end > object_end) return false;
            if (is(i, key_end, key)) {
                at = value;
                to = value_end;
                return true;
            }
            const u32 next = space(value_end);
            if (next >= object_end || project_text[next] != ',') return false;
            i = space(next + 1);
        }
        return false;
    }
    bool number(u32 at, u32 to, u32& v) const {
        v = 0;
        if (at >= to) return false;
        for (u32 i = at; i < to; ++i) {
            if (project_text[i] < '0' || project_text[i] > '9' || v > 400000000) return false;
            v = v * 10 + static_cast<u32>(project_text[i] - '0');
        }
        return true;
    }
    bool hex(u32 at, u32 to, u32& v) const {
        v = 0;
        if (to - at != 10 || project_text[at] != '"') return false;
        for (u32 i = at + 1; i < to - 1; ++i) {
            const char c = project_text[i];
            const u32 d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
            if (d > 15) return false;
            v = v << 4 | d;
        }
        return true;
    }
    // A base64 string into kept: its byte count, or 0.
    u32 base64(u32 at, u32 to, u32 capacity) const {
        if (to - at < 2 || project_text[at] != '"' || (to - at - 2) % 4) return 0;
        u32 count = 0;
        for (u32 i = at + 1; i + 4 <= to - 1; i += 4) {
            u32 v = 0, pad = 0;
            for (u32 j = 0; j < 4; ++j) {
                const char c = project_text[i + j];
                u32 d = 64;
                for (u32 k = 0; k < 64; ++k)
                    if (kBase64[k] == c) d = k;
                if (c == '=' && i + 4 == to - 1 && j >= 2) d = 0, ++pad;
                if (d > 63 || (pad && c != '=')) return 0;
                v = v << 6 | d;
            }
            for (u32 j = 0; j < 3 - pad; ++j) {
                if (count >= capacity) return 0;
                kept[count++] = static_cast<unsigned char>(v >> (16 - 8 * j));
            }
        }
        return count;
    }
};

// The companion's "open-xy.control" object, when the companion matches the
// project's bytes.
bool kept_section(const Reader& r, u32 bytes, u32 crc, u32& at, u32& to) {
    const u32 root = r.space(0), root_end = r.end(root);
    u32 a, b, c, d, v;
    if (!root_end || !r.member(root, root_end, "format", a, b) || !r.is(a, b, "open-xy-project")) return false;
    if (!r.member(root, root_end, "version", a, b) || !r.number(a, b, v) || v != 1) return false;
    if (!r.member(root, root_end, "project", a, b)) return false;
    if (!r.member(a, b, "bytes", c, d) || !r.number(c, d, v) || v != bytes) return false;
    if (!r.member(a, b, "crc32", c, d) || !r.hex(c, d, v) || v != crc) return false;
    if (!r.member(root, root_end, "sections", a, b) || !r.member(a, b, "open-xy.control", c, d)) return false;
    if (!r.member(c, d, "version", a, b) || !r.number(a, b, v) || v != 1) return false;
    at = c;
    to = d;
    return true;
}

// The aux tracks the machines may take over, as track indices; their keys in
// the "aux" object are the indices in decimal.
const char* const kAuxKeys[opxy::control::all_tracks] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15"};
const u32 kAuxControls = 8;

void load_kept(const char* path) {
    bool keeping = false;
    for (u32 i = 0; i < sdk_control_machine_count; ++i)
        keeping = keeping || sdk_control_machines[i].load || sdk_control_machines[i].aux;
    if (!keeping) return;  // nothing to give back: no project I/O
    u32 bytes, crc, length = 0, at = 0, to = 0;
    bool found = false;
    if (companion_path(path) && measure(path, bytes, crc)) {
        if (File* file = file_open(companion, "rb")) {
            length = file_read(project_text, 1, kReadBytes, file);
            file_close(file);
        }
        const Reader r = {length < kReadBytes ? length : 0};
        found = r.length && kept_section(r, bytes, crc, at, to);
    }
    const Reader r = {found ? length : 0};
    u32 machines_at, machines_to, aux_at, aux_to;
    const bool machines = found && r.member(at, to, "machines", machines_at, machines_to);
    for (u32 i = 0; i < sdk_control_machine_count; ++i) {
        const opxy::platform::ControlMachine& m = sdk_control_machines[i];
        if (!m.load || m.ready() != 1) continue;
        u32 a, b, count = 0;
        if (machines && r.member(machines_at, machines_to, m.name, a, b)) count = r.base64(a, b, m.saved_bytes);
        m.load(kept, count);
    }
    // Each aux track a machine held: {"machine": id, "controls": base64 of the
    // role's eight controls}. Tracks without one keep their stock role.
    sdk_aux_forget();
    if (!found || !r.member(at, to, "aux", aux_at, aux_to)) return;
    for (u32 t = 0; t < opxy::control::all_tracks; ++t) {
        u32 a, b, c, d, e, f;
        if (!r.member(aux_at, aux_to, kAuxKeys[t], a, b) || !r.member(a, b, "machine", c, d) ||
            !r.member(a, b, "controls", e, f) || d - c < 3 || project_text[c] != '"' ||
            r.base64(e, f, kAuxControls * 4) != kAuxControls * 4)
            continue;
        opxy::i32 controls[kAuxControls];
        for (u32 i = 0; i < kAuxControls; ++i)
            controls[i] = static_cast<opxy::i32>(kept[4 * i] | kept[4 * i + 1] << 8 | kept[4 * i + 2] << 16 |
                                                 static_cast<u32>(kept[4 * i + 3]) << 24);
        sdk_aux_restore(t, project_text + c + 1, d - c - 2, controls);
    }
}

// After a new project: every machine that keeps data gets nothing back, and
// every aux track has its stock role.
void forget_kept() {
    for (u32 i = 0; i < sdk_control_machine_count; ++i) {
        const opxy::platform::ControlMachine& m = sdk_control_machines[i];
        if (m.load && m.ready() == 1) m.load(kept, 0);
    }
    sdk_aux_forget();
}
}

// "open-xy.control": the machine holding each aux track it took over, by track
// index, and what each track control machine keeps, by machine ID, in the
// canonical form (docs/projects.md): keys sorted, two-space indentation.
extern "C" u32 sdk_control_kept_write(void* file) {
    Stream out(static_cast<File*>(file));
    // Aux track keys sort as strings: 10..15, then 8 and 9.
    static const u32 kAuxOrder[] = {10, 11, 12, 13, 14, 15, 8, 9};
    u32 held = 0, listed = 0;
    for (u32 t : kAuxOrder) {
        const char* id = sdk_aux_holder_id(t);
        if (!id) continue;
        out.put(held++ ? "\n        },\n        \"" : "    \"open-xy.control\": {\n      \"aux\": {\n        \"");
        out.put(kAuxKeys[t]);
        out.put("\": {\n          \"controls\": \"");
        const opxy::i32* controls = sdk_aux_kept_controls(t);
        for (u32 i = 0; i < kAuxControls; ++i)
            for (u32 b = 0; b < 4; ++b) kept[4 * i + b] = static_cast<unsigned char>(static_cast<u32>(controls[i]) >> (8 * b));
        out.base64(kept, kAuxControls * 4);
        out.put("\",\n          \"machine\": \"");
        out.put(id);
        out.byte('"');
    }
    if (held) out.put("\n        }\n      },\n");
    for (u32 i = 0; i < sdk_control_machine_count; ++i) {
        const opxy::platform::ControlMachine& m = sdk_control_machines[i];
        if (!m.save || m.ready() != 1) continue;
        const u32 count = m.save(kept);
        if (!count || count > m.saved_bytes) continue;
        out.put(listed++ ? ",\n        \"" : held ? "      \"machines\": {\n        \""
                                                 : "    \"open-xy.control\": {\n      \"machines\": {\n        \"");
        out.put(m.name);
        out.put("\": \"");
        out.base64(kept, count);
        out.byte('"');
    }
    if (listed) out.put("\n      },\n");
    if (held || listed) out.put("      \"version\": 1\n    },\n");
    return held + listed;
}

u32 sdk_project_read(void* project, const char* path) {
    const u32 loaded = read(project, path);
    if (loaded && path) load_kept(path);
    return loaded;
}

u32 sdk_project_clear(u32 a, u32 b, u32 c) {
    const bool cleared = !opxy::platform::word(kProjectLoading);
    const u32 result = stock<u32(u32, u32, u32)>(0x8d588114)(a, b, c);
    if (cleared) forget_kept();
    return result;
}

u32 sdk_project_clear_random(u32 a, u32 b, u32 c) {
    const bool cleared = !opxy::platform::word(kProjectLoading);
    const u32 result = stock<u32(u32, u32, u32)>(0x8d588988)(a, b, c);
    if (cleared) forget_kept();
    return result;
}
