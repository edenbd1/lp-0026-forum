// Load a Logos ui_qml plugin the way the host does, against Basecamp's own
// bundled libraries: every dependency must resolve (RTLD_NOW), and the Qt
// plugin entry points must be exported.
#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char** argv) {
    int bad = 0;
    for (int i = 1; i < argc; ++i) {
        void* h = dlopen(argv[i], RTLD_NOW | RTLD_LOCAL);
        if (!h) { printf("FAIL %s\n  %s\n", argv[i], dlerror()); bad = 1; continue; }
        void* inst = dlsym(h, "qt_plugin_instance");
        void* meta = dlsym(h, "qt_plugin_query_metadata_v2");
        printf("ok   %s  (qt_plugin_instance %s, metadata %s)\n", argv[i], inst ? "exported" : "MISSING", meta ? "exported" : "MISSING");
        if (!inst) bad = 1;
    }
    return bad;
}
