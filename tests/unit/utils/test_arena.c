// SPDX-License-Identifier: MIT
// Unit tests of the arena (src/utils/zalloc.h) as a long-running tool reuses it, and of
// append_flag's bounds.
#include "parser/parser.h"
#include "utils/utils.h"
#include <string.h>
#include "unit_test.h"

static size_t arena_blocks(zarena *a)
{
    size_t n = 0;
    for (zarena_block *b = a->first; b; b = b->next)
    {
        n++;
    }
    return n;
}

// The same pattern of allocations after each reset reuses the blocks it made the first
// time: the fuzz harness resets between inputs, and the LSP rewinds between requests. (It
// used to look one block ahead only, and add a block whenever a large allocation came after
// small ones there: one more each time, never freed.)
UTEST(zarena, a_reset_arena_reuses_its_blocks)
{
    zarena a;
    zarena_init(&a);
    const size_t sizes[] = {100, 5000, 100, 70000, 100, 300000, 100, 9000};
    size_t first_round = 0;
    for (int round = 0; round < 50; round++)
    {
        // A different order each round, as different inputs would make.
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        {
            size_t k = (i + (size_t)round) % (sizeof(sizes) / sizeof(sizes[0]));
            void *p = zarena_alloc(&a, sizes[k]);
            ASSERT_TRUE(p != NULL);
            memset(p, 0xab, sizes[k]);
        }
        if (round == 0)
        {
            first_round = arena_blocks(&a);
        }
        zarena_reset(&a);
    }
    // A few more than the first round at most (an order can need one more), never one a round.
    ASSERT_LE(arena_blocks(&a), first_round + 4);
    zarena_free(&a);
}

// Rewinding to a mark (the LSP's read-only requests) reuses the blocks after it the same way.
UTEST(zarena, a_rewound_arena_reuses_its_blocks)
{
    zarena a;
    zarena_init(&a);
    ASSERT_TRUE(zarena_alloc(&a, 64) != NULL);
    ZarenaMark mark = zarena_save(&a);
    const size_t sizes[] = {100, 5000, 100, 70000, 100, 300000, 100, 9000};
    const size_t n_sizes = sizeof(sizes) / sizeof(sizes[0]);
    size_t after_first = 0;
    for (int round = 0; round < 50; round++)
    {
        for (size_t i = 0; i < n_sizes; i++)
        {
            ASSERT_TRUE(zarena_alloc(&a, sizes[(i + (size_t)round) % n_sizes]) != NULL);
        }
        if (round == 0)
        {
            after_first = arena_blocks(&a);
        }
        zarena_restore(&a, mark);
    }
    ASSERT_LE(arena_blocks(&a), after_first + 4);
    zarena_free(&a);
}

// A flag is cut to what fits, and a full buffer takes nothing; nothing is written past the
// end. (A buffer filled to the last byte used to make the next append write past its end.)
UTEST(append_flag, stays_within_the_buffer)
{
    struct
    {
        char buf[16];
        char after[16];
    } s;
    memset(&s, 0, sizeof(s));
    memset(s.after, 'Z', sizeof(s.after) - 1);

    append_flag(s.buf, sizeof(s.buf), "-labcdefghijk", NULL); // 13 characters: fits
    ASSERT_STREQ("-labcdefghijk", s.buf);
    append_flag(s.buf, sizeof(s.buf), "x", NULL); // a space and 1: 15, exactly fits
    ASSERT_STREQ("-labcdefghijk x", s.buf);
    append_flag(s.buf, sizeof(s.buf), "-lm", NULL); // full: nothing
    ASSERT_STREQ("-labcdefghijk x", s.buf);
    ASSERT_EQ('Z', s.after[0]);
    ASSERT_EQ('Z', s.after[14]);

    memset(s.buf, 0, sizeof(s.buf));
    append_flag(s.buf, sizeof(s.buf), "-labcdefghij", NULL); // 12
    append_flag(s.buf, sizeof(s.buf), "-L", "/usr");         // a space and 2 fit of 6
    ASSERT_STREQ("-labcdefghij -L", s.buf);
    ASSERT_EQ('Z', s.after[0]);
}
