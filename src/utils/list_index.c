// SPDX-License-Identifier: MIT
#include "list_index.h"
#include "../arena.h"
#include <string.h>

static unsigned list_index_hash(const char *s)
{
    unsigned h = 2166136261u;
    while (*s)
    {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

// The slot of `name`, or the empty one it would go in. The table is never full.
static ListIndexSlot *list_index_slot(ListIndexSlot *slots, unsigned cap, const char *name)
{
    unsigned i = list_index_hash(name) & (cap - 1);
    while (slots[i].name && strcmp(slots[i].name, name) != 0)
    {
        i = (i + 1) & (cap - 1);
    }
    return &slots[i];
}

static void list_index_add(ListIndex *x, const char *name, void *entry)
{
    // Kept at most half full.
    if ((x->used + 1) * 2 > x->cap)
    {
        unsigned cap = x->cap ? x->cap * 2 : 64;
        ListIndexSlot *slots = xcalloc(cap, sizeof(ListIndexSlot));
        for (unsigned i = 0; i < x->cap; i++)
        {
            if (x->slots[i].name)
            {
                *list_index_slot(slots, cap, x->slots[i].name) = x->slots[i];
            }
        }
        zfree(x->slots);
        x->slots = slots;
        x->cap = cap;
    }
    ListIndexSlot *s = list_index_slot(x->slots, x->cap, name);
    if (!s->name)
    {
        s->name = name;
        x->used++;
    }
    if (s->count == s->cap)
    {
        unsigned cap = s->cap ? s->cap * 2 : 2;
        void **grown = xmalloc(cap * sizeof(void *));
        if (s->count)
        {
            memcpy(grown, s->entries, s->count * sizeof(void *));
        }
        zfree(s->entries);
        s->entries = grown;
        s->cap = cap;
    }
    s->entries[s->count++] = entry;
}

static void list_index_clear(ListIndex *x)
{
    for (unsigned i = 0; i < x->cap; i++)
    {
        zfree(x->slots[i].entries);
    }
    zfree(x->slots);
    x->slots = NULL;
    x->cap = 0;
    x->used = 0;
    x->head = NULL;
}

// The `next` of entry `e`, read at its offset (memcpy: the entry's type isn't known here).
static const void *list_index_next(const void *e, size_t next_offset)
{
    const void *next;
    memcpy(&next, (const char *)e + next_offset, sizeof next);
    return next;
}

// Indexes the entries added since the last call: those from `head` to the head seen then.
static void list_index_catch_up(ListIndex *x, const void *head, size_t next_offset,
                                ListIndexName name_of)
{
    if (head == x->head)
    {
        return;
    }
    size_t n = 0;
    const void *e = head;
    while (e && e != x->head)
    {
        n++;
        e = list_index_next(e, next_offset);
    }
    if (e != x->head)
    {
        list_index_clear(x); // the list was replaced: index all of it
        list_index_catch_up(x, head, next_offset, name_of);
        return;
    }
    // Added oldest first, so each name's entries stay in the list's order.
    const void **added = xmalloc((n ? n : 1) * sizeof(void *));
    e = head;
    for (size_t i = 0; i < n; i++)
    {
        added[i] = e;
        e = list_index_next(e, next_offset);
    }
    for (size_t i = n; i-- > 0;)
    {
        const char *name = name_of(added[i]);
        if (name)
        {
            list_index_add(x, name, (void *)added[i]);
        }
    }
    zfree(added);
    x->head = head;
}

void **list_index_all(ListIndex *x, const void *head, size_t next_offset, ListIndexName name_of,
                      const char *name, unsigned *count)
{
    *count = 0;
    list_index_catch_up(x, head, next_offset, name_of);
    if (!name || !x->slots)
    {
        return NULL;
    }
    ListIndexSlot *s = list_index_slot(x->slots, x->cap, name);
    if (!s->name)
    {
        return NULL;
    }
    *count = s->count;
    return s->entries;
}

void *list_index_find(ListIndex *x, const void *head, size_t next_offset, ListIndexName name_of,
                      const char *name)
{
    unsigned n;
    void **all = list_index_all(x, head, next_offset, name_of, name, &n);
    return n ? all[n - 1] : NULL;
}
