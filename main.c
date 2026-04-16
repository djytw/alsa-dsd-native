#include <alsa/asoundlib.h>
#include <errno.h>
#include <getopt.h>
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/param.h>

#define LOGE(format, ...) printf("\e[31m[ERROR] " format "\e[0m", ##__VA_ARGS__)
#define LOGI(format, ...) printf("\e[32m[INFO] " format "\e[0m", ##__VA_ARGS__)
#define CHECK_ALSA_ERR(expr)                            \
    do {                                                \
        int _err = (expr);                              \
        if (_err < 0) {                                 \
            LOGE("ALSA Error in '%s': %s (code: %d)\n", \
                #expr, snd_strerror(_err), _err);       \
            goto error;                                 \
        }                                               \
    } while (0)
#define BUFFER_SIZE 16384

char device_name[64] = "";

void check_card_dsd_support(int card_index) {
    snd_ctl_t* ctl_handle;
    snd_ctl_card_info_t* card_info;
    char hw_name[32];
    int dev = -1;

    sprintf(hw_name, "hw:%d", card_index);
    if (snd_ctl_open(&ctl_handle, hw_name, 0) < 0) {
        LOGE("Card %d: [Error] Cannot open control interface\n", card_index);
        return;
    }

    snd_ctl_card_info_alloca(&card_info);
    snd_ctl_card_info(ctl_handle, card_info);

    while (snd_ctl_pcm_next_device(ctl_handle, &dev) == 0 && dev >= 0) {
        snd_pcm_t* pcm_handle;
        snd_pcm_hw_params_t* params;
        char pcm_name[32];

        sprintf(pcm_name, "hw:%d,%d", card_index, dev);

        if (snd_pcm_open(&pcm_handle, pcm_name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0) {
            LOGE("[%s] %s: [Fail] Could not open pcm (device busy?)\n", pcm_name, snd_ctl_card_info_get_name(card_info));
            continue;
        }

        snd_pcm_hw_params_alloca(&params);
        snd_pcm_hw_params_any(pcm_handle, params);

        if (snd_pcm_hw_params_test_format(pcm_handle, params, SND_PCM_FORMAT_DSD_U32_BE) == 0) {
            LOGI("[%s] %s: Supports SND_PCM_FORMAT_DSD_U32_BE\n", pcm_name, snd_ctl_card_info_get_name(card_info));
            strcpy(device_name, pcm_name);
        }

        snd_pcm_close(pcm_handle);
    }
    snd_ctl_close(ctl_handle);
}

void find_card_dsd_support() {

    int card = -1;

    if (snd_card_next(&card) < 0 || card < 0) {
        LOGE("No sound cards found.\n");
        return;
    }

    while (card >= 0) {
        check_card_dsd_support(card);
        if (snd_card_next(&card) < 0)
            break;
    }
}

void deinterleave_scalar(uint8_t* data, size_t len) {
    for (size_t i = 0; i + 8 <= len; i += 8) {
        uint8_t* p = data + i;

        uint8_t a1 = p[0], b1 = p[1];
        uint8_t a2 = p[2], b2 = p[3];
        uint8_t a3 = p[4], b3 = p[5];
        uint8_t a4 = p[6], b4 = p[7];

        p[0] = a1;
        p[1] = a2;
        p[2] = a3;
        p[3] = a4;
        p[4] = b1;
        p[5] = b2;
        p[6] = b3;
        p[7] = b4;
    }
}

__attribute__((target("default")))
void deinterleave(uint8_t* data, size_t len) {
    deinterleave_scalar(data, len);
}

__attribute__((target("avx2")))
void deinterleave(uint8_t* data, size_t len) {
    __m256i mask = _mm256_setr_epi8(
        0, 2, 4, 6, 1, 3, 5, 7,
        8, 10, 12, 14, 9, 11, 13, 15,
        0, 2, 4, 6, 1, 3, 5, 7,
        8, 10, 12, 14, 9, 11, 13, 15);

    size_t i = 0;
    for (; i + 32 <= len; i += 32) {
        __m256i v = _mm256_loadu_si256((__m256i*)(data + i));
        v = _mm256_shuffle_epi8(v, mask);
        _mm256_storeu_si256((__m256i*)(data + i), v);
    }
    uint8_t* now = data + i;
    size_t len2 = len - i;
    if (len2 >= 8) {
        deinterleave_scalar(now, len2);
    }
}

int play_dsd(const char* device_name, uint32_t rate_raw, FILE* fp, uint64_t offset, uint64_t length) {
    snd_pcm_t* handle = NULL;
    snd_pcm_hw_params_t* params = NULL;
    unsigned int rate = rate_raw / 32;

    CHECK_ALSA_ERR(snd_pcm_open(&handle, device_name, SND_PCM_STREAM_PLAYBACK, 0));

    snd_pcm_hw_params_alloca(&params);

    CHECK_ALSA_ERR(snd_pcm_hw_params_any(handle, params));
    CHECK_ALSA_ERR(snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED));
    CHECK_ALSA_ERR(snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_DSD_U32_BE));
    CHECK_ALSA_ERR(snd_pcm_hw_params_set_channels(handle, params, 2));
    CHECK_ALSA_ERR(snd_pcm_hw_params_set_rate_near(handle, params, &rate, 0));
    CHECK_ALSA_ERR(snd_pcm_hw_params_set_buffer_size(handle, params, BUFFER_SIZE));
    CHECK_ALSA_ERR(snd_pcm_hw_params(handle, params));

    LOGI("ALSA DSD init ok\n");

    unsigned char buffer[BUFFER_SIZE];
    size_t bytes_read;

    LOGI("Starting playback loop...\n");
    fseeko(fp, offset, SEEK_SET);
    uint64_t left = length;

    while (left > 0 && (bytes_read = fread(buffer, 1, MIN(left, sizeof(buffer)), fp)) > 0) {
        left -= bytes_read;
        deinterleave(buffer, bytes_read);
        snd_pcm_uframes_t frames_to_write = bytes_read / 8;
        snd_pcm_sframes_t frames = snd_pcm_writei(handle, buffer, frames_to_write);

        if (frames < 0) {
            LOGE("Write error: %s\n", snd_strerror((int)frames));
            if (frames == -EPIPE)
                snd_pcm_prepare(handle);
        }
    }

    snd_pcm_drain(handle);
    snd_pcm_close(handle);
    return 0;

error:
    if (handle)
        snd_pcm_close(handle);
    return -1;
}

int read_dff(FILE* fp, uint64_t* _offset, uint64_t* _len, uint32_t* _rate) {
    uint8_t buffer[64];
    int readret = 0;
    fseeko(fp, 0, SEEK_SET);
    readret = fread(buffer, 16, 1, fp);
    uint64_t offset = 0;
    uint64_t len = 0;
    uint32_t rate = 0;
    if (readret != 1 || memcmp(buffer, "FRM8", 4) || memcmp(buffer + 12, "DSD ", 4)) {
        LOGE("Not DFF File\n");
        return -1;
    }
    uint64_t size = 0;
    while ((readret = fread(buffer, 12, 1, fp)) == 1) {
        uint64_t start = ftello(fp);
        size = __builtin_bswap64(*(uint64_t*)(buffer + 4));
        if (!memcmp(buffer, "DSD ", 4)) {
            len = size;
            offset = ftello(fp);
        } else if (!memcmp(buffer, "PROP", 4)) {
            uint64_t propstart = ftello(fp);
            fread(buffer, 4, 1, fp);
            while (ftello(fp) < propstart + size) {
                readret = fread(buffer, 12, 1, fp);
                if (readret != 1) {
                    LOGE("read fail");
                    return -1;
                }
                uint64_t pstart = ftello(fp);
                uint64_t psize = __builtin_bswap64(*(uint64_t*)(buffer + 4));
                if (!memcmp(buffer, "FS  ", 4)) {
                    fread(&rate, 4, 1, fp);
                    rate = __builtin_bswap32(rate);
                }
                fseeko(fp, pstart + psize, SEEK_SET);
            }
        }
        fseeko(fp, start + size, SEEK_SET);
    }
    LOGI("rate=%u, offset=%lu, len=%lu\n", rate, offset, len);
    if (rate == 0 || len == 0 || offset == 0) {
        LOGE("not all params found\n");
        return -2;
    }
    *_rate = rate;
    *_offset = offset;
    *_len = len;
    return 0;
}

void help(const char* argv0) {
    LOGI("Play DSD audio using DSD Native\n");
    LOGI("Usage: %s [OPTION]... [FILE]... \n", argv0);
    LOGI("\n");
    LOGI("Options:\n");
    LOGI("\t-D,--audio-device\tManually set an alsa device\n");
    LOGI("\n");
}

int main(int argc, char** argv) {

    int opt;
    char** files = NULL;
    int num_files = 0;

    static struct option long_options[] = {
        { "audio-device", required_argument, 0, 'D' },
        { "help", no_argument, 0, 'h' },
        { 0, 0, 0, 0 }
    };
    while ((opt = getopt_long(argc, argv, "D:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 'D':
            strcpy(device_name, optarg);
            break;
        case 'h':
            help(argv[0]);
            return 0;
        default:
            return 1;
        }
    }

    if (optind < argc) {
        num_files = argc - optind;
        files = &argv[optind];
    }
    if (num_files == 0) {
        help(argv[0]);
        return 0;
    }

    if (device_name[0] == 0) {
        LOGI("Detecting sound cards..\n");
        find_card_dsd_support();
        if (device_name[0] == 0) {
            LOGE("No sound card with DSD Native support found\n");
            return 1;
        }
    }

    LOGI("Using card %s\n", device_name);

    for (int i = 0; i < num_files; i++) {
        FILE* fp = fopen(files[i], "r");
        if (fp == NULL) {
            LOGE("Open file %s failed, err=%d: %s\n", files[i], errno, strerror(errno));
            continue;
        }
        LOGI("Opening file %s\n", files[i]);
        uint64_t offset;
        uint64_t len;
        uint32_t rate;
        int ret = read_dff(fp, &offset, &len, &rate);
        if (ret < 0) {
            LOGE("read_dff failed with %d\n", ret);
            fclose(fp);
            continue;
        }
        play_dsd(device_name, rate, fp, offset, len);
        fclose(fp);
    }

    return 0;
}

