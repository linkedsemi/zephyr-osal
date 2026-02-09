/*
 * memtester version 4
 *
 * Very simple but very effective user-space memory tester.
 * Originally by Simon Kirby <sim@stormix.com> <sim@neato.org>
 * Version 2 by Charles Cazabon <charlesc-memtester@pyropus.ca>
 * Version 3 not publicly released.
 * Version 4 rewrite:
 * Copyright (C) 2004-2020 Charles Cazabon <charlesc-memtester@pyropus.ca>
 * Licensed under the terms of the GNU General Public License version 2 (only).
 * See the file COPYING for details.
 *
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#include "types.h"
#include "sizes.h"
#include "tests.h"

#ifdef __ZEPHYR__
#include <zephyr/shell/shell.h>
#endif

#define EXIT_FAIL_NONSTARTER    0x01
#define EXIT_FAIL_ADDRESSLINES  0x02
#define EXIT_FAIL_OTHERTEST     0x04

enum
{
    Error_Stuck = 0,
    Error_Random,
    Error_XOR,
    Error_SUB,
    Error_MUL,
    Error_DIV,
    Error_OR,
    Error_AND,
    Error_Sequential,
    Error_Solid,
    Error_Block,
    Error_Checkerboard,
    Error_Spread,
    Error_Flip,
    Error_Walking1,
    Error_Walking0,
    Error_Writes8,
    Error_Writes16
};

struct test tests[] = {
    { "Random Value", test_random_value, Error_Random},
    { "Compare XOR", test_xor_comparison, Error_XOR},
    { "Compare SUB", test_sub_comparison, Error_SUB},
    { "Compare MUL", test_mul_comparison, Error_MUL},
    { "Compare DIV",test_div_comparison, Error_DIV},
    { "Compare OR", test_or_comparison, Error_OR},
    { "Compare AND", test_and_comparison, Error_AND},
    { "Sequential Increment", test_seqinc_comparison, Error_Sequential},
    { "Solid Bits", test_solidbits_comparison, Error_Solid},
    { "Block Sequential", test_blockseq_comparison, Error_Block},
    { "Checkerboard", test_checkerboard_comparison, Error_Checkerboard},
    { "Bit Spread", test_bitspread_comparison, Error_Spread},
    { "Bit Flip", test_bitflip_comparison, Error_Flip},
    { "Walking Ones", test_walkbits1_comparison, Error_Walking1},
    { "Walking Zeroes", test_walkbits0_comparison, Error_Walking0},
#ifdef TEST_NARROW_WRITES
    { "8-bit Writes", test_8bit_wide_random, Error_Writes8},
    { "16-bit Writes", test_16bit_wide_random, Error_Writes16},
#endif
    { NULL, NULL }
};

#ifdef _SC_PAGE_SIZE
int memtester_pagesize(void) {
    int pagesize = sysconf(_SC_PAGE_SIZE);
    if (pagesize == -1) {
        perror("get page size failed");
        exit(EXIT_FAIL_NONSTARTER);
    }
    printf("pagesize is %ld\n", (long) pagesize);
    return pagesize;
}
#else
int memtester_pagesize(void) {
    // printf("sysconf(_SC_PAGE_SIZE) not supported; using pagesize of 8192\n");
    return 8192;
}
#endif

/* Global vars - so tests have access to this information */
int use_phys = 0;
off_t physaddrbase = 0;

/* Function definitions */
void usage(char *me)
{
    fprintf(stderr, "\n"
            "Usage: %s  <mem>[B|K|M|G] [loops]\n",
            me);
    return;
}

int cmd_memtester_main(int argc, char **argv)
{
    ul loops, loop, i;
    size_t pagesize, wantraw, wantmb, wantbytes, wantbytes_orig, bufsize = 0,
         halflen, count;
    char *memsuffix, *loopsuffix;
    ptrdiff_t pagesizemask;
    void volatile *buf, *aligned;
    ulv *bufa, *bufb;
    int memshift;
    size_t maxbytes = 1048576; /* 1m, 1024 * 1024, addressable memory, in bytes */
    size_t maxmb = 1; /* addressable memory, in MB */
    ul testmask = 0;
    int opts=1;

    // fprintf(stderr, "argc=%d \n", argc); 
    if(argc < 2)
    {
      usage(argv[0]);   
      return 0;
    }

    // out_initialize();
  
    pagesize = memtester_pagesize();
    pagesizemask = (ptrdiff_t) ~(pagesize - 1);
    // fprintf(stderr, "pagesizemask is 0x%tx\n", pagesizemask);
   
    errno = 0;
    wantraw = (size_t) strtoul(argv[opts], &memsuffix, 0);
    if (errno != 0)
    {
        fprintf(stderr, "failed to parse memory argument");
        usage(argv[0]); /* doesn't return */
        return 0;
    }

    switch (*memsuffix)
    {
        case 'G':
        case 'g':
            memshift = 30; /* gigabytes */
            break;
        case 'M':
        case 'm':
            memshift = 20; /* megabytes */
            break;
        case 'K':
        case 'k':
            memshift = 10; /* kilobytes */
            break;
        case 'B':
        case 'b':
            memshift = 0; /* bytes*/
            break;
        case '\0':  /* no suffix */
            memshift = 10; /* kilobytes */
            break;
        default:
            /* bad suffix */
            usage(argv[0]); /* doesn't return */
            return 0;
    }

    wantbytes_orig = wantbytes = ((size_t) wantraw << memshift);
    wantmb = (wantbytes_orig >> 20);
    opts++;
    if (wantmb > maxmb)
    {
        fprintf(stderr, "This system can only address %llu MB.\n", (ull) maxmb);
         return 0;
    }

    if (wantbytes < pagesize) {
        fprintf(stderr, "bytes %ld < pagesize %ld -- memory argument too large?\n",
                wantbytes, pagesize);
         return 0;
    }
    // fprintf(stderr, "want %lluMB (%llu bytes)\n", (ull) wantmb, (ull) wantbytes);

    if (opts >= argc)
    {
        loops = 1;
    } 
    else 
    {
        errno = 0;
        loops = strtoul(argv[opts], &loopsuffix, 0);
        if (errno != 0)
        {
            fprintf(stderr, "failed to parse number of loops");
            usage(argv[0]); /* doesn't return */
            return 0;
        }

        if (*loopsuffix != '\0')
        {
            fprintf(stderr, "loop suffix %c\n", *loopsuffix);
            usage(argv[0]); /* doesn't return */
            return 0;
        }
    }
    if (loops == 0)
    {
        fprintf(stderr, "loop number is invalid.\n");
        return 0;
    }
    

    buf = NULL;
    while (wantbytes > 0)
    {
        buf = (void volatile *) malloc(wantbytes);
        if (buf)
        {
            break;
        }
        if(wantbytes <= pagesize)
        {
            break;
        }
        wantbytes -= pagesize;
    }
    if (buf == NULL)
    {
        fprintf(stderr, "failed to molloc. size is %llu bytes.\n", (ull)wantbytes);
        return 0;
    }
    bufsize = wantbytes;
    fprintf(stderr, "got %lluMB (%llu bytes)\n", (ull) wantbytes >> 20,
        (ull)wantbytes);

    /* Do alighnment here as well, as some cases won't trigger above if you
       define out the use of mlock() (cough HP/UX 10 cough). */
    if ((size_t) buf % pagesize)
    {
        /* printf("aligning to page -- was 0x%tx\n", buf); */
        aligned = (void volatile *) ((size_t) buf & pagesizemask) + pagesize;
        /* printf("  now 0x%tx -- lost %d bytes\n", aligned,
         *      (size_t) aligned - (size_t) buf);
         */
        bufsize -= ((size_t) aligned - (size_t) buf);
    } 
    else 
    {
        aligned = buf;
    }

    halflen = bufsize / 2;
    count = halflen / sizeof(ul);
    bufa = (ulv *) aligned;
    bufb = (ulv *) ((size_t) aligned + halflen);

    for (loop=1; loop <= loops; loop++)
    {
        //fprintf(stderr, "Loop %lu/%lu:\n", loop, loops);
        
        if (!test_stuck_address(aligned, bufsize / sizeof(ul)))
        {
            // fprintf(stderr, "  %-20s: ok\n", "Stuck Address");
        }
        else
        {
            fprintf(stderr, "  %-20s: fail, loop is %lu.\n", "Stuck Address", loop); 
            free(buf);
            return Error_Stuck;
        }

        for (i=0;; i++)
        {
            if (!tests[i].name) break;

            if (!tests[i].fp(bufa, bufb, count))
            {
                // fprintf(stderr, "  %-20s: ok\n", tests[i].name);
            }
            else
            {
                fprintf(stderr, "  %-20s: fail, loop is %lu.\n", tests[i].name, loop);
                free(buf);
                return tests[i].errorCode;
            }

            /* clear buffer */
            memset((void *) buf, 255, wantbytes);
        }
    }

    fprintf(stderr, "memtester Done.\n"); 
    free(buf);
    return 0;
}

#ifdef __ZEPHYR__
int cmd_memtester(const struct shell* sh, size_t argc, char* argv[])
{
    return cmd_memtester_main(argc, argv);
}

SHELL_CMD_REGISTER(memtester, NULL, "memtester", cmd_memtester);
#endif