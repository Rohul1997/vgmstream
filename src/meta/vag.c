#include "meta.h"
#include "../coding/coding.h"


/* VAGp - Sony SDK format, created by various tools */
VGMSTREAM * init_vgmstream_vag(STREAMFILE *streamFile) {
    VGMSTREAM * vgmstream = NULL;
    off_t start_offset;
    size_t file_size, channel_size, interleave;
    meta_t meta_type;
    int channel_count = 0, loop_flag, sample_rate;
    uint32_t vag_id, version;
    int32_t loop_start_sample = 0, loop_end_sample = 0;
    int allow_dual_stereo = 0;


    /* checks */
    /* .vag: standard
     * .swag: Frantix (PSP)
     * .str: Ben10 Galactic Racing
     * .vig: MX vs. ATV Untamed (PS2)
     * .l/r: Crash Nitro Kart (PS2), Gradius V (PS2)
     * .vas: Kingdom Hearts II (PS2)
     * .khv: fake for .vas */
    if ( !check_extensions(streamFile,"vag,swag,str,vig,l,r,vas,khv") )
        goto fail;

    /* check VAG Header */
    if (((read_32bitBE(0x00,streamFile) & 0xFFFFFF00) != 0x56414700) && /* "VAG" */
        ((read_32bitLE(0x00,streamFile) & 0xFFFFFF00) != 0x56414700))
        goto fail;

    file_size = get_streamfile_size(streamFile);

    /* version used to create the file:
     * - 00000000 = v1.8 PC,
     * - 00000002 = v1.3 Mac (used?)
     * - 00000003 = v1.6+ Mac
     * - 00000020 = v2.0 PC (most common)
     * - 00000004 = ? (later games)
     * - 00000006 = ? (vagconv)
     * - 00020001 = v2.1 (vagconv2)
     * - 00030000 = v3.0 (vagconv2) */
    version = (uint32_t)read_32bitBE(0x04,streamFile);
    /* 0x08-0c: reserved */
    channel_size = read_32bitBE(0x0c,streamFile);
    sample_rate = read_32bitBE(0x10,streamFile);
    /* 0x14-20 reserved */
    /* 0x20-30: name (optional) */
    /* 0x30: data start (first 0x10 usually 0s to init SPU) */


    /* check variation */
    vag_id = read_32bitBE(0x00,streamFile);
    switch(vag_id) {

        case 0x56414731: /* "VAG1" (1 channel) [Metal Gear Solid 3 (PS2)] */
            meta_type = meta_PS2_VAG1;
            start_offset = 0x40; /* 0x30 is extra data in VAG1 */
            channel_count = 1;
            interleave = 0;
            loop_flag = 0;
            break;

        case 0x56414732: /* "VAG2" (2 channels) [Metal Gear Solid 3 (PS2)] */
            meta_type = meta_PS2_VAG2;
            start_offset = 0x40; /* 0x30 is extra data in VAG2 */
            channel_count = 2;
            interleave = 0x800;
            loop_flag = 0;
            break;

        case 0x56414769: /* "VAGi" (interleaved) */
            meta_type = meta_PS2_VAGi;
            start_offset = 0x800;
            channel_count = 2;
            interleave = read_32bitLE(0x08,streamFile);
            loop_flag = 0;
            break;

        case 0x70474156: /* pGAV (little endian / stereo) [Jak 3 (PS2), Jak X (PS2)] */
            meta_type = meta_PS2_pGAV;
            start_offset = 0x00; //todo 0x30, requires interleave_first

            if (read_32bitBE(0x20,streamFile) == 0x53746572) { /* "Ster" */
                channel_count = 2;

                if (read_32bitLE(0x2000,streamFile) == 0x56414770) /* "pGAV" */
                    interleave = 0x2000; /* Jak 3 interleave, includes header */
                else if (read_32bitLE(0x1000,streamFile) == 0x56414770) /* "pGAV" */
                    interleave = 0x1000; /* Jak X interleave, includes header */
                else
                    interleave = 0x2000; /* Jak 3 interleave in rare files, no header */
                //todo interleave_first = interleave - start_offset; /* interleave includes header */
            }
            else {
                channel_count = 1;
                interleave = 0;
            }

            channel_size = read_32bitLE(0x0C,streamFile) / channel_count;
            sample_rate = read_32bitLE(0x10,streamFile);
            //todo adjust channel_size, includes part of header?
            loop_flag = 0;
            break;

        case 0x56414770: /* "VAGp" (standard and variations) */
            meta_type = meta_PS2_VAGp;

            if (check_extensions(streamFile,"vig")) {
                /* MX vs. ATV Untamed (PS2) */
                start_offset = 0x800 - 0x20;
                channel_count = 2;
                interleave = 0x10;
                loop_flag = 0;
            }
            else if (check_extensions(streamFile,"swag")) { /* also "VAGp" at (file_size / channels) */
                /* Frantix (PSP) */
                start_offset = 0x40; /* channel_size ignores empty frame */
                channel_count = 2;
                interleave = file_size / channel_count;

                channel_size = read_32bitLE(0x0c,streamFile);
                sample_rate = read_32bitLE(0x10,streamFile);

                loop_flag = ps_find_loop_offsets(streamFile, start_offset, channel_size*channel_count, channel_count, interleave, &loop_start_sample, &loop_end_sample);
            }
            else if (read_32bitBE(0x6000,streamFile) == 0x56414770) { /* "VAGp" */
                /* The Simpsons Wrestling (PS1) */
                start_offset = 0x00; //todo 0x30, requires interleave_first
                channel_count = 2;
                interleave = 0x6000;
                //todo interleave_first = interleave - start_offset; /* includes header */
                channel_size += 0x30;

                loop_flag = 0;
            }
            else if (read_32bitBE(0x1000,streamFile) == 0x56414770) { /* "VAGp" */
                /* Shikigami no Shiro (PS2) */
                start_offset = 0x00; //todo 0x30, requires interleave_first
                channel_count = 2;
                interleave = 0x1000;
                //todo interleave_first = interleave - start_offset; /* includes header */
                channel_size += 0x30;

                loop_flag = ps_find_loop_offsets(streamFile, start_offset, channel_size*channel_count, channel_count, interleave, &loop_start_sample, &loop_end_sample);
            }
            else if (read_32bitBE(0x30,streamFile) == 0x56414770) { /* "VAGp" */
                /* The Red Star (PS2) */
                start_offset = 0x60; /* two VAGp headers */
                channel_count = 2;

                if ((file_size - start_offset) % 0x4000 == 0)
                    interleave = 0x4000;
                else if ((file_size - start_offset) % 0x4180 == 0)
                    interleave = 0x4180;
                else
                    goto fail;

                loop_flag = 0; /* loop segments */
            }
            else if (version == 0x40000000) {
                /* Killzone (PS2) */
                start_offset = 0x30;
                channel_count = 1;
                interleave = 0;

                channel_size = read_32bitLE(0x0C,streamFile) / channel_count;
                sample_rate = read_32bitLE(0x10,streamFile);
                loop_flag = 0;
            }
            else if (version == 0x00020001 || version == 0x00030000) {
                /* standard Vita/PS4 .vag [Chronovolt (Vita), Grand Kingdom (PS4)] */
                start_offset = 0x30;
                interleave = 0x10;

                /* channels are at 0x1e, except Ukiyo no Roushi (Vita), which has
                 * loop start/end frame (but also uses PS-ADPCM flags) */
                if (read_32bitBE(0x18,streamFile) == 0
                        && (read_32bitBE(0x1c,streamFile) & 0xFFFF00FF) == 0
                        && read_8bit(0x1e,streamFile) < 16) {
                    channel_count = read_8bit(0x1e,streamFile);
                    if (channel_count == 0)
                        channel_count = 1;  /* ex. early games [Lumines (Vita)] */
                }
                else {
                    channel_count = 1;
                }

                channel_size = channel_size / channel_count;
                loop_flag = ps_find_loop_offsets(streamFile, start_offset, channel_size*channel_count, channel_count, interleave, &loop_start_sample, &loop_end_sample);
            }
            else if (version == 0x00000004 && channel_size == file_size - 0x60 && read_32bitBE(0x1c, streamFile) != 0) { /* also .vas */
                /* Kingdom Hearts II (PS2) */
                start_offset = 0x60;
                interleave = 0x10;

                loop_start_sample = read_32bitBE(0x14,streamFile);
                loop_end_sample = read_32bitBE(0x18,streamFile);
                loop_flag = (loop_end_sample > 0); /* maybe at 0x1d */
                channel_count = read_8bit(0x1e,streamFile);
                /* 0x1f: possibly volume */
                channel_size = channel_size / channel_count;
                /* mono files also have channel/volume, but start at 0x30 and are probably named .vag */
            }
            else if (read_32bitBE(0x30,streamFile) == 0x53544552   /* "STER" */
                  && read_32bitBE(0x34,streamFile) == 0x454F5641   /* "EOVA" */
                  && read_32bitBE(0x38,streamFile) == 0x47324B00){ /* "G2K " */
                /* The Simpsons Skateboarding (PS2) */
                start_offset = 0x800;
                channel_count = 2;
                interleave = 0x800;
                loop_flag = 0;
            }
            else {
                /* standard PS1/PS2/PS3 .vag [Ecco the Dolphin (PS2), Legasista (PS3)] */
                start_offset = 0x30;
                interleave = 0;

                channel_count = 1;
                loop_flag = ps_find_loop_offsets_full(streamFile, start_offset, channel_size*channel_count, channel_count, interleave, &loop_start_sample, &loop_end_sample);
                allow_dual_stereo = 1; /* often found with external L/R files */
            }
            break;

        default:
            goto fail;
    }


    /* build the VGMSTREAM */
    vgmstream = allocate_vgmstream(channel_count,loop_flag);
    if (!vgmstream) goto fail;

    vgmstream->meta_type = meta_type;
    vgmstream->allow_dual_stereo = allow_dual_stereo;

    vgmstream->sample_rate = sample_rate;
    vgmstream->num_samples = ps_bytes_to_samples(channel_size,1);
    vgmstream->loop_start_sample = loop_start_sample;
    vgmstream->loop_end_sample = loop_end_sample;
    vgmstream->coding_type = coding_PSX;
    if (version == 0x00020001 || version == 0x00030000)
        vgmstream->coding_type = coding_HEVAG;
    vgmstream->layout_type = (channel_count == 1) ? layout_none : layout_interleave;
    vgmstream->interleave_block_size = interleave;

    read_string(vgmstream->stream_name,0x10+1, 0x20,streamFile); /* always, can be null */

    if ( !vgmstream_open_stream(vgmstream, streamFile, start_offset) )
        goto fail;
    return vgmstream;

fail:
    close_vgmstream(vgmstream);
    return NULL;
}
