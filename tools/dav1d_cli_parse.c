/*
 * Copyright © 2018, VideoLAN and dav1d authors
 * Copyright © 2018, Two Orioles, LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <assert.h>
#include <getopt.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include "dav1d_cli_parse.h"

static const char short_opts[] = "i:o:vql:";

enum {
    ARG_MUXER,
    ARG_FRAME_THREADS,
    ARG_TILE_THREADS,
};

static const struct option long_opts[] = {
    { "input",          1, NULL, 'i' },
    { "output",         1, NULL, 'o' },
    { "quiet",          0, NULL, 'q' },
    { "muxer",          1, NULL, ARG_MUXER },
    { "version",        0, NULL, 'v' },
    { "limit",          1, NULL, 'l' },
    { "skip",           1, NULL, 's' },
    { "framethreads",   1, NULL, ARG_FRAME_THREADS },
    { "tilethreads",    1, NULL, ARG_TILE_THREADS },
    { NULL,             0, NULL, 0 },
};

static void usage(const char *const app, const char *const reason, ...) {
    if (reason) {
        va_list args;

        va_start(args, reason);
        vfprintf(stderr, reason, args);
        va_end(args);
        fprintf(stderr, "\n\n");
    }
    fprintf(stderr, "Usage: %s [options]\n\n", app);
    fprintf(stderr, "Supported options:\n"
            " --input/-i  $file:   input file\n"
            " --output/-o $file:   output file\n"
            " --muxer $name:       force muxer type (default: detect from extension)\n"
            " --quiet/-q:          disable status messages\n"
            " --limit/-l $num:     stop decoding after $num frames\n"
            " --skip/-s $num:      skip decoding of the first $num frames\n"
            " --version/-v:        print version and exit\n"
            " --framethreads $num: number of frame threads (default: 1)\n"
            " --tilethreads $num:  number of tile threads (default: 1)\n");
    exit(1);
}

static void error(const char *const app, const char *const optarg,
                  const int option, const char *const shouldbe)
{
    char optname[256];
    int n;

    for (n = 0; long_opts[n].name; n++)
        if (long_opts[n].val == option)
            break;
    assert(long_opts[n].name);
    if (long_opts[n].val < 256) {
        sprintf(optname, "-%c/--%s", long_opts[n].val, long_opts[n].name);
    } else {
        sprintf(optname, "--%s", long_opts[n].name);
    }

    usage(app, "Invalid argument \"%s\" for option %s; should be %s",
          optarg, optname, shouldbe);
}

static unsigned parse_unsigned(char *optarg, const int option, const char *app) {
    char *end;
    const double res = strtoul(optarg, &end, 0);
    if (*end || end == optarg) error(app, optarg, option, "an integer");
    return res;
}

void parse(const int argc, char *const *const argv,
           CLISettings *const cli_settings, Dav1dSettings *const lib_settings)
{
    int o;

    memset(cli_settings, 0, sizeof(*cli_settings));
    dav1d_default_settings(lib_settings);

    while ((o = getopt_long(argc, argv, short_opts, long_opts, NULL)) >= 0) {
        switch (o) {
        case 'o':
            cli_settings->outputfile = optarg;
            break;
        case 'i':
            cli_settings->inputfile = optarg;
            break;
        case 'q':
            cli_settings->quiet = 1;
            break;
        case 'l':
            cli_settings->limit = parse_unsigned(optarg, 'l', argv[0]);
            break;
        case 's':
            cli_settings->skip = parse_unsigned(optarg, 's', argv[0]);
            break;
        case ARG_MUXER:
            cli_settings->muxer = optarg;
            break;
        case ARG_FRAME_THREADS:
            lib_settings->n_frame_threads =
                parse_unsigned(optarg, ARG_FRAME_THREADS, argv[0]);
            break;
        case ARG_TILE_THREADS:
            lib_settings->n_tile_threads =
                parse_unsigned(optarg, ARG_TILE_THREADS, argv[0]);
            break;
        case 'v':
            fprintf(stderr, "%s\n", dav1d_version());
            exit(0);
        default:
            break;
        }
    }

    if (!cli_settings->inputfile)
        usage(argv[0], "Input file (-i/--input) is required");
    if (!cli_settings->outputfile)
        usage(argv[0], "Output file (-o/--output) is required");
}
