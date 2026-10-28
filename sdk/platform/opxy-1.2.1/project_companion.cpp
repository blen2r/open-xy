// Stock OS 1.2.1 project companions (docs/projects.md). Stock saves every project,
// the workspace included, through one call of save(project, path) (8d599074, from
// 8d585eda). After a save that succeeds, the hook writes the .oxy beside the .xy: the
// saved file's size and CRC-32, then this firmware's machine numbers, in the canonical
// form tooling/projects.py writes, with what track control machines keep. It runs where
// stock saves, never on the audio role.
// A failure leaves the previous companion, which no longer matches, or a partial
// one; readers ignore both. What track control machines keep comes from the control
// service, through startup.cpp, which writes nothing in a build without it.
#include "project_companion.hpp"
#include "project_files.hpp"

using opxy::platform::CompanionMachine;

extern "C" opxy::u32 sdk_project_save(void* project, const char* path) __asm__("sdk_project_save");

namespace {
u32 save(void* project, const char* path) { return stock<u32(void*, const char*)>(0x8d599074)(project, path); }
u32 value(const char* symbol) { return static_cast<u32>(reinterpret_cast<__UINTPTR_TYPE__>(symbol)); }

const u32 kTextBytes = 8192;
char text[kTextBytes];

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
};

void write_companion(u32 bytes, u32 crc) {
    Writer w;
    w.put("{\n  \"format\": \"open-xy-project\",\n  \"project\": {\n    \"bytes\": ");
    w.number(bytes);
    w.put(",\n    \"crc32\": \"");
    w.hex(crc);
    w.put("\"\n  },\n  \"sections\": {\n");
    // What track control machines keep comes first, in key order, written by the
    // control service straight to the file.
    const u32 head = w.length;
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
    if (w.full) return;
    File* file = file_open(companion, "wb");
    if (!file) return;
    file_write(text, head, 1, file);
    sdk_project_kept(file);
    file_write(text + head, w.length - head, 1, file);
    file_close(file);
}
}

u32 sdk_project_save(void* project, const char* path) {
    const u32 saved = save(project, path);
    u32 bytes, crc;
    if (saved && path && companion_path(path) && measure(path, bytes, crc)) write_companion(bytes, crc);
    return saved;
}
