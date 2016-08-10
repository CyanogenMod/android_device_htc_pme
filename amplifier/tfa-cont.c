#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "tfa-cont.h"
#include "tfa9888.h"

#define MAX_PROFILES 16
#define MAX_LIVE_DATA 16
#define MAX_REGISTERS 40

#define PACKED __attribute__((packed))

typedef struct {
    unsigned offset:24;
    unsigned type:8;
} PACKED hunk_t;

typedef struct {
    unsigned char n_hunks;              /* 0 */
    unsigned char group;
    unsigned char slave;
    unsigned char unknown2;
    unsigned name_offset;               /* 4 */
    unsigned unknown;                   /* 8 */
    hunk_t hunk[];                      /* 12 */
} PACKED tfa_cont_profile_t;

typedef struct {
    unsigned short magic;
    unsigned char  version[2];
    unsigned char  sub_version[2];
    unsigned short size;
    int            crc;
    char           customer[8];
    char           application[8];
    char           type[8];
} PACKED header_t;

typedef struct {
    header_t       hdr;
    unsigned char  unknown[4];
    unsigned short n_devices;
} PACKED device_info_file_t;

typedef struct {
    header_t       hdr;
    unsigned char  data[];
} PACKED patch_t;

struct tfa_contS {
    unsigned char *raw;
    int         n_devices;
    /* All of this should be per device, but we only have 1 */
    int         dev_id;
    tfa_cont_profile_t *profiles[MAX_PROFILES];
    int         n_profiles;
    tfa_cont_profile_t *dev_profile;
};

static const char *get_string(tfa_cont_t *tc, unsigned offset)
{
    if (offset >> 24 != 3) return "Undefined string";
    return (const char *) &tc->raw[offset&0xffffff];
}

static void dump_header(header_t *hdr)
{
    printf("magic    %x\n", hdr->magic);
    printf("version  %c%c.%c%c\n", hdr->version[0], hdr->version[1], hdr->sub_version[0], hdr->sub_version[1]);
    printf("size     %d\n", hdr->size);
    printf("crc      %x\n", hdr->crc);
    printf("customer %.8s\n", hdr->customer);
    printf("appl     %.8s\n", hdr->application);
    printf("type     %.8s\n", hdr->type);
}

static void dump_hunks(hunk_t *hunks, int n_hunks)
{
    int i;

    for (i = 0; i < n_hunks; i++) {
        printf(" %d[%d]", hunks[i].type, hunks[i].offset);
    }
}

static void dump_profile(tfa_cont_t *tc, tfa_cont_profile_t *p)
{
    printf("n-hunks: %x\n", p->n_hunks);
    printf("group: %x\n", p->group);
    printf("slave: %u\n", p->slave);
printf("unknown: %x\n", p->unknown2);
    printf("name: %s\n", get_string(tc, p->name_offset));
    printf("hunks:");
    dump_hunks(p->hunk, p->n_hunks);
    printf("\n\n");
}

static void handle_patch(tfa_cont_t *tc, unsigned char *patch_raw)
{
    tc->dev_id = patch_raw[44];
    if (patch_raw[45] == 0xff && patch_raw[46] == 0xff) {
        unsigned id = patch_raw[49] | (patch_raw[48]<<8) | (patch_raw[47]<<16);
        if (id && id != 0xffffff) {
            tc->dev_id = id;
        }
    }
    printf("Device info at %ld has dev_id %d\n", patch_raw - tc->raw, tc->dev_id);
}

tfa_cont_t *tfa_cont_new(const char *fname)
{
    FILE *f = NULL;
    tfa_cont_t *tc = NULL;
    device_info_file_t *dev_info;
    unsigned char *dev;
    unsigned char *buf = NULL;
    size_t n_buf;
    struct stat stat_buf;
    int j;

    if (stat(fname, &stat_buf) < 0) {
        perror(fname);
        return NULL;
    }

    n_buf = stat_buf.st_size;

    if ((f = fopen(fname, "r")) == 0) {
        perror(fname);
        goto error;
    }

    buf = calloc(n_buf, 1);
    if (buf == NULL) {
        perror("buf");
        goto error;
    }

    if (fread(buf, 1, n_buf, f) != n_buf) {
        perror("fread");
        goto error;
    }
    dev_info = (device_info_file_t *) buf;

    if (dev_info->hdr.magic != 0x4d50) {
        fprintf(stderr, "Invalid header magic number: %s\n", fname);
        goto error;
    }

    if (dev_info->n_devices != 1) {
        fprintf(stderr, "This code only supports 1 device, do more work.\n");
        goto error;
    }

    dump_header(&dev_info->hdr);

    tc = malloc(sizeof(*tc));
    if (tc == NULL) {
        perror("tc");
        goto error;
    }

    tc->raw = buf;

    /* If more than 1 device is needed, turn this into a loop:
       tc->n_devices = dev_info->n_devices;
       for (i = 0; i < tc->n_devices; i++) {
          dev = &buf[4*(i+10)];
    */
    dev = &buf[4*(0+10)];
    tc->dev_profile = (tfa_cont_profile_t *) &buf[dev[6] | (dev[7]<<8) | (dev[8]<<16)];

    printf("device hunks: ");
    dump_hunks(tc->dev_profile->hunk, tc->dev_profile->n_hunks);
    printf("\n");

    for (j = 0; j < tc->dev_profile->n_hunks; j++) {
        hunk_t *hunk = &tc->dev_profile->hunk[j];

        switch(hunk->type) {
        case 1:
            tc->profiles[tc->n_profiles] = (tfa_cont_profile_t *) &buf[hunk->offset];
            printf("Profile %d at %d\n", tc->n_profiles, hunk->offset);
            dump_profile(tc, tc->profiles[tc->n_profiles]);
            tc->n_profiles++;
            break;
        case 5:
            handle_patch(tc, &buf[hunk->offset]);
            break;
        }
    }

    return tc;

error:
    if (f) fclose(f);
    if (tc) free(tc);
    if (buf) free(buf);

    return NULL;
}

void tfa_cont_destroy(tfa_cont_t *tc)
{
    free(tc);
}

/* Note: All these should have a device number but we only have 1 device so I'm lazy */

int tfa_cont_get_cal_profile(tfa_cont_t *tc)
{
     int i;

     for (i = 0; i < tc->n_profiles; i++) {
        const char *name = get_string(tc, tc->profiles[i]->name_offset);
        if (strstr(name, ".cal")) {
            return i;
        }
     }
     return -1;
}

const char *tfa_cont_get_profile_name(tfa_cont_t *tc, int profile)
{
    if (profile < 0 || profile > tc->n_profiles) return NULL;
    return get_string(tc, tc->profiles[profile]->name_offset);
}

typedef struct {
    unsigned char reg;
    unsigned short mask;
    unsigned short value;
} PACKED write_register_t;

static int write_register(write_register_t *wr, tfa_t *t)
{
    unsigned value;

    value = tfa_get_register(t, wr->reg);
    value = (value & ~wr->mask) | (wr->value & wr->mask);

    return tfa_set_register(t, wr->reg, value);
}

typedef struct {
    unsigned char unknown1;
    unsigned char unknown2;
    unsigned char n_payload[3]; /* in MSB byte order */
    unsigned short cmd;
    unsigned char unknown3;
    unsigned char data[];
} PACKED volume_file_t;

static int write_volume_file(tfa_cont_t *tc, unsigned char *ptr, int vstep, int a4, tfa_t *t)
{
    int reg;
    int cur_step = 0;
    unsigned n_payload;
    unsigned char *v15 = 0;
    unsigned v16 = 0;
    volume_file_t *blob;
    int v23;
    int v50;
    int v52;

LABEL_28:
printf("CRP cur_step %d vstep %d\n", cur_step, vstep);
    if (cur_step <= vstep) {
        v15 = &ptr[v16 + 4];
        v50 = 0;
        v52 = *(&ptr[4 * *v15 + 5] + v16);
        while (1) {
            unsigned v17;
            unsigned v18 = *v15;
            unsigned v20;

printf("CRP v50 %d v52 %d\n", v50, v52);
            if (v50 >= v52) {
                ++cur_step;
                v16 += 4*v18 + 2;
                goto LABEL_28;
            }

            blob = (volume_file_t *) (&ptr[4 * v18 + 5] + v16);
printf("CRP blob %p %ld\n", blob, ((unsigned char *) blob) - ptr);
            n_payload = (blob->n_payload[0]<<16) | (blob->n_payload[1] << 8) | blob->n_payload[0];
printf("CRP payload %d\n", n_payload);

            if (cur_step == vstep) {
                if (a4 > 99) {
                    break;
                }
                if (a4 == v50) {
                    break;
                }
            }
LABEL_65:
            if (blob->unknown2 != 3) {
                n_payload *= 3;
            }
            v16 += n_payload * 4;
            v50++;
        }
        v23 = blob->unknown2;
        if (!tfa_get_bitfield(t, BF_COOLFLUX_CONFIGURED)) {
            if (!blob->unknown2) {
                if (blob->unknown3) {
                    tfa_dsp_msg_id(t, 3*(n_payload-1), blob->data, blob->cmd);
                    goto LABEL_65;
                }
                tfa_dsp_msg(t, 3*n_payload, (unsigned char *) &blob->cmd);
                goto LABEL_65;
            } else if (blob->unknown2 == 2) {
                if (blob->unknown3 != 7) {
                    tfa_dsp_msg_id(t, 3*(n_payload-1), blob->data, blob->cmd);
                    goto LABEL_65;
                }
                tfa_dsp_msg(t, 3*n_payload, (unsigned char *) &blob->cmd);
                goto LABEL_65;
            }
        }
        if (blob->unknown2 == 3) {
            tfa_dsp_msg(t, 3*n_payload, (unsigned char *) &blob->cmd);
            goto LABEL_65;
        }
    }

    for (reg = 0; reg < *v15; reg++) {
        unsigned bf = (v15[1+4*reg]<<8) | v15[2+4*reg];
        unsigned value = v15[4+4*reg];
printf("CRP register %x %x\n", bf, value);
        tfa_set_bitfield(t, bf, value);
    }
tc;
    return 0;
}

static int write_file(tfa_cont_t *tc, unsigned char *ptr, int vstep, int a4, tfa_t *t)
{
    header_t *h = (header_t *) &ptr[8];
    dump_header(h);
    switch(h->magic) {
    case 0x4150: {
        patch_t *patch = (patch_t *) &ptr[8];
        return tfa_dsp_patch(t, patch->hdr.size - sizeof(patch->hdr), patch->data);
    }
    case 0x5056:
        return write_volume_file(tc, &ptr[44], vstep, a4, t);
    default:
        printf("CRP: Unknown magic %x\n", h->magic);
        assert(0);
    }
    return 0;
}

static int write_item(tfa_cont_t *tc, hunk_t *hunk, int vstep, tfa_t *t)
{
    unsigned char *ptr = &tc->raw[hunk->offset];

    switch(hunk->type) {
    case 3: /* log string? */
    case 7: /* select mode */
    case 23: /* write filter */
    case 26: /* write dsp mem */
    default:
        assert(0);
        break;
    case 4:
    case 5:
        return write_file(tc, &tc->raw[hunk->offset], vstep, 100, t);
    case 16:
        return tfa_set_bitfield(t, ptr[2] | (ptr[3]<<8), ptr[0] | (ptr[1]<<8));
    case 2:
        return write_register((write_register_t *) ptr, t);
    }

    return 0;
}

typedef enum {
    ALL, ON, OFF
} item_mode_t;

static int write_items(tfa_cont_t *tc, tfa_cont_profile_t *profile, item_mode_t mode,
                       unsigned valid, int vstep, tfa_t *t)
{
    int found_marker = 0;
    int i;

    for (i = 0; i < profile->n_hunks; i++) {
        if (profile->hunk[i].type == 17) {
            found_marker = 1;
        } else if (mode == ALL || (mode == OFF && found_marker) || (mode == ON && !found_marker)) {
            if ((valid & (1<<profile->hunk[i].type)) != 0) {
                write_item(tc, &profile->hunk[i], vstep, t);
            }
        }
    }

    return 0;
}

int tfa_cont_write_patch(tfa_cont_t *tc, tfa_t *t)
{
printf("CRP %s %d\n", __func__, __LINE__);
    write_items(tc, tc->dev_profile, ALL, 1<<5, 0, t);
printf("CRP %s %d\n", __func__, __LINE__);
    return 0;
}

static void create_dsp_buffer_msg(unsigned char *raw, unsigned char *msg, int *len)
{
    int i;

    msg[0] = raw[3];
    msg[1] = raw[2];
    msg[2] = raw[1];

    *len = 3*(raw[0] + 1);

    for (i = 1; i < raw[0]; i++) {
        msg[i*3+0] = raw[i*4+2];
        msg[i*3+1] = raw[i*4+1];
        msg[i*3+2] = raw[i*4+0];
    }
}

int tfa_cont_write_device_files(tfa_cont_t *tc, tfa_t *t)
{
    int i;
    tfa_cont_profile_t *p = tc->dev_profile;

printf("CRP %s %d\n", __func__, __LINE__);

    for (i = 0; i < p->n_hunks; i++) {
        unsigned char *ptr = &tc->raw[p->hunk[i].offset];

        if (p->hunk[i].type == 4) {
            write_file(tc, ptr, 0, 100, t);
        } else if ((p->hunk[i].type >= 8 && p->hunk[i].type < 15) || p->hunk[i].type == 22) {
            int len;
            unsigned char msg[255];

            create_dsp_buffer_msg(ptr, msg, &len);
            tfa_dsp_msg(t, len, msg);
        } else if (p->hunk[i].type == 21) {
            int len = *(short *) ptr;
            tfa_dsp_msg(t, len, ptr+2);
        } else if (p->hunk[i].type == 26) {
            /* tfaRunWriteDspMem */
            assert(0);
        }
    }
printf("CRP %s %d\n", __func__, __LINE__);
    return 0;
}

int tfa_cont_write_profile_files(tfa_cont_t *tc, int profile_num, tfa_t *t, int vstep)
{
    int i;
    tfa_cont_profile_t *p = tc->profiles[profile_num];

printf("CRP %s %d\n", __func__, __LINE__);
    for (i = 0; i < p->n_hunks; i++) {
        unsigned char *ptr = &tc->raw[p->hunk[i].offset];

        if (p->hunk[i].type == 5) {
            patch_t *patch = (patch_t *) &ptr[8];
            tfa_dsp_patch(t, patch->hdr.size - sizeof(patch->hdr), patch->data);
        } else if (p->hunk[i].type == 4) {
            write_file(tc, ptr, vstep, 100, t);
            assert(0);
        } else if (p->hunk[i].type == 26) {
            /* tfaRunWriteDspMem */
            assert(0);
        }
    }
printf("CRP %s %d\n", __func__, __LINE__);
    return 0;
}

int tfa_cont_write_device_registers(tfa_cont_t *tc, tfa_t *t)
{
printf("CRP %s %d\n", __func__, __LINE__);
    /* TODO: Should this be stopping the registers when it hits T == 0 or 1 ? */
    write_items(tc, tc->dev_profile, ALL, (1<<2) | (1<<16), 0, t);
printf("CRP %s %d\n", __func__, __LINE__);
    return 0;
}

int tfa_cont_write_profile_registers(tfa_cont_t *tc, int profile_num, tfa_t *t)
{
printf("CRP %s %d\n", __func__, __LINE__);
    write_items(tc, tc->profiles[profile_num], ALL, (1<<2) | (1<<16), 0, t);
printf("CRP %s %d\n", __func__, __LINE__);
    return 0;
}

static int get_audio_fs(tfa_cont_t *tc, tfa_cont_profile_t *prof)
{
    int i;

    for (i = 0; i < prof->n_hunks; i++) {
        unsigned char *ptr = (unsigned char *) &tc->raw[prof->hunk[i].offset];

        if (prof->hunk[i].type == 17 && *(short *) &ptr[1] == 0x203) {
            return ptr[0];
        }
    }
    return 8;
}

int tfa_cont_write_profile(tfa_cont_t *tc, int prof_num, int vstep, tfa_t *t)
{
    int swprof_num;
    tfa_cont_profile_t *profile, *swprof;
    int new_audio_fs, old_audio_fs, audio_fs;

printf("CRP %s %d\n", __func__, __LINE__);
    profile = tc->profiles[prof_num];

    swprof_num = tfa_get_swprof(t);
    if (swprof_num < 0 || swprof_num >= tc->n_profiles) {
        printf("Invalid current profile: %d\n", swprof_num);
        return -EINVAL;
    }

    swprof = tc->profiles[swprof_num];

    if (swprof->group == profile->group && profile->group) {
        old_audio_fs = 8;
    } else {
        old_audio_fs = tfa_get_bitfield(t, BF_AUDIO_FS);
        tfa_set_bitfield(t, BF_COOLFLUX_CONFIGURED, 0);
        tfa_power_off(t);
    }

    printf("Disabling profile %d\n", swprof_num);
    write_items(tc, swprof, OFF, UINT_MAX, vstep, t);

    printf("Writing profile %d\n", prof_num);
    // (T < 4 || T == 6 || T == 7 || T == 16 || T >= 18) {
#   define VALID_ON ((0x7) | (1<<6) | (1<<7) | (1<<16) | (UINT_MAX>>18<<18))
    write_items(tc, profile, ALL, VALID_ON, vstep, t);

    if (!swprof || swprof->group != profile->group || !profile->group) {
        tfa_set_bitfield(t, BF_SRC_SET_CONFIGURED, 1);
        tfa_power_on(t);
        tfa_set_bitfield(t, BF_COOLFLUX_CONFIGURED, 0);
    }

    write_items(tc, swprof, OFF, (1<<4) | (1<<5), vstep, t);
    write_items(tc, profile, ALL, (1<<4) | (1<<5), vstep, t);

    new_audio_fs = get_audio_fs(tc, profile);
    old_audio_fs = swprof ? get_audio_fs(tc, swprof) : -1;
    audio_fs = tfa_get_bitfield(t, BF_AUDIO_FS);

printf("new_audio_fs %d old_audio_fs %d audio_fs %d\n", new_audio_fs, old_audio_fs, audio_fs);

    if (new_audio_fs != old_audio_fs || new_audio_fs != audio_fs) {
        tfa_dsp_write_tables(t, new_audio_fs);
    }

printf("CRP %s %d\n", __func__, __LINE__);
    return 0;
}
