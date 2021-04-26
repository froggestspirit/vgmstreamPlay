/* vgmstream123.c
 *
 * Simple player frontend for vgmstream
 * Copyright (c) 2017 Daniel Richard G. <skunk@iSKUNK.ORG>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <ao/ao.h>

#include "src/vgmstream.h"
#include "src/plugins.h"

#include <string.h>
#include <unistd.h>
#include <fcntl.h>


#define LITTLE_ENDIAN_OUTPUT 1 /* untested in BE */

#define DEFAULT_PARAMS { 0, -1, 2.0, 10.0, 0.0,   0, 0, 0, 0 }
typedef struct {
    int stream_index;

    double min_time;
    double loop_count;
    double fade_time;
    double fade_delay;

    int ignore_loop;
    int force_loop;
    int really_force_loop;
    int play_forever;
} song_settings_t;

static int driver_id;
static ao_device *device = NULL;
static ao_option *device_options = NULL;
static ao_sample_format current_sample_format;

static sample_t *buffer = NULL;
/* reportedly 1kb helps Raspberry Pi Zero play FFmpeg formats without stuttering
 * (presumably other low powered devices too), plus it's the default in other plugins */
static int buffer_size_kb = 1;

static int verbose = 0;

static volatile int interrupted = 0;

/* Opens the audio device with the appropriate parameters
 */
static int set_sample_format(int channels, int sample_rate) {
    ao_sample_format format;


    memset(&format, 0, sizeof(format));
    format.bits = 8 * sizeof(sample_t);
    format.channels = channels;
    format.rate = sample_rate;
    format.byte_format =
#if LITTLE_ENDIAN_OUTPUT
        AO_FMT_LITTLE
#else
        AO_FMT_BIG
#endif
    ;

    if (memcmp(&format, &current_sample_format, sizeof(format))) {

        /* Sample format has changed, so (re-)open audio device */

        ao_info *info = ao_driver_info(driver_id);
        if (!info) return -1;

        if (device)
            ao_close(device);

        memcpy(&current_sample_format, &format, sizeof(format));

        device = ao_open_live(driver_id, &format, device_options);

        if (!device) {
            fprintf(stderr, "Error opening \"%s\" audio device\n", info->short_name);
            return -1;
        }
    }

    return 0;
}

static void apply_config(VGMSTREAM* vgmstream, song_settings_t* cfg) {
    vgmstream_cfg_t vcfg = {0};

    vcfg.allow_play_forever = 1;

    vcfg.play_forever = cfg->play_forever;
    vcfg.fade_time = cfg->fade_time;
    vcfg.loop_count = cfg->loop_count;
    vcfg.fade_delay = cfg->fade_delay;

    vcfg.ignore_loop  = cfg->ignore_loop;
    vcfg.force_loop = cfg->force_loop;
    vcfg.really_force_loop = cfg->really_force_loop;

    vgmstream_apply_config(vgmstream, &vcfg);
}

static int play_vgmstream(const char *filename, song_settings_t *cfg) {
    int ret = 0;
    STREAMFILE* sf;
    VGMSTREAM *vgmstream;
    FILE *save_fps[4];
    size_t buffer_size;
    int32_t max_buffer_samples;
    int i;
    int output_channels;


    sf = open_stdio_streamfile(filename);
    if (!sf) {
        fprintf(stderr, "%s: cannot open file\n", filename);
        return -1;
    }

    sf->stream_index = cfg->stream_index;
    vgmstream = init_vgmstream_from_STREAMFILE(sf);
    close_streamfile(sf);

    if (!vgmstream) {
        fprintf(stderr, "%s: error opening stream\n", filename);
        return -1;
    }

    printf("Playing stream: %s\n", filename);

    /* Print metadata in verbose mode
     */
    if (verbose) {
        char description[4096] = { '\0' };
        describe_vgmstream(vgmstream, description, sizeof(description));
        puts(description);
        putchar('\n');
    }

    /* If the audio device hasn't been opened yet, then describe it
     */
    if (!device) {
        ao_info *info = ao_driver_info(driver_id);
        printf("Audio device: %s\n", info->name);
        printf("Comment: %s\n", info->comment);
        putchar('\n');
    }

    /* Calculate how many loops are needed to achieve a minimum
     * playback time. Note: This calculation is derived from the
     * logic in get_vgmstream_play_samples().
     */
    if (vgmstream->loop_flag && cfg->loop_count < 0) {
        double intro = (double)vgmstream->loop_start_sample / vgmstream->sample_rate;
        double loop = (double)(vgmstream->loop_end_sample - vgmstream->loop_start_sample) / vgmstream->sample_rate;
        double end = cfg->fade_time + cfg->fade_delay;
        if (loop < 1.0) loop = 1.0;
        cfg->loop_count = ((cfg->min_time - intro - end) / loop + 0.99);
        if (cfg->loop_count < 1.0) cfg->loop_count = 1.0;
    }

    /* Config
     */
    apply_config(vgmstream, cfg);

    output_channels = vgmstream->channels;
    vgmstream_mixing_enable(vgmstream, 0, NULL, &output_channels); /* query */

    /* Buffer size in bytes (after getting channels)
     */
    buffer_size = 1024 * buffer_size_kb;
    if (!buffer) {
        if (buffer_size_kb < 1) {
            fprintf(stderr, "Invalid buffer size '%d'\n", buffer_size_kb);
            return -1;
        }

        buffer = malloc(buffer_size);
        if (!buffer) goto fail;
    }

    max_buffer_samples = buffer_size / (output_channels * sizeof(sample));

    vgmstream_mixing_enable(vgmstream, max_buffer_samples, NULL, NULL); /* enable */

    /* Init
     */
    ret = set_sample_format(output_channels, vgmstream->sample_rate);
    if (ret) goto fail;

    /* Decode
     */

    int32_t decode_pos_samples = 0;
    int32_t length_samples = vgmstream_get_samples(vgmstream);
    if (length_samples <= 0) goto fail;
    
    char buf[100];
    while (!interrupted) {
        scanf("%s", buf);
        if (strlen(buf)) {
            if(strcmp(buf, "QUIT") == 0){
                ret = -1;
                break;
            }
            *buf = 0;
        }

        int to_do;

        if (decode_pos_samples + max_buffer_samples > length_samples)
            to_do = length_samples - decode_pos_samples;
        else
            to_do = max_buffer_samples;

        if (to_do <= 0) {
            break; /* EOF */
        }
        
        render_vgmstream(buffer, to_do, vgmstream);

#if LITTLE_ENDIAN_OUTPUT
        swap_samples_le(buffer, output_channels * to_do);
#endif


        if (!ao_play(device, (char *)buffer, to_do * output_channels * sizeof(sample))) {
            fputs("\nAudio playback error\n", stderr);
            ao_close(device);
            device = NULL;
            ret = -1;
            break;
        }

        decode_pos_samples += to_do;
    }



fail:
    close_vgmstream(vgmstream);
    return ret;
}


int main(int argc, char **argv) {
    int error = 0;
    song_settings_t cfg;

    ao_initialize();
    driver_id = ao_default_driver_id();
    memset(&current_sample_format, 0, sizeof(current_sample_format));

    int cmdCount = 0;
    char buf[100];
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    while(1){
        scanf("%s", buf);
        if(!cmdCount++) *buf = 0;
        if (strlen(buf)) {
            if(strcmp(buf, "QUIT") == 0) goto done;
            if(strcmp(buf, "LOAD") == 0){
                scanf("%s", buf);
                if (strlen(buf)) {
                    song_settings_t default_par = DEFAULT_PARAMS;
                    cfg = default_par;
                    if (play_vgmstream(buf, &cfg)) {
                        error = 1;
                        goto done;
                    }
                }             

            }
            *buf = 0;
        }
    }

done:
    if (device)
        ao_close(device);
    if (buffer)
        free(buffer);

    ao_free_options(device_options);
    ao_shutdown();

    return error;
}
