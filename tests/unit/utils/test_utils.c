// SPDX-License-Identifier: MIT
// Unit tests of src/utils/utils.c.
#include "utils/utils.h"
#include "parser/parser.h"
#include <string.h>
#include "unit_test.h"

UTEST(str_join, several_parts)
{
    const char *parts[] = {"a", "b", "c", NULL};
    char out[32];
    str_join(out, sizeof(out), parts, ", ");
    ASSERT_STREQ("a, b, c", out);
}

UTEST(str_join, one_part_has_no_separator)
{
    const char *parts[] = {"only", NULL};
    char out[32];
    str_join(out, sizeof(out), parts, ", ");
    ASSERT_STREQ("only", out);
}

UTEST(str_join, no_parts_gives_an_empty_string)
{
    char out[8] = "junk";
    str_join(out, sizeof(out), NULL, ", ");
    ASSERT_STREQ("", out);
}

UTEST(str_join, truncates_to_the_buffer)
{
    const char *parts[] = {"abc", "def", NULL};
    char out[6];
    str_join(out, sizeof(out), parts, "-");
    ASSERT_STREQ("abc-d", out);
}

UTEST(str_appendf, appends_one_after_another)
{
    char out[32];
    size_t used = 0;
    ASSERT_TRUE(str_appendf(out, sizeof(out), &used, "fn %s(", "f"));
    ASSERT_TRUE(str_appendf(out, sizeof(out), &used, "%d, %d)", 1, 2));
    ASSERT_STREQ("fn f(1, 2)", out);
    ASSERT_EQ((size_t)10, used);
}

UTEST(str_appendf, cuts_to_the_buffer_and_stops_there)
{
    char out[8];
    size_t used = 0;
    ASSERT_FALSE(str_appendf(out, sizeof(out), &used, "%s", "abcdefghij"));
    ASSERT_STREQ("abcdefg", out);
    ASSERT_EQ((size_t)7, used);
    // A full buffer stays as it is: `used` cannot run past the end.
    ASSERT_FALSE(str_appendf(out, sizeof(out), &used, "%s", "xyz"));
    ASSERT_STREQ("abcdefg", out);
    ASSERT_EQ((size_t)7, used);
}

// Runs scan_build_directives on `src` with a fresh compiler, whose flags the test then reads.
static ZenCompiler *scan(const char *src)
{
    static ZenCompiler compiler;
    static ParserContext ctx;
    memset(&compiler, 0, sizeof(compiler));
    memset(&ctx, 0, sizeof(ctx));
    ctx.compiler = &compiler;
    ctx.config = &compiler.config;
    scan_build_directives(&ctx, src, NULL);
    return &compiler;
}

// //> include: gives the C compiler's include paths, each an argument of its own (not the
// cflags string, which is split at spaces); a quoted one holds spaces.
UTEST(scan_build_directives, include_adds_one_path_per_word)
{
    ZenCompiler *c = scan("//> include: /opt/a/include   /opt/b/inc \"/opt/c d/inc\"\n");
    ASSERT_EQ((size_t)3, c->config.c_include_paths.length);
    ASSERT_STREQ("/opt/a/include", c->config.c_include_paths.data[0]);
    ASSERT_STREQ("/opt/b/inc", c->config.c_include_paths.data[1]);
    ASSERT_STREQ("/opt/c d/inc", c->config.c_include_paths.data[2]);
    ASSERT_STREQ("", c->cflags);
}

UTEST(scan_build_directives, lib_adds_one_flag_per_path)
{
    ZenCompiler *c = scan("//> lib: /opt/a/lib /opt/b/lib\n");
    ASSERT_STREQ("-L/opt/a/lib -L/opt/b/lib", c->link_flags);
}

UTEST(scan_build_directives, framework_adds_one_flag_per_name)
{
    ZenCompiler *c = scan("//> framework:  Cocoa\tMetal\n");
    ASSERT_STREQ("-framework Cocoa -framework Metal", c->link_flags);
}

UTEST(scan_build_directives, a_path_longer_than_the_flags_is_cut_safely)
{
    char src[2100];
    int n = snprintf(src, sizeof(src), "//> lib: /");
    memset(src + n, 'a', 2000);
    snprintf(src + n + 2000, sizeof(src) - (size_t)n - 2000, "\n");
    ZenCompiler *c = scan(src);
    ASSERT_LT(strlen(c->link_flags), sizeof(c->link_flags));
    ASSERT_EQ(0, strncmp(c->link_flags, "-L/aaa", 6));
}

UTEST(scan_build_directives, a_long_include_path_is_cut_safely)
{
    char src[2100];
    int n = snprintf(src, sizeof(src), "//> include: /");
    memset(src + n, 'a', 2000);
    snprintf(src + n + 2000, sizeof(src) - (size_t)n - 2000, "\n");
    ZenCompiler *c = scan(src);
    ASSERT_EQ((size_t)1, c->config.c_include_paths.length);
    ASSERT_EQ(0, strncmp(c->config.c_include_paths.data[0], "/aaa", 4));
}
