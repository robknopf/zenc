// A minimal reload host for testing hot reload of Zen C modules.
//
// Usage: host ZC CASE_DIR WORK_DIR [--delete-old]
//
// Each CASE_DIR/stepN.zc (N = 1, 2, ...) is one version of the module, in order. For each
// step the host transpiles it with `ZC transpile --cc tcc`, compiles the C in memory with
// libtcc and calls its `reload_step(void *state)`, as a program's host would after a save.
// `state` is a zeroed block the host owns, the same one for every step.
//
// A step that fails to build is reported and the previous module is called again, as a
// host keeps running the last good code.
//
// Previous modules stay loaded until the host exits, so anything that still points into
// one (a trait object's vtable, a string literal, a function pointer) keeps working, and
// calls the old code. --delete-old deletes the previous module after each good build
// instead, to show what that breaks.
//
// The host prints "--- step N" before each step, so the expected output shows which
// module printed what.

#include <libtcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum
{
    STATE_BYTES = 65536,
    MAX_STEPS = 64
};

typedef void (*StepFn)(void *state);

// One loaded version of the module.
//
// With libtcc 0.9.27 the host gives libtcc the memory the code runs from, and fills it with
// 0xCC (x86's breakpoint instruction) when the module is deleted. Anything that still uses
// the deleted module then fails every time, instead of reading what happens to be left in
// freed memory. libtcc 0.9.28 always allocates that memory itself, so there it is not
// overwritten and a dangling pointer may go unnoticed.
typedef struct
{
    TCCState *tcc;
    void *mem; // ours, with 0.9.27; NULL with 0.9.28
    size_t mem_size;
} Module;

static int relocate(Module *m)
{
#ifdef TCC_RELOCATE_AUTO
    int size = tcc_relocate(m->tcc, NULL);
    if (size < 0)
    {
        return -1;
    }
    long page = sysconf(_SC_PAGESIZE);
    m->mem_size = ((size_t)size + (size_t)page - 1) / (size_t)page * (size_t)page;
    if (posix_memalign(&m->mem, (size_t)page, m->mem_size) != 0)
    {
        m->mem = NULL;
        return -1;
    }
    return tcc_relocate(m->tcc, m->mem);
#else
    return tcc_relocate(m->tcc);
#endif
}

static void module_delete(Module *m)
{
    tcc_delete(m->tcc);
    if (m->mem)
    {
        memset(m->mem, 0xCC, m->mem_size);
        free(m->mem);
    }
    m->tcc = NULL;
    m->mem = NULL;
}

static void on_tcc_error(void *opaque, const char *msg)
{
    (void)opaque;
    fprintf(stderr, "tcc: %s\n", msg);
}

// Build CASE_DIR/stepN.zc into *m. Returns 0 if it did not build.
static int build_step(const char *zc, const char *case_dir, const char *work_dir, int n, Module *m,
                      StepFn *out_step)
{
    char src[4096], c_file[4096], cmd[12288];
    snprintf(src, sizeof(src), "%s/step%d.zc", case_dir, n);
    snprintf(c_file, sizeof(c_file), "%s/step%d.c", work_dir, n);
    snprintf(cmd, sizeof(cmd), "\"%s\" transpile --cc tcc -q \"%s\" -o \"%s\" 1>&2", zc, src,
             c_file);
    if (system(cmd) != 0)
    {
        return 0;
    }

    memset(m, 0, sizeof(*m));
    m->tcc = tcc_new();
    if (!m->tcc)
    {
        return 0;
    }
    tcc_set_error_func(m->tcc, NULL, on_tcc_error);
    tcc_set_output_type(m->tcc, TCC_OUTPUT_MEMORY);
    if (tcc_add_file(m->tcc, c_file) == -1 || relocate(m) < 0)
    {
        module_delete(m);
        return 0;
    }
    *out_step = (StepFn)tcc_get_symbol(m->tcc, "reload_step");
    if (!*out_step)
    {
        fprintf(stderr, "host: step%d.zc has no reload_step\n", n);
        module_delete(m);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: host ZC CASE_DIR WORK_DIR [--delete-old]\n");
        return 2;
    }
    const char *zc = argv[1];
    const char *case_dir = argv[2];
    const char *work_dir = argv[3];
    int keep_old = !(argc > 4 && strcmp(argv[4], "--delete-old") == 0);

    setvbuf(stdout, NULL, _IONBF, 0); // the module's output and ours, in order

    void *state = calloc(1, STATE_BYTES);
    Module kept[MAX_STEPS];
    int kept_count = 0;
    Module current = {0};
    StepFn step = NULL;

    for (int n = 1; n <= MAX_STEPS; n++)
    {
        char src[4096];
        snprintf(src, sizeof(src), "%s/step%d.zc", case_dir, n);
        FILE *f = fopen(src, "r");
        if (!f)
        {
            break;
        }
        fclose(f);

        printf("--- step %d\n", n);
        StepFn next_step = NULL;
        Module next;
        if (build_step(zc, case_dir, work_dir, n, &next, &next_step))
        {
            if (current.tcc)
            {
                if (keep_old)
                {
                    kept[kept_count++] = current;
                }
                else
                {
                    module_delete(&current);
                }
            }
            current = next;
            step = next_step;
        }
        else
        {
            printf("(build failed, running the previous module)\n");
        }
        if (step)
        {
            step(state);
        }
    }

    for (int i = 0; i < kept_count; i++)
    {
        module_delete(&kept[i]);
    }
    if (current.tcc)
    {
        module_delete(&current);
    }
    free(state);
    return 0;
}
