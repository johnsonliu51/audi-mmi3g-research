/* SPDX-License-Identifier: GPL-3.0-only
 * Offline CSM stream metadata inspector; never connects to USB/iPhone.
 */
#include "iap2_csm.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct { unsigned long frames; int malformed; } report_state;

static void report(void *context, uint16_t message, const uint8_t *body, size_t length)
{
    report_state *state = (report_state *)context;
    size_t offset = 0, value_length;
    uint16_t id;
    const uint8_t *value;
    int result;
    ++state->frames;
    printf("frame=%lu message=0x%04x body=%lu\n", state->frames,
           (unsigned int)message, (unsigned long)length);
    while ((result = mmi_csm_param_next(body, length, &offset,
                                       &id, &value, &value_length)) > 0) {
        printf("  parameter=0x%04x bytes=%lu\n", (unsigned int)id,
               (unsigned long)value_length);
    }
    if (result < 0) {
        fprintf(stderr, "malformed parameter body at offset %lu\n", (unsigned long)offset);
        state->malformed = 1;
    }
}

int main(int argc, char **argv)
{
    FILE *input = stdin;
    uint8_t chunk[4096];
    uint8_t *storage;
    size_t count, pending;
    mmi_csm_parser parser;
    report_state state = {0, 0};
    if (argc > 2) {
        fprintf(stderr, "usage: csm-inspect [raw-control-session-file]\n");
        return 2;
    }
    if (argc == 2 && !(input = fopen(argv[1], "rb"))) {
        perror("open input"); return 2;
    }
    storage = (uint8_t *)malloc(MMI_CSM_MAX_FRAME);
    if (!storage || mmi_csm_init(&parser, storage, MMI_CSM_MAX_FRAME) != 0) {
        free(storage); if (input != stdin) fclose(input); return 2;
    }
    while ((count = fread(chunk, 1, sizeof(chunk), input)) != 0) {
        if (mmi_csm_feed(&parser, chunk, count, report, &state) != 0) {
            free(storage); if (input != stdin) fclose(input); return 2;
        }
    }
    if (ferror(input)) {
        perror("read input"); free(storage);
        if (input != stdin) fclose(input);
        return 2;
    }
    pending = mmi_csm_pending(&parser);
    fprintf(stderr, "frames=%lu trailing_bytes=%lu\n", state.frames, (unsigned long)pending);
    free(storage);
    if (input != stdin) fclose(input);
    return state.malformed || pending || !state.frames ? 1 : 0;
}
