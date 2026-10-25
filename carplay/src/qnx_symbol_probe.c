/* Read-only metadata probe: resolves symbols, never calls device APIs.
 * Dynamic loading may execute library constructors; not a pure passive scan.
 * Run only as a reviewed native experiment after target ABI validation.
 */
#include <dlfcn.h>
#include <stdio.h>
struct probe { const char *library; const char *symbols[9]; };
int main(void)
{
    static const struct probe probes[] = {
        {"libusbdi.so.2", {"usbd_connect", "usbd_attach", "usbd_open_pipe",
         "usbd_setup_control", "usbd_setup_bulk", "usbd_io", "usbd_select_config", NULL}},
        {"libgf.so.1", {"gf_dev_attach", "gf_display_attach", "gf_surface_create",
         "gf_surface_free", "gf_context_create", "gf_draw_blit2", NULL}},
        {"libasound.so.2", {"snd_pcm_open_preferred", "snd_pcm_plugin_write",
         "snd_pcm_plugin_read", "snd_pcm_close", NULL}},
        {"libmmedia.so.1", {"MmInitialize", "MmCreateGraph", "MmFindFilter",
         "MmAcquireInputChannel", "MmAcquireOutputChannel", "MmAttachChannels",
         "MmStart", "MmDestroyGraph", NULL}}
    };
    unsigned int i, j;
    int unavailable = 0;
    for (i = 0; i < sizeof(probes)/sizeof(probes[0]); ++i) {
        void *handle = dlopen(probes[i].library, RTLD_LAZY | RTLD_LOCAL);
        if (!handle) {
            printf("library=%s status=unavailable error=%s\n", probes[i].library, dlerror());
            unavailable = 1;
            continue;
        }
        printf("library=%s status=loaded\n", probes[i].library);
        for (j = 0; probes[i].symbols[j]; ++j) {
            const char *error;
            dlerror();
            (void)dlsym(handle, probes[i].symbols[j]);
            error = dlerror();
            printf("symbol=%s status=%s\n", probes[i].symbols[j], error ? "unavailable" : "resolved");
            if (error) unavailable = 1;
        }
        if (dlclose(handle) != 0) unavailable = 1;
    }
    printf("probe=complete device_api_calls=0 carplay_session=not_attempted\n");
    return unavailable ? 1 : 0;
}
