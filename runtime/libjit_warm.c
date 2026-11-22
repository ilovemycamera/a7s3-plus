extern void *dlopen(const char *, int);
#define RTLD_NOW      2
#define RTLD_NODELETE 0x1000

__attribute__((visibility("default")))
void _init(void) {
    dlopen("/setting/mode/libcinema_worker.so", RTLD_NOW | RTLD_NODELETE);
}

/* Satisfy libJiritsu CImJiritsu::jit_thread symbols so zero-DMM cmd_start (0x0004)
 * completes cleanly with zero UI_PrintString error banners and clean release(). */
__attribute__((visibility("default")))
void *CreateScriptI(void) {
    return (void *)1;
}

__attribute__((visibility("default")))
int LoadScriptI(void *ctx, const char *path) {
    (void)ctx;
    (void)path;
    return 1;
}

__attribute__((visibility("default")))
void DestroyScriptI(void *ctx) {
    (void)ctx;
}
