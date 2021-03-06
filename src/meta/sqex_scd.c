#include "meta.h"
#include "../coding/coding.h"
#include "../layout/layout.h"
#include "sqex_scd_streamfile.h"


#ifdef VGM_USE_VORBIS
static void scd_ogg_v2_decryption_callback(void *ptr, size_t size, size_t nmemb, void *datasource);
static void scd_ogg_v3_decryption_callback(void *ptr, size_t size, size_t nmemb, void *datasource);
#endif

/* SCD - Square-Enix games (FF XIII, XIV) */
VGMSTREAM * init_vgmstream_sqex_scd(STREAMFILE *streamFile) {
    VGMSTREAM * vgmstream = NULL;
    off_t start_offset, tables_offset, meta_offset, extradata_offset, name_offset = 0;
    int32_t stream_size, extradata_size, loop_start, loop_end;

    int loop_flag = 0, channel_count, codec, sample_rate;
    int version, target_entry, aux_chunk_count;
    int total_subsongs, target_subsong = streamFile->stream_index;

    int32_t (*read_32bit)(off_t,STREAMFILE*) = NULL;
    int16_t (*read_16bit)(off_t,STREAMFILE*) = NULL;


    /* check extension, case insensitive */
    if ( !check_extensions(streamFile, "scd") )
        goto fail;

    /** main header **/
    if (read_32bitBE(0x00,streamFile) != 0x53454442 &&  /* "SEDB" */
        read_32bitBE(0x04,streamFile) != 0x53534346)    /* "SSCF" */
        goto fail;

    if (read_8bit(0x0c,streamFile) == 0x01) { /* big endian flag */
        //size_offset = 0x14;
        read_32bit = read_32bitBE;
        read_16bit = read_16bitBE;
    } else {
        //size_offset = 0x10;
        read_32bit = read_32bitLE;
        read_16bit = read_16bitLE;
    }

    /* SSCF version? (older SSCFs from Crisis Core/FFXI X360 seem to be V3/2) */
    if (read_8bit(0x0d,streamFile) != 0x04)
        goto fail;

    /* v2: FFXIII demo (PS3), FFT0 test files (PC); v3: common; v4: Kingdom Hearts 2.8 (PS4) */
    version = read_32bit(0x08,streamFile);
    if (version != 2 && version != 3 && version != 4)
        goto fail;

    tables_offset = read_16bit(0x0e,streamFile); /* usually 0x30 or 0x20 */

#if 0
    /* never mind, FFXIII music_68tak.ps3.scd is 0x80 shorter */
    /* check file size with header value */
    if (read_32bit(size_offset,streamFile) != get_streamfile_size(streamFile))
        goto fail;
#endif


    /** offset tables **/
    /* 0x00(2): table1/4 (unknown) entries */
    /* 0x02(2): table2 (unknown) entries */
    /* 0x04(2): table3 (headers) entries */
    /* 0x06(2): unknown, varies even for clone files */

    /* (implicit: table1 starts at 0x20) */
    /* 0x08: table2 (unknown) start offset */
    /* 0x0c: table3 (headers) start offset */
    /* 0x10: table4 (unknown) start offset */
    /* 0x14: always null? */
    /* 0x18: table5? (unknown) start offset? */
    /* 0x1c: unknown, often null */
    /* each table entry is an uint32_t offset; after entries there is padding */
    /* if a table isn't present entries is 0 and offset points to next table */

    /* find meta_offset in table3 (headers) and total subsongs */
    {
        int i;
        int headers_entries = read_16bit(tables_offset+0x04,streamFile);
        off_t headers_offset = read_32bit(tables_offset+0x0c,streamFile);

        if (target_subsong == 0) target_subsong = 1;
        total_subsongs = 0;
        meta_offset = 0;

        /* manually find subsongs as entries can be dummy (ex. sfx banks in FF XIV or FF Type-0) */
        for (i = 0; i < headers_entries; i++) {
            off_t entry_offset = read_32bit(headers_offset + i*0x04,streamFile);

            if (read_32bit(entry_offset+0x0c,streamFile) == -1)
                continue; /* codec -1 when dummy */

            total_subsongs++;
            if (!meta_offset && total_subsongs == target_subsong) {
                meta_offset = entry_offset;
                target_entry = i;
            }
        }
        if (meta_offset == 0) goto fail;
        /* SCD can contain 0 entries too */
    }

    /** stream header **/
    stream_size     = read_32bit(meta_offset+0x00,streamFile);
    channel_count   = read_32bit(meta_offset+0x04,streamFile);
    sample_rate     = read_32bit(meta_offset+0x08,streamFile);
    codec           = read_32bit(meta_offset+0x0c,streamFile);

    loop_start      = read_32bit(meta_offset+0x10,streamFile);
    loop_end        = read_32bit(meta_offset+0x14,streamFile);
    extradata_size  = read_32bit(meta_offset+0x18,streamFile);
    aux_chunk_count = read_32bit(meta_offset+0x1c,streamFile);
    /* 0x01e(2): unknown, seen in some FF XIV sfx (MSADPCM) */

    loop_flag       = (loop_end > 0);
    extradata_offset = meta_offset + 0x20;
    start_offset = extradata_offset + extradata_size;

    /* only "MARK" chunk is known (some FF XIV PS3 have "STBL" but it's not counted) */
    if (aux_chunk_count > 1 && aux_chunk_count < 0xFFFF) { /* some FF XIV Heavensward IMA sfx have 0x01000000 */
        VGM_LOG("SCD: unknown aux chunk count %i\n", aux_chunk_count);
        goto fail;
    }

    /* skips aux chunks, sometimes needed (Lightning Returns X360, FF XIV PC) */
    if (aux_chunk_count && read_32bitBE(extradata_offset, streamFile) == 0x4D41524B) { /* "MARK" */
        extradata_offset += read_32bit(extradata_offset+0x04, streamFile);
    }

    /* find name if possible */
    if (version == 4) {
        int info_entries    = read_16bit(tables_offset+0x00,streamFile);
        int headers_entries = read_16bit(tables_offset+0x04,streamFile);
        off_t info_offset   = tables_offset+0x20;

        /* not very exact as table1 and table3 entries may differ in V3, not sure about V4 */
        if (info_entries == headers_entries) {
            off_t entry_offset = read_16bit(info_offset + 0x04*target_entry,streamFile);
            name_offset = entry_offset+0x30;
        }
    }


#ifdef VGM_USE_VORBIS
    /* special case using init_vgmstream_ogg_vorbis */
    if (codec == 0x06) {
        VGMSTREAM *ogg_vgmstream;
        uint8_t ogg_version, ogg_byte;
        ogg_vorbis_meta_info_t ovmi = {0};

        ovmi.meta_type = meta_SQEX_SCD;
        ovmi.total_subsongs = total_subsongs;
        /* loop values are in bytes, let init_vgmstream_ogg_vorbis find loop comments instead */

        ogg_version = read_8bit(extradata_offset + 0x00, streamFile);
        /* 0x01(1): 0x20 in v2/3, this ogg miniheader size? */
        ogg_byte    = read_8bit(extradata_offset + 0x02, streamFile);
        /* 0x03(1): ? in v3 */

        if (ogg_version == 0) { /* 0x10? header, then custom Vorbis header before regular Ogg (FF XIV PC v1) */
            ovmi.stream_size = stream_size;
        }
        else { /* 0x20 header, then seek table */
            size_t seek_table_size  = read_32bit(extradata_offset+0x10, streamFile);
            size_t vorb_header_size = read_32bit(extradata_offset+0x14, streamFile);
            /* 0x18(4): ? (can be 0) */

            if ((extradata_offset-meta_offset) + seek_table_size + vorb_header_size != extradata_size)
                goto fail;

            ovmi.stream_size = vorb_header_size + stream_size;
            start_offset = extradata_offset + 0x20 + seek_table_size; /* extradata_size skips vorb_header */

            if (ogg_version == 2) { /* header is XOR'ed using byte (FF XIV PC) */
                ovmi.decryption_callback = scd_ogg_v2_decryption_callback;
                ovmi.scd_xor = ogg_byte;
                ovmi.scd_xor_length = vorb_header_size;
            }
            else if (ogg_version == 3) { /* file is XOR'ed using table (FF XIV Heavensward PC)  */
                ovmi.decryption_callback = scd_ogg_v3_decryption_callback;
                ovmi.scd_xor = stream_size & 0xFF; /* ogg_byte not used? */
                ovmi.scd_xor_length = vorb_header_size + stream_size;
            }
            else {
                VGM_LOG("SCD: unknown ogg_version 0x%x\n", ogg_version);
            }
        }

        /* actual Ogg init */
        ogg_vgmstream = init_vgmstream_ogg_vorbis_callbacks(streamFile, NULL, start_offset, &ovmi);
        if (ogg_vgmstream && name_offset)
            read_string(ogg_vgmstream->stream_name, PATH_LIMIT, name_offset, streamFile);
        return ogg_vgmstream;
    }
#endif


    /* build the VGMSTREAM */
    vgmstream = allocate_vgmstream(channel_count,loop_flag);
    if (!vgmstream) goto fail;

    vgmstream->sample_rate = sample_rate;
    vgmstream->num_streams = total_subsongs;
    vgmstream->stream_size = stream_size;
    vgmstream->meta_type = meta_SQEX_SCD;
    if (name_offset)
        read_string(vgmstream->stream_name, PATH_LIMIT, name_offset, streamFile);

    switch (codec) {
        case 0x01:      /* PCM */
            vgmstream->coding_type = coding_PCM16LE;
            vgmstream->layout_type = layout_interleave;
            vgmstream->interleave_block_size = 0x02;

            vgmstream->num_samples = pcm_bytes_to_samples(stream_size, channel_count, 16);
            if (loop_flag) {
                vgmstream->loop_start_sample = pcm_bytes_to_samples(loop_start, channel_count, 16);
                vgmstream->loop_end_sample = pcm_bytes_to_samples(loop_end, channel_count, 16);
            }
            break;

        case 0x03:      /* PS-ADPCM [Final Fantasy Type-0] */
            vgmstream->coding_type = coding_PSX;
            vgmstream->layout_type = layout_interleave;
            vgmstream->interleave_block_size = 0x10;

            vgmstream->num_samples = ps_bytes_to_samples(stream_size, channel_count);
            if (loop_flag) {
                vgmstream->loop_start_sample = ps_bytes_to_samples(loop_start, channel_count);
                vgmstream->loop_end_sample = ps_bytes_to_samples(loop_end, channel_count);
            }
            break;

        case 0x06:      /* OGG [Final Fantasy XIII-2 (PC), Final Fantasy XIV (PC)] */
            goto fail; /* handled above */

#ifdef VGM_USE_MPEG
        case 0x07: {    /* MPEG [Final Fantasy XIII (PS3)] */
            mpeg_codec_data *mpeg_data = NULL;
            mpeg_custom_config cfg = {0};

            cfg.interleave = 0x800; /* for multistream [Final Fantasy XIII-2 (PS3)], otherwise ignored */
            cfg.data_size = stream_size;

            mpeg_data = init_mpeg_custom(streamFile, start_offset, &vgmstream->coding_type, vgmstream->channels, MPEG_SCD, &cfg);
            if (!mpeg_data) goto fail;
            vgmstream->codec_data = mpeg_data;
            vgmstream->layout_type = layout_none;

            /* some Drakengard 3, Kingdom Hearts HD have adjusted sample rate (47999, 44099), for looping? */

            vgmstream->num_samples = mpeg_bytes_to_samples(stream_size, mpeg_data);
            vgmstream->loop_start_sample = mpeg_bytes_to_samples(loop_start, mpeg_data);
            vgmstream->loop_end_sample = mpeg_bytes_to_samples(loop_end, mpeg_data);

            /* somehow loops offsets aren't always frame-aligned, and the code below supposedly helped,
             * but there isn't much difference since MPEG loops are rough (1152-aligned). Seems it
             * would help more loop_start - ~1000, loop_end + ~1000 (ex. FFXIII-2 music_SunMizu.ps3.scd) */
            //vgmstream->num_samples -= vgmstream->num_samples % 576;
            //vgmstream->loop_start_sample -= vgmstream->loop_start_sample % 576;
            //vgmstream->loop_end_sample -= vgmstream->loop_end_sample % 576;
            break;
        }
#endif
        case 0x0C:      /* MS ADPCM [Final Fantasy XIV (PC) sfx] */
            vgmstream->coding_type = coding_MSADPCM;
            vgmstream->layout_type = layout_none;
            vgmstream->interleave_block_size = read_16bit(extradata_offset+0x0c,streamFile);
            /* in extradata_offset is a WAVEFORMATEX (including coefs and all) */

            vgmstream->num_samples = msadpcm_bytes_to_samples(stream_size, vgmstream->interleave_block_size, vgmstream->channels);
            if (loop_flag) {
                vgmstream->loop_start_sample = msadpcm_bytes_to_samples(loop_start, vgmstream->interleave_block_size, vgmstream->channels);
                vgmstream->loop_end_sample = msadpcm_bytes_to_samples(loop_end, vgmstream->interleave_block_size, vgmstream->channels);
            }
            break;

        case 0x0A:      /* DSP ADPCM [Dragon Quest X (Wii)] */
        case 0x15: {    /* DSP ADPCM [Dragon Quest X (Wii U)] (no apparent differences except higher sample rate) */
            const off_t interleave_size = 0x800;
            const off_t stride_size = interleave_size * channel_count;
            int i;
            size_t total_size;
            layered_layout_data * data = NULL;

            /* interleaved DSPs including the header (so the first 0x800 is 0x60 header + 0x740 data)
             * so interleave layout can't used; we'll setup de-interleaving streamfiles as layers/channels instead */
            //todo this could be simplified using a block layout or adding interleave_first_block
            vgmstream->coding_type = coding_NGC_DSP;
            vgmstream->layout_type = layout_layered;

            /* read from the first DSP header and verify other channel headers */
            {
                total_size = (read_32bitBE(start_offset+0x04,streamFile)+1)/2; /* rounded nibbles / 2 */
                vgmstream->num_samples = read_32bitBE(start_offset+0x00,streamFile);
                if (loop_flag) {
                    vgmstream->loop_start_sample = loop_start;
                    vgmstream->loop_end_sample = loop_end+1;
                }

                for (i = 1; i < channel_count; i++) {
                    if ((read_32bitBE(start_offset+4,streamFile)+1)/2 != total_size ||
                        read_32bitBE(start_offset+interleave_size*i+0x00,streamFile) != vgmstream->num_samples) {
                        goto fail;
                    }
                }
            }

            /* init layout */
            data = init_layout_layered(channel_count);
            if (!data) goto fail;
            vgmstream->layout_data = data;

            /* open each layer subfile */
            for (i = 0; i < channel_count; i++) {
                STREAMFILE* temp_streamFile = setup_scd_dsp_streamfile(streamFile, start_offset+interleave_size*i, interleave_size, stride_size, total_size);
                if (!temp_streamFile) goto fail;

                data->layers[i] = init_vgmstream_ngc_dsp_std(temp_streamFile);
                close_streamfile(temp_streamFile);
                if (!data->layers[i]) goto fail;
            }

            /* setup layered VGMSTREAMs */
            if (!setup_layout_layered(data))
                goto fail;

            break;
        }

#ifdef VGM_USE_FFMPEG
        case 0x0B: {    /* XMA2 [Final Fantasy (X360), Lightning Returns (X360) sfx, Kingdom Hearts 2.8 (X1)] */
            ffmpeg_codec_data *ffmpeg_data = NULL;
            uint8_t buf[200];
            int32_t bytes;

            /* extradata_offset+0x00: fmt0x166 header (BE),  extradata_offset+0x34: seek table */
            bytes = ffmpeg_make_riff_xma_from_fmt_chunk(buf,200, extradata_offset,0x34, stream_size, streamFile, 1);
            ffmpeg_data = init_ffmpeg_header_offset(streamFile, buf,bytes, start_offset,stream_size);
            if (!ffmpeg_data) goto fail;
            vgmstream->codec_data = ffmpeg_data;
            vgmstream->coding_type = coding_FFmpeg;
            vgmstream->layout_type = layout_none;

            vgmstream->num_samples = ffmpeg_data->totalSamples;
            vgmstream->loop_start_sample = loop_start;
            vgmstream->loop_end_sample = loop_end;

            xma_fix_raw_samples(vgmstream, streamFile, start_offset,stream_size, 0, 0,0); /* samples are ok, loops? */
            break;
        }

        case 0x0E: {    /* ATRAC3/ATRAC3plus [Lord of Arcana (PSP), Final Fantasy Type-0] */
            ffmpeg_codec_data *ffmpeg_data = NULL;

            /* full RIFF header at start_offset/extradata_offset (same) */
            ffmpeg_data = init_ffmpeg_offset(streamFile, start_offset,stream_size);
            if (!ffmpeg_data) goto fail;
            vgmstream->codec_data = ffmpeg_data;
            vgmstream->coding_type = coding_FFmpeg;
            vgmstream->layout_type = layout_none;

            vgmstream->num_samples = ffmpeg_data->totalSamples; /* fact samples */
            vgmstream->loop_start_sample = loop_start;
            vgmstream->loop_end_sample = loop_end;

            if (ffmpeg_data->skipSamples <= 0) /* in case FFmpeg didn't get them */
                ffmpeg_set_skip_samples(ffmpeg_data, riff_get_fact_skip_samples(streamFile, start_offset));
            /* SCD loop/sample values are relative (without skip samples) vs RIFF (with skip samples), no need to adjust */
            break;
        }
#endif

#ifdef VGM_USE_ATRAC9
        case 0x16: { /* ATRAC9 [Kingdom Hearts 2.8 (PS4)] */
            atrac9_config cfg = {0};

            /* post header has various typical ATRAC9 values */
            cfg.channels = vgmstream->channels;
            cfg.config_data = read_32bit(extradata_offset+0x0c,streamFile);
            cfg.encoder_delay = read_32bit(extradata_offset+0x18,streamFile);

            vgmstream->codec_data = init_atrac9(&cfg);
            if (!vgmstream->codec_data) goto fail;
            vgmstream->coding_type = coding_ATRAC9;
            vgmstream->layout_type = layout_none;

            vgmstream->num_samples = read_32bit(extradata_offset+0x10,streamFile); /* loop values above are also weird and ignored */
            vgmstream->loop_start_sample = read_32bit(extradata_offset+0x20, streamFile) - (loop_flag ? cfg.encoder_delay : 0); //loop_start
            vgmstream->loop_end_sample   = read_32bit(extradata_offset+0x24, streamFile) - (loop_flag ? cfg.encoder_delay : 0); //loop_end
            break;
        }
#endif

        case -1:    /* used for dummy entries */
        default:
            VGM_LOG("SCD: unknown codec 0x%x\n", codec);
            goto fail;
    }


    if ( !vgmstream_open_stream(vgmstream, streamFile, start_offset) )
        goto fail;

    return vgmstream;

fail:
    close_vgmstream(vgmstream);
    return NULL;
}


#ifdef VGM_USE_VORBIS
static void scd_ogg_v2_decryption_callback(void *ptr, size_t size, size_t nmemb, void *datasource) {
    size_t bytes_read = size*nmemb;
    ogg_vorbis_streamfile * ov_streamfile = (ogg_vorbis_streamfile*)datasource;

    /* no encryption, sometimes happens */
    if (ov_streamfile->scd_xor == 0x00)
        return;

    /* header is XOR'd with a constant byte */
    if (ov_streamfile->offset < ov_streamfile->scd_xor_length) {
        int i, num_crypt;

        num_crypt = ov_streamfile->scd_xor_length - ov_streamfile->offset;
        if (num_crypt > bytes_read)
            num_crypt = bytes_read;

        for (i = 0; i < num_crypt; i++) {
            ((uint8_t*)ptr)[i] ^= (uint8_t)ov_streamfile->scd_xor;
        }
    }
}

static void scd_ogg_v3_decryption_callback(void *ptr, size_t size, size_t nmemb, void *datasource) {
    /* V3 decryption table found in the .exe of FF XIV Heavensward */
    static const uint8_t scd_ogg_v3_lookuptable[256] = {
        0x3A, 0x32, 0x32, 0x32, 0x03, 0x7E, 0x12, 0xF7, 0xB2, 0xE2, 0xA2, 0x67, 0x32, 0x32, 0x22, 0x32, // 00-0F
        0x32, 0x52, 0x16, 0x1B, 0x3C, 0xA1, 0x54, 0x7B, 0x1B, 0x97, 0xA6, 0x93, 0x1A, 0x4B, 0xAA, 0xA6, // 10-1F
        0x7A, 0x7B, 0x1B, 0x97, 0xA6, 0xF7, 0x02, 0xBB, 0xAA, 0xA6, 0xBB, 0xF7, 0x2A, 0x51, 0xBE, 0x03, // 20-2F
        0xF4, 0x2A, 0x51, 0xBE, 0x03, 0xF4, 0x2A, 0x51, 0xBE, 0x12, 0x06, 0x56, 0x27, 0x32, 0x32, 0x36, // 30-3F
        0x32, 0xB2, 0x1A, 0x3B, 0xBC, 0x91, 0xD4, 0x7B, 0x58, 0xFC, 0x0B, 0x55, 0x2A, 0x15, 0xBC, 0x40, // 40-4F
        0x92, 0x0B, 0x5B, 0x7C, 0x0A, 0x95, 0x12, 0x35, 0xB8, 0x63, 0xD2, 0x0B, 0x3B, 0xF0, 0xC7, 0x14, // 50-5F
        0x51, 0x5C, 0x94, 0x86, 0x94, 0x59, 0x5C, 0xFC, 0x1B, 0x17, 0x3A, 0x3F, 0x6B, 0x37, 0x32, 0x32, // 60-6F
        0x30, 0x32, 0x72, 0x7A, 0x13, 0xB7, 0x26, 0x60, 0x7A, 0x13, 0xB7, 0x26, 0x50, 0xBA, 0x13, 0xB4, // 70-7F
        0x2A, 0x50, 0xBA, 0x13, 0xB5, 0x2E, 0x40, 0xFA, 0x13, 0x95, 0xAE, 0x40, 0x38, 0x18, 0x9A, 0x92, // 80-8F
        0xB0, 0x38, 0x00, 0xFA, 0x12, 0xB1, 0x7E, 0x00, 0xDB, 0x96, 0xA1, 0x7C, 0x08, 0xDB, 0x9A, 0x91, // 90-9F
        0xBC, 0x08, 0xD8, 0x1A, 0x86, 0xE2, 0x70, 0x39, 0x1F, 0x86, 0xE0, 0x78, 0x7E, 0x03, 0xE7, 0x64, // A0-AF
        0x51, 0x9C, 0x8F, 0x34, 0x6F, 0x4E, 0x41, 0xFC, 0x0B, 0xD5, 0xAE, 0x41, 0xFC, 0x0B, 0xD5, 0xAE, // B0-BF
        0x41, 0xFC, 0x3B, 0x70, 0x71, 0x64, 0x33, 0x32, 0x12, 0x32, 0x32, 0x36, 0x70, 0x34, 0x2B, 0x56, // C0-CF
        0x22, 0x70, 0x3A, 0x13, 0xB7, 0x26, 0x60, 0xBA, 0x1B, 0x94, 0xAA, 0x40, 0x38, 0x00, 0xFA, 0xB2, // D0-DF
        0xE2, 0xA2, 0x67, 0x32, 0x32, 0x12, 0x32, 0xB2, 0x32, 0x32, 0x32, 0x32, 0x75, 0xA3, 0x26, 0x7B, // E0-EF
        0x83, 0x26, 0xF9, 0x83, 0x2E, 0xFF, 0xE3, 0x16, 0x7D, 0xC0, 0x1E, 0x63, 0x21, 0x07, 0xE3, 0x01, // F0-FF
    };

    size_t bytes_read = size*nmemb;
    ogg_vorbis_streamfile *ov_streamfile = (ogg_vorbis_streamfile*)datasource;

    /* file is XOR'd with a table (algorithm and table by Ioncannon) */
    { //if (ov_streamfile->offset < ov_streamfile->scd_xor_length)
        int i, num_crypt;
        uint8_t byte1, byte2, xor_byte;

        num_crypt = bytes_read;
        byte1 = ov_streamfile->scd_xor & 0x7F;
        byte2 = ov_streamfile->scd_xor & 0x3F;

        for (i = 0; i < num_crypt; i++) {
            xor_byte = scd_ogg_v3_lookuptable[(byte2 + ov_streamfile->offset + i) & 0xFF];
            xor_byte &= 0xFF;
            xor_byte ^= ((uint8_t*)ptr)[i];
            xor_byte ^= byte1;
            ((uint8_t*)ptr)[i] = (uint8_t)xor_byte;
        }
    }
}
#endif
