// Stock OS 1.2.1 project companions (docs/projects.md). Stock saves every project,
// the workspace included, through one call of save(project, path) (8d599074, from
// 8d585eda). After a save that succeeds, the hook writes the .oxy beside the .xy: the
// saved file's size and CRC-32, what track control machines keep with the project,
// then this firmware's machine numbers, in the canonical form tooling/projects.py
// writes. It runs where stock saves, never on the audio role. A failure leaves the
// previous companion, which no longer matches, or a partial one; readers ignore both.
//
// Stock loads every project through one call of read(project, path) (8d599b10, from
// 8d58913e in 8d5890b0). After a read that succeeds, the hook reads the companion
// beside it and, when it matches the project's bytes, gives each track control
// machine what it kept; a machine gets nothing (0 bytes) from any other project.
#include "project_companion.hpp"
#include "stock.hpp"
#include "track_control.hpp"

using opxy::u32;
using opxy::platform::CompanionMachine;
using opxy::platform::stock;

extern "C" u32 sdk_project_save(void* project, const char* path) __asm__("sdk_project_save");
extern "C" u32 sdk_project_read(void* project, const char* path) __asm__("sdk_project_read");

namespace {
typedef void File;
// newlib, as stock's project reader and writer call it (8d590d7c, 8d590e28, 8d591304, 8d590da2).
File* file_open(const char* path, const char* mode) {
    return stock<File*(const char*, const char*)>(0x8fed0348)(path, mode);
}
u32 file_read(void* data, u32 size, u32 count, File* file) {
    return stock<u32(void*, u32, u32, File*)>(0x8fed05bc)(data, size, count, file);
}
u32 file_write(const void* data, u32 size, u32 count, File* file) {
    return stock<u32(const void*, u32, u32, File*)>(0x8fed0780)(data, size, count, file);
}
void file_close(File* file) { stock<int(File*)>(0x8fed00ae)(file); }
u32 save(void* project, const char* path) { return stock<u32(void*, const char*)>(0x8d599074)(project, path); }
u32 read(void* project, const char* path) { return stock<u32(void*, const char*)>(0x8d599b10)(project, path); }

const u32 kPathBytes = 256;
// The format's 64 KiB, less what a reader never needs.
const u32 kTextBytes = 32768;
unsigned char chunk[4096];
char text[kTextBytes];
char companion[kPathBytes];
u32 crc_table[256];
// One machine's kept bytes, as it saved them or as read back.
unsigned char kept[opxy::control::saved_capacity];
const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

u32 value(const char* symbol) { return static_cast<u32>(reinterpret_cast<__UINTPTR_TYPE__>(symbol)); }

// zlib's CRC-32.
u32 crc32(u32 crc, const unsigned char* data, u32 count) {
    if (!crc_table[1]) {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int bit = 0; bit < 8; ++bit) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    }
    crc = ~crc;
    while (count--) crc = crc_table[(crc ^ *data++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

// The project's bytes as saved: false when it cannot be read back.
bool measure(const char* path, u32& bytes, u32& crc) {
    File* file = file_open(path, "rb");
    if (!file) return false;
    bytes = crc = 0;
    for (u32 count; (count = file_read(chunk, 1, sizeof chunk, file)) != 0;) {
        crc = crc32(crc, chunk, count);
        bytes += count;
    }
    file_close(file);
    return true;
}

// song.xy -> song.oxy; false for any other name.
bool companion_path(const char* path) {
    u32 length = 0;
    while (path[length]) ++length;
    if (length < 4 || length + 2 >= kPathBytes || path[length - 3] != '.' || path[length - 2] != 'x' ||
        path[length - 1] != 'y')
        return false;
    for (u32 i = 0; i < length - 2; ++i) companion[i] = path[i];
    companion[length - 2] = 'o';
    companion[length - 1] = 'x';
    companion[length] = 'y';
    companion[length + 1] = 0;
    return true;
}

class Writer {
public:
    u32 length = 0;
    bool full = false;
    void put(const char* s) { while (*s) byte(*s++); }
    void byte(char c) {
        if (length < kTextBytes) text[length++] = c;
        else full = true;
    }
    void number(u32 v) {
        char digits[10];
        u32 count = 0;
        do { digits[count++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        while (count) byte(digits[--count]);
    }
    void hex(u32 v) {
        for (int shift = 28; shift >= 0; shift -= 4) byte("0123456789abcdef"[(v >> shift) & 15]);
    }
    void base64(const unsigned char* data, u32 count) {
        for (u32 i = 0; i < count; i += 3) {
            const u32 n = count - i, v = data[i] << 16 | (n > 1 ? data[i + 1] << 8 : 0) | (n > 2 ? data[i + 2] : 0);
            byte(kBase64[v >> 18]);
            byte(kBase64[v >> 12 & 63]);
            byte(n > 1 ? kBase64[v >> 6 & 63] : '=');
            byte(n > 2 ? kBase64[v & 63] : '=');
        }
    }
};

// "open-xy.control": what each track control machine keeps, by machine ID.
void write_kept(Writer& w) {
    u32 listed = 0;
    for (u32 i = 0; i < sdk_control_machine_count; ++i) {
        const opxy::platform::ControlMachine& m = sdk_control_machines[i];
        if (!m.save || m.ready() != 1) continue;
        const u32 count = m.save(kept);
        if (!count || count > m.saved_bytes) continue;
        w.put(listed++ ? ",\n        \"" : "    \"open-xy.control\": {\n      \"machines\": {\n        \"");
        w.put(m.name);
        w.put("\": \"");
        w.base64(kept, count);
        w.byte('"');
    }
    if (listed) w.put("\n      },\n      \"version\": 1\n    },\n");
}

// Without what machines keep when it does not fit.
void write_companion(u32 bytes, u32 crc, bool keep) {
    Writer w;
    w.put("{\n  \"format\": \"open-xy-project\",\n  \"project\": {\n    \"bytes\": ");
    w.number(bytes);
    w.put(",\n    \"crc32\": \"");
    w.hex(crc);
    w.put("\"\n  },\n  \"sections\": {\n");
    if (keep) write_kept(w);
    w.put("    \"open-xy.machines\": {\n      \"machines\": {");
    u32 listed = 0;
    for (const CompanionMachine* m = sdk_companion_machines; m->id; ++m) {
        const u32 number = value(m->number);
        if (!number) continue;  // a kit machine this selection left out
        w.put(listed++ ? ",\n        \"" : "\n        \"");
        w.put(m->id);
        w.put(m->effect ? "\": {\n          \"effect\": " : "\": {\n          \"engine\": ");
        w.number(number);
        w.put(",\n          \"page\": ");
        w.number(value(m->page));
        w.put("\n        }");
    }
    w.put(listed ? "\n      },\n" : "},\n");
    w.put("      \"profile\": \"opxy-1.2.1\",\n      \"version\": 1\n    }\n  },\n  \"version\": 1\n}\n");
    if (w.full) {
        if (keep) write_companion(bytes, crc, false);
        return;
    }
    File* file = file_open(companion, "wb");
    if (!file) return;
    file_write(text, w.length, 1, file);
    file_close(file);
}

// A small reader for companions any writer made: the span [at, to) of a value.
struct Reader {
    u32 length;
    u32 space(u32 i) const {
        while (i < length && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t')) ++i;
        return i;
    }
    // The end of the value at i, or 0 when it is malformed.
    u32 end(u32 i) const {
        if (i >= length) return 0;
        if (text[i] == '"') {
            for (++i; i < length; ++i) {
                if (text[i] == '\\') ++i;
                else if (text[i] == '"') return i + 1;
            }
            return 0;
        }
        if (text[i] == '{' || text[i] == '[') {
            u32 depth = 0;
            for (; i < length; ++i) {
                if (text[i] == '"') {
                    const u32 after = end(i);
                    if (!after) return 0;
                    i = after - 1;
                } else if (text[i] == '{' || text[i] == '[') {
                    ++depth;
                } else if (text[i] == '}' || text[i] == ']') {
                    if (!--depth) return i + 1;
                }
            }
            return 0;
        }
        while (i < length && text[i] != ',' && text[i] != '}' && text[i] != ']' && text[i] != ' ' &&
               text[i] != '\n' && text[i] != '\r' && text[i] != '\t')
            ++i;
        return i;
    }
    bool is(u32 at, u32 to, const char* key) const {
        if (to - at < 2 || text[at] != '"') return false;
        u32 i = at + 1;
        for (; *key && i < to - 1; ++i, ++key)
            if (text[i] != *key) return false;
        return !*key && i == to - 1;
    }
    // The member `key` of the object [object, object_end): its value's span.
    bool member(u32 object, u32 object_end, const char* key, u32& at, u32& to) const {
        if (object >= object_end || text[object] != '{') return false;
        for (u32 i = space(object + 1); i < object_end && text[i] == '"';) {
            const u32 key_end = end(i);
            const u32 colon = key_end ? space(key_end) : object_end;
            if (colon >= object_end || text[colon] != ':') return false;
            const u32 value = space(colon + 1), value_end = end(value);
            if (!value_end || value_end > object_end) return false;
            if (is(i, key_end, key)) {
                at = value;
                to = value_end;
                return true;
            }
            const u32 next = space(value_end);
            if (next >= object_end || text[next] != ',') return false;
            i = space(next + 1);
        }
        return false;
    }
    bool number(u32 at, u32 to, u32& v) const {
        v = 0;
        if (at >= to) return false;
        for (u32 i = at; i < to; ++i) {
            if (text[i] < '0' || text[i] > '9' || v > 400000000) return false;
            v = v * 10 + static_cast<u32>(text[i] - '0');
        }
        return true;
    }
    bool hex(u32 at, u32 to, u32& v) const {
        v = 0;
        if (to - at != 10 || text[at] != '"') return false;
        for (u32 i = at + 1; i < to - 1; ++i) {
            const char c = text[i];
            const u32 d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
            if (d > 15) return false;
            v = v << 4 | d;
        }
        return true;
    }
    // A base64 string into kept: its byte count, or 0.
    u32 base64(u32 at, u32 to, u32 capacity) const {
        if (to - at < 2 || text[at] != '"' || (to - at - 2) % 4) return 0;
        u32 count = 0;
        for (u32 i = at + 1; i + 4 <= to - 1; i += 4) {
            u32 v = 0, pad = 0;
            for (u32 j = 0; j < 4; ++j) {
                const char c = text[i + j];
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

// The companion's "open-xy.control" machines object, when the companion
// matches the project's bytes.
bool kept_machines(const Reader& r, u32 bytes, u32 crc, u32& at, u32& to) {
    const u32 root = r.space(0), root_end = r.end(root);
    u32 a, b, c, d, v;
    if (!root_end || !r.member(root, root_end, "format", a, b) || !r.is(a, b, "open-xy-project")) return false;
    if (!r.member(root, root_end, "version", a, b) || !r.number(a, b, v) || v != 1) return false;
    if (!r.member(root, root_end, "project", a, b)) return false;
    if (!r.member(a, b, "bytes", c, d) || !r.number(c, d, v) || v != bytes) return false;
    if (!r.member(a, b, "crc32", c, d) || !r.hex(c, d, v) || v != crc) return false;
    if (!r.member(root, root_end, "sections", a, b) || !r.member(a, b, "open-xy.control", c, d)) return false;
    if (!r.member(c, d, "version", a, b) || !r.number(a, b, v) || v != 1) return false;
    return r.member(c, d, "machines", at, to);
}

void load_kept(const char* path) {
    u32 bytes, crc, length = 0, at = 0, to = 0;
    bool found = false;
    if (companion_path(path) && measure(path, bytes, crc)) {
        if (File* file = file_open(companion, "rb")) {
            length = file_read(text, 1, kTextBytes, file);
            file_close(file);
        }
        const Reader r = {length < kTextBytes ? length : 0};
        found = r.length && kept_machines(r, bytes, crc, at, to);
    }
    const Reader r = {found ? length : 0};
    for (u32 i = 0; i < sdk_control_machine_count; ++i) {
        const opxy::platform::ControlMachine& m = sdk_control_machines[i];
        if (!m.load || m.ready() != 1) continue;
        u32 a, b, count = 0;
        if (found && r.member(at, to, m.name, a, b)) count = r.base64(a, b, m.saved_bytes);
        m.load(kept, count);
    }
}
}

u32 sdk_project_save(void* project, const char* path) {
    const u32 saved = save(project, path);
    u32 bytes, crc;
    if (saved && path && companion_path(path) && measure(path, bytes, crc)) write_companion(bytes, crc, true);
    return saved;
}

u32 sdk_project_read(void* project, const char* path) {
    const u32 loaded = read(project, path);
    if (loaded && path) load_kept(path);
    return loaded;
}
