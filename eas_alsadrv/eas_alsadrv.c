/**
 *
 *  Copyright (C) 2022-2025 Roman Pauer
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy of
 *  this software and associated documentation files (the "Software"), to deal in
 *  the Software without restriction, including without limitation the rights to
 *  use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 *  of the Software, and to permit persons to whom the Software is furnished to do
 *  so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 */

#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <pwd.h>
#include <alsa/asoundlib.h>
#include <eas.h>
#include <eas_reverb.h>
#include <eas_chorus.h>


typedef struct {
    uint8_t *dls_address;
    int dls_size;
} dls_file_handle_t;


static const char midi_name[] = "Sonivox EAS";
static const char port_name[] = "Sonivox EAS port";

static snd_seq_t *midi_seq;
static int midi_port_id;
static pthread_t midi_thread;
static snd_pcm_t *midi_pcm;
static volatile int midi_init_state;
static volatile int midi_event_written;

static int polyphony, master_volume, daemonize;
static int reverb_preset, reverb_wet;
static int chorus_preset, chorus_rate, chorus_depth, chorus_level;
static const char *dls_filepath;

static EAS_DATA_HANDLE data_handle;
static EAS_HANDLE stream_handle;

static unsigned int frequency, num_channels, bytes_per_call, samples_per_call, num_subbuffers, subbuf_counter;
static uint8_t midi_buffer[65536];

static uint8_t event_buffer[65536];
static volatile int event_read_index;
static volatile int event_write_index;


static void set_thread_scheduler(void) __attribute__((noinline));
static void set_thread_scheduler(void)
{
    struct sched_param param;

    memset(&param, 0, sizeof(struct sched_param));
    param.sched_priority = sched_get_priority_min(SCHED_FIFO);
    if (param.sched_priority > 0)
    {
        sched_setscheduler(0, SCHED_FIFO, &param);
    }
}

static void wait_for_midi_initialization(void) __attribute__((noinline));
static void wait_for_midi_initialization(void)
{
    while (midi_init_state == 0)
    {
        struct timespec req;

        req.tv_sec = 0;
        req.tv_nsec = 10000000;
        nanosleep(&req, NULL);
    };
}

static void subscription_event(snd_seq_event_t *event) __attribute__((noinline));
static void subscription_event(snd_seq_event_t *event)
{
    snd_seq_client_info_t *cinfo;
    int err;

    snd_seq_client_info_alloca(&cinfo);
    err = snd_seq_get_any_client_info(midi_seq, event->data.connect.sender.client, cinfo);
    if (err >= 0)
    {
        if (event->type == SND_SEQ_EVENT_PORT_SUBSCRIBED)
        {
            printf("Client subscribed: %s\n", snd_seq_client_info_get_name(cinfo));
        }
        else
        {
            printf("Client unsubscribed: %s\n", snd_seq_client_info_get_name(cinfo));
        }
    }
    else
    {
        printf("Client unsubscribed\n");
    }
}

static void write_event(const uint8_t *event, unsigned int length)
{
    int read_index, write_index;
    unsigned int free_space;

    // read global volatile variables to local variables
    read_index = event_read_index;
    write_index = event_write_index;

    if (write_index >= read_index)
    {
        free_space = 65535 - (write_index - read_index);
    }
    else
    {
        free_space = (read_index - write_index) - 1;
    }

    if (length > free_space)
    {
        fprintf(stderr, "Event buffer overflow\n");
        return;
    }

    // write event to event buffer
    for (; length != 0; length--,event++)
    {
        event_buffer[write_index] = *event;
        write_index = (write_index + 1) & 0xffff;
    }

    // update global volatile variable
    event_write_index = write_index;

    midi_event_written = 1;
}

static void process_event(snd_seq_event_t *event, uint8_t *running_status)
{
    uint8_t data[12];
    int length;

    switch (event->type)
    {
        case SND_SEQ_EVENT_NOTEON:
            data[0] = 0x90 | event->data.note.channel;
            data[1] = event->data.note.note;
            data[2] = event->data.note.velocity;
            length = 3;

            if (data[0] != *running_status)
            {
                *running_status = data[0];
                write_event(data, length);
            }
            else
            {
                write_event(data + 1, length - 1);
            }

#ifdef PRINT_EVENTS
            printf("Note ON, channel:%d note:%d velocity:%d\n", event->data.note.channel, event->data.note.note, event->data.note.velocity);
#endif

            break;

        case SND_SEQ_EVENT_NOTEOFF:
            // send note off event as note on with zero velocity to increase the chance of using running status
            data[0] = 0x90 | event->data.note.channel;
            data[1] = event->data.note.note;
            data[2] = 0;
            length = 3;

            if (data[0] != *running_status)
            {
                *running_status = data[0];
                write_event(data, length);
            }
            else
            {
                write_event(data + 1, length - 1);
            }

#ifdef PRINT_EVENTS
            printf("Note OFF, channel:%d note:%d velocity:%d\n", event->data.note.channel, event->data.note.note, event->data.note.velocity);
#endif

            break;

        case SND_SEQ_EVENT_KEYPRESS:
            // Not used by Sonivox EAS
#if 0
            data[0] = 0xA0 | event->data.note.channel;
            data[1] = event->data.note.note;
            data[2] = event->data.note.velocity;
            length = 3;

            if (data[0] != *running_status)
            {
                *running_status = data[0];
                write_event(data, length);
            }
            else
            {
                write_event(data + 1, length - 1);
            }
#endif

#ifdef PRINT_EVENTS
            printf("Keypress, channel:%d note:%d velocity:%d\n", event->data.note.channel, event->data.note.note, event->data.note.velocity);
#endif

            break;

        case SND_SEQ_EVENT_CONTROLLER:
            data[0] = 0xB0 | event->data.control.channel;
            data[1] = event->data.control.param;
            data[2] = event->data.control.value;
            length = 3;

            if (data[0] != *running_status)
            {
                *running_status = data[0];
                write_event(data, length);
            }
            else
            {
                write_event(data + 1, length - 1);
            }

#ifdef PRINT_EVENTS
            printf("Controller, channel:%d param:%d value:%d\n", event->data.control.channel, event->data.control.param, event->data.control.value);
#endif

            break;

        case SND_SEQ_EVENT_PGMCHANGE:
            data[0] = 0xC0 | event->data.control.channel;
            data[1] = event->data.control.value;
            length = 2;

            if (data[0] != *running_status)
            {
                *running_status = data[0];
                write_event(data, length);
            }
            else
            {
                write_event(data + 1, length - 1);
            }

#ifdef PRINT_EVENTS
            printf("Program change, channel:%d value:%d\n", event->data.control.channel, event->data.control.value);
#endif

            break;

        case SND_SEQ_EVENT_CHANPRESS:
            data[0] = 0xD0 | event->data.control.channel;
            data[1] = event->data.control.value;
            length = 2;

            if (data[0] != *running_status)
            {
                *running_status = data[0];
                write_event(data, length);
            }
            else
            {
                write_event(data + 1, length - 1);
            }

#ifdef PRINT_EVENTS
            printf("Channel pressure, channel:%d value:%d\n", event->data.control.channel, event->data.control.value);
#endif

            break;

        case SND_SEQ_EVENT_PITCHBEND:
            data[0] = 0xE0 | event->data.control.channel;
            data[1] = (event->data.control.value + 0x2000) & 0x7f;
            data[2] = ((event->data.control.value + 0x2000) >> 7) & 0x7f;
            length = 3;

            if (data[0] != *running_status)
            {
                *running_status = data[0];
                write_event(data, length);
            }
            else
            {
                write_event(data + 1, length - 1);
            }

#ifdef PRINT_EVENTS
            printf("Pitch bend, channel:%d value:%d\n", event->data.control.channel, event->data.control.value);
#endif

            break;

        case SND_SEQ_EVENT_CONTROL14:
            if (event->data.control.param >= 0 && event->data.control.param < 32)
            {
                data[0] = 0xB0 | event->data.control.channel;
                data[1] = event->data.control.param;
                data[2] = (event->data.control.value >> 7) & 0x7f;
                data[3] = event->data.control.param + 32;
                data[4] = event->data.control.value & 0x7f;
                length = 5;

                if (data[0] != *running_status)
                {
                    *running_status = data[0];
                    write_event(data, length);
                }
                else
                {
                    write_event(data + 1, length - 1);
                }

#ifdef PRINT_EVENTS
                printf("Controller 14-bit, channel:%d param:%d value:%d\n", event->data.control.channel, event->data.control.param, event->data.control.value);
#endif
            }
            else
            {
#ifdef PRINT_EVENTS
                printf("Unknown controller, channel:%d param:%d value:%d\n", event->data.control.channel, event->data.control.param, event->data.control.value);
#endif
            }


            break;

        case SND_SEQ_EVENT_NONREGPARAM:
            // Not used by Sonivox EAS
#if 0
            data[0] = 0xB0 | event->data.control.channel;
            data[1] = 0x63; // NRPN MSB
            data[2] = (event->data.control.param >> 7) & 0x7f;
            data[3] = 0x62; // NRPN LSB
            data[4] = event->data.control.param & 0x7f;
            data[5] = 0x06; // data entry MSB
            data[6] = (event->data.control.value >> 7) & 0x7f;
            data[7] = 0x26; // data entry LSB
            data[8] = event->data.control.value & 0x7f;
            length = 9;

            if (data[0] != *running_status)
            {
                *running_status = data[0];
                write_event(data, length);
            }
            else
            {
                write_event(data + 1, length - 1);
            }
#endif

#ifdef PRINT_EVENTS
            printf("NRPN, channel:%d param:%d value:%d\n", event->data.control.channel, event->data.control.param, event->data.control.value);
#endif

            break;

        case SND_SEQ_EVENT_REGPARAM:
            data[0] = 0xB0 | event->data.control.channel;
            data[1] = 0x65; // RPN MSB
            data[2] = (event->data.control.param >> 7) & 0x7f;
            data[3] = 0x64; // RPN LSB
            data[4] = event->data.control.param & 0x7f;
            data[5] = 0x06; // data entry MSB
            data[6] = (event->data.control.value >> 7) & 0x7f;
            data[7] = 0x26; // data entry LSB
            data[8] = event->data.control.value & 0x7f;
            length = 9;

            if (data[0] != *running_status)
            {
                *running_status = data[0];
                write_event(data, length);
            }
            else
            {
                write_event(data + 1, length - 1);
            }

#ifdef PRINT_EVENTS
            printf("RPN, channel:%d param:%d value:%d\n", event->data.control.channel, event->data.control.param, event->data.control.value);
#endif

            break;

        case SND_SEQ_EVENT_SYSEX:
            length = event->data.ext.len;

            *running_status = 0;
            write_event(event->data.ext.ptr, length);

#ifdef PRINT_EVENTS
            printf("SysEx (fragment) of size %d\n", event->data.ext.len);
#endif

            break;

        case SND_SEQ_EVENT_QFRAME:
            // Not used by Sonivox EAS
#if 0
            data[0] = 0xF1;
            data[1] = ev->data.control.value;
            length = 2;

            *running_status = 0;
            write_event(data, length);
#endif

#ifdef PRINT_EVENTS
            printf("MTC Quarter Frame, value:%d\n", event->data.control.value);
#endif

            break;

        case SND_SEQ_EVENT_SONGPOS:
            // Not used by Sonivox EAS
#if 0
            data[0] = 0xF2;
            data[1] = (event->data.control.value + 0x2000) & 0x7f;
            data[2] = ((event->data.control.value + 0x2000) >> 7) & 0x7f;
            length = 3;

            *running_status = 0;
            write_event(data, length);
#endif

#ifdef PRINT_EVENTS
            printf("Song Position, value:%d\n", event->data.control.value);
#endif

            break;

        case SND_SEQ_EVENT_SONGSEL:
            // Not used by Sonivox EAS
#if 0
            data[0] = 0xF3;
            data[1] = ev->data.control.value;
            length = 2;

            *running_status = 0;
            write_event(data, length);
#endif

#ifdef PRINT_EVENTS
            printf("Song Select, value:%d\n", event->data.control.value);
#endif

            break;

        case SND_SEQ_EVENT_TUNE_REQUEST:
            // Not used by Sonivox EAS
#if 0
            data[0] = 0xF6;
            length = 1;

            *running_status = 0;
            write_event(data, length);
#endif

#ifdef PRINT_EVENTS
            printf("Tune Request\n");
#endif

            break;

        case SND_SEQ_EVENT_CLOCK:
            // Not used by Sonivox EAS
#if 0
            data[0] = 0xF8;
            length = 1;

            write_event(data, length);
#endif

#ifdef PRINT_EVENTS
            printf("Clock\n");
#endif

            break;

        case SND_SEQ_EVENT_TICK:
            // Not used by Sonivox EAS
#if 0
            data[0] = 0xF9;
            length = 1;

            write_event(data, length);
#endif

#ifdef PRINT_EVENTS
            printf("Tick\n");
#endif

            break;

        case SND_SEQ_EVENT_START:
            // Not used by Sonivox EAS
#if 0
            data[0] = 0xFA;
            length = 1;

            write_event(data, length);
#endif

#ifdef PRINT_EVENTS
            printf("Start\n");
#endif

            break;

        case SND_SEQ_EVENT_CONTINUE:
            // Not used by Sonivox EAS
#if 0
            data[0] = 0xFB;
            length = 1;

            write_event(data, length);
#endif

#ifdef PRINT_EVENTS
            printf("Continue\n");
#endif

            break;

        case SND_SEQ_EVENT_STOP:
            // Not used by Sonivox EAS
#if 0
            data[0] = 0xFC;
            length = 1;

            write_event(data, length);
#endif

#ifdef PRINT_EVENTS
            printf("Stop\n");
#endif

            break;

        case SND_SEQ_EVENT_SENSING:
            // Not used by Sonivox EAS
#if 0
            data[0] = 0xFE;
            length = 1;

            write_event(data, length);
#endif

#ifdef PRINT_EVENTS
            printf("Active Sense\n");
#endif

            break;

        case SND_SEQ_EVENT_RESET:
            // Not used by Sonivox EAS
#if 0
            data[0] = 0xFF;
            length = 1;

            write_event(data, length);
#endif

#ifdef PRINT_EVENTS
            printf("Reset\n");
#endif

            break;

        case SND_SEQ_EVENT_PORT_SUBSCRIBED:
            subscription_event(event);
            break;

        case SND_SEQ_EVENT_PORT_UNSUBSCRIBED:
            subscription_event(event);
            break;

        default:
            fprintf(stderr, "Unhandled event type: %i\n", event->type);
            break;
    }
}

static void *midi_thread_proc(void *arg)
{
    snd_seq_event_t *event;
    uint8_t running_status;

    // try setting thread scheduler (only root)
    set_thread_scheduler();

    // set thread as initialized
    *(int *)arg = 1;

    wait_for_midi_initialization();

    running_status = 0;

    while (midi_init_state > 0)
    {
        if (snd_seq_event_input(midi_seq, &event) < 0)
        {
            continue;
        }

        process_event(event, &running_status);
    }

    return NULL;
}

static void usage(const char *progname)
{
    static const char basename[] = "eas_alsadrv";

    if (progname == NULL)
    {
        progname = basename;
    }
    else
    {
        const char *slash;

        slash = strrchr(progname, '/');
        if (slash != NULL)
        {
            progname = slash + 1;
        }
    }

    printf(
        "%s - Sonivox EAS\n"
        "Usage: %s [OPTIONS]...\n"
        "  -p NUM   Polyphony\n"
        "  -m NUM   Master volume (0-100)\n"
        "  -s PATH  Dls soundfont path (path to .dls file)\n"
        "  -r NUM   Reverb preset (0=off, 1=large hall, 2=hall, 3=chamber, 4=room)\n"
        "  -w NUM   Reverb wet (0-32767)\n"
        "  -c NUM   Chorus preset (0=off, 1=preset 1, 2=preset 2, 3=preset 3, 4=preset 4)\n"
        "  -a NUM   Chorus rate (10-50)\n"
        "  -e NUM   Chorus depth (15-60)\n"
        "  -l NUM   Chorus level (0-32767)\n"
        "  -d       Daemonize\n"
        "  -h       Help\n",
        basename,
        progname
    );
    exit(1);
}

static void read_arguments(int argc, char *argv[]) __attribute__((noinline));
static void read_arguments(int argc, char *argv[])
{
    int i, j;

    polyphony = 0;
    master_volume = -1;
    daemonize = 0;
    reverb_preset = 0;
    reverb_wet = -1;
    chorus_preset = 0;
    chorus_rate = -1;
    chorus_depth = -1;
    chorus_level = -1;
    dls_filepath = NULL;

    if (argc <= 1)
    {
        return;
    }

    for (i = 1; i < argc; i++)
    {
        if (argv[i][0] == '-' && argv[i][1] != 0 && argv[i][2] == 0)
        {
            switch (argv[i][1])
            {
                case 'p': // polyphony
                    if ((i + 1) < argc)
                    {
                        i++;
                        j = atoi(argv[i]);
                        if (j >= 0)
                        {
                            polyphony = j;
                        }
                    }
                    break;
                case 'm': // master volume
                    if ((i + 1) < argc)
                    {
                        i++;
                        j = atoi(argv[i]);
                        if (j >= 0 && j <= 100)
                        {
                            master_volume = j;
                        }
                    }
                    break;
                case 's': // dls soundfont path
                    if ((i + 1) < argc)
                    {
                        i++;
                        dls_filepath = argv[i];
                    }
                    break;
                case 'r': // reverb preset
                    if ((i + 1) < argc)
                    {
                        i++;
                        j = atoi(argv[i]);
                        if (j >= 0 && j <= 4)
                        {
                            reverb_preset = j;
                        }
                    }
                    break;
                case 'w': // reverb wet
                    if ((i + 1) < argc)
                    {
                        i++;
                        j = atoi(argv[i]);
                        if (j >= 0 && j <= 32767)
                        {
                            reverb_wet = j;
                        }
                    }
                    break;
                case 'c': // chorus preset
                    if ((i + 1) < argc)
                    {
                        i++;
                        j = atoi(argv[i]);
                        if (j >= 0 && j <= 4)
                        {
                            chorus_preset = j;
                        }
                    }
                    break;
                case 'a': // chorus rate
                    if ((i + 1) < argc)
                    {
                        i++;
                        j = atoi(argv[i]);
                        if (j >= 10 && j <= 50)
                        {
                            chorus_rate = j;
                        }
                    }
                    break;
                case 'e': // chorus depth
                    if ((i + 1) < argc)
                    {
                        i++;
                        j = atoi(argv[i]);
                        if (j >= 15 && j <= 60)
                        {
                            chorus_depth = j;
                        }
                    }
                    break;
                case 'l': // chorus level
                    if ((i + 1) < argc)
                    {
                        i++;
                        j = atoi(argv[i]);
                        if (j >= 0 && j <= 32767)
                        {
                            chorus_level = j;
                        }
                    }
                    break;
                case 'd': // daemonize
                    daemonize = 1;
                    break;
                case 'h': // help
                    usage(argv[0]);
                default:
                    break;
            }
        }
        else if (strcmp(argv[i], "--help") == 0)
        {
            usage(argv[0]);
        }
    }
}


static int dls_read_at(void *handle, void *buf, int offset, int size)
{
    int dls_size;

    dls_size = ((dls_file_handle_t *)handle)->dls_size;
    if ((offset < 0) || (offset >= dls_size)) return 0;
    if (size > dls_size - offset) size = dls_size - offset;

    memcpy(buf, offset + ((dls_file_handle_t *)handle)->dls_address, size);

    return size;
}

static int dls_get_size(void *handle)
{
    return ((dls_file_handle_t *)handle)->dls_size;
}

static int load_dls_file(void)
{
    int dls_fd;
    struct stat statbuf;
    dls_file_handle_t dls_file_handle;
    EAS_FILE eas_file;
    EAS_RESULT res;

    dls_fd = open(dls_filepath, O_RDONLY);
    if (dls_fd < 0)
    {
        char *dlspathcopy, *slash, *filename;
        DIR *dir;
        struct dirent *entry;

        dlspathcopy = strdup(dls_filepath);
        if (dlspathcopy == NULL) return -1;

        slash = strrchr(dlspathcopy, '/');
        if (slash != NULL)
        {
            filename = slash + 1;
            if (slash != dlspathcopy)
            {
                *slash = 0;
                dir = opendir(dlspathcopy);
                *slash = '/';
            }
            else
            {
                dir = opendir("/");
            }
        }
        else
        {
            filename = dlspathcopy;
            dir = opendir(".");
        }

        if (dir == NULL)
        {
            free(dlspathcopy);
            return -2;
        }

        while (1)
        {
            entry = readdir(dir);
            if (entry == NULL) break;

            if (entry->d_type != DT_UNKNOWN && entry->d_type != DT_REG && entry->d_type != DT_LNK) continue;

            if (0 != strcasecmp(filename, entry->d_name)) continue;

            strcpy(filename, entry->d_name);
            break;
        };

        closedir(dir);

        if (entry == NULL)
        {
            free(dlspathcopy);
            return -3;
        }

        dls_fd = open(dlspathcopy, O_RDONLY);

        free(dlspathcopy);

        if (dls_fd < 0)
        {
            return -4;
        }
    }

    if (fstat(dls_fd, &statbuf) < 0)
    {
        close(dls_fd);
        return -5;
    }

    dls_file_handle.dls_size = statbuf.st_size;
    dls_file_handle.dls_address = (uint8_t *) mmap(NULL, dls_file_handle.dls_size, PROT_READ, MAP_PRIVATE, dls_fd, 0);

    close(dls_fd);

    if (dls_file_handle.dls_address == MAP_FAILED)
    {
        return -6;
    }

    eas_file.handle = &dls_file_handle;
    eas_file.readAt = dls_read_at;
    eas_file.size = dls_get_size;

    res = EAS_LoadDLSCollection(data_handle, NULL, &eas_file);

    munmap(dls_file_handle.dls_address, dls_file_handle.dls_size);

    if (res != EAS_SUCCESS)
    {
        return -7;
    }

    return 0;
}


static int start_synth(void) __attribute__((noinline));
static int start_synth(void)
{
    const S_EAS_LIB_CONFIG *eas_config;
    EAS_RESULT res;

    eas_config = EAS_Config();

    num_channels = eas_config->numChannels;
    frequency = eas_config->sampleRate;
    samples_per_call = eas_config->mixBufferSize;
    bytes_per_call = samples_per_call * num_channels * sizeof(EAS_PCM);

    num_subbuffers = (4096 * (int64_t)frequency) / (11025 * (int64_t)samples_per_call);
    if (num_subbuffers > 65536 / bytes_per_call)
    {
        num_subbuffers = 65536 / bytes_per_call;
    }
    if (num_subbuffers < 4)
    {
        fprintf(stderr, "Unsupported EAS parameters: %i, %i, %i\n", num_channels, frequency, samples_per_call);
        return -1;
    }

    // initialize EAS
    res = EAS_Init(&data_handle);
    if (res != EAS_SUCCESS)
    {
        fprintf(stderr, "Error initializing EAS: %i\n", (int)res);
        return -2;
    }

    if (dls_filepath != NULL && *dls_filepath != 0)
    {
        if (load_dls_file() < 0)
        {
            fprintf(stderr, "Error loading DLS file: %s\n", dls_filepath);
            return -3;
        }
    }

    // set master volume
    if (master_volume >= 0)
    {
        EAS_SetVolume(data_handle, NULL, master_volume);
    }

    // set polyphony
    if ((polyphony > 0) && (polyphony <= eas_config->maxVoices))
    {
        EAS_SetSynthPolyphony(data_handle, EAS_MCU_SYNTH, polyphony);
    }

    // set reverb
    if (reverb_preset == 0)
    {
        EAS_SetParameter(data_handle, EAS_MODULE_REVERB, EAS_PARAM_REVERB_BYPASS, EAS_TRUE);
    }
    else
    {
        EAS_SetParameter(data_handle, EAS_MODULE_REVERB, EAS_PARAM_REVERB_BYPASS, EAS_FALSE);
        EAS_SetParameter(data_handle, EAS_MODULE_REVERB, EAS_PARAM_REVERB_PRESET, reverb_preset - 1);
        if (reverb_wet >= 0)
        {
            EAS_SetParameter(data_handle, EAS_MODULE_REVERB, EAS_PARAM_REVERB_WET, reverb_wet);
        }
    }

    // set chorus
    if (chorus_preset == 0)
    {
        EAS_SetParameter(data_handle, EAS_MODULE_CHORUS, EAS_PARAM_CHORUS_BYPASS, EAS_TRUE);
    }
    else
    {
        EAS_SetParameter(data_handle, EAS_MODULE_CHORUS, EAS_PARAM_CHORUS_BYPASS, EAS_FALSE);
        EAS_SetParameter(data_handle, EAS_MODULE_CHORUS, EAS_PARAM_CHORUS_PRESET, chorus_preset - 1);
        if (chorus_rate >= 0)
        {
            EAS_SetParameter(data_handle, EAS_MODULE_CHORUS, EAS_PARAM_CHORUS_RATE, chorus_rate);
        }
        if (chorus_depth >= 0)
        {
            EAS_SetParameter(data_handle, EAS_MODULE_CHORUS, EAS_PARAM_CHORUS_DEPTH, chorus_depth);
        }
        if (chorus_level >= 0)
        {
            EAS_SetParameter(data_handle, EAS_MODULE_CHORUS, EAS_PARAM_CHORUS_LEVEL, chorus_level);
        }
    }

    // open midi stream
    res = EAS_OpenMIDIStream(data_handle, &stream_handle, NULL);
    if (res != EAS_SUCCESS)
    {
        fprintf(stderr, "Error opening EAS midi stream: %i\n", (int)res);
        EAS_Shutdown(data_handle);
        return -4;
    }

    // prepare variables
    event_read_index = 0;
    event_write_index = 0;
    subbuf_counter = 0;
    memset(midi_buffer, 0, 65536);


    return 0;
}

static void stop_synth(void)
{
    // close midi stream
    EAS_CloseMIDIStream(data_handle, stream_handle);

    // shutdown EAS
    EAS_Shutdown(data_handle);
}

static int run_as_daemon(void) __attribute__((noinline));
static int run_as_daemon(void)
{
    int err;

    printf("Running as daemon...\n");

    err = daemon(0, 0);
    if (err < 0)
    {
        fprintf(stderr, "Error running as daemon: %i\n", err);
        return -1;
    }

    return 0;
}

static int open_midi_port(void) __attribute__((noinline));
static int open_midi_port(void)
{
    int err;
    unsigned int caps, type;

    err = snd_seq_open(&midi_seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
    if (err < 0)
    {
        fprintf(stderr, "Error opening ALSA sequencer: %i\n%s\n", err, snd_strerror(err));
        return -1;
    }

    err = snd_seq_set_client_name(midi_seq, midi_name);
    if (err < 0)
    {
        snd_seq_close(midi_seq);
        fprintf(stderr, "Error setting sequencer client name: %i\n%s\n", err, snd_strerror(err));
        return -2;
    }

    caps = SND_SEQ_PORT_CAP_SUBS_WRITE | SND_SEQ_PORT_CAP_WRITE;
    type = SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_MIDI_GM | SND_SEQ_PORT_TYPE_SYNTHESIZER;
    err = snd_seq_create_simple_port(midi_seq, port_name, caps, type);
    if (err < 0)
    {
        snd_seq_close(midi_seq);
        fprintf(stderr, "Error creating sequencer port: %i\n%s\n", err, snd_strerror(err));
        return -3;
    }
    midi_port_id = err;

    printf("%s ALSA address is %i:0\n", midi_name, snd_seq_client_id(midi_seq));

    return 0;
}

static void close_midi_port(void)
{
    snd_seq_delete_port(midi_seq, midi_port_id);
    snd_seq_close(midi_seq);
}


static int set_hw_params(void)
{
    int err, dir;
    unsigned int rate;
    snd_pcm_uframes_t buffer_size, period_size;
    snd_pcm_hw_params_t *pcm_hwparams;

    snd_pcm_hw_params_alloca(&pcm_hwparams);

    err = snd_pcm_hw_params_any(midi_pcm, pcm_hwparams);
    if (err < 0)
    {
        fprintf(stderr, "Error getting hwparams: %i\n%s\n", err, snd_strerror(err));
        return -1;
    }

    err = snd_pcm_hw_params_set_access(midi_pcm, pcm_hwparams, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0)
    {
        fprintf(stderr, "Error setting access: %i\n%s\n", err, snd_strerror(err));
        return -2;
    }

    err = snd_pcm_hw_params_set_format(midi_pcm, pcm_hwparams, SND_PCM_FORMAT_S16);
    if (err < 0)
    {
        fprintf(stderr, "Error setting format: %i\n%s\n", err, snd_strerror(err));
        return -3;
    }

    err = snd_pcm_hw_params_set_channels(midi_pcm, pcm_hwparams, num_channels);
    if (err < 0)
    {
        fprintf(stderr, "Error setting channels: %i\n%s\n", err, snd_strerror(err));
        return -4;
    }

    rate = frequency;
    dir = 0;
    err = snd_pcm_hw_params_set_rate_near(midi_pcm, pcm_hwparams, &rate, &dir);
    if (err < 0)
    {
        fprintf(stderr, "Error setting rate: %i\n%s\n", err, snd_strerror(err));
        return -5;
    }

    buffer_size = samples_per_call * num_subbuffers;
    err = snd_pcm_hw_params_set_buffer_size_near(midi_pcm, pcm_hwparams, &buffer_size);
    if (err < 0)
    {
        fprintf(stderr, "Error setting buffer size: %i\n%s\n", err, snd_strerror(err));
        return -6;
    }

    period_size = samples_per_call;
    dir = 0;
    err = snd_pcm_hw_params_set_period_size_near(midi_pcm, pcm_hwparams, &period_size, &dir);
    if (err < 0)
    {
        fprintf(stderr, "Error setting period size: %i\n%s\n", err, snd_strerror(err));
        return -7;
    }

    err = snd_pcm_hw_params(midi_pcm, pcm_hwparams);
    if (err < 0)
    {
        fprintf(stderr, "Error setting hwparams: %i\n%s\n", err, snd_strerror(err));
        return -8;
    }

    return 0;
}

static int set_sw_params(void)
{
    snd_pcm_sw_params_t *swparams;
    int err;

    snd_pcm_sw_params_alloca(&swparams);

    err = snd_pcm_sw_params_current(midi_pcm, swparams);
    if (err < 0)
    {
        fprintf(stderr, "Error getting swparams: %i\n%s\n", err, snd_strerror(err));
        return -1;
    }

    err = snd_pcm_sw_params_set_avail_min(midi_pcm, swparams, samples_per_call);
    if (err < 0)
    {
        fprintf(stderr, "Error setting avail min: %i\n%s\n", err, snd_strerror(err));
        return -2;
    }

    err = snd_pcm_sw_params(midi_pcm, swparams);
    if (err < 0)
    {
        fprintf(stderr, "Error setting sw params: %i\n%s\n", err, snd_strerror(err));
        return -3;
    }

    return 0;
}

static int open_pcm_output(void) __attribute__((noinline));
static int open_pcm_output(void)
{
    int err;

    err = snd_pcm_open(&midi_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0)
    {
        fprintf(stderr, "Error opening PCM device: %i\n%s\n", err, snd_strerror(err));
        return -1;
    }

    if (set_hw_params() < 0)
    {
        return -2;
    }

    if (set_sw_params() < 0)
    {
        return -3;
    }

    // set nonblock mode
    snd_pcm_nonblock(midi_pcm, 1);

    snd_pcm_prepare(midi_pcm);

    return 0;
}

static void close_pcm_output(void)
{
    snd_pcm_close(midi_pcm);
}


static int drop_privileges(void)
{
    uid_t uid;
    gid_t gid;
    const char *sudo_id;
    long long int llid;
    const char *xdg_dir;
    char buf[32];
    struct stat statbuf;
    struct passwd *passwdbuf;

    if (getuid() != 0)
    {
        return 0;
    }

    sudo_id = secure_getenv("SUDO_UID");
    if (sudo_id == NULL)
    {
        sudo_id = secure_getenv("PKEXEC_UID");
        if (sudo_id == NULL)
        {
            return -1;
        }
    }

    errno = 0;
    llid = strtoll(sudo_id, NULL, 10);
    uid = (uid_t) llid;
    if (errno != 0 || uid == 0 || llid != (long long int)uid)
    {
        return -2;
    }

    gid = getgid();
    if (gid == 0)
    {
        sudo_id = secure_getenv("SUDO_GID");
        if (sudo_id == NULL)
        {
            passwdbuf = getpwuid(uid);
            if (passwdbuf != NULL)
            {
                gid = passwdbuf->pw_gid;
            }

            if (gid == 0)
            {
                return -3;
            }
        }
        else
        {
            errno = 0;
            llid = strtoll(sudo_id, NULL, 10);
            gid = (gid_t) llid;
            if (errno != 0 || gid == 0 || llid != (long long int)gid)
            {
                return -4;
            }
        }
    }

    if (setgid(gid) != 0)
    {
        return -5;
    }
    if (setuid(uid) != 0)
    {
        return -6;
    }

    printf("Dropped root privileges\n");

    chdir("/");

    // define some environment variables

    xdg_dir = getenv("XDG_RUNTIME_DIR");
    if ((xdg_dir == NULL) || (*xdg_dir == 0))
    {
        snprintf(buf, 32, "/run/user/%lli", (long long int)uid);

        if ((stat(buf, &statbuf) == 0) && ((statbuf.st_mode & S_IFMT) == S_IFDIR) && (statbuf.st_uid == uid))
        {
            // if XDG_RUNTIME_DIR is not defined and directory /run/user/$USER exists then use it for XDG_RUNTIME_DIR
            setenv("XDG_RUNTIME_DIR", buf, 1);

            xdg_dir = getenv("XDG_CONFIG_HOME");
            if ((xdg_dir == NULL) || (*xdg_dir == 0))
            {
                passwdbuf = getpwuid(uid);
                if (passwdbuf != NULL)
                {
                    // also if XDG_CONFIG_HOME is not defined then define it as user's home directory
                    setenv("XDG_CONFIG_HOME", passwdbuf->pw_dir, 1);
                }
            }
        }
    }

    return 0;
}

static int start_thread(void) __attribute__((noinline));
static int start_thread(void)
{
    pthread_attr_t attr;
    int err;
    volatile int initialized;

    // try to increase priority (only root)
    nice(-20);

    err = pthread_attr_init(&attr);
    if (err != 0)
    {
        fprintf(stderr, "Error creating thread attribute: %i\n", err);
        return -1;
    }

    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    midi_init_state = 0;
    initialized = 0;
    err = pthread_create(&midi_thread, &attr, &midi_thread_proc, (void *)&initialized);
    pthread_attr_destroy(&attr);

    if (err != 0)
    {
        fprintf(stderr, "Error creating thread: %i\n", err);
        return -2;
    }

    // wait for thread initialization
    while (initialized == 0)
    {
        struct timespec req;

        req.tv_sec = 0;
        req.tv_nsec = 10000000;
        nanosleep(&req, NULL);
    };

    if (drop_privileges() < 0)
    {
        fprintf(stderr, "Error dropping root privileges\n");
    }

    return 0;
}


static int render_subbuffer(int num)
{
    int read_index, write_index;
    EAS_RESULT res;
    EAS_I32 num_generated;

    // read global volatile variables to local variables
    read_index = event_read_index;
    write_index = event_write_index;

    if (read_index != write_index)
    {
        // read events from buffer
        if (read_index < write_index)
        {
            EAS_WriteMIDIStream(data_handle, stream_handle, &(event_buffer[read_index]), write_index - read_index);
        }
        else
        {
            EAS_WriteMIDIStream(data_handle, stream_handle, &(event_buffer[read_index]), 65536 - read_index);
            EAS_WriteMIDIStream(data_handle, stream_handle, &(event_buffer[0]), write_index);
        }

        // update global volatile variable
        event_read_index = write_index;
    }

    // render audio data
    res = EAS_Render(data_handle, (EAS_PCM *) &(midi_buffer[num * bytes_per_call]), samples_per_call, &num_generated);
    if (res != EAS_SUCCESS) return -1;
    if (num_generated != samples_per_call) return -2;

    return 0;
}

static int output_subbuffer(int num)
{
    snd_pcm_uframes_t remaining;
    snd_pcm_sframes_t written;
    uint8_t *buf_ptr;

    remaining = samples_per_call;
    buf_ptr = &(midi_buffer[num * bytes_per_call]);

    while (remaining)
    {
        written = snd_pcm_writei(midi_pcm, buf_ptr, remaining);
        if (written < 0)
        {
            return -1;
        }

        remaining -= written;
        buf_ptr += written << 2;
    };

    return 0;
}

static void main_loop(void) __attribute__((noinline));
static void main_loop(void)
{
    int is_paused;
    struct timespec last_written_time, current_time;
#if defined(CLOCK_MONOTONIC_RAW)
    clockid_t monotonic_clock_id;

    #define MONOTONIC_CLOCK_TYPE monotonic_clock_id

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &current_time))
    {
        monotonic_clock_id = CLOCK_MONOTONIC;
    }
    else
    {
        monotonic_clock_id = CLOCK_MONOTONIC_RAW;
    }
#else
    #define MONOTONIC_CLOCK_TYPE CLOCK_MONOTONIC
#endif

    for (int i = 2; i < num_subbuffers; i++)
    {
        output_subbuffer(i);
    }

    is_paused = 0;
    // pause pcm playback at the beginning
    if (0 == snd_pcm_pause(midi_pcm, 1))
    {
        is_paused = 1;
        printf("PCM playback paused\n");
    }
    else
    {
        // if pausing doesn't work then set time of last written event as current time, so the next attempt to pause will be in 60 seconds
        clock_gettime(MONOTONIC_CLOCK_TYPE, &last_written_time);
    }

    midi_event_written = 0;
    midi_init_state = 1;

    while (1)
    {
        struct timespec req;
        snd_pcm_state_t pcmstate;
        snd_pcm_sframes_t available_frames;

        req.tv_sec = 0;
        req.tv_nsec = 10000000;
        nanosleep(&req, NULL);

        if (midi_event_written)
        {
            midi_event_written = 0;

            // remember time of last written event
            clock_gettime(MONOTONIC_CLOCK_TYPE, &last_written_time);

            if (is_paused)
            {
                is_paused = 0;
                snd_pcm_pause(midi_pcm, 0);
                printf("PCM playback unpaused\n");
            }
        }
        else
        {
            if (is_paused)
            {
                continue;
            }

            clock_gettime(MONOTONIC_CLOCK_TYPE, &current_time);
            // if more than 60 seconds elapsed from last written event, then pause pcm playback
            if (current_time.tv_sec - last_written_time.tv_sec > 60)
            {
                if (0 == snd_pcm_pause(midi_pcm, 1))
                {
                    is_paused = 1;
                    printf("PCM playback paused\n");
                    continue;
                }
                else
                {
                    // if pausing doesn't work then set time of last written event as current time, so the next attempt to pause will be in 60 seconds
                    last_written_time = current_time;
                }
            }
        }

        pcmstate = snd_pcm_state(midi_pcm);
        if (pcmstate == SND_PCM_STATE_XRUN)
        {
            fprintf(stderr, "Buffer underrun\n");
            snd_pcm_prepare(midi_pcm);
        }

        available_frames = snd_pcm_avail_update(midi_pcm);
        while (available_frames >= (3 * samples_per_call))
        {
            if (render_subbuffer(subbuf_counter) < 0)
            {
                fprintf(stderr, "Error rendering audio data\n");
            }

            if (output_subbuffer(subbuf_counter) < 0)
            {
                fprintf(stderr, "Error writing audio data\n");
                available_frames = 0;
                break;
            }
            else
            {
                available_frames -= samples_per_call;
            }

            subbuf_counter++;
            if (subbuf_counter == num_subbuffers)
            {
                subbuf_counter = 0;
            }
        };
    };
}

int main(int argc, char *argv[])
{
    read_arguments(argc, argv);

    if (start_synth() < 0)
    {
        return 2;
    }

    if (daemonize)
    {
        if (run_as_daemon() < 0)
        {
            stop_synth();
            return 3;
        }
    }

    if (start_thread() < 0)
    {
        stop_synth();
        return 4;
    }

    if (open_pcm_output() < 0)
    {
        midi_init_state = -1;
        stop_synth();
        return 5;
    }

    if (open_midi_port() < 0)
    {
        midi_init_state = -1;
        close_pcm_output();
        stop_synth();
        return 6;
    }

    main_loop();

    midi_init_state = -1;
    close_midi_port();
    close_pcm_output();
    stop_synth();
    return 0;
}

