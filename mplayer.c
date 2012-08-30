/*
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <assert.h>

#include <libavutil/intreadwrite.h>

#include "config.h"
#include "talloc.h"

#include "osdep/io.h"

#if defined(__MINGW32__) || defined(__CYGWIN__)
#include <windows.h>
// No proper file descriptor event handling; keep waking up to poll input
#define WAKEUP_PERIOD 0.02
#else
/* Even if we can immediately wake up in response to most input events,
 * there are some timers which are not registered to the event loop
 * and need to be checked periodically (like automatic mouse cursor hiding).
 * OSD content updates behave similarly. Also some uncommon input devices
 * may not have proper FD event support.
 */
#define WAKEUP_PERIOD 0.5
#endif
#include <string.h>
#include <unistd.h>

// #include <sys/mman.h>
#include <sys/types.h>
#ifndef __MINGW32__
#include <sys/ioctl.h>
#include <sys/wait.h>
#endif

#include <sys/time.h>
#include <sys/stat.h>

#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <limits.h>

#include <errno.h>

#include "mp_msg.h"
#include "av_log.h"


#include "m_option.h"
#include "m_config.h"
#include "mplayer.h"
#include "m_property.h"

#include "sub/subreader.h"
#include "sub/find_subfiles.h"
#include "sub/dec_sub.h"

#include "mp_osd.h"
#include "libvo/video_out.h"
#include "screenshot.h"

#include "sub/sub.h"
#include "sub/av_sub.h"
#include "cpudetect.h"

#ifdef CONFIG_X11
#include "libvo/x11_common.h"
#endif

#include "libao2/audio_out.h"

#include "codec-cfg.h"

#include "sub/spudec.h"
#include "sub/vobsub.h"

#include "osdep/getch2.h"
#include "osdep/timer.h"

#include "input/input.h"

int slave_mode = 0;
int enable_mouse_movements = 0;
float start_volume = -1;

#include "osdep/priority.h"

char *heartbeat_cmd;

#include "stream/tv.h"
#include "stream/stream_radio.h"
#ifdef CONFIG_DVBIN
#include "stream/dvbin.h"
#endif
#include "stream/cache2.h"

//**************************************************************************//
//             Playtree
//**************************************************************************//
#include "playlist.h"
#include "playlist_parser.h"

//**************************************************************************//
//             Config
//**************************************************************************//
#include "parser-cfg.h"
#include "parser-mpcmd.h"

//**************************************************************************//
//             Config file
//**************************************************************************//

#include "path.h"

//**************************************************************************//
//**************************************************************************//
//             Input media streaming & demultiplexer:
//**************************************************************************//

static int max_framesize = 0;

#include "stream/stream.h"
#include "libmpdemux/demuxer.h"
#include "libmpdemux/stheader.h"

#ifdef CONFIG_DVDREAD
#include "stream/stream_dvd.h"
#endif

#include "libmpcodecs/dec_audio.h"
#include "libmpcodecs/dec_video.h"
#include "libmpcodecs/mp_image.h"
#include "libmpcodecs/vf.h"
#include "libmpcodecs/vd.h"

#include "mixer.h"

#include "mp_core.h"
#include "options.h"
#include "defaultopts.h"

static const char help_text[] = _(
"Usage:   mplayer [options] [url|path/]filename\n"
"\n"
"Basic options: (complete list in the man page)\n"
" --ss=<position>   seek to given (seconds or hh:mm:ss) position\n"
" --no-audio        do not play sound\n"
" --no-video        do not play video\n"
" --fs              fullscreen playback\n"
" --sub=<file>      specify subtitle file to use\n"
" --playlist=<file> specify playlist file\n"
"\n");

static const char av_desync_help_text[] = _(
"\n\n"
"           *************************************************\n"
"           **** Audio/Video desynchronisation detected! ****\n"
"           *************************************************\n\n"
"This means either the audio or the video is played too slowly.\n"
"Possible reasons, problems, workarounds:\n"
"- Your system is simply too slow for this file.\n"
"     Transcode it to a lower bitrate file with tools like HandBrake.\n"
"- Broken/buggy _audio_ driver.\n"
"     Experiment with different values for --autosync, 30 is a good start.\n"
"     If you have PulseAudio, try --ao=alsa .\n"
"- Slow video output.\n"
"     Try a different -vo driver (-vo help for a list) or try -framedrop!\n"
"- Playing a video file with --vo=gl/gl3 with higher FPS than your monitor.\n"
"     This is due to vsync limiting the framerate. Try --no-vsync, or a\n"
"     different VO.\n"
"- Playing from a slow network source.\n"
"     Download the file instead.\n"
"- Try to find out whether audio or video is causing this by experimenting\n"
"  with --no-video and --no-audio.\n"
"If none of this helps you, file a bug report.\n\n");


//**************************************************************************//
//**************************************************************************//

#include "mp_fifo.h"

static int drop_frame_cnt; // total number of dropped frames

// seek:
static off_t seek_to_byte;
static off_t step_sec;

static m_time_size_t end_at = { .type = END_AT_NONE, .pos = 0 };

// codecs:
char **audio_codec_list; // override audio codec
char **video_codec_list; // override video codec
char **audio_fm_list;    // override audio codec family
char **video_fm_list;    // override video codec family

// this dvdsub_id was selected via slang
// use this to allow dvdnav to follow -slang across stream resets,
// in particular the subtitle ID for a language changes
int dvdsub_lang_id;
int vobsub_id = -1;
static char *spudec_ifo = NULL;
int forced_subs_only = 0;

// cache2:
int stream_cache_size = -1;

// A-V sync:
static float default_max_pts_correction = -1;
float audio_delay = 0;
static int ignore_start = 0;

double force_fps = 0;
static int force_srate = 0;
int frame_dropping = 0;      // option  0=no drop  1= drop vo  2= drop decode
static int play_n_frames = -1;
static int play_n_frames_mf = -1;

#include "sub/ass_mp.h"


// ---

FILE *edl_fd;  // file to write to when in -edlout mode.
char *edl_output_filename; // file to put EDL entries in (-edlout)

int use_filedir_conf;

#include "mpcommon.h"
#include "command.h"

#include "metadata.h"

static float get_relative_time(struct MPContext *mpctx)
{
    unsigned int new_time = GetTimer();
    unsigned int delta = new_time - mpctx->last_time;
    mpctx->last_time = new_time;
    return delta * 0.000001;
}

static int is_valid_metadata_type(struct MPContext *mpctx, metadata_t type)
{
    switch (type) {
    /* check for valid video stream */
    case META_VIDEO_CODEC:
    case META_VIDEO_BITRATE:
    case META_VIDEO_RESOLUTION:
        if (!mpctx->sh_video)
            return 0;
        break;

    /* check for valid audio stream */
    case META_AUDIO_CODEC:
    case META_AUDIO_BITRATE:
    case META_AUDIO_SAMPLES:
        if (!mpctx->sh_audio)
            return 0;
        break;

    /* check for valid demuxer */
    case META_INFO_TITLE:
    case META_INFO_ARTIST:
    case META_INFO_ALBUM:
    case META_INFO_YEAR:
    case META_INFO_COMMENT:
    case META_INFO_TRACK:
    case META_INFO_GENRE:
        if (!mpctx->demuxer)
            return 0;
        break;

    default:
        break;
    }

    return 1;
}

static char *get_demuxer_info(struct MPContext *mpctx, char *tag)
{
    char **info = mpctx->demuxer->info;
    int n;

    if (!info || !tag)
        return talloc_strdup(NULL, "");

    for (n = 0; info[2 * n] != NULL; n++)
        if (!strcasecmp(info[2 * n], tag))
            break;

    return talloc_strdup(NULL, info[2 * n + 1] ? info[2 * n + 1] : "");
}

char *get_metadata(struct MPContext *mpctx, metadata_t type)
{
    sh_audio_t * const sh_audio = mpctx->sh_audio;
    sh_video_t * const sh_video = mpctx->sh_video;

    if (!is_valid_metadata_type(mpctx, type))
        return NULL;

    switch (type) {
    case META_NAME:
        return talloc_strdup(NULL, mp_basename(mpctx->filename));
    case META_VIDEO_CODEC:
        if (sh_video->format == 0x10000001)
            return talloc_strdup(NULL, "mpeg1");
        else if (sh_video->format == 0x10000002)
            return talloc_strdup(NULL, "mpeg2");
        else if (sh_video->format == 0x10000004)
            return talloc_strdup(NULL, "mpeg4");
        else if (sh_video->format == 0x10000005)
            return talloc_strdup(NULL, "h264");
        else if (sh_video->format >= 0x20202020)
            return talloc_asprintf(NULL, "%.4s", (char *) &sh_video->format);
        else
            return talloc_asprintf(NULL, "0x%08X", sh_video->format);
    case META_VIDEO_BITRATE:
        return talloc_asprintf(NULL, "%d kbps",
                               (int) (sh_video->i_bps * 8 / 1024));
    case META_VIDEO_RESOLUTION:
        return talloc_asprintf(NULL, "%d x %d", sh_video->disp_w,
                               sh_video->disp_h);
    case META_AUDIO_CODEC:
        if (sh_audio->codec && sh_audio->codec->name)
            return talloc_strdup(NULL, sh_audio->codec->name);
        return talloc_strdup(NULL, "");
    case META_AUDIO_BITRATE:
        return talloc_asprintf(NULL, "%d kbps",
                               (int) (sh_audio->i_bps * 8 / 1000));
    case META_AUDIO_SAMPLES:
        return talloc_asprintf(NULL, "%d Hz, %d ch.", sh_audio->samplerate,
                               sh_audio->channels);

    /* check for valid demuxer */
    case META_INFO_TITLE:
        return get_demuxer_info(mpctx, "Title");

    case META_INFO_ARTIST:
        return get_demuxer_info(mpctx, "Artist");

    case META_INFO_ALBUM:
        return get_demuxer_info(mpctx, "Album");

    case META_INFO_YEAR:
        return get_demuxer_info(mpctx, "Year");

    case META_INFO_COMMENT:
        return get_demuxer_info(mpctx, "Comment");

    case META_INFO_TRACK:
        return get_demuxer_info(mpctx, "Track");

    case META_INFO_GENRE:
        return get_demuxer_info(mpctx, "Genre");

    default:
        break;
    }

    return talloc_strdup(NULL, "");
}

static void print_stream(struct MPContext *mpctx, struct sh_stream *s)
{
    const char *tname = "?";
    const char *selopt = "?";
    const char *langopt = "?";
    switch (s->type) {
    case STREAM_VIDEO:
        tname = "video"; selopt = "vid"; langopt = "vlang";
        break;
    case STREAM_AUDIO:
        tname = "audio"; selopt = "aid"; langopt = "alang";
        break;
    case STREAM_SUB:
        tname = "subtitle"; selopt = "sid"; langopt = "slang";
        break;
    }
    mp_msg(MSGT_CPLAYER, MSGL_INFO, "[stream] ID %d: %s", s->demuxer_id, tname);
    mp_msg(MSGT_CPLAYER, MSGL_INFO, " --%s=%d", selopt, s->tid);
    char *lang = demuxer_stream_lang(mpctx->demuxer, s);
    if (lang)
        mp_msg(MSGT_CPLAYER, MSGL_INFO, " --%s=%s", langopt, lang);
    talloc_free(lang);
    if (s->default_track)
        mp_msg(MSGT_CPLAYER, MSGL_INFO, " (*)");
    if (s->title)
        mp_msg(MSGT_CPLAYER, MSGL_INFO, " '%s'", s->title);
    mp_msg(MSGT_CPLAYER, MSGL_INFO, " (");
    if (s->common_header->format) {
        int format = s->common_header->format;
        // not sure about endian crap
        char name[sizeof(format) + 1] = {0};
        memcpy(name, &format, sizeof(format));
        bool ok = true;
        for (int n = 0; name[n]; n++) {
            if ((name[n] < 32 || name[n] >= 128) && name[n] != 0)
                ok = false;
        }
        if (ok && strlen(name) > 0) {
            mp_msg(MSGT_CPLAYER, MSGL_INFO, "%s", name);
        } else {
            mp_msg(MSGT_CPLAYER, MSGL_INFO, "%#x", format);
        }
    } else if (s->type == STREAM_SUB) {
        char t = s->sub->type;
        const char *name = NULL;
        switch (t) {
        case 't': name = "SRT"; break;
        case 'a': name = "ASS"; break;
        case 'v': name = "VobSub"; break;
        }
        if (!name)
            name = (char[2]){t, '\0'};
        mp_msg(MSGT_CPLAYER, MSGL_INFO, "%s", name);
    }
    if (s->common_header->demuxer_codecname)
        mp_msg(MSGT_CPLAYER, MSGL_INFO, "/%s", s->common_header->demuxer_codecname);
    mp_msg(MSGT_CPLAYER, MSGL_INFO, ")");
    mp_msg(MSGT_CPLAYER, MSGL_INFO, "\n");
}

static void print_file_properties(struct MPContext *mpctx, const char *filename)
{
    double start_pts = MP_NOPTS_VALUE;
    double video_start_pts = MP_NOPTS_VALUE;
    mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_FILENAME=%s\n",
           filename);
    if (mpctx->demuxer)
        mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_DEMUXER=%s\n",
           mpctx->demuxer->desc->name);
    if (mpctx->sh_video) {
        /* Assume FOURCC if all bytes >= 0x20 (' ') */
        if (mpctx->sh_video->format >= 0x20202020)
            mp_msg(MSGT_IDENTIFY, MSGL_INFO,
                   "ID_VIDEO_FORMAT=%.4s\n", (char *)&mpctx->sh_video->format);
        else
            mp_msg(MSGT_IDENTIFY, MSGL_INFO,
                   "ID_VIDEO_FORMAT=0x%08X\n", mpctx->sh_video->format);
        mp_msg(MSGT_IDENTIFY, MSGL_INFO,
               "ID_VIDEO_BITRATE=%d\n", mpctx->sh_video->i_bps * 8);
        mp_msg(MSGT_IDENTIFY, MSGL_INFO,
               "ID_VIDEO_WIDTH=%d\n", mpctx->sh_video->disp_w);
        mp_msg(MSGT_IDENTIFY, MSGL_INFO,
               "ID_VIDEO_HEIGHT=%d\n", mpctx->sh_video->disp_h);
        mp_msg(MSGT_IDENTIFY, MSGL_INFO,
               "ID_VIDEO_FPS=%5.3f\n", mpctx->sh_video->fps);
        mp_msg(MSGT_IDENTIFY, MSGL_INFO,
               "ID_VIDEO_ASPECT=%1.4f\n", mpctx->sh_video->aspect);
        video_start_pts = ds_get_next_pts(mpctx->d_video);
    }
    if (mpctx->sh_audio) {
        /* Assume FOURCC if all bytes >= 0x20 (' ') */
        if (mpctx->sh_audio->format >= 0x20202020)
            mp_msg(MSGT_IDENTIFY, MSGL_INFO,
                   "ID_AUDIO_FORMAT=%.4s\n", (char *)&mpctx->sh_audio->format);
        else
            mp_msg(MSGT_IDENTIFY, MSGL_INFO,
                   "ID_AUDIO_FORMAT=%d\n", mpctx->sh_audio->format);
        mp_msg(MSGT_IDENTIFY, MSGL_INFO,
               "ID_AUDIO_BITRATE=%d\n", mpctx->sh_audio->i_bps * 8);
        mp_msg(MSGT_IDENTIFY, MSGL_INFO,
               "ID_AUDIO_RATE=%d\n", mpctx->sh_audio->samplerate);
        mp_msg(MSGT_IDENTIFY, MSGL_INFO,
               "ID_AUDIO_NCH=%d\n", mpctx->sh_audio->channels);
        start_pts = ds_get_next_pts(mpctx->d_audio);
    }
    if (video_start_pts != MP_NOPTS_VALUE) {
        if (start_pts == MP_NOPTS_VALUE || !mpctx->sh_audio ||
            (mpctx->sh_video && video_start_pts < start_pts))
            start_pts = video_start_pts;
    }
    if (start_pts != MP_NOPTS_VALUE)
        mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_START_TIME=%.2f\n", start_pts);
    else
        mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_START_TIME=unknown\n");
    mp_msg(MSGT_IDENTIFY, MSGL_INFO,
           "ID_LENGTH=%.2f\n", get_time_length(mpctx));
    mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_SEEKABLE=%d\n",
           mpctx->stream->seek
           && (!mpctx->demuxer || mpctx->demuxer->seekable));
    if (mpctx->demuxer) {
        int chapter_count = get_chapter_count(mpctx);
        mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_CHAPTERS=%d\n", chapter_count);
        for (int i = 0; i < chapter_count; i++) {
            mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_CHAPTER_ID=%d\n", i);
            // print in milliseconds
            double time = chapter_start_time(mpctx, i) * 1000.0;
            mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_CHAPTER_%d_START=%"PRId64"\n",
                   i, (int64_t)(time < 0 ? -1 : time));
            char *name = chapter_name(mpctx, i);
            if (name) {
                mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_CHAPTER_%d_NAME=%s\n", i,
                       name);
                talloc_free(name);
            }
        }
        for (int n = 0; n < mpctx->demuxer->num_streams; n++)
            print_stream(mpctx, mpctx->demuxer->streams[n]);
    }
}

/// step size of mixer changes
int volstep = 3;

static void uninit_subs(struct demuxer *demuxer)
{
    for (int i = 0; i < MAX_S_STREAMS; i++) {
        struct sh_sub *sh = demuxer->s_streams[i];
        if (sh && sh->initialized)
            sub_uninit(sh);
    }
}

void uninit_player(struct MPContext *mpctx, unsigned int mask)
{
    mask &= mpctx->initialized_flags;

    mp_msg(MSGT_CPLAYER, MSGL_DBG2, "\n*** uninit(0x%X)\n", mask);

    if (mask & INITIALIZED_ACODEC) {
        mpctx->initialized_flags &= ~INITIALIZED_ACODEC;
        if (mpctx->sh_audio)
            uninit_audio(mpctx->sh_audio);
        mpctx->sh_audio = NULL;
        mpctx->mixer.afilter = NULL;
    }

    if (mask & INITIALIZED_SUB) {
        mpctx->initialized_flags &= ~INITIALIZED_SUB;
        if (mpctx->d_sub->sh)
            sub_switchoff(mpctx->d_sub->sh, mpctx->osd);
    }

    if (mask & INITIALIZED_VCODEC) {
        mpctx->initialized_flags &= ~INITIALIZED_VCODEC;
        if (mpctx->sh_video)
            uninit_video(mpctx->sh_video);
        mpctx->sh_video = NULL;
    }

    if (mask & INITIALIZED_DEMUXER) {
        mpctx->initialized_flags &= ~INITIALIZED_DEMUXER;
        if (mpctx->num_sources) {
            mpctx->demuxer = mpctx->sources[0].demuxer;
            for (int i = 1; i < mpctx->num_sources; i++) {
                uninit_subs(mpctx->sources[i].demuxer);
                free_stream(mpctx->sources[i].stream);
                free_demuxer(mpctx->sources[i].demuxer);
            }
        }
        talloc_free(mpctx->sources);
        mpctx->sources = NULL;
        mpctx->num_sources = 0;
        talloc_free(mpctx->timeline);
        mpctx->timeline = NULL;
        mpctx->num_timeline_parts = 0;
        talloc_free(mpctx->chapters);
        mpctx->chapters = NULL;
        mpctx->num_chapters = 0;
        mpctx->video_offset = 0;
        if (mpctx->demuxer) {
            mpctx->stream = mpctx->demuxer->stream;
            uninit_subs(mpctx->demuxer);
            free_demuxer(mpctx->demuxer);
        }
        mpctx->demuxer = NULL;
    }

    // kill the cache process:
    if (mask & INITIALIZED_STREAM) {
        mpctx->initialized_flags &= ~INITIALIZED_STREAM;
        if (mpctx->stream)
            free_stream(mpctx->stream);
        mpctx->stream = NULL;
    }

    if (mask & INITIALIZED_VO) {
        mpctx->initialized_flags &= ~INITIALIZED_VO;
        vo_destroy(mpctx->video_out);
        mpctx->video_out = NULL;
    }

    // Must be after libvo uninit, as few vo drivers (svgalib) have tty code.
    if (mask & INITIALIZED_GETCH2) {
        mpctx->initialized_flags &= ~INITIALIZED_GETCH2;
        mp_msg(MSGT_CPLAYER, MSGL_DBG2, "\n[[[uninit getch2]]]\n");
        // restore terminal:
        getch2_disable();
    }

    if (mask & INITIALIZED_VOBSUB) {
        mpctx->initialized_flags &= ~INITIALIZED_VOBSUB;
        if (vo_vobsub)
            vobsub_close(vo_vobsub);
        vo_vobsub = NULL;
    }

    if (mask & INITIALIZED_SPUDEC) {
        mpctx->initialized_flags &= ~INITIALIZED_SPUDEC;
        spudec_free(vo_spudec);
        vo_spudec = NULL;
    }

    if (mask & INITIALIZED_AO) {
        mpctx->initialized_flags &= ~INITIALIZED_AO;
        if (mpctx->ao) {
            mixer_uninit(&mpctx->mixer);
            ao_uninit(mpctx->ao, mpctx->stop_play != AT_END_OF_FILE);
        }
        mpctx->ao = NULL;
        mpctx->mixer.ao = NULL;
    }
}

static void exit_player(struct MPContext *mpctx, enum exit_reason how, int rc)
{
    uninit_player(mpctx, INITIALIZED_ALL);
#if defined(__MINGW32__) || defined(__CYGWIN__)
    timeEndPeriod(1);
#endif

    mp_input_uninit(mpctx->input);

    osd_free(mpctx->osd);

#ifdef CONFIG_ASS
    ass_library_done(mpctx->ass_library);
    mpctx->ass_library = NULL;
#endif

    talloc_free(mpctx->key_fifo);

    switch (how) {
    case EXIT_QUIT:
        mp_tmsg(MSGT_CPLAYER, MSGL_INFO, "\nExiting... (%s)\n", "Quit");
        mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_EXIT=QUIT\n");
        break;
    case EXIT_EOF:
        mp_tmsg(MSGT_CPLAYER, MSGL_INFO, "\nExiting... (%s)\n", "End of file");
        mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_EXIT=EOF\n");
        break;
    case EXIT_ERROR:
        mp_tmsg(MSGT_CPLAYER, MSGL_INFO, "\nExiting... (%s)\n", "Fatal error");
        mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_EXIT=ERROR\n");
        break;
    default:
        mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_EXIT=NONE\n");
    }
    mp_msg(MSGT_CPLAYER, MSGL_DBG2,
           "max framesize was %d bytes\n", max_framesize);

    // must be last since e.g. mp_msg uses option values
    // that will be freed by this.
    if (mpctx->mconfig)
        m_config_free(mpctx->mconfig);
    mpctx->mconfig = NULL;

    talloc_free(mpctx);

    exit(rc);
}

#include "cfg-mplayer.h"

static int cfg_include(struct m_config *conf, char *filename)
{
    return m_config_parse_config_file(conf, filename);
}

#define DEF_CONFIG "# Write your default config options here!\n\n\n"

static bool parse_cfgfiles(struct MPContext *mpctx, m_config_t *conf)
{
    struct MPOpts *opts = &mpctx->opts;
    char *conffile;
    int conffile_fd;
    if (!(opts->noconfig & 2) &&
        m_config_parse_config_file(conf, MPLAYER_CONFDIR "/mplayer.conf") < 0)
        return false;
    if ((conffile = get_path("")) == NULL)
        mp_tmsg(MSGT_CPLAYER, MSGL_WARN, "Cannot find HOME directory.\n");
    else {
        mkdir(conffile, 0777);
        free(conffile);
        if ((conffile = get_path("config")) == NULL)
            mp_tmsg(MSGT_CPLAYER, MSGL_ERR, "get_path(\"config\") problem\n");
        else {
            if ((conffile_fd = open(conffile, O_CREAT | O_EXCL | O_WRONLY,
                        0666)) != -1) {
                mp_tmsg(MSGT_CPLAYER, MSGL_INFO,
                        "Creating config file: %s\n", conffile);
                write(conffile_fd, DEF_CONFIG, sizeof(DEF_CONFIG) - 1);
                close(conffile_fd);
            }
            if (!(opts->noconfig & 1) &&
                m_config_parse_config_file(conf, conffile) < 0)
                return false;
            free(conffile);
        }
    }
    return true;
}

#define PROFILE_CFG_PROTOCOL "protocol."

static void load_per_protocol_config(m_config_t *conf, const char * const file)
{
    char *str;
    char protocol[strlen(PROFILE_CFG_PROTOCOL) + strlen(file) + 1];
    m_profile_t *p;

    /* does filename actually uses a protocol ? */
    str = strstr(file, "://");
    if (!str)
        return;

    sprintf(protocol, "%s%s", PROFILE_CFG_PROTOCOL, file);
    protocol[strlen(PROFILE_CFG_PROTOCOL) + strlen(file) - strlen(str)] = '\0';
    p = m_config_get_profile(conf, protocol);
    if (p) {
        mp_tmsg(MSGT_CPLAYER, MSGL_INFO,
                "Loading protocol-related profile '%s'\n", protocol);
        m_config_set_profile(conf, p);
    }
}

#define PROFILE_CFG_EXTENSION "extension."

static void load_per_extension_config(m_config_t *conf, const char * const file)
{
    char *str;
    char extension[strlen(PROFILE_CFG_EXTENSION) + 8];
    m_profile_t *p;

    /* does filename actually have an extension ? */
    str = strrchr(file, '.');
    if (!str)
        return;

    sprintf(extension, PROFILE_CFG_EXTENSION);
    strncat(extension, ++str, 7);
    p = m_config_get_profile(conf, extension);
    if (p) {
        mp_tmsg(MSGT_CPLAYER, MSGL_INFO,
                "Loading extension-related profile '%s'\n", extension);
        m_config_set_profile(conf, p);
    }
}

#define PROFILE_CFG_VO "vo."
#define PROFILE_CFG_AO "ao."

static void load_per_output_config(m_config_t *conf, char *cfg, char *out)
{
    char profile[strlen(cfg) + strlen(out) + 1];
    m_profile_t *p;

    sprintf(profile, "%s%s", cfg, out);
    p = m_config_get_profile(conf, profile);
    if (p) {
        mp_tmsg(MSGT_CPLAYER, MSGL_INFO,
                "Loading extension-related profile '%s'\n", profile);
        m_config_set_profile(conf, p);
    }
}

/**
 * Tries to load a config file
 * @return 0 if file was not found, 1 otherwise
 */
static int try_load_config(m_config_t *conf, const char *file)
{
    if (!mp_path_exists(file))
        return 0;
    mp_tmsg(MSGT_CPLAYER, MSGL_INFO, "Loading config '%s'\n", file);
    m_config_parse_config_file(conf, file);
    return 1;
}

static void load_per_file_config(m_config_t *conf, const char * const file)
{
    char *confpath;
    char cfg[MP_PATH_MAX];
    const char *name;

    if (strlen(file) > MP_PATH_MAX - 14) {
        mp_msg(MSGT_CPLAYER, MSGL_WARN, "Filename is too long, "
               "can not load file or directory specific config files\n");
        return;
    }
    sprintf(cfg, "%s.conf", file);

    name = mp_basename(cfg);
    if (use_filedir_conf) {
        char dircfg[MP_PATH_MAX];
        strcpy(dircfg, cfg);
        strcpy(dircfg + (name - cfg), "mplayer.conf");
        try_load_config(conf, dircfg);

        if (try_load_config(conf, cfg))
            return;
    }

    if ((confpath = get_path(name)) != NULL) {
        try_load_config(conf, confpath);

        free(confpath);
    }
}

static void load_per_file_options(m_config_t *conf,
                                  struct playlist_param *params,
                                  int params_count)
{
    for (int n = 0; n < params_count; n++)
        m_config_set_option(conf, params[n].name, params[n].value);
}

/* When libmpdemux performs a blocking operation (network connection or
 * cache filling) if the operation fails we use this function to check
 * if it was interrupted by the user.
 * The function returns whether it was interrupted. */
static bool libmpdemux_was_interrupted(struct MPContext *mpctx)
{
    // Basically, give queued up user commands a chance to run, if the normal
    // play loop (which does run_command()) hasn't been executed for a while.
    mp_cmd_t *cmd = mp_input_get_cmd(mpctx->input, 0, 0);
    if (cmd) {
        // Only run "safe" commands. Consider the case someone queues up a
        // command to load a file, and immediately after that to select a
        // subtitle stream. This function can be called between opening the
        // file and opening the demuxer. We don't want the subtitle command to
        // be lost.
        if (mp_input_is_abort_cmd(cmd->id)) {
            run_command(mpctx, cmd);
            mp_cmd_free(cmd);
        }
    }
    return mpctx->stop_play != KEEP_PLAYING
        || mpctx->stop_play != AT_END_OF_FILE;
}

void add_subtitles(struct MPContext *mpctx, char *filename, float fps,
                   int noerr)
{
    struct MPOpts *opts = &mpctx->opts;
    sub_data *subd = NULL;
    struct ass_track *asst = NULL;
    bool is_native_ass = false;

    if (filename == NULL || mpctx->set_of_sub_size >= MAX_SUBTITLE_FILES)
        return;

#ifdef CONFIG_ASS
    if (opts->ass_enabled) {
        asst = mp_ass_read_stream(mpctx->ass_library, filename, sub_cp);
        is_native_ass = asst;
        if (!asst) {
            subd = sub_read_file(filename, fps, &mpctx->opts);
            if (subd) {
                asst = mp_ass_read_subdata(mpctx->ass_library, opts, subd, fps);
                sub_free(subd);
                subd = NULL;
            }
        }
    } else
#endif
    subd = sub_read_file(filename, fps, &mpctx->opts);


    if (!asst && !subd) {
        mp_tmsg(MSGT_CPLAYER, noerr ? MSGL_WARN : MSGL_ERR,
                "Cannot load subtitles: %s\n", filename);
        return;
    }

    mpctx->set_of_ass_tracks[mpctx->set_of_sub_size] = asst;
    mpctx->set_of_subtitles[mpctx->set_of_sub_size] = subd;
    mpctx->track_was_native_ass[mpctx->set_of_sub_size] = is_native_ass;
    mp_msg(MSGT_IDENTIFY, MSGL_INFO,
           "ID_FILE_SUB_ID=%d\n", mpctx->set_of_sub_size);
    mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_FILE_SUB_FILENAME=%s\n", filename);
    ++mpctx->set_of_sub_size;
    mp_tmsg(MSGT_CPLAYER, MSGL_INFO, "SUB: Added subtitle file (%d): %s\n",
            mpctx->set_of_sub_size, filename);
}

void init_vo_spudec(struct MPContext *mpctx)
{
    unsigned width, height;
    spudec_free(vo_spudec);
    mpctx->initialized_flags &= ~INITIALIZED_SPUDEC;
    vo_spudec = NULL;

    // we currently can't work without video stream
    if (!mpctx->sh_video)
        return;

    if (spudec_ifo) {
        unsigned int palette[16];
        if (vobsub_parse_ifo(NULL, spudec_ifo, palette, &width, &height,
                             1, -1, NULL) >= 0)
            vo_spudec = spudec_new_scaled(palette, width, height, NULL, 0);
    }

    width  = mpctx->sh_video->disp_w;
    height = mpctx->sh_video->disp_h;

#ifdef CONFIG_DVDREAD
    if (vo_spudec == NULL && mpctx->stream->type == STREAMTYPE_DVD) {
        vo_spudec = spudec_new_scaled(((dvd_priv_t *)(mpctx->stream->priv))->
                cur_pgc->palette, width, height, NULL, 0);
    }
#endif

    if (vo_spudec == NULL) {
        sh_sub_t *sh = mpctx->d_sub->sh;
        vo_spudec = spudec_new_scaled(NULL, width, height, sh->extradata,
                                      sh->extradata_len);
        spudec_set_font_factor(vo_spudec, font_factor);
    }

    if (vo_spudec != NULL) {
        mpctx->initialized_flags |= INITIALIZED_SPUDEC;
        mp_property_do("sub_forced_only", M_PROPERTY_SET, &forced_subs_only,
                       mpctx);
    }
}

/**
 * \brief append a formatted string
 * \param buf buffer to print into
 * \param len maximum number of characters in buf, not including terminating 0
 * \param format printf format string
 */
static void saddf(char *buf, int len, const char *format, ...)
{
    va_list va;
    va_start(va, format);
    int pos = strlen(buf);
    pos += vsnprintf(buf + pos, len - pos, format, va);
    va_end(va);
    if (pos >= len && len > 0)
        buf[len - 1] = 0;
}

/**
 * \brief append time in the hh:mm:ss.f format
 * \param buf buffer to print into
  * \param len maximum number of characters in buf, not including terminating 0
 * \param time time value to convert/append
 */
static void sadd_hhmmssff(char *buf, int len, double time, bool fractions)
{
    if (time < 0) {
        saddf(buf, len, "unknown");
        return;
    }
    int h, m, s = time;
    h = s / 3600;
    s -= h * 3600;
    m = s / 60;
    s -= m * 60;
    saddf(buf, len, "%02d:", h);
    saddf(buf, len, "%02d:", m);
    saddf(buf, len, "%02d", s);
    if (fractions)
        saddf(buf, len, ".%02d", (int)((time - (int)time) * 100));
}

static void sadd_percentage(char *buf, int len, int percent) {
    if (percent >= 0)
        saddf(buf, len, " (%d%%)", percent);
}

static void print_status(struct MPContext *mpctx, double a_pos, bool at_frame)
{
    struct MPOpts *opts = &mpctx->opts;
    sh_video_t * const sh_video = mpctx->sh_video;

    if (mpctx->sh_audio && a_pos == MP_NOPTS_VALUE)
        a_pos = playing_audio_pts(mpctx);
    if (mpctx->sh_audio && sh_video && at_frame) {
        mpctx->last_av_difference = a_pos - mpctx->video_pts - audio_delay;
        if (mpctx->time_frame > 0)
            mpctx->last_av_difference +=
                    mpctx->time_frame * opts->playback_speed;
        if (a_pos == MP_NOPTS_VALUE || mpctx->video_pts == MP_NOPTS_VALUE)
            mpctx->last_av_difference = MP_NOPTS_VALUE;
        if (mpctx->last_av_difference > 0.5 && drop_frame_cnt > 50
            && !mpctx->drop_message_shown) {
            mp_tmsg(MSGT_AVSYNC, MSGL_WARN, mp_gtext(av_desync_help_text));
            mpctx->drop_message_shown = true;
        }
    }
    if (opts->quiet)
        return;

    int width;
    char *line;
    get_screen_size();
    if (screen_width > 0)
        width = screen_width;
    else
        width = 80;
#if defined(__MINGW32__) || defined(__CYGWIN__)
    /* Windows command line is broken (MinGW's rxvt works, but we
     * should not depend on that). */
    width--;
#endif
    line = malloc(width + 1); // one additional char for the terminating null
    line[0] = '\0';

    // Playback status
    if (mpctx->paused)
        saddf(line, width, "(Paused) ");
    if (mpctx->sh_audio)
        saddf(line, width, "A");
    if (mpctx->sh_video)
        saddf(line, width, "V");
    saddf(line, width, ":");

    // Playback position
    double cur = MP_NOPTS_VALUE;
    if (mpctx->sh_audio && a_pos != MP_NOPTS_VALUE) {
        cur = a_pos;
    } else if (mpctx->sh_video && mpctx->video_pts != MP_NOPTS_VALUE) {
        cur = mpctx->video_pts;
    }
    if (cur != MP_NOPTS_VALUE) {
        saddf(line, width, " %.1f ", cur);
        saddf(line, width, "(");
        sadd_hhmmssff(line, width, cur, mpctx->opts.osd_fractions);
        saddf(line, width, ")");
    } else
        saddf(line, width, " ???");

    double len = get_time_length(mpctx);
    if (len >= 0) {
        saddf(line, width, " / %.1f (", len);
        sadd_hhmmssff(line, width, len, mpctx->opts.osd_fractions);
        saddf(line, width, ")");
    }

    sadd_percentage(line, width, get_percent_pos(mpctx));

    // other
    if (opts->playback_speed != 1)
        saddf(line, width, " x%4.2f", opts->playback_speed);

    // A-V sync
    if (mpctx->sh_audio && sh_video) {
        if (mpctx->last_av_difference != MP_NOPTS_VALUE)
            saddf(line, width, " A-V:%7.3f", mpctx->last_av_difference);
        else
            saddf(line, width, " A-V: ???");
        if (fabs(mpctx->total_avsync_change) > 0.05)
            saddf(line, width, " ct:%7.3f", mpctx->total_avsync_change);
    }

    // VO stats
    if (sh_video && drop_frame_cnt)
        saddf(line, width, " D: %d", drop_frame_cnt);

#ifdef CONFIG_STREAM_CACHE
    // cache stats
    if (stream_cache_size > 0)
        saddf(line, width, " C: %d%%", cache_fill_status(mpctx->stream));
#endif

    // end
    if (erase_to_end_of_line) {
        mp_msg(MSGT_STATUSLINE, MSGL_STATUS,
               "%s%s\r", line, erase_to_end_of_line);
    } else {
        int pos = strlen(line);
        memset(&line[pos], ' ', width - pos);
        line[width] = 0;
        mp_msg(MSGT_STATUSLINE, MSGL_STATUS, "%s\r", line);
    }
    free(line);
}

/**
 * \brief build a chain of audio filters that converts the input format
 * to the ao's format, taking into account the current playback_speed.
 * sh_audio describes the requested input format of the chain.
 * ao describes the requested output format of the chain.
 */
static int build_afilter_chain(struct MPContext *mpctx)
{
    struct sh_audio *sh_audio = mpctx->sh_audio;
    struct ao *ao = mpctx->ao;
    struct MPOpts *opts = &mpctx->opts;
    int new_srate;
    int result;
    if (!sh_audio) {
        mpctx->mixer.afilter = NULL;
        return 0;
    }
    if (af_control_any_rev(sh_audio->afilter,
                           AF_CONTROL_PLAYBACK_SPEED | AF_CONTROL_SET,
                           &opts->playback_speed))
        new_srate = sh_audio->samplerate;
    else {
        new_srate = sh_audio->samplerate * opts->playback_speed;
        if (new_srate != ao->samplerate) {
            // limits are taken from libaf/af_resample.c
            if (new_srate < 8000)
                new_srate = 8000;
            if (new_srate > 192000)
                new_srate = 192000;
            opts->playback_speed = (float)new_srate / sh_audio->samplerate;
        }
    }
    result =  init_audio_filters(sh_audio, new_srate,
                                 &ao->samplerate, &ao->channels, &ao->format);
    mpctx->mixer.afilter = sh_audio->afilter;
    return result;
}


typedef struct mp_osd_msg mp_osd_msg_t;
struct mp_osd_msg {
    /// Previous message on the stack.
    mp_osd_msg_t *prev;
    /// Message text.
    char *msg;
    int id, level, started;
    /// Display duration in ms.
    unsigned time;
    // Show full OSD for duration of message instead of msg
    // (osd_show_progression command)
    bool show_position;
};

/**
 *  \brief Add a message on the OSD message stack
 *
 *  If a message with the same id is already present in the stack
 *  it is pulled on top of the stack, otherwise a new message is created.
 *
 */
static mp_osd_msg_t *add_osd_msg(struct MPContext *mpctx, int id, int level,
                                 int time)
{
    mp_osd_msg_t *msg, *last = NULL;

    // look if the id is already in the stack
    for (msg = mpctx->osd_msg_stack; msg && msg->id != id;
         last = msg, msg = msg->prev) ;
    // not found: alloc it
    if (!msg) {
        msg = talloc_zero(mpctx, mp_osd_msg_t);
        msg->prev = mpctx->osd_msg_stack;
        mpctx->osd_msg_stack = msg;
    } else if (last) { // found, but it's not on top of the stack
        last->prev = msg->prev;
        msg->prev = mpctx->osd_msg_stack;
        mpctx->osd_msg_stack = msg;
    }
    talloc_free_children(msg);
    msg->msg = "";
    // set id and time
    msg->id = id;
    msg->level = level;
    msg->time = time;
    return msg;
}

static void set_osd_msg_va(struct MPContext *mpctx, int id, int level, int time,
                           const char *fmt, va_list ap)
{
    mp_osd_msg_t *msg = add_osd_msg(mpctx, id, level, time);
    msg->msg = talloc_vasprintf(msg, fmt, ap);
}

void set_osd_msg(struct MPContext *mpctx, int id, int level, int time,
                 const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    set_osd_msg_va(mpctx, id, level, time, fmt, ap);
    va_end(ap);
}

void set_osd_tmsg(struct MPContext *mpctx, int id, int level, int time,
                  const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    set_osd_msg_va(mpctx, id, level, time, mp_gtext(fmt), ap);
    va_end(ap);
}

/**
 *  \brief Remove a message from the OSD stack
 *
 *  This function can be used to get rid of a message right away.
 *
 */

void rm_osd_msg(struct MPContext *mpctx, int id)
{
    mp_osd_msg_t *msg, *last = NULL;

    // Search for the msg
    for (msg = mpctx->osd_msg_stack; msg && msg->id != id;
         last = msg, msg = msg->prev) ;
    if (!msg)
        return;

    // Detach it from the stack and free it
    if (last)
        last->prev = msg->prev;
    else
        mpctx->osd_msg_stack = msg->prev;
    talloc_free(msg);
}

/**
 *  \brief Remove all messages from the OSD stack
 *
 */

static void clear_osd_msgs(struct MPContext *mpctx)
{
    mp_osd_msg_t *msg = mpctx->osd_msg_stack, *prev = NULL;
    while (msg) {
        prev = msg->prev;
        talloc_free(msg);
        msg = prev;
    }
    mpctx->osd_msg_stack = NULL;
}

/**
 *  \brief Get the current message from the OSD stack.
 *
 *  This function decrements the message timer and destroys the old ones.
 *  The message that should be displayed is returned (if any).
 *
 */

static mp_osd_msg_t *get_osd_msg(struct MPContext *mpctx)
{
    struct MPOpts *opts = &mpctx->opts;
    mp_osd_msg_t *msg, *prev, *last = NULL;
    static unsigned last_update = 0;
    unsigned now = GetTimerMS();
    unsigned diff;
    char hidden_dec_done = 0;

    if (mpctx->osd_visible) {
        // 36000000 means max timed visibility is 1 hour into the future, if
        // the difference is greater assume it's wrapped around from below 0
        if (mpctx->osd_visible - now > 36000000) {
            mpctx->osd_visible = 0;
            vo_osd_progbar_type = -1; // disable
            vo_osd_changed(OSDTYPE_PROGBAR);
            mpctx->osd_function = mpctx->paused ? OSD_PAUSE : OSD_PLAY;
        }
    }

    if (!last_update)
        last_update = now;
    diff = now >= last_update ? now - last_update : 0;

    last_update = now;

    // Look for the first message in the stack with high enough level.
    for (msg = mpctx->osd_msg_stack; msg; last = msg, msg = prev) {
        prev = msg->prev;
        if (msg->level > opts->osd_level && hidden_dec_done)
            continue;
        // The message has a high enough level or it is the first hidden one
        // in both cases we decrement the timer or kill it.
        if (!msg->started || msg->time > diff) {
            if (msg->started)
                msg->time -= diff;
            else
                msg->started = 1;
            // display it
            if (msg->level <= opts->osd_level)
                return msg;
            hidden_dec_done = 1;
            continue;
        }
        // kill the message
        talloc_free(msg);
        if (last) {
            last->prev = prev;
            msg = last;
        } else {
            mpctx->osd_msg_stack = prev;
            msg = NULL;
        }
    }
    // Nothing found
    return NULL;
}

/**
 * \brief Display the OSD bar.
 *
 * Display the OSD bar or fall back on a simple message.
 *
 */

void set_osd_bar(struct MPContext *mpctx, int type, const char *name,
                 double min, double max, double val)
{
    struct MPOpts *opts = &mpctx->opts;
    if (opts->osd_level < 1)
        return;

    if (mpctx->sh_video && opts->term_osd != 1) {
        mpctx->osd_visible = (GetTimerMS() + 1000) | 1;
        vo_osd_progbar_type = type;
        vo_osd_progbar_value = 256 * (val - min) / (max - min);
        vo_osd_changed(OSDTYPE_PROGBAR);
        return;
    }

    set_osd_msg(mpctx, OSD_MSG_BAR, 1, opts->osd_duration, "%s: %d %%",
                name, ROUND(100 * (val - min) / (max - min)));
}

/**
 * \brief Display text subtitles on the OSD
 */
void set_osd_subtitle(struct MPContext *mpctx, subtitle *subs)
{
    int i;
    vo_sub = subs;
    vo_osd_changed(OSDTYPE_SUBTITLE);
    if (!mpctx->sh_video) {
        // reverse order, since newest set_osd_msg is displayed first
        for (i = SUB_MAX_TEXT - 1; i >= 0; i--) {
            if (!subs || i >= subs->lines || !subs->text[i])
                rm_osd_msg(mpctx, OSD_MSG_SUB_BASE + i);
            else {
                // HACK: currently display time for each sub line
                // except the last is set to 2 seconds.
                int display_time = i == subs->lines - 1 ? 180000 : 2000;
                set_osd_msg(mpctx, OSD_MSG_SUB_BASE + i, 1, display_time,
                            "%s", subs->text[i]);
            }
        }
    }
}

// sym == mpctx->osd_function
static void saddf_osd_function_sym(char *buffer, int len, int sym)
{
    char temp[10];
    osd_get_function_sym(temp, sizeof(temp), sym);
    saddf(buffer, len, "%s ", temp);
}

static void sadd_osd_status(char *buffer, int len, struct MPContext *mpctx,
                            bool full)
{
    bool fractions = mpctx->opts.osd_fractions;
    saddf_osd_function_sym(buffer, len, mpctx->osd_function);
    sadd_hhmmssff(buffer, len, get_current_time(mpctx), fractions);
    if (full) {
        saddf(buffer, len, " / ");
        sadd_hhmmssff(buffer, len, get_time_length(mpctx), fractions);
        sadd_percentage(buffer, len, get_percent_pos(mpctx));
    }
}

/**
 * \brief Update the OSD message line.
 *
 * This function displays the current message on the vo OSD or on the term.
 * If the stack is empty and the OSD level is high enough the timer
 * is displayed (only on the vo OSD).
 *
 */

static void update_osd_msg(struct MPContext *mpctx)
{
    struct MPOpts *opts = &mpctx->opts;
    struct osd_state *osd = mpctx->osd;

    if (mpctx->add_osd_seek_info) {
        set_osd_bar(mpctx, 0, "Position", 0, 100, get_percent_pos(mpctx));
        mpctx->add_osd_seek_info = false;
    }

    // Look if we have a msg
    mp_osd_msg_t *msg = get_osd_msg(mpctx);
    if (msg && !msg->show_position) {
        if (mpctx->sh_video && opts->term_osd != 1) {
            osd_set_text(osd, msg->msg);
        } else if (opts->term_osd) {
            if (strcmp(mpctx->terminal_osd_text, msg->msg)) {
                talloc_free(mpctx->terminal_osd_text);
                mpctx->terminal_osd_text = talloc_strdup(mpctx, msg->msg);
                mp_msg(MSGT_CPLAYER, MSGL_STATUS, "%s%s\n", opts->term_osd_esc,
                       mpctx->terminal_osd_text);
            }
        }
        return;
    }

    int osd_level = opts->osd_level;
    if (msg && msg->show_position)
        osd_level = 3;

    if (mpctx->sh_video && opts->term_osd != 1) {
        // fallback on the timer
        char text[128] = "";
        int len = sizeof(text);

        if (osd_level >= 2)
            sadd_osd_status(text, len, mpctx, osd_level == 3);

        osd_set_text(osd, text);
        return;
    }

    // Clear the term osd line
    if (opts->term_osd && mpctx->terminal_osd_text[0]) {
        mpctx->terminal_osd_text[0] = '\0';
        mp_msg(MSGT_CPLAYER, MSGL_STATUS, "%s\n", opts->term_osd_esc);
    }
}

void mp_show_osd_progression(struct MPContext *mpctx)
{
    mp_osd_msg_t *msg = add_osd_msg(mpctx, OSD_MSG_TEXT, 1,
                                    mpctx->opts.osd_duration);
    msg->show_position = true;

    set_osd_bar(mpctx, 0, "Position", 0, 100, get_percent_pos(mpctx));
}

void reinit_audio_chain(struct MPContext *mpctx)
{
    struct MPOpts *opts = &mpctx->opts;
    struct ao *ao;
    if (!mpctx->sh_audio) {
        uninit_player(mpctx, INITIALIZED_AO);
        return;
    }
    if (!(mpctx->initialized_flags & INITIALIZED_ACODEC)) {
        if (!init_best_audio_codec(mpctx->sh_audio, audio_codec_list, audio_fm_list))
            goto init_error;
        mpctx->initialized_flags |= INITIALIZED_ACODEC;
    }

    if (!(mpctx->initialized_flags & INITIALIZED_AO)) {
        mpctx->initialized_flags |= INITIALIZED_AO;
        mpctx->ao = ao_create(opts, mpctx->input);
        mpctx->ao->samplerate = force_srate;
        mpctx->ao->format = opts->audio_output_format;
    }
    ao = mpctx->ao;

    // first init to detect best values
    if (!init_audio_filters(mpctx->sh_audio,  // preliminary init
                            // input:
                            mpctx->sh_audio->samplerate,
                            // output:
                            &ao->samplerate, &ao->channels, &ao->format)) {
        mp_tmsg(MSGT_CPLAYER, MSGL_ERR, "Error at audio filter chain "
                "pre-init!\n");
        goto init_error;
    }
    if (!ao->initialized) {
        ao->buffersize = opts->ao_buffersize;
        ao_init(ao, opts->audio_driver_list);
        if (!ao->initialized) {
            mp_tmsg(MSGT_CPLAYER, MSGL_ERR,
                    "Could not open/initialize audio device -> no sound.\n");
            goto init_error;
        }
        ao->buffer.start = talloc_new(ao);
        mp_msg(MSGT_CPLAYER, MSGL_INFO,
               "AO: [%s] %dHz %dch %s (%d bytes per sample)\n",
               ao->driver->info->short_name,
               ao->samplerate, ao->channels,
               af_fmt2str_short(ao->format),
               af_fmt2bits(ao->format) / 8);
        mp_msg(MSGT_CPLAYER, MSGL_V, "AO: Description: %s\nAO: Author: %s\n",
               ao->driver->info->name, ao->driver->info->author);
        if (strlen(ao->driver->info->comment) > 0)
            mp_msg(MSGT_CPLAYER, MSGL_V, "AO: Comment: %s\n",
                   ao->driver->info->comment);
    }

    // init audio filters:
    if (!build_afilter_chain(mpctx)) {
        mp_tmsg(MSGT_CPLAYER, MSGL_ERR,
                "Couldn't find matching filter/ao format!\n");
        goto init_error;
    }
    mpctx->mixer.volstep = volstep;
    mpctx->mixer.softvol = opts->softvol;
    mpctx->mixer.softvol_max = opts->softvol_max;
    mixer_reinit(&mpctx->mixer, ao);
    mpctx->syncing_audio = true;
    return;

init_error:
    uninit_player(mpctx, INITIALIZED_ACODEC | INITIALIZED_AO);
    mpctx->sh_audio = mpctx->d_audio->sh = NULL; // -> nosound
    mpctx->d_audio->id = -2;
}


// Return pts value corresponding to the end point of audio written to the
// ao so far.
static double written_audio_pts(struct MPContext *mpctx)
{
    sh_audio_t *sh_audio = mpctx->sh_audio;
    if (!sh_audio)
        return MP_NOPTS_VALUE;
    demux_stream_t *d_audio = mpctx->d_audio;
    // first calculate the end pts of audio that has been output by decoder
    double a_pts = sh_audio->pts;
    if (a_pts != MP_NOPTS_VALUE)
        // Good, decoder supports new way of calculating audio pts.
        // sh_audio->pts is the timestamp of the latest input packet with
        // known pts that the decoder has decoded. sh_audio->pts_bytes is
        // the amount of bytes the decoder has written after that timestamp.
        a_pts += sh_audio->pts_bytes / (double) sh_audio->o_bps;
    else {
        // Decoder doesn't support new way of calculating pts (or we're
        // being called before it has decoded anything with known timestamp).
        // Use the old method of audio pts calculation: take the timestamp
        // of last packet with known pts the decoder has read data from,
        // and add amount of bytes read after the beginning of that packet
        // divided by input bps. This will be inaccurate if the input/output
        // ratio is not constant for every audio packet or if it is constant
        // but not accurately known in sh_audio->i_bps.

        a_pts = d_audio->pts;
        if (a_pts == MP_NOPTS_VALUE)
            return a_pts;

        // ds_tell_pts returns bytes read after last timestamp from
        // demuxing layer, decoder might use sh_audio->a_in_buffer for bytes
        // it has read but not decoded
        if (sh_audio->i_bps)
            a_pts += (ds_tell_pts(d_audio) - sh_audio->a_in_buffer_len) /
                     (double)sh_audio->i_bps;
    }
    // Now a_pts hopefully holds the pts for end of audio from decoder.
    // Substract data in buffers between decoder and audio out.

    // Decoded but not filtered
    a_pts -= sh_audio->a_buffer_len / (double)sh_audio->o_bps;

    // Data buffered in audio filters, measured in bytes of "missing" output
    double buffered_output = af_calc_delay(sh_audio->afilter);

    // Data that was ready for ao but was buffered because ao didn't fully
    // accept everything to internal buffers yet
    buffered_output += mpctx->ao->buffer.len;

    // Filters divide audio length by playback_speed, so multiply by it
    // to get the length in original units without speedup or slowdown
    a_pts -= buffered_output * mpctx->opts.playback_speed / mpctx->ao->bps;

    return a_pts + mpctx->video_offset;
}

// Return pts value corresponding to currently playing audio.
double playing_audio_pts(struct MPContext *mpctx)
{
    double pts = written_audio_pts(mpctx);
    if (pts == MP_NOPTS_VALUE)
        return pts;
    return pts - mpctx->opts.playback_speed *ao_get_delay(mpctx->ao);
}

static bool is_av_sub(int type)
{
    return type == 'b' || type == 'p' || type == 'x';
}

void update_subtitles(struct MPContext *mpctx, double refpts_tl, bool reset)
{
    mpctx->osd->sub_offset = mpctx->video_offset;
    struct MPOpts *opts = &mpctx->opts;
    struct sh_video *sh_video = mpctx->sh_video;
    struct demux_stream *d_sub = mpctx->d_sub;
    double refpts_s = refpts_tl - mpctx->osd->sub_offset;
    double curpts_s = refpts_s + sub_delay;
    unsigned char *packet = NULL;
    int len;
    struct sh_sub *sh_sub = d_sub->sh;
    int type = sh_sub ? sh_sub->type : 'v';
    static subtitle subs;
    if (reset) {
        if (sh_sub)
            sub_reset(sh_sub, mpctx->osd);
        sub_clear_text(&subs, MP_NOPTS_VALUE);
        if (vo_sub)
            set_osd_subtitle(mpctx, NULL);
        if (vo_spudec) {
            spudec_reset(vo_spudec);
            vo_osd_changed(OSDTYPE_SPU);
        }
        if (is_av_sub(type))
            reset_avsub(sh_sub);
        return;
    }
    // find sub
    if (mpctx->subdata) {
        if (sub_fps == 0)
            sub_fps = sh_video ? sh_video->fps : 25;
        find_sub(mpctx, mpctx->subdata, curpts_s *
                 (mpctx->subdata->sub_uses_time ? 100. : sub_fps));
        if (vo_sub)
            mpctx->vo_sub_last = vo_sub;
    }

    // DVD sub:
    if (vobsub_id >= 0 || type == 'v') {
        int timestamp;
        /* Get a sub packet from the DVD or a vobsub */
        while (1) {
            // Vobsub
            len = 0;
            if (vo_vobsub) {
                if (curpts_s >= 0) {
                    len = vobsub_get_packet(vo_vobsub, curpts_s,
                                            (void **)&packet, &timestamp);
                    if (len > 0)
                        mp_dbg(MSGT_CPLAYER, MSGL_V, "\rVOB sub: len=%d "
                               "v_pts=%5.3f v_timer=%5.3f sub=%5.3f ts=%d \n",
                               len, refpts_s, sh_video->timer,
                               timestamp / 90000.0, timestamp);
                }
            } else {
                // DVD sub
                len = ds_get_packet_sub(d_sub, (unsigned char **)&packet);
                if (len > 0) {
                    // XXX This is wrong, sh_video->pts can be arbitrarily
                    // much behind demuxing position. Unfortunately using
                    // d_video->pts which would have been the simplest
                    // improvement doesn't work because mpeg specific hacks
                    // in video.c set d_video->pts to 0.
                    float x = d_sub->pts - refpts_s;
                    if (x > -20 && x < 20) // prevent missing subs on pts reset
                        timestamp = 90000 * d_sub->pts;
                    else
                        timestamp = 90000 * curpts_s;
                    mp_dbg(MSGT_CPLAYER, MSGL_V, "\rDVD sub: len=%d  "
                           "v_pts=%5.3f  s_pts=%5.3f  ts=%d \n", len,
                           refpts_s, d_sub->pts, timestamp);
                }
            }
            if (len <= 0 || !packet)
                break;
            // create it only here, since with some broken demuxers we might
            // type = v but no DVD sub and we currently do not change the
            // "original frame size" ever after init, leading to wrong-sized
            // PGS subtitles.
            if (!vo_spudec)
                vo_spudec = spudec_new(NULL);
            if (vo_vobsub || timestamp >= 0)
                spudec_assemble(vo_spudec, packet, len, timestamp);
        }
    } else if (is_text_sub(type) || is_av_sub(type)) {
        if (d_sub->non_interleaved)
            ds_get_next_pts(d_sub);

        while (d_sub->first) {
            double subpts_s = ds_get_next_pts(d_sub);
            if (subpts_s > curpts_s) {
                // Libass handled subs can be fed to it in advance
                if (!opts->ass_enabled || !is_text_sub(type))
                    break;
                // Try to avoid demuxing whole file at once
                if (d_sub->non_interleaved && subpts_s > curpts_s + 1)
                    break;
            }
            double duration = d_sub->first->duration;
            len = ds_get_packet_sub(d_sub, &packet);
            if (is_av_sub(type)) {
                int ret = decode_avsub(sh_sub, packet, len, subpts_s, duration);
                if (ret < 0)
                    mp_msg(MSGT_SPUDEC, MSGL_WARN, "lavc failed decoding "
                           "subtitle\n");
                continue;
            }
            if (type == 'm') {
                if (len < 2)
                    continue;
                len = FFMIN(len - 2, AV_RB16(packet));
                packet += 2;
            }
            if (sh_sub && sh_sub->active) {
                sub_decode(sh_sub, mpctx->osd, packet, len, subpts_s, duration);
                continue;
            }
            if (subpts_s != MP_NOPTS_VALUE) {
                if (duration < 0)
                    sub_clear_text(&subs, MP_NOPTS_VALUE);
                if (type == 'a') { // ssa/ass subs without libass => convert to plaintext
                    int i;
                    unsigned char *p = packet;
                    for (i = 0; i < 8 && *p != '\0'; p++)
                        if (*p == ',')
                            i++;
                    if (*p == '\0')  /* Broken line? */
                        continue;
                    len -= p - packet;
                    packet = p;
                }
                double endpts_s = MP_NOPTS_VALUE;
                if (subpts_s != MP_NOPTS_VALUE && duration >= 0)
                    endpts_s = subpts_s + duration;
                sub_add_text(&subs, packet, len, endpts_s);
                set_osd_subtitle(mpctx, &subs);
            }
            if (d_sub->non_interleaved)
                ds_get_next_pts(d_sub);
        }
        if (!opts->ass_enabled)
            if (sub_clear_text(&subs, curpts_s))
                set_osd_subtitle(mpctx, &subs);
    }
    if (vo_spudec) {
        spudec_heartbeat(vo_spudec, 90000 * curpts_s);
        if (spudec_changed(vo_spudec))
            vo_osd_changed(OSDTYPE_SPU);
    }
}

static int check_framedrop(struct MPContext *mpctx, double frame_time)
{
    struct MPOpts *opts = &mpctx->opts;
    // check for frame-drop:
    if (mpctx->sh_audio && !mpctx->ao->untimed && !mpctx->d_audio->eof) {
        static int dropped_frames;
        float delay = opts->playback_speed * ao_get_delay(mpctx->ao);
        float d = delay - mpctx->delay;
        // we should avoid dropping too many frames in sequence unless we
        // are too late. and we allow 100ms A-V delay here:
        if (d < -dropped_frames * frame_time - 0.100 && !mpctx->paused
            && !mpctx->restart_playback) {
            ++drop_frame_cnt;
            ++dropped_frames;
            return frame_dropping;
        } else
            dropped_frames = 0;
    }
    return 0;
}

static float timing_sleep(struct MPContext *mpctx, float time_frame)
{
    // assume kernel HZ=100 for softsleep, works with larger HZ but with
    // unnecessarily high CPU usage
    struct MPOpts *opts = &mpctx->opts;
    float margin = opts->softsleep ? 0.011 : 0;
    while (time_frame > margin) {
        usec_sleep(1000000 * (time_frame - margin));
        time_frame -= get_relative_time(mpctx);
    }
    if (opts->softsleep) {
        if (time_frame < 0)
            mp_tmsg(MSGT_AVSYNC, MSGL_WARN,
                    "Warning! Softsleep underflow!\n");
        while (time_frame > 0)
            time_frame -= get_relative_time(mpctx);  // burn the CPU
    }
    return time_frame;
}

static int select_subtitle(MPContext *mpctx)
{
    struct MPOpts *opts = &mpctx->opts;
    // find the best sub to use
    int id;
    int found = 0;
    mpctx->global_sub_pos = -1; // no subs by default
    if (vobsub_id >= 0) {
        // if user asks for a vobsub id, use that first.
        id = vobsub_id;
        found = mp_property_do("sub_vob", M_PROPERTY_SET, &id, mpctx) ==
                M_PROPERTY_OK;
    }

    if (!found && opts->sub_id >= 0) {
        // if user asks for a dvd sub id, use that next.
        id = opts->sub_id;
        found = mp_property_do("sub_demux", M_PROPERTY_SET, &id, mpctx) ==
                M_PROPERTY_OK;
    }

    if (!found) {
        // if there are text subs to use, use those.  (autosubs come last here)
        id = 0;
        found = mp_property_do("sub_file", M_PROPERTY_SET, &id, mpctx) ==
                M_PROPERTY_OK;
    }

    if (!found && opts->sub_id == -1) {
        // finally select subs by language and container hints
        if (opts->sub_id == -1)
            opts->sub_id =
                demuxer_sub_track_by_lang_and_default(mpctx->d_sub->demuxer,
                                                      opts->sub_lang);
        if (opts->sub_id >= 0) {
            id = opts->sub_id;
            found = mp_property_do("sub_demux", M_PROPERTY_SET, &id, mpctx) ==
                    M_PROPERTY_OK;
        }
    }
    return found;
}

/* Modify video timing to match the audio timeline. There are two main
 * reasons this is needed. First, video and audio can start from different
 * positions at beginning of file or after a seek (MPlayer starts both
 * immediately even if they have different pts). Second, the file can have
 * audio timestamps that are inconsistent with the duration of the audio
 * packets, for example two consecutive timestamp values differing by
 * one second but only a packet with enough samples for half a second
 * of playback between them.
 */
static void adjust_sync(struct MPContext *mpctx, double frame_time)
{
    if (!mpctx->sh_audio || mpctx->syncing_audio)
        return;

    double a_pts = written_audio_pts(mpctx) - mpctx->delay;
    double v_pts = mpctx->sh_video->pts;
    double av_delay = a_pts - v_pts;
    // Try to sync vo_flip() so it will *finish* at given time
    av_delay += mpctx->last_vo_flip_duration;
    av_delay -= audio_delay;   // This much pts difference is desired

    double change = av_delay * 0.1;
    double max_change = default_max_pts_correction >= 0 ?
                        default_max_pts_correction : frame_time * 0.1;
    if (change < -max_change)
        change = -max_change;
    else if (change > max_change)
        change = max_change;
    mpctx->delay += change;
    mpctx->total_avsync_change += change;
}

static int write_to_ao(struct MPContext *mpctx, void *data, int len, int flags,
                       double pts)
{
    if (mpctx->paused)
        return 0;
    struct ao *ao = mpctx->ao;
    double bps = ao->bps / mpctx->opts.playback_speed;
    ao->pts = pts;
    // hack used by some mpeg-writing AOs
    ao->brokenpts = ((mpctx->sh_video ? mpctx->sh_video->timer : 0) +
                     mpctx->delay) * 90000.0;
    int played = ao_play(mpctx->ao, data, len, flags);
    if (played > 0) {
        mpctx->delay += played / bps;
        // Keep correct pts for remaining data - could be used to flush
        // remaining buffer when closing ao.
        ao->pts += played / bps;
    }
    return played;
}

#define ASYNC_PLAY_DONE -3
static int audio_start_sync(struct MPContext *mpctx, int playsize)
{
    struct ao *ao = mpctx->ao;
    struct MPOpts *opts = &mpctx->opts;
    sh_audio_t * const sh_audio = mpctx->sh_audio;
    int res;

    // Timing info may not be set without
    res = decode_audio(sh_audio, &ao->buffer, 1);
    if (res < 0)
        return res;

    int bytes;
    bool did_retry = false;
    double written_pts;
    double bps = ao->bps / opts->playback_speed;
    bool hrseek = mpctx->hrseek_active;   // audio only hrseek
    mpctx->hrseek_active = false;
    while (1) {
        written_pts = written_audio_pts(mpctx);
        double ptsdiff;
        if (hrseek)
            ptsdiff = written_pts - mpctx->hrseek_pts;
        else
            ptsdiff = written_pts - mpctx->sh_video->pts - mpctx->delay
                      - audio_delay;
        bytes = ptsdiff * bps;
        bytes -= bytes % (ao->channels * af_fmt2bits(ao->format) / 8);

        // ogg demuxers give packets without timing
        if (written_pts <= 1 && sh_audio->pts == MP_NOPTS_VALUE) {
            if (!did_retry) {
                // Try to read more data to see packets that have pts
                int res = decode_audio(sh_audio, &ao->buffer, ao->bps);
                if (res < 0)
                    return res;
                did_retry = true;
                continue;
            }
            bytes = 0;
        }

        if (fabs(ptsdiff) > 300)   // pts reset or just broken?
            bytes = 0;

        if (bytes > 0)
            break;

        mpctx->syncing_audio = false;
        int a = FFMIN(-bytes, FFMAX(playsize, 20000));
        int res = decode_audio(sh_audio, &ao->buffer, a);
        bytes += ao->buffer.len;
        if (bytes >= 0) {
            memmove(ao->buffer.start,
                    ao->buffer.start + ao->buffer.len - bytes, bytes);
            ao->buffer.len = bytes;
            if (res < 0)
                return res;
            return decode_audio(sh_audio, &ao->buffer, playsize);
        }
        ao->buffer.len = 0;
        if (res < 0)
            return res;
    }
    if (hrseek)
        // Don't add silence in audio-only case even if position is too late
        return 0;
    int fillbyte = 0;
    if ((ao->format & AF_FORMAT_SIGN_MASK) == AF_FORMAT_US)
        fillbyte = 0x80;
    if (bytes >= playsize) {
        /* This case could fall back to the one below with
         * bytes = playsize, but then silence would keep accumulating
         * in a_out_buffer if the AO accepts less data than it asks for
         * in playsize. */
        char *p = malloc(playsize);
        memset(p, fillbyte, playsize);
        write_to_ao(mpctx, p, playsize, 0, written_pts - bytes / bps);
        free(p);
        return ASYNC_PLAY_DONE;
    }
    mpctx->syncing_audio = false;
    decode_audio_prepend_bytes(&ao->buffer, bytes, fillbyte);
    return decode_audio(sh_audio, &ao->buffer, playsize);
}

static int fill_audio_out_buffers(struct MPContext *mpctx, double endpts)
{
    struct MPOpts *opts = &mpctx->opts;
    struct ao *ao = mpctx->ao;
    unsigned int t;
    int playsize;
    int playflags = 0;
    bool audio_eof = false;
    bool partial_fill = false;
    sh_audio_t * const sh_audio = mpctx->sh_audio;
    bool modifiable_audio_format = !(ao->format & AF_FORMAT_SPECIAL_MASK);
    int unitsize = ao->channels * af_fmt2bits(ao->format) / 8;

    // hack used by some mpeg-writing AOs
    ao->brokenpts = ((mpctx->sh_video ? mpctx->sh_video->timer : 0) +
                     mpctx->delay) * 90000.0;

    if (mpctx->paused)
        playsize = 1;   // just initialize things (audio pts at least)
    else
        playsize = ao_get_space(ao);

    // Fill buffer if needed:
    t = GetTimer();

    // Coming here with hrseek_active still set means audio-only
    if (!mpctx->sh_video)
        mpctx->syncing_audio = false;
    if (!opts->initial_audio_sync || !modifiable_audio_format) {
        mpctx->syncing_audio = false;
        mpctx->hrseek_active = false;
    }

    int res;
    if (mpctx->syncing_audio || mpctx->hrseek_active)
        res = audio_start_sync(mpctx, playsize);
    else
        res = decode_audio(sh_audio, &ao->buffer, playsize);
    if (res < 0) {  // EOF, error or format change
        if (res == -2) {
            /* The format change isn't handled too gracefully. A more precise
             * implementation would require draining buffered old-format audio
             * while displaying video, then doing the output format switch.
             */
            uninit_player(mpctx, INITIALIZED_AO);
            reinit_audio_chain(mpctx);
            return -1;
        } else if (res == ASYNC_PLAY_DONE)
            return 0;
        else if (mpctx->d_audio->eof)
            audio_eof = true;
    }
    t = GetTimer() - t;
    if (endpts != MP_NOPTS_VALUE && modifiable_audio_format) {
        double bytes = (endpts - written_audio_pts(mpctx) + audio_delay)
                       * ao->bps / opts->playback_speed;
        if (playsize > bytes) {
            playsize = FFMAX(bytes, 0);
            playflags |= AOPLAY_FINAL_CHUNK;
            audio_eof = true;
            partial_fill = true;
        }
    }

    assert(ao->buffer.len % unitsize == 0);
    if (playsize > ao->buffer.len) {
        partial_fill = true;
        playsize = ao->buffer.len;
        if (audio_eof)
            playflags |= AOPLAY_FINAL_CHUNK;
    }
    playsize -= playsize % unitsize;
    if (!playsize)
        return partial_fill && audio_eof ? -2 : -partial_fill;

    // play audio:

    int played = write_to_ao(mpctx, ao->buffer.start, playsize, playflags,
                             written_audio_pts(mpctx));
    assert(played % unitsize == 0);
    ao->buffer_playable_size = playsize - played;

    if (played > 0) {
        ao->buffer.len -= played;
        memmove(ao->buffer.start, ao->buffer.start + played, ao->buffer.len);
    } else if (!mpctx->paused && audio_eof && ao_get_delay(ao) < .04) {
        // Sanity check to avoid hanging in case current ao doesn't output
        // partial chunks and doesn't check for AOPLAY_FINAL_CHUNK
        return -2;
    }

    return -partial_fill;
}

static void vo_update_window_title(struct MPContext *mpctx)
{
    if (!mpctx->video_out)
        return;
    char *title = property_expand_string(mpctx, mpctx->opts.vo_wintitle);
    talloc_free(mpctx->video_out->window_title);
    mpctx->video_out->window_title = talloc_strdup(mpctx->video_out, title);
    free(title);
}

int reinit_video_chain(struct MPContext *mpctx)
{
    struct MPOpts *opts = &mpctx->opts;
    sh_video_t * const sh_video = mpctx->sh_video;
    if (!sh_video) {
        uninit_player(mpctx, INITIALIZED_VO);
        return 0;
    }
    double ar = -1.0;
    //================== Init VIDEO (codec & libvo) ==========================
    if (!opts->fixed_vo || !(mpctx->initialized_flags & INITIALIZED_VO)) {
        //shouldn't we set dvideo->id=-2 when we fail?
        //if((mpctx->video_out->preinit(vo_subdevice))!=0){
        if (!(mpctx->video_out = init_best_video_out(opts, mpctx->key_fifo,
                                                     mpctx->input))) {
            mp_tmsg(MSGT_CPLAYER, MSGL_FATAL, "Error opening/initializing "
                    "the selected video_out (-vo) device.\n");
            goto err_out;
        }
        mpctx->initialized_flags |= INITIALIZED_VO;
    }

    vo_update_window_title(mpctx);

    if (stream_control(mpctx->demuxer->stream, STREAM_CTRL_GET_ASPECT_RATIO,
                &ar) != STREAM_UNSUPPORTED)
        mpctx->sh_video->stream_aspect = ar;
    {
        char *vf_arg[] = {
            "_oldargs_", (char *)mpctx->video_out, NULL
        };
        sh_video->vfilter = vf_open_filter(opts, NULL, "vo", vf_arg);
    }

#ifdef CONFIG_ASS
    if (opts->ass_enabled) {
        int i;
        int insert = 1;
        if (opts->vf_settings)
            for (i = 0; opts->vf_settings[i].name; ++i)
                if (strcmp(opts->vf_settings[i].name, "ass") == 0) {
                    insert = 0;
                    break;
                }
        if (insert) {
            extern vf_info_t vf_info_ass;
            const vf_info_t *libass_vfs[] = {
                &vf_info_ass, NULL
            };
            char *vf_arg[] = {
                "auto", "1", NULL
            };
            int retcode = 0;
            struct vf_instance *vf_ass = vf_open_plugin_noerr(opts, libass_vfs,
                                                              sh_video->vfilter,
                                                              "ass", vf_arg,
                                                              &retcode);
            if (vf_ass)
                sh_video->vfilter = vf_ass;
            else if (retcode == -1) // vf_ass open() returns -1 VO has EOSD
                mp_msg(MSGT_CPLAYER, MSGL_V, "[ass] vf_ass not needed\n");
            else
                mp_msg(MSGT_CPLAYER, MSGL_ERR,
                       "ASS: cannot add video filter\n");
        }
    }
#endif

    sh_video->vfilter = append_filters(sh_video->vfilter, opts->vf_settings);

#ifdef CONFIG_ASS
    if (opts->ass_enabled)
        sh_video->vfilter->control(sh_video->vfilter, VFCTRL_INIT_EOSD,
                                   mpctx->ass_library);
#endif

    init_best_video_codec(sh_video, video_codec_list, video_fm_list);

    if (!sh_video->initialized) {
        if (!opts->fixed_vo)
            uninit_player(mpctx, INITIALIZED_VO);
        goto err_out;
    }

    mpctx->initialized_flags |= INITIALIZED_VCODEC;

    if (sh_video->codec)
        mp_msg(MSGT_IDENTIFY, MSGL_INFO,
               "ID_VIDEO_CODEC=%s\n", sh_video->codec->name);

    sh_video->last_pts = MP_NOPTS_VALUE;
    sh_video->num_buffered_pts = 0;
    sh_video->next_frame_time = 0;
    mpctx->restart_playback = true;
    mpctx->delay = 0;

    // ========== Init display (sh_video->disp_w*sh_video->disp_h/out_fmt) ============

    return 1;

err_out:
    mpctx->sh_video = mpctx->d_video->sh = NULL;
    return 0;
}

static double update_video_nocorrect_pts(struct MPContext *mpctx)
{
    struct sh_video *sh_video = mpctx->sh_video;
    double frame_time = 0;
    struct vo *video_out = mpctx->video_out;
    while (1) {
        // In nocorrect-pts mode there is no way to properly time these frames
        if (vo_get_buffered_frame(video_out, 0) >= 0)
            break;
        if (vf_output_queued_frame(sh_video->vfilter))
            break;
        unsigned char *packet = NULL;
        frame_time = sh_video->next_frame_time;
        if (mpctx->restart_playback)
            frame_time = 0;
        int in_size = 0;
        while (!in_size)
            in_size = video_read_frame(sh_video, &sh_video->next_frame_time,
                                       &packet, force_fps);
        if (in_size < 0)
            return -1;
        if (in_size > max_framesize)
            max_framesize = in_size;
        sh_video->timer += frame_time;
        if (mpctx->sh_audio)
            mpctx->delay -= frame_time;
        // video_read_frame can change fps (e.g. for ASF video)
        vo_fps = sh_video->fps;
        int framedrop_type = check_framedrop(mpctx, frame_time);

        void *decoded_frame;
        decoded_frame = decode_video(sh_video, sh_video->ds->current, packet,
                                     in_size, framedrop_type, sh_video->pts);
        if (decoded_frame) {
            filter_video(sh_video, decoded_frame, sh_video->pts);
        }
        break;
    }
    return frame_time;
}

static void determine_frame_pts(struct MPContext *mpctx)
{
    struct sh_video *sh_video = mpctx->sh_video;
    struct MPOpts *opts = &mpctx->opts;

    if (opts->user_pts_assoc_mode)
        sh_video->pts_assoc_mode = opts->user_pts_assoc_mode;
    else if (sh_video->pts_assoc_mode == 0) {
        if (mpctx->d_video->demuxer->timestamp_type == TIMESTAMP_TYPE_PTS
            && sh_video->codec_reordered_pts != MP_NOPTS_VALUE)
            sh_video->pts_assoc_mode = 1;
        else
            sh_video->pts_assoc_mode = 2;
    } else {
        int probcount1 = sh_video->num_reordered_pts_problems;
        int probcount2 = sh_video->num_sorted_pts_problems;
        if (sh_video->pts_assoc_mode == 2) {
            int tmp = probcount1;
            probcount1 = probcount2;
            probcount2 = tmp;
        }
        if (probcount1 >= probcount2 * 1.5 + 2) {
            sh_video->pts_assoc_mode = 3 - sh_video->pts_assoc_mode;
            mp_msg(MSGT_CPLAYER, MSGL_V, "Switching to pts association mode "
                   "%d.\n", sh_video->pts_assoc_mode);
        }
    }
    sh_video->pts = sh_video->pts_assoc_mode == 1 ?
                    sh_video->codec_reordered_pts : sh_video->sorted_pts;
}

static double update_video(struct MPContext *mpctx)
{
    struct sh_video *sh_video = mpctx->sh_video;
    struct vo *video_out = mpctx->video_out;
    sh_video->vfilter->control(sh_video->vfilter, VFCTRL_SET_OSD_OBJ,
                               mpctx->osd); // for vf_ass
    if (!mpctx->opts.correct_pts)
        return update_video_nocorrect_pts(mpctx);

    double pts;

    while (1) {
        if (vo_get_buffered_frame(video_out, false) >= 0)
            break;
        // XXX Time used in this call is not counted in any performance
        // timer now
        if (vf_output_queued_frame(sh_video->vfilter))
            break;
        int in_size = 0;
        unsigned char *buf = NULL;
        pts = MP_NOPTS_VALUE;
        struct demux_packet *pkt;
        while (1) {
            pkt = ds_get_packet2(mpctx->d_video, false);
            if (!pkt || pkt->len)
                break;
            /* Packets with size 0 are assumed to not correspond to frames,
             * but to indicate the absence of a frame in formats like AVI
             * that must have packets at fixed timecode intervals. */
        }
        if (pkt) {
            in_size = pkt->len;
            buf = pkt->buffer;
            pts = pkt->pts;
        }
        if (pts != MP_NOPTS_VALUE)
            pts += mpctx->video_offset;
        if (in_size > max_framesize)
            max_framesize = in_size;
        if (pts >= mpctx->hrseek_pts - .005)
            mpctx->hrseek_framedrop = false;
        int framedrop_type = mpctx->hrseek_framedrop ? 1 :
                             check_framedrop(mpctx, sh_video->frametime);
        void *decoded_frame = decode_video(sh_video, pkt, buf, in_size,
                                           framedrop_type, pts);
        if (decoded_frame) {
            determine_frame_pts(mpctx);
            filter_video(sh_video, decoded_frame, sh_video->pts);
        } else if (!pkt) {
            if (vo_get_buffered_frame(video_out, true) < 0)
                return -1;
        }
        break;
    }

    if (!video_out->frame_loaded)
        return 0;

    pts = video_out->next_pts;
    if (pts == MP_NOPTS_VALUE) {
        mp_msg(MSGT_CPLAYER, MSGL_ERR, "Video pts after filters MISSING\n");
        // Try to use decoder pts from before filters
        pts = sh_video->pts;
        if (pts == MP_NOPTS_VALUE)
            pts = sh_video->last_pts;
    }
    if (mpctx->hrseek_active && pts < mpctx->hrseek_pts - .005) {
        vo_skip_frame(video_out);
        return 0;
    }
    mpctx->hrseek_active = false;
    sh_video->pts = pts;
    if (sh_video->last_pts == MP_NOPTS_VALUE)
        sh_video->last_pts = sh_video->pts;
    else if (sh_video->last_pts > sh_video->pts) {
        mp_msg(MSGT_CPLAYER, MSGL_INFO, "Decreasing video pts: %f < %f\n",
               sh_video->pts, sh_video->last_pts);
        /* If the difference in pts is small treat it as jitter around the
         * right value (possibly caused by incorrect timestamp ordering) and
         * just show this frame immediately after the last one.
         * Treat bigger differences as timestamp resets and start counting
         * timing of later frames from the position of this one. */
        if (sh_video->last_pts - sh_video->pts > 0.5)
            sh_video->last_pts = sh_video->pts;
        else
            sh_video->pts = sh_video->last_pts;
    }
    double frame_time = sh_video->pts - sh_video->last_pts;
    sh_video->last_pts = sh_video->pts;
    sh_video->timer += frame_time;
    if (mpctx->sh_audio)
        mpctx->delay -= frame_time;
    return frame_time;
}

void pause_player(struct MPContext *mpctx)
{
    if (mpctx->paused)
        return;
    mpctx->paused = 1;
    mpctx->step_frames = 0;
    mpctx->time_frame -= get_relative_time(mpctx);
    mpctx->osd_function = OSD_PAUSE;

    if (mpctx->video_out && mpctx->sh_video && mpctx->video_out->config_ok)
        vo_control(mpctx->video_out, VOCTRL_PAUSE, NULL);

    if (mpctx->ao && mpctx->sh_audio)
        ao_pause(mpctx->ao);    // pause audio, keep data if possible

    // Only print status if there's actually a file being played.
    if (mpctx->demuxer)
        print_status(mpctx, MP_NOPTS_VALUE, false);

    if (!mpctx->opts.quiet)
        mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_PAUSED\n");
}

void unpause_player(struct MPContext *mpctx)
{
    if (!mpctx->paused)
        return;
    mpctx->paused = 0;
    if (!mpctx->step_frames)
        mpctx->osd_function = OSD_PLAY;

    if (mpctx->ao && mpctx->sh_audio)
        ao_resume(mpctx->ao);
    if (mpctx->video_out && mpctx->sh_video && mpctx->video_out->config_ok
        && !mpctx->step_frames)
        vo_control(mpctx->video_out, VOCTRL_RESUME, NULL);      // resume video
    (void)get_relative_time(mpctx);     // ignore time that passed during pause
}

static int redraw_osd(struct MPContext *mpctx)
{
    struct sh_video *sh_video = mpctx->sh_video;
    struct vf_instance *vf = sh_video->vfilter;
    if (sh_video->output_flags & VFCAP_OSD_FILTER)
        return -1;
    if (vo_redraw_frame(mpctx->video_out) < 0)
        return -1;
    mpctx->osd->pts = mpctx->video_pts - mpctx->osd->sub_offset;
    if (!(sh_video->output_flags & VFCAP_EOSD_FILTER))
        vf->control(vf, VFCTRL_DRAW_EOSD, mpctx->osd);
    vf->control(vf, VFCTRL_DRAW_OSD, mpctx->osd);
    vo_osd_reset_changed();
    vo_flip_page(mpctx->video_out, 0, -1);
    return 0;
}

void add_step_frame(struct MPContext *mpctx)
{
    mpctx->step_frames++;
    if (mpctx->video_out && mpctx->sh_video && mpctx->video_out->config_ok)
        vo_control(mpctx->video_out, VOCTRL_PAUSE, NULL);
    unpause_player(mpctx);
}

static void seek_reset(struct MPContext *mpctx, bool reset_ao, bool reset_ac)
{
    if (mpctx->sh_video) {
        resync_video_stream(mpctx->sh_video);
        mpctx->sh_video->timer = 0;
        vo_seek_reset(mpctx->video_out);
        mpctx->sh_video->timer = 0;
        mpctx->sh_video->num_buffered_pts = 0;
        mpctx->sh_video->last_pts = MP_NOPTS_VALUE;
        mpctx->delay = 0;
        mpctx->time_frame = 0;
        // Not all demuxers set d_video->pts during seek, so this value
        // (which is used by at least vobsub code below) may be completely
        // wrong (probably 0).
        mpctx->sh_video->pts = mpctx->d_video->pts + mpctx->video_offset;
        mpctx->video_pts = mpctx->sh_video->pts;
        update_subtitles(mpctx, mpctx->sh_video->pts, true);
    }

    if (mpctx->sh_audio && reset_ac) {
        resync_audio_stream(mpctx->sh_audio);
        if (reset_ao)
            ao_reset(mpctx->ao);
        mpctx->ao->buffer.len = mpctx->ao->buffer_playable_size;
        mpctx->sh_audio->a_buffer_len = 0;
        if (!mpctx->sh_video)
            update_subtitles(mpctx, mpctx->sh_audio->pts, true);
    }

    if (vo_vobsub && mpctx->sh_video) {
        vobsub_seek(vo_vobsub, mpctx->sh_video->pts);
    }

    mpctx->restart_playback = true;
    mpctx->hrseek_active = false;
    mpctx->hrseek_framedrop = false;
    mpctx->total_avsync_change = 0;
    drop_frame_cnt = 0;
}

static bool timeline_set_part(struct MPContext *mpctx, int i)
{
    struct timeline_part *p = mpctx->timeline + mpctx->timeline_part;
    struct timeline_part *n = mpctx->timeline + i;
    mpctx->timeline_part = i;
    mpctx->video_offset = n->start - n->source_start;
    if (n->source == p->source)
        return false;
    enum stop_play_reason orig_stop_play = mpctx->stop_play;
    if (!mpctx->sh_video && mpctx->stop_play == KEEP_PLAYING)
        mpctx->stop_play = AT_END_OF_FILE;  // let audio uninit drain data
    uninit_player(mpctx, INITIALIZED_VCODEC | (mpctx->opts.fixed_vo ? 0 : INITIALIZED_VO) | (mpctx->opts.gapless_audio ? 0 : INITIALIZED_AO) | INITIALIZED_ACODEC | INITIALIZED_SUB);
    mpctx->stop_play = orig_stop_play;
    mpctx->demuxer = n->source->demuxer;
    mpctx->d_video = mpctx->demuxer->video;
    mpctx->d_audio = mpctx->demuxer->audio;
    mpctx->d_sub = mpctx->demuxer->sub;
    mpctx->sh_video = mpctx->d_video->sh;
    mpctx->sh_audio = mpctx->d_audio->sh;
    return true;
}

// Given pts, switch playback to the corresponding part.
// Return offset within that part.
static double timeline_set_from_time(struct MPContext *mpctx, double pts,
                                     bool *need_reset)
{
    if (pts < 0)
        pts = 0;
    for (int i = 0; i < mpctx->num_timeline_parts; i++) {
        struct timeline_part *p = mpctx->timeline + i;
        if (pts < (p + 1)->start) {
            *need_reset = timeline_set_part(mpctx, i);
            return pts - p->start + p->source_start;
        }
    }
    return -1;
}


// return -1 if seek failed (non-seekable stream?), 0 otherwise
static int seek(MPContext *mpctx, struct seek_params seek,
                bool timeline_fallthrough)
{
    struct MPOpts *opts = &mpctx->opts;

    if (!mpctx->demuxer)
        return -1;

    if (mpctx->stop_play == AT_END_OF_FILE)
        mpctx->stop_play = KEEP_PLAYING;
    bool hr_seek = mpctx->demuxer->accurate_seek && opts->correct_pts;
    hr_seek &= seek.exact >= 0 && seek.type != MPSEEK_FACTOR;
    hr_seek &= opts->hr_seek == 0 && seek.type == MPSEEK_ABSOLUTE
               || opts->hr_seek > 0 || seek.exact > 0;
    if (seek.type == MPSEEK_FACTOR
        || seek.type == MPSEEK_ABSOLUTE
        && seek.amount < mpctx->last_chapter_pts
        || seek.amount < 0)
        mpctx->last_chapter_seek = -2;
    if (mpctx->timeline && seek.type == MPSEEK_FACTOR) {
        seek.amount *= mpctx->timeline[mpctx->num_timeline_parts].start;
        seek.type = MPSEEK_ABSOLUTE;
    }
    if ((mpctx->demuxer->accurate_seek || mpctx->timeline)
        && seek.type == MPSEEK_RELATIVE) {
        seek.type = MPSEEK_ABSOLUTE;
        seek.direction = seek.amount > 0 ? 1 : -1;
        seek.amount += get_current_time(mpctx);
    }

    /* At least the liba52 decoder wants to read from the input stream
     * during initialization, so reinit must be done after the demux_seek()
     * call that clears possible stream EOF. */
    bool need_reset = false;
    double demuxer_amount = seek.amount;
    if (mpctx->timeline) {
        demuxer_amount = timeline_set_from_time(mpctx, seek.amount,
                                                &need_reset);
        if (demuxer_amount == -1) {
            mpctx->stop_play = AT_END_OF_FILE;
            // Clear audio from current position
            if (mpctx->sh_audio && !timeline_fallthrough) {
                ao_reset(mpctx->ao);
                mpctx->sh_audio->a_buffer_len = 0;
            }
            return -1;
        }
    }
    if (need_reset) {
        reinit_video_chain(mpctx);
        mp_property_do("sub", M_PROPERTY_SET, &(int){mpctx->global_sub_pos},
                       mpctx);
    }

    int demuxer_style = 0;
    switch (seek.type) {
    case MPSEEK_FACTOR:
        demuxer_style |= SEEK_FACTOR; // fallthrough
    case MPSEEK_ABSOLUTE:
        demuxer_style |= SEEK_ABSOLUTE;
    }
    if (hr_seek || seek.direction < 0)
        demuxer_style |= SEEK_BACKWARD;
    else if (seek.direction > 0)
        demuxer_style |= SEEK_FORWARD;

    if (hr_seek)
        demuxer_amount -= opts->hr_seek_demuxer_offset;
    int seekresult = demux_seek(mpctx->demuxer, demuxer_amount, audio_delay,
                                demuxer_style);
    if (seekresult == 0) {
        if (need_reset) {
            reinit_audio_chain(mpctx);
            seek_reset(mpctx, !timeline_fallthrough, false);
        }
        return -1;
    }

    if (need_reset)
        reinit_audio_chain(mpctx);
    /* If we just reinitialized audio it doesn't need to be reset,
     * and resetting could lose audio some decoders produce during init. */
    seek_reset(mpctx, !timeline_fallthrough, !need_reset);

    /* Use the target time as "current position" for further relative
     * seeks etc until a new video frame has been decoded */
    if (seek.type == MPSEEK_ABSOLUTE) {
        mpctx->video_pts = seek.amount;
        mpctx->last_seek_pts = seek.amount;
    } else
        mpctx->last_seek_pts = MP_NOPTS_VALUE;

    if (hr_seek) {
        mpctx->hrseek_active = true;
        mpctx->hrseek_framedrop = true;
        mpctx->hrseek_pts = seek.amount;
    }

    mpctx->start_timestamp = GetTimerMS();

    return 0;
}

void queue_seek(struct MPContext *mpctx, enum seek_type type, double amount,
                int exact)
{
    struct seek_params *seek = &mpctx->seek;
    switch (type) {
    case MPSEEK_RELATIVE:
        if (seek->type == MPSEEK_FACTOR)
            return;  // Well... not common enough to bother doing better
        seek->amount += amount;
        seek->exact = FFMAX(seek->exact, exact);
        if (seek->type == MPSEEK_NONE)
            seek->exact = exact;
        if (seek->type == MPSEEK_ABSOLUTE)
            return;
        if (seek->amount == 0) {
            *seek = (struct seek_params){ 0 };
            return;
        }
        seek->type = MPSEEK_RELATIVE;
        return;
    case MPSEEK_ABSOLUTE:
    case MPSEEK_FACTOR:
        *seek = (struct seek_params) {
            .type = type,
            .amount = amount,
            .exact = exact,
        };
        return;
    case MPSEEK_NONE:
        *seek = (struct seek_params){ 0 };
        return;
    }
    abort();
}


double get_time_length(struct MPContext *mpctx)
{
    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer)
        return 0;

    if (mpctx->timeline)
        return mpctx->timeline[mpctx->num_timeline_parts].start;

    double get_time_ans;
    // <= 0 means DEMUXER_CTRL_NOTIMPL or DEMUXER_CTRL_DONTKNOW
    if (demux_control(demuxer, DEMUXER_CTRL_GET_TIME_LENGTH,
                      (void *) &get_time_ans) > 0)
        return get_time_ans;

    struct sh_video *sh_video = mpctx->d_video->sh;
    struct sh_audio *sh_audio = mpctx->d_audio->sh;
    if (sh_video && sh_video->i_bps && sh_audio && sh_audio->i_bps)
        return (double) (demuxer->movi_end - demuxer->movi_start) /
               (sh_video->i_bps + sh_audio->i_bps);
    if (sh_video && sh_video->i_bps)
        return (double) (demuxer->movi_end - demuxer->movi_start) /
               sh_video->i_bps;
    if (sh_audio && sh_audio->i_bps)
        return (double) (demuxer->movi_end - demuxer->movi_start) /
               sh_audio->i_bps;
    return 0;
}

/* If there are timestamps from stream level then use those (for example
 * DVDs can have consistent times there while the MPEG-level timestamps
 * reset). */
double get_current_time(struct MPContext *mpctx)
{
    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer)
        return 0;
    if (demuxer->stream_pts != MP_NOPTS_VALUE)
        return demuxer->stream_pts;
    if (mpctx->sh_video) {
        double pts = mpctx->video_pts;
        if (pts != MP_NOPTS_VALUE)
            return pts;
    }
    double apts = playing_audio_pts(mpctx);
    if (apts != MP_NOPTS_VALUE)
        return apts;
    return mpctx->last_seek_pts;
}

int get_percent_pos(struct MPContext *mpctx)
{
    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer)
        return 0;
    int ans = 0;
    if (mpctx->timeline)
        ans = get_current_time(mpctx) * 100 /
              mpctx->timeline[mpctx->num_timeline_parts].start;
    else if (demux_control(demuxer, DEMUXER_CTRL_GET_PERCENT_POS, &ans) > 0)
        ;
    else {
        int len = (demuxer->movi_end - demuxer->movi_start) / 100;
        off_t pos = demuxer->filepos > 0 ?
                    demuxer->filepos : stream_tell(demuxer->stream);
        if (len > 0)
            ans = (pos - demuxer->movi_start) / len;
        else
            ans = 0;
    }
    if (ans < 0)
        ans = 0;
    if (ans > 100)
        ans = 100;
    return ans;
}

// -2 is no chapters, -1 is before first chapter
int get_current_chapter(struct MPContext *mpctx)
{
    double current_pts = get_current_time(mpctx);
    if (mpctx->chapters) {
        int i;
        for (i = 1; i < mpctx->num_chapters; i++)
            if (current_pts < mpctx->chapters[i].start)
                break;
        return FFMAX(mpctx->last_chapter_seek, i - 1);
    }
    if (mpctx->demuxer)
        return FFMAX(mpctx->last_chapter_seek,
                     demuxer_get_current_chapter(mpctx->demuxer, current_pts));
    return -2;
}

char *chapter_display_name(struct MPContext *mpctx, int chapter)
{
    char *name = chapter_name(mpctx, chapter);
    char *dname = name;
    if (name) {
        dname = talloc_asprintf(NULL, "(%d) %s", chapter + 1, name);
    } else {
        int chapter_count = get_chapter_count(mpctx);
        if (chapter_count <= 0)
            dname = talloc_asprintf(NULL, "(%d)", chapter + 1);
        else
            dname = talloc_asprintf(NULL, "(%d) of %d", chapter + 1,
                                    chapter_count);
    }
    if (dname != name)
        talloc_free(name);
    return dname;
}

// returns NULL if chapter name unavailable
char *chapter_name(struct MPContext *mpctx, int chapter)
{
    if (mpctx->chapters)
        return talloc_strdup(NULL, mpctx->chapters[chapter].name);
    if (mpctx->demuxer)
        return demuxer_chapter_name(mpctx->demuxer, chapter);
    return NULL;
}

// returns the start of the chapter in seconds (-1 if unavailable)
double chapter_start_time(struct MPContext *mpctx, int chapter)
{
    if (mpctx->chapters)
        return mpctx->chapters[chapter].start;
    if (mpctx->demuxer)
        return demuxer_chapter_time(mpctx->demuxer, chapter, NULL);
    return -1;
}

int get_chapter_count(struct MPContext *mpctx)
{
    if (mpctx->chapters)
        return mpctx->num_chapters;
    if (mpctx->demuxer)
        return demuxer_chapter_count(mpctx->demuxer);
    return 0;
}

int seek_chapter(struct MPContext *mpctx, int chapter, double *seek_pts)
{
    mpctx->last_chapter_seek = -2;
    if (mpctx->chapters) {
        if (chapter >= mpctx->num_chapters)
            return -1;
        if (chapter < 0)
            chapter = 0;
        *seek_pts = mpctx->chapters[chapter].start;
        mpctx->last_chapter_seek = chapter;
        mpctx->last_chapter_pts = *seek_pts;
        return chapter;
    }
    if (mpctx->demuxer) {
        int res = demuxer_seek_chapter(mpctx->demuxer, chapter, seek_pts);
        if (res >= 0) {
            if (*seek_pts == -1)
                seek_reset(mpctx, true, true);
            else {
                mpctx->last_chapter_seek = res;
                mpctx->last_chapter_pts = *seek_pts;
            }
        }
        return res;
    }
    return -1;
}


static void run_playloop(struct MPContext *mpctx)
{
    struct MPOpts *opts = &mpctx->opts;
    bool full_audio_buffers = false;
    bool audio_left = false, video_left = false;
    double endpts = end_at.type == END_AT_TIME ? end_at.pos : MP_NOPTS_VALUE;
    bool end_is_chapter = false;
    double sleeptime = WAKEUP_PERIOD;
    bool was_restart = mpctx->restart_playback;

    if (mpctx->timeline) {
        double end = mpctx->timeline[mpctx->timeline_part + 1].start;
        if (endpts == MP_NOPTS_VALUE || end < endpts) {
            endpts = end;
            end_is_chapter = true;
        }
    }

    if (opts->chapterrange[1] > 0) {
        int cur_chapter = get_current_chapter(mpctx);
        if (cur_chapter != -1 && cur_chapter + 1 > opts->chapterrange[1])
            mpctx->stop_play = PT_NEXT_ENTRY;
    }

    if (!mpctx->sh_audio && mpctx->d_audio->sh) {
        mpctx->sh_audio = mpctx->d_audio->sh;
        mpctx->sh_audio->ds = mpctx->d_audio;
        reinit_audio_chain(mpctx);
    }

    if (mpctx->step_frames && !mpctx->sh_video) {
        mpctx->step_frames = 0;
        pause_player(mpctx);
    }

    if (mpctx->sh_audio && !mpctx->restart_playback && !mpctx->ao->untimed) {
        int status = fill_audio_out_buffers(mpctx, endpts);
        full_audio_buffers = status >= 0;
        // Not at audio stream EOF yet
        audio_left = status > -2;
    }

    double buffered_audio = -1;
    while (mpctx->sh_video) {   // never loops, for "break;" only
        struct vo *vo = mpctx->video_out;
        vo_pts = mpctx->sh_video->timer * 90000.0;
        vo_fps = mpctx->sh_video->fps;

        video_left = vo->hasframe || vo->frame_loaded;
        if (!vo->frame_loaded && (!mpctx->paused || mpctx->restart_playback)) {
            double frame_time = update_video(mpctx);
            mp_dbg(MSGT_AVSYNC, MSGL_DBG2, "*** ftime=%5.3f ***\n", frame_time);
            if (mpctx->sh_video->vf_initialized < 0) {
                mp_tmsg(MSGT_CPLAYER, MSGL_FATAL,
                        "\nFATAL: Could not initialize video filters (-vf) "
                        "or video output (-vo).\n");
                mpctx->stop_play = PT_NEXT_ENTRY;
                return;
            }
            video_left = frame_time >= 0;
            if (video_left && !mpctx->restart_playback) {
                mpctx->time_frame += frame_time / opts->playback_speed;
                adjust_sync(mpctx, frame_time);
            }
        }

        if (endpts != MP_NOPTS_VALUE)
            video_left &= mpctx->sh_video->pts < endpts;

        // ================================================================
        vo_check_events(vo);

#ifdef CONFIG_X11
        if (stop_xscreensaver && vo->x11) {
            xscreensaver_heartbeat(vo->x11);
        }
#endif
        if (heartbeat_cmd) {
            static unsigned last_heartbeat;
            unsigned now = GetTimerMS();
            if (now - last_heartbeat > 30000) {
                last_heartbeat = now;
                system(heartbeat_cmd);
            }
        }

        if (!video_left || (mpctx->paused && !mpctx->restart_playback))
            break;
        if (!vo->frame_loaded) {
            sleeptime = 0;
            break;
        }

        mpctx->time_frame -= get_relative_time(mpctx);
        if (full_audio_buffers && !mpctx->restart_playback) {
            buffered_audio = ao_get_delay(mpctx->ao);
            mp_dbg(MSGT_AVSYNC, MSGL_DBG2, "delay=%f\n", buffered_audio);

            if (opts->autosync) {
                /* Smooth reported playback position from AO by averaging
                 * it with the value expected based on previus value and
                 * time elapsed since then. May help smooth video timing
                 * with audio output that have inaccurate position reporting.
                 * This is badly implemented; the behavior of the smoothing
                 * now undesirably depends on how often this code runs
                 * (mainly depends on video frame rate). */
                float predicted = (mpctx->delay / opts->playback_speed +
                                   mpctx->time_frame);
                float difference = buffered_audio - predicted;
                buffered_audio = predicted + difference / opts->autosync;
            }

            mpctx->time_frame = (buffered_audio -
                                 mpctx->delay / opts->playback_speed);
        } else {
            /* If we're more than 200 ms behind the right playback
             * position, don't try to speed up display of following
             * frames to catch up; continue with default speed from
             * the current frame instead.
             * If untimed is set always output frames immediately
             * without sleeping.
             */
            if (mpctx->time_frame < -0.2 || opts->untimed)
                mpctx->time_frame = 0;
        }

        double vsleep = mpctx->time_frame - vo->flip_queue_offset;
        if (vsleep > 0.050) {
            sleeptime = FFMIN(sleeptime, vsleep - 0.040);
            break;
        }
        sleeptime = 0;

        //=================== FLIP PAGE (VIDEO BLT): ======================

        vo_new_frame_imminent(vo);
        struct sh_video *sh_video = mpctx->sh_video;
        mpctx->video_pts = sh_video->pts;
        update_subtitles(mpctx, sh_video->pts, false);
        update_osd_msg(mpctx);
        struct vf_instance *vf = sh_video->vfilter;
        mpctx->osd->pts = mpctx->video_pts - mpctx->osd->sub_offset;
        vf->control(vf, VFCTRL_DRAW_EOSD, mpctx->osd);
        vf->control(vf, VFCTRL_DRAW_OSD, mpctx->osd);
        vo_osd_reset_changed();

        mpctx->time_frame -= get_relative_time(mpctx);
        mpctx->time_frame -= vo->flip_queue_offset;
        if (mpctx->time_frame > 0.001
            && !(mpctx->sh_video->output_flags & VFCAP_TIMER))
            mpctx->time_frame = timing_sleep(mpctx, mpctx->time_frame);
        mpctx->time_frame += vo->flip_queue_offset;

        unsigned int t2 = GetTimer();
        /* Playing with playback speed it's possible to get pathological
         * cases with mpctx->time_frame negative enough to cause an
         * overflow in pts_us calculation, thus the FFMAX. */
        double time_frame = FFMAX(mpctx->time_frame, -1);
        unsigned int pts_us = mpctx->last_time + time_frame * 1e6;
        int duration = -1;
        double pts2 = vo->next_pts2;
        if (pts2 != MP_NOPTS_VALUE && opts->correct_pts &&
                !mpctx->restart_playback) {
            // expected A/V sync correction is ignored
            double diff = (pts2 - mpctx->video_pts);
            diff /= opts->playback_speed;
            if (mpctx->time_frame < 0)
                diff += mpctx->time_frame;
            if (diff < 0)
                diff = 0;
            if (diff > 10)
                diff = 10;
            duration = diff * 1e6;
        }
        vo_flip_page(vo, pts_us | 1, duration);

        mpctx->last_vo_flip_duration = (GetTimer() - t2) * 0.000001;
        if (vo->driver->flip_page_timed) {
            // No need to adjust sync based on flip speed
            mpctx->last_vo_flip_duration = 0;
            // For print_status - VO call finishing early is OK for sync
            mpctx->time_frame -= get_relative_time(mpctx);
        }
        if (mpctx->restart_playback) {
            mpctx->syncing_audio = true;
            if (mpctx->sh_audio)
                fill_audio_out_buffers(mpctx, endpts);
            mpctx->restart_playback = false;
            mpctx->time_frame = 0;
            get_relative_time(mpctx);
        }
        print_status(mpctx, MP_NOPTS_VALUE, true);
        screenshot_flip(mpctx);

        if (play_n_frames >= 0) {
            --play_n_frames;
            if (play_n_frames <= 0)
                mpctx->stop_play = PT_NEXT_ENTRY;
        }
        if (mpctx->step_frames > 0) {
            mpctx->step_frames--;
            if (mpctx->step_frames == 0)
                pause_player(mpctx);
        }
        break;
    } // video

    if (mpctx->sh_audio && (mpctx->restart_playback ? !video_left :
                            mpctx->ao->untimed && (mpctx->delay <= 0 ||
                                                   !video_left))) {
        int status = fill_audio_out_buffers(mpctx, endpts);
        full_audio_buffers = status >= 0 && !mpctx->ao->untimed;
        // Not at audio stream EOF yet
        audio_left = status > -2;
    }
    if (!video_left)
        mpctx->restart_playback = false;
    if (mpctx->sh_audio && buffered_audio == -1)
        buffered_audio = mpctx->paused ? 0 : ao_get_delay(mpctx->ao);

    update_osd_msg(mpctx);

#ifdef CONFIG_STREAM_CACHE
    // The cache status is part of the status line. Possibly update it.
    if (mpctx->paused && stream_cache_size > 0)
        print_status(mpctx, MP_NOPTS_VALUE, false);
#endif

    if (!video_left && (!mpctx->paused || was_restart)) {
        double a_pos = 0;
        if (mpctx->sh_audio) {
            a_pos = (written_audio_pts(mpctx) -
                     mpctx->opts.playback_speed * buffered_audio);
        }
        print_status(mpctx, a_pos, false);

        if (!mpctx->sh_video)
            update_subtitles(mpctx, a_pos, false);
    }

    /* It's possible for the user to simultaneously switch both audio
     * and video streams to "disabled" at runtime. Handle this by waiting
     * rather than immediately stopping playback due to EOF.
     *
     * When all audio has been written to output driver, stay in the
     * main loop handling commands until it has been mostly consumed,
     * except in the gapless case, where the next file will be started
     * while audio from the current one still remains to be played.
     *
     * We want this check to trigger if we seeked to this position,
     * but not if we paused at it with audio possibly still buffered in
     * the AO. There's currently no working way to check buffered audio
     * inside AO while paused. Thus the "was_restart" check below, which
     * should trigger after seek only, when we know there's no audio
     * buffered.
     */
    if ((mpctx->sh_audio || mpctx->sh_video) && !audio_left && !video_left
        && (opts->gapless_audio || buffered_audio < 0.05)
        && (!mpctx->paused || was_restart)) {
        if (end_is_chapter) {
            seek(mpctx, (struct seek_params){
                        .type = MPSEEK_ABSOLUTE,
                        .amount = mpctx->timeline[mpctx->timeline_part+1].start
                        }, true);
        } else
            mpctx->stop_play = AT_END_OF_FILE;
    } else if (!mpctx->stop_play) {
        double audio_sleep = 9;
        if (mpctx->sh_audio && !mpctx->paused) {
            if (mpctx->ao->untimed) {
                if (!video_left)
                    audio_sleep = 0;
            } else if (full_audio_buffers) {
                audio_sleep = buffered_audio - 0.050;
                // Keep extra safety margin if the buffers are large
                if (audio_sleep > 0.100)
                    audio_sleep = FFMAX(audio_sleep - 0.200, 0.100);
                else
                    audio_sleep = FFMAX(audio_sleep, 0.020);
            } else
                audio_sleep = 0.020;
        }
        sleeptime = FFMIN(sleeptime, audio_sleep);
        if (sleeptime > 0) {
            if (!mpctx->sh_video)
                goto novideo;
            if (vo_osd_has_changed(mpctx->osd) || mpctx->video_out->want_redraw)
            {
                if (redraw_osd(mpctx) < 0) {
                    if (mpctx->paused && video_left)
                        add_step_frame(mpctx);
                    else
                        goto novideo;
                }
            } else {
            novideo:
                mp_input_get_cmd(mpctx->input, sleeptime * 1000, true);
            }
        }
    }

    //================= Keyboard events, SEEKing ====================

    mp_cmd_t *cmd;
    while ((cmd = mp_input_get_cmd(mpctx->input, 0, 1)) != NULL) {
        /* Allow running consecutive seek commands to combine them,
         * but execute the seek before running other commands.
         * If the user seeks continuously (keeps arrow key down)
         * try to finish showing a frame from one location before doing
         * another seek (which could lead to unchanging display). */
        if (mpctx->seek.type && cmd->id != MP_CMD_SEEK
            || mpctx->restart_playback && cmd->id == MP_CMD_SEEK
            && GetTimerMS() - mpctx->start_timestamp < 300)
            break;
        cmd = mp_input_get_cmd(mpctx->input, 0, 0);
        run_command(mpctx, cmd);
        mp_cmd_free(cmd);
        if (mpctx->stop_play)
            break;
    }

    // handle -sstep
    if (step_sec > 0 && !mpctx->paused && !mpctx->restart_playback) {
        mpctx->osd_function = OSD_FFW;
        queue_seek(mpctx, MPSEEK_RELATIVE, step_sec, 0);
    }

    /* Looping. */
    if (opts->loop_times >= 0 && (mpctx->stop_play == AT_END_OF_FILE ||
                                  mpctx->stop_play == PT_NEXT_ENTRY)) {
        mp_msg(MSGT_CPLAYER, MSGL_V, "loop_times = %d\n", opts->loop_times);

        if (opts->loop_times > 1)
            opts->loop_times--;
        else if (opts->loop_times == 1)
            opts->loop_times = -1;
        play_n_frames = play_n_frames_mf;
        mpctx->stop_play = 0;
        queue_seek(mpctx, MPSEEK_ABSOLUTE, opts->seek_to_sec, 0);
    }

    if (mpctx->seek.type) {
        seek(mpctx, mpctx->seek, false);
        mpctx->seek = (struct seek_params){ 0 };
    }
}


static int read_keys(void *ctx, int fd)
{
    if (getch2(ctx))
        return MP_INPUT_NOTHING;
    return MP_INPUT_DEAD;
}

static bool attachment_is_font(struct demux_attachment *att)
{
    if (!att->name || !att->type || !att->data || !att->data_size)
        return false;
    // match against MIME types
    if (strcmp(att->type, "application/x-truetype-font") == 0
        || strcmp(att->type, "application/x-font") == 0)
        return true;
    // fallback: match against file extension
    if (strlen(att->name) > 4) {
        char *ext = att->name + strlen(att->name) - 4;
        if (strcasecmp(ext, ".ttf") == 0 || strcasecmp(ext, ".ttc") == 0
            || strcasecmp(ext, ".otf") == 0)
            return true;
    }
    return false;
}

static int select_audio(demuxer_t *demuxer, int audio_id, char **audio_lang)
{
    if (audio_id == -1)
        audio_id = demuxer_audio_track_by_lang_and_default(demuxer, audio_lang);
    if (audio_id != -1) // -1 (automatic) is the default behaviour of demuxers
        demuxer_switch_audio(demuxer, audio_id);
    if (audio_id == -2) { // some demuxers don't yet know how to switch to no sound
        demuxer->audio->id = -2;
        demuxer->audio->sh = NULL;
    }
    return demuxer->audio->id;
}

static void init_input(struct MPContext *mpctx)
{
    mpctx->input = mp_input_init(&mpctx->opts.input);
    mpctx->key_fifo = mp_fifo_create(mpctx->input, &mpctx->opts);
    if (slave_mode)
        mp_input_add_cmd_fd(mpctx->input, 0, USE_FD0_CMD_SELECT, MP_INPUT_SLAVE_CMD_FUNC, NULL);
    else if (mpctx->opts.consolecontrols)
        mp_input_add_key_fd(mpctx->input, 0, 1, read_keys, NULL, mpctx->key_fifo);
    // Set the libstream interrupt callback
    stream_set_interrupt_callback(mp_input_check_interrupt, mpctx->input);
}

static void open_vobsubs_from_options(struct MPContext *mpctx)
{
    if (mpctx->opts.vobsub_name) {
        vo_vobsub = vobsub_open(mpctx->opts.vobsub_name, spudec_ifo, 1, &vo_spudec);
        if (vo_vobsub == NULL)
            mp_tmsg(MSGT_CPLAYER, MSGL_ERR, "Cannot load subtitles: %s\n",
                    mpctx->opts.vobsub_name);
    } else if (mpctx->opts.sub_auto) {
        char **vob = find_vob_subtitles(&mpctx->opts, mpctx->filename);
        for (int i = 0; i < MP_TALLOC_ELEMS(vob); i++) {
            vo_vobsub = vobsub_open(vob[i], spudec_ifo, 0, &vo_spudec);
            if (vo_vobsub)
                break;
        }
        talloc_free(vob);
    }
    if (vo_vobsub) {
        mpctx->initialized_flags |= INITIALIZED_VOBSUB;
        vobsub_set_from_lang(vo_vobsub, mpctx->opts.sub_lang);
        mp_property_do("sub_forced_only", M_PROPERTY_SET, &forced_subs_only,
                       mpctx);

        // setup global sub numbering
        mpctx->sub_counts[SUB_SOURCE_VOBSUB] =
                vobsub_get_indexes_count(vo_vobsub);
    }
}

static void open_subtitles_from_options(struct MPContext *mpctx)
{
    // after reading video params we should load subtitles because
    // we know fps so now we can adjust subtitle time to ~6 seconds AST
    // check .sub
    double sub_fps = mpctx->sh_video ? mpctx->sh_video->fps : 25;
    if (mpctx->opts.sub_name) {
        for (int i = 0; mpctx->opts.sub_name[i] != NULL; ++i)
            add_subtitles(mpctx, mpctx->opts.sub_name[i], sub_fps, 0);
    }
    if (mpctx->opts.sub_auto) { // auto load sub file ...
        char **tmp = find_text_subtitles(&mpctx->opts, mpctx->filename);
        int nsub = MP_TALLOC_ELEMS(tmp);
        for (int i = 0; i < nsub; i++)
            add_subtitles(mpctx, tmp[i], sub_fps, 1);
        talloc_free(tmp);
    }
    if (mpctx->set_of_sub_size > 0)
        mpctx->sub_counts[SUB_SOURCE_SUBS] = mpctx->set_of_sub_size;
}

static void print_timeline(struct MPContext *mpctx)
{
    if (mpctx->timeline) {
        int part_count = mpctx->num_timeline_parts;
        mp_msg(MSGT_CPLAYER, MSGL_V, "Timeline contains %d parts from %d "
               "sources. Total length %.3f seconds.\n", part_count,
               mpctx->num_sources, mpctx->timeline[part_count].start);
        mp_msg(MSGT_CPLAYER, MSGL_V, "Source files:\n");
        for (int i = 0; i < mpctx->num_sources; i++)
            mp_msg(MSGT_CPLAYER, MSGL_V, "%d: %s\n", i,
                   mpctx->sources[i].demuxer->filename);
        mp_msg(MSGT_CPLAYER, MSGL_V, "Timeline parts: (number, start, "
               "source_start, source):\n");
        for (int i = 0; i < part_count; i++) {
            struct timeline_part *p = mpctx->timeline + i;
            mp_msg(MSGT_CPLAYER, MSGL_V, "%3d %9.3f %9.3f %3td\n", i, p->start,
                   p->source_start, p->source - mpctx->sources);
        }
        mp_msg(MSGT_CPLAYER, MSGL_V, "END %9.3f\n",
               mpctx->timeline[part_count].start);
    }
}

static void add_subtitle_fonts_from_sources(struct MPContext *mpctx)
{
#ifdef CONFIG_ASS
    if (mpctx->opts.ass_enabled && mpctx->ass_library) {
        for (int j = 0; j < mpctx->num_sources; j++) {
            struct demuxer *d = mpctx->sources[j].demuxer;
            for (int i = 0; i < d->num_attachments; i++) {
                struct demux_attachment *att = d->attachments + i;
                if (mpctx->opts.use_embedded_fonts && attachment_is_font(att))
                    ass_add_font(mpctx->ass_library, att->name, att->data,
                                 att->data_size);
            }
        }
    }
#endif
}

// Waiting for the slave master to send us a new file to play.
static void idle_loop(struct MPContext *mpctx)
{
    // ================= idle loop (STOP state) =========================
    while (mpctx->opts.player_idle_mode && !mpctx->playlist->current
           && mpctx->stop_play != PT_QUIT)
    {
        uninit_player(mpctx, INITIALIZED_AO | INITIALIZED_VO);
        mp_cmd_t *cmd;
        while (!(cmd = mp_input_get_cmd(mpctx->input, WAKEUP_PERIOD * 1000,
                                        false)));
        run_command(mpctx, cmd);
        mp_cmd_free(cmd);
    }
}

// Start playing the current playlist entry.
// Handle initialization and deinitialization.
static void play_current_file(struct MPContext *mpctx)
{
    struct MPOpts *opts = &mpctx->opts;

    mpctx->stop_play = 0;

    // init global sub numbers
    mpctx->global_sub_size = 0;
    memset(mpctx->sub_counts, 0, sizeof(mpctx->sub_counts));

    mpctx->filename = NULL;
    if (mpctx->playlist->current)
        mpctx->filename = mpctx->playlist->current->filename;

    if (!mpctx->filename)
        goto terminate_playback;

    m_config_enter_file_local(mpctx->mconfig);

    load_per_protocol_config(mpctx->mconfig, mpctx->filename);
    load_per_extension_config(mpctx->mconfig, mpctx->filename);
    load_per_file_config(mpctx->mconfig, mpctx->filename);

    if (opts->video_driver_list)
        load_per_output_config(mpctx->mconfig, PROFILE_CFG_VO,
                               opts->video_driver_list[0]);
    if (opts->audio_driver_list)
        load_per_output_config(mpctx->mconfig, PROFILE_CFG_AO,
                               opts->audio_driver_list[0]);

    assert(mpctx->playlist->current);
    load_per_file_options(mpctx->mconfig, mpctx->playlist->current->params,
                          mpctx->playlist->current->num_params);

    // We must enable getch2 here to be able to interrupt network connection
    // or cache filling
    if (opts->consolecontrols && !slave_mode) {
        if (mpctx->initialized_flags & INITIALIZED_GETCH2)
            mp_tmsg(MSGT_CPLAYER, MSGL_WARN,
                    "WARNING: getch2_init called twice!\n");
        else
            getch2_enable();  // prepare stdin for hotkeys...
        mpctx->initialized_flags |= INITIALIZED_GETCH2;
        mp_msg(MSGT_CPLAYER, MSGL_DBG2, "\n[[[init getch2]]]\n");
    }

#ifdef CONFIG_ASS
    ass_set_style_overrides(mpctx->ass_library, opts->ass_force_style_list);
#endif
    if (mpctx->video_out && mpctx->sh_video && mpctx->video_out->config_ok)
        vo_control(mpctx->video_out, VOCTRL_RESUME, NULL);

    mp_tmsg(MSGT_CPLAYER, MSGL_INFO, "Playing %s.\n", mpctx->filename);

    if (edl_output_filename) {
        if (edl_fd)
            fclose(edl_fd);
        if ((edl_fd = fopen(edl_output_filename, "w")) == NULL) {
            mp_tmsg(MSGT_CPLAYER, MSGL_ERR,
                    "Can't open EDL file [%s] for writing.\n",
                    edl_output_filename);
        }
    }

    open_vobsubs_from_options(mpctx);

    //============ Open & Sync STREAM --- fork cache2 ====================

    mpctx->stream = NULL;
    mpctx->demuxer = NULL;
    mpctx->d_audio = NULL;
    mpctx->d_video = NULL;
    mpctx->d_sub = NULL;
    mpctx->sh_audio = NULL;
    mpctx->sh_video = NULL;

    mpctx->stream = open_stream(mpctx->filename, opts, &mpctx->file_format);
    if (!mpctx->stream) { // error...
        libmpdemux_was_interrupted(mpctx);
        goto terminate_playback;
    }
    mpctx->initialized_flags |= INITIALIZED_STREAM;

    if (mpctx->file_format == DEMUXER_TYPE_PLAYLIST) {
        mp_msg(MSGT_CPLAYER, MSGL_ERR, "\nThis looks like a playlist, but "
               "playlist support will not be used automatically.\n"
               "MPlayer's playlist code is unsafe and should only be used with "
               "trusted sources.\nPlayback will probably fail.\n\n");
#if 0
        // Handle playlist
        mp_msg(MSGT_CPLAYER, MSGL_WARN, "Parsing playlist %s...\n",
               mpctx->filename);
        bool empty = true;
        struct playlist *pl = playlist_parse(mpctx->stream);
        if (pl) {
            empty = pl->first == NULL;
            playlist_transfer_entries(mpctx->playlist, pl);
            talloc_free(pl);
        }
        if (empty)
            mp_msg(MSGT_CPLAYER, MSGL_ERR, "Playlist was invalid or empty!\n");
        mpctx->stop_play = PT_NEXT_ENTRY;
        goto terminate_playback;
#endif
    }
    mpctx->stream->start_pos += seek_to_byte;

#ifdef CONFIG_DVDREAD
    if (mpctx->stream->type == STREAMTYPE_DVD) {
        if (opts->audio_lang && opts->audio_id == -1)
            opts->audio_id = dvd_aid_from_lang(mpctx->stream, opts->audio_lang);
        if (opts->sub_lang && opts->sub_id == -1)
            opts->sub_id = dvd_sid_from_lang(mpctx->stream, opts->sub_lang);
        // setup global sub numbering
        mpctx->sub_counts[SUB_SOURCE_DEMUX] = dvd_number_of_subs(mpctx->stream);
    }
#endif

    // CACHE2: initial prefill: 20%  later: 5%  (should be set by -cacheopts)
goto_enable_cache:
    if (stream_cache_size > 0) {
        int res;
        float stream_cache_min_percent = opts->stream_cache_min_percent;
        float stream_cache_seek_min_percent = opts->stream_cache_seek_min_percent;
        res = stream_enable_cache(mpctx->stream, stream_cache_size * 1024ull,
                                  stream_cache_size * 1024ull * (stream_cache_min_percent / 100.0),
                                  stream_cache_size * 1024ull * (stream_cache_seek_min_percent / 100.0));
        if (res == 0)
            if (libmpdemux_was_interrupted(mpctx))
                goto terminate_playback;
    }

    //============ Open DEMUXERS --- DETECT file type =======================

    mpctx->demuxer = demux_open(opts, mpctx->stream, mpctx->file_format,
                                opts->audio_id, opts->video_id, opts->sub_id,
                                mpctx->filename);

    if (!mpctx->demuxer) {
        mp_tmsg(MSGT_CPLAYER, MSGL_ERR, "Failed to recognize file format.\n");
        goto terminate_playback;
    }

    if (mpctx->demuxer->matroska_data.ordered_chapters)
        build_ordered_chapter_timeline(mpctx);

    if (mpctx->demuxer->type == DEMUXER_TYPE_EDL)
        build_edl_timeline(mpctx);

    if (mpctx->demuxer->type == DEMUXER_TYPE_CUE)
        build_cue_timeline(mpctx);

    if (mpctx->timeline) {
        mpctx->timeline_part = 0;
        mpctx->demuxer = mpctx->timeline[0].source->demuxer;
    }
    print_timeline(mpctx);

    if (!mpctx->sources) {
        mpctx->sources = talloc_ptrtype(NULL, mpctx->sources);
        *mpctx->sources = (struct content_source){
            .stream = mpctx->stream,
            .demuxer = mpctx->demuxer
        };
        mpctx->num_sources = 1;
    }

    mpctx->initialized_flags |= INITIALIZED_DEMUXER;

    add_subtitle_fonts_from_sources(mpctx);

    mpctx->d_audio = mpctx->demuxer->audio;
    mpctx->d_video = mpctx->demuxer->video;
    mpctx->d_sub = mpctx->demuxer->sub;

    // select audio stream
    for (int i = 0; i < mpctx->num_sources; i++)
        select_audio(mpctx->sources[i].demuxer->audio->demuxer, opts->audio_id,
                     opts->audio_lang);

    mpctx->sh_audio = mpctx->d_audio->sh;
    mpctx->sh_video = mpctx->d_video->sh;

    if (mpctx->sh_video) {
        if (!video_read_properties(mpctx->sh_video)) {
            mp_tmsg(MSGT_CPLAYER, MSGL_ERR, "Video: Cannot read properties.\n");
            mpctx->sh_video = mpctx->d_video->sh = NULL;
        } else {
            mp_tmsg(MSGT_CPLAYER, MSGL_V, "[V] filefmt:%d  fourcc:0x%X  "
                    "size:%dx%d  fps:%5.3f  ftime:=%6.4f\n",
                    mpctx->demuxer->file_format, mpctx->sh_video->format,
                    mpctx->sh_video->disp_w, mpctx->sh_video->disp_h,
                    mpctx->sh_video->fps, mpctx->sh_video->frametime);
            if (force_fps) {
                mpctx->sh_video->fps = force_fps;
                mpctx->sh_video->frametime = 1.0f / mpctx->sh_video->fps;
            }
            vo_fps = mpctx->sh_video->fps;

            if (!mpctx->sh_video->fps && !force_fps && !opts->correct_pts) {
                mp_tmsg(MSGT_CPLAYER, MSGL_ERR, "FPS not specified in the "
                        "header or invalid, use the -fps option.\n");
            }
        }

    }

    if (!mpctx->sh_video && !mpctx->sh_audio) {
        mp_tmsg(MSGT_CPLAYER, MSGL_FATAL, "No stream found.\n");
#ifdef CONFIG_DVBIN
        if (mpctx->stream->type == STREAMTYPE_DVB) {
            int dir;
            int v = mpctx->last_dvb_step;
            if (v > 0)
                dir = DVB_CHANNEL_HIGHER;
            else
                dir = DVB_CHANNEL_LOWER;

            if (dvb_step_channel(mpctx->stream, dir)) {
                mpctx->stop_play = PT_NEXT_ENTRY;
                mpctx->dvbin_reopen = 1;
            }
        }
#endif
        goto terminate_playback;
    }

    /* display clip info */
    demux_info_print(mpctx->demuxer);

    //================= Read SUBTITLES (DVD & TEXT) =========================
    if (vo_spudec == NULL && (mpctx->stream->type == STREAMTYPE_DVD))
        init_vo_spudec(mpctx);

    open_subtitles_from_options(mpctx);

    select_subtitle(mpctx);

    print_file_properties(mpctx, mpctx->filename);

    reinit_video_chain(mpctx);
    if (mpctx->sh_video) {
        osd_font_invalidate();
    } else if (!mpctx->sh_audio)
        goto terminate_playback;

    //================== MAIN: ==========================

    if (opts->playing_msg) {
        char *msg = property_expand_string(mpctx, opts->playing_msg);
        mp_msg(MSGT_CPLAYER, MSGL_INFO, "%s", msg);
        free(msg);
    }

    // Disable the term OSD in verbose mode
    if (verbose)
        opts->term_osd = 0;

    // Make sure old OSD does not stay around
    clear_osd_msgs(mpctx);

    //================ SETUP STREAMS ==========================

    if (mpctx->sh_audio) {
        reinit_audio_chain(mpctx);
        if (mpctx->sh_audio && mpctx->sh_audio->codec)
            mp_msg(MSGT_IDENTIFY, MSGL_INFO,
                   "ID_AUDIO_CODEC=%s\n", mpctx->sh_audio->codec->name);
    }

    if (mpctx->sh_video) {
        mpctx->sh_video->timer = 0;
        if (!ignore_start)
            audio_delay += mpctx->sh_video->stream_delay;
    }
    if (mpctx->sh_audio) {
        if (start_volume >= 0)
            mixer_setvolume(&mpctx->mixer, start_volume, start_volume);
        if (!ignore_start)
            audio_delay -= mpctx->sh_audio->stream_delay;
    }

    if (!mpctx->sh_audio) {
        mp_tmsg(MSGT_CPLAYER, MSGL_INFO, "Audio: no sound\n");
        mp_msg(MSGT_CPLAYER, MSGL_V, "Freeing %d unused audio chunks.\n",
               mpctx->d_audio->packs);
        ds_free_packs(mpctx->d_audio); // free buffered chunks
    }
    if (!mpctx->sh_video) {
        mp_tmsg(MSGT_CPLAYER, MSGL_INFO, "Video: no video\n");
        mp_msg(MSGT_CPLAYER, MSGL_V, "Freeing %d unused video chunks.\n",
               mpctx->d_video->packs);
        ds_free_packs(mpctx->d_video);
        mpctx->d_video->id = -2;
    }

    if (!mpctx->sh_video && !mpctx->sh_audio)
        goto terminate_playback;

    if (force_fps && mpctx->sh_video) {
        vo_fps = mpctx->sh_video->fps = force_fps;
        mpctx->sh_video->frametime = 1.0f / mpctx->sh_video->fps;
        mp_tmsg(MSGT_CPLAYER, MSGL_INFO,
                "FPS forced to be %5.3f  (ftime: %5.3f).\n",
                mpctx->sh_video->fps, mpctx->sh_video->frametime);
    }

    mp_input_set_section(mpctx->input, NULL, 0);
    //TODO: add desired (stream-based) sections here
    if (mpctx->stream->type == STREAMTYPE_TV)
        mp_input_set_section(mpctx->input, "tv", 0);

    //==================== START PLAYING =======================

    if (opts->loop_times > 1)
        opts->loop_times--;
    else if (opts->loop_times == 1)
        opts->loop_times = -1;

    mp_tmsg(MSGT_CPLAYER, MSGL_V, "Starting playback...\n");

    drop_frame_cnt = 0;          // fix for multifile fps benchmark
    play_n_frames = play_n_frames_mf;

    if (play_n_frames == 0) {
        mpctx->stop_play = PT_NEXT_ENTRY;
        goto terminate_playback;
    }

    mpctx->time_frame = 0;
    mpctx->drop_message_shown = 0;
    mpctx->restart_playback = true;
    mpctx->video_pts = 0;
    mpctx->last_seek_pts = 0;
    mpctx->hrseek_active = false;
    mpctx->hrseek_framedrop = false;
    mpctx->step_frames = 0;
    mpctx->total_avsync_change = 0;
    mpctx->last_chapter_seek = -2;

    // If there's a timeline force an absolute seek to initialize state
    if (opts->seek_to_sec || mpctx->timeline) {
        queue_seek(mpctx, MPSEEK_ABSOLUTE, opts->seek_to_sec, 0);
        seek(mpctx, mpctx->seek, false);
        end_at.pos += opts->seek_to_sec;
    }
    if (opts->chapterrange[0] > 0) {
        double pts;
        if (seek_chapter(mpctx, opts->chapterrange[0] - 1, &pts) >= 0
            && pts > -1.0) {
            queue_seek(mpctx, MPSEEK_ABSOLUTE, pts, 0);
            seek(mpctx, mpctx->seek, false);
        }
    }

    if (end_at.type == END_AT_SIZE) {
        mp_tmsg(MSGT_CPLAYER, MSGL_WARN,
                "Option -endpos in MPlayer does not yet support size units.\n");
        end_at.type = END_AT_NONE;
    }

    mpctx->seek = (struct seek_params){ 0 };
    get_relative_time(mpctx); // reset current delta
    // Make sure VO knows current pause state
    if (mpctx->sh_video)
        vo_control(mpctx->video_out,
                   mpctx->paused ? VOCTRL_PAUSE : VOCTRL_RESUME, NULL);

    if (mpctx->opts.start_paused)
        pause_player(mpctx);

    while (!mpctx->stop_play)
        run_playloop(mpctx);

    mp_msg(MSGT_GLOBAL, MSGL_V, "EOF code: %d  \n", mpctx->stop_play);

#ifdef CONFIG_DVBIN
    if (mpctx->dvbin_reopen) {
        mpctx->stop_play = 0;
        uninit_player(mpctx, INITIALIZED_ALL - (INITIALIZED_STREAM | INITIALIZED_GETCH2 | (opts->fixed_vo ? INITIALIZED_VO : 0)));
        cache_uninit(mpctx->stream);
        mpctx->dvbin_reopen = 0;
        goto goto_enable_cache;
    }
#endif

terminate_playback:  // don't jump here after ao/vo/getch initialization!

    mp_msg(MSGT_CPLAYER, MSGL_INFO, "\n");

    // xxx handle this as INITIALIZED_CONFIG?
    m_config_leave_file_local(mpctx->mconfig);

    // time to uninit all, except global stuff:
    int uninitialize_parts = INITIALIZED_ALL;
    if (opts->fixed_vo)
        uninitialize_parts -= INITIALIZED_VO;
    if (opts->gapless_audio && mpctx->stop_play == AT_END_OF_FILE)
        uninitialize_parts -= INITIALIZED_AO;
    uninit_player(mpctx, uninitialize_parts);

    mpctx->filename = NULL;

    if (mpctx->set_of_sub_size > 0) {
        for (int i = 0; i < mpctx->set_of_sub_size; ++i) {
            sub_free(mpctx->set_of_subtitles[i]);
#ifdef CONFIG_ASS
            if (mpctx->set_of_ass_tracks[i])
                ass_free_track(mpctx->set_of_ass_tracks[i]);
#endif
        }
        mpctx->set_of_sub_size = 0;
    }
    mpctx->vo_sub_last = vo_sub = NULL;
    mpctx->subdata = NULL;
#ifdef CONFIG_ASS
    mpctx->osd->ass_track = NULL;
    if (mpctx->ass_library)
        ass_clear_fonts(mpctx->ass_library);
#endif
}

// Play all entries on the playlist, starting from the current entry.
// Return if all done.
static void play_files(struct MPContext *mpctx)
{
    for (;;) {
        idle_loop(mpctx);
        if (mpctx->stop_play == PT_QUIT)
            break;

        play_current_file(mpctx);
        if (mpctx->stop_play == PT_QUIT)
            break;

        if (!mpctx->stop_play || mpctx->stop_play == AT_END_OF_FILE)
            mpctx->stop_play = PT_NEXT_ENTRY;

        struct playlist_entry *new_entry = NULL;

        if (mpctx->stop_play == PT_NEXT_ENTRY) {
            new_entry = playlist_get_next(mpctx->playlist, +1);
        } else if (mpctx->stop_play == PT_CURRENT_ENTRY) {
            new_entry = mpctx->playlist->current;
        } else { // PT_STOP
            playlist_clear(mpctx->playlist);
        }

        mpctx->playlist->current = new_entry;
        mpctx->playlist->current_was_replaced = false;
        mpctx->stop_play = 0;

        if (!mpctx->playlist->current && !mpctx->opts.player_idle_mode)
            break;
    }
}

static void print_version(int always)
{
    mp_msg(MSGT_CPLAYER, always ? MSGL_INFO : MSGL_V,
           "%s (C) 2000-2012\n", mplayer_version);
}

static bool handle_help_options(struct MPContext *mpctx)
{
    struct MPOpts *opts = &mpctx->opts;
    int opt_exit = 0;
    if (opts->video_driver_list &&
            strcmp(opts->video_driver_list[0], "help") == 0) {
        list_video_out();
        opt_exit = 1;
    }
    if (opts->audio_driver_list &&
            strcmp(opts->audio_driver_list[0], "help") == 0) {
        list_audio_out();
        opt_exit = 1;
    }
    if (audio_codec_list && strcmp(audio_codec_list[0], "help") == 0) {
        mp_tmsg(MSGT_CPLAYER, MSGL_INFO, "Available audio codecs:\n");
        mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_AUDIO_CODECS\n");
        list_codecs(1);
        mp_msg(MSGT_FIXME, MSGL_FIXME, "\n");
        opt_exit = 1;
    }
    if (video_codec_list && strcmp(video_codec_list[0], "help") == 0) {
        mp_tmsg(MSGT_CPLAYER, MSGL_INFO, "Available video codecs:\n");
        mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_VIDEO_CODECS\n");
        list_codecs(0);
        mp_msg(MSGT_FIXME, MSGL_FIXME, "\n");
        opt_exit = 1;
    }
    if (video_fm_list && strcmp(video_fm_list[0], "help") == 0) {
        vfm_help();
        mp_msg(MSGT_FIXME, MSGL_FIXME, "\n");
        opt_exit = 1;
    }
    if (audio_fm_list && strcmp(audio_fm_list[0], "help") == 0) {
        afm_help();
        mp_msg(MSGT_FIXME, MSGL_FIXME, "\n");
        opt_exit = 1;
    }
    if (af_cfg.list && strcmp(af_cfg.list[0], "help") == 0) {
        af_help();
        printf("\n");
        opt_exit = 1;
    }
#ifdef CONFIG_X11
    if (vo_fstype_list && strcmp(vo_fstype_list[0], "help") == 0) {
        fstype_help();
        mp_msg(MSGT_FIXME, MSGL_FIXME, "\n");
        opt_exit = 1;
    }
#endif
    if ((opts->demuxer_name && strcmp(opts->demuxer_name, "help") == 0) ||
        (opts->audio_demuxer_name && strcmp(opts->audio_demuxer_name, "help") == 0) ||
        (opts->sub_demuxer_name && strcmp(opts->sub_demuxer_name, "help") == 0)) {
        demuxer_help();
        mp_msg(MSGT_CPLAYER, MSGL_INFO, "\n");
        opt_exit = 1;
    }
    if (opts->list_properties) {
        property_print_help();
        opt_exit = 1;
    }
    return opt_exit;
}

static bool load_codecs_conf(struct MPContext *mpctx)
{
    /* Check codecs.conf. */
    if (!codecs_file || !parse_codec_cfg(codecs_file)) {
        char *mem_ptr;
        if (!parse_codec_cfg(mem_ptr = get_path("codecs.conf"))) {
            if (!parse_codec_cfg(MPLAYER_CONFDIR "/codecs.conf")) {
                if (!parse_codec_cfg(NULL))
                    return false;
                mp_tmsg(MSGT_CPLAYER, MSGL_V,
                        "Using built-in default codecs.conf.\n");
            }
        }
        free(mem_ptr); // release the buffer created by get_path()
    }
    return true;
}

#ifdef PTW32_STATIC_LIB
static void detach_ptw32(void)
{
    pthread_win32_thread_detach_np();
    pthread_win32_process_detach_np();
}
#endif

static void osdep_preinit(int *p_argc, char ***p_argv)
{
    char *enable_talloc = getenv("MPLAYER_LEAK_REPORT");
    if (*p_argc > 1 && (strcmp((*p_argv)[1], "-leak-report") == 0
                     || strcmp((*p_argv)[1], "--leak-report") == 0))
        enable_talloc = "1";
    if (enable_talloc && strcmp(enable_talloc, "1") == 0)
        talloc_enable_leak_report();

    GetCpuCaps(&gCpuCaps);

#ifdef __MINGW32__
    mp_get_converted_argv(p_argc, p_argv);
#endif

#ifdef PTW32_STATIC_LIB
    pthread_win32_process_attach_np();
    pthread_win32_thread_attach_np();
    atexit(detach_ptw32);
#endif

    InitTimer();
    srand(GetTimerMS());

#if defined(__MINGW32__) || defined(__CYGWIN__)
    // stop Windows from showing all kinds of annoying error dialogs
    SetErrorMode(0x8003);
    // request 1ms timer resolution
    timeBeginPeriod(1);
#endif

#ifdef HAVE_TERMCAP
    load_termcap(NULL); // load key-codes
#endif
}

/* This preprocessor directive is a hack to generate a mplayer-nomain.o object
 * file for some tools to link against. */
#ifndef DISABLE_MAIN
int main(int argc, char *argv[])
{
    osdep_preinit(&argc, &argv);

    if (argc >= 1) {
        argc--;
        argv++;
    }

    struct MPContext *mpctx = talloc(NULL, MPContext);
    *mpctx = (struct MPContext){
        .osd_function = OSD_PLAY,
        .begin_skip = MP_NOPTS_VALUE,
        .global_sub_pos = -1,
        .set_of_sub_pos = -1,
        .file_format = DEMUXER_TYPE_UNKNOWN,
        .last_dvb_step = 1,
        .terminal_osd_text = talloc_strdup(mpctx, ""),
        .playlist = talloc_struct(mpctx, struct playlist, {0}),
    };

    mp_msg_init();
    init_libav();
    screenshot_init(mpctx);

    struct MPOpts *opts = &mpctx->opts;
    set_default_mplayer_options(opts);
    // Create the config context and register the options
    mpctx->mconfig = m_config_new(opts, cfg_include);
    m_config_register_options(mpctx->mconfig, mplayer_opts);
    m_config_register_options(mpctx->mconfig, common_opts);
    mp_input_register_options(mpctx->mconfig);

    // Preparse the command line
    m_config_preparse_command_line(mpctx->mconfig, argc, argv);

    print_version(false);
    print_libav_versions();

    if (!parse_cfgfiles(mpctx, mpctx->mconfig))
        exit_player(mpctx, EXIT_NONE, 1);

    if (!m_config_parse_mp_command_line(mpctx->mconfig, mpctx->playlist,
                                        argc, argv))
    {
        exit_player(mpctx, EXIT_ERROR, 1);
    }

    if (!load_codecs_conf(mpctx))
        exit_player(mpctx, EXIT_ERROR, 1);

    if (handle_help_options(mpctx))
        exit_player(mpctx, EXIT_NONE, 1);

    mp_msg(MSGT_CPLAYER, MSGL_V, "Configuration: " CONFIGURATION "\n");
    mp_tmsg(MSGT_CPLAYER, MSGL_V, "Command line:");
    for (int i = 0; i < argc; i++)
        mp_msg(MSGT_CPLAYER, MSGL_V, " '%s'", argv[i]);
    mp_msg(MSGT_CPLAYER, MSGL_V, "\n");

    if (!mpctx->playlist->first && !opts->player_idle_mode) {
        print_version(true);
        mp_msg(MSGT_CPLAYER, MSGL_INFO, "%s", mp_gtext(help_text));
        exit_player(mpctx, EXIT_NONE, 0);
    }

#ifdef CONFIG_PRIORITY
    set_priority();
#endif

#ifdef CONFIG_ASS
    mpctx->ass_library = mp_ass_init(opts);
#endif

    mpctx->osd = osd_create(opts, mpctx->ass_library);

    init_input(mpctx);

    mpctx->playlist->current = mpctx->playlist->first;
    play_files(mpctx);

    exit_player(mpctx, EXIT_EOF, mpctx->quit_player_rc);

    return 1;
}
#endif /* DISABLE_MAIN */
