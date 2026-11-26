// SPDX-License-Identifier: MIT
#ifndef ZC_LIST_INDEX_H
#define ZC_LIST_INDEX_H

#include <stddef.h>

// An index by name of a linked list that only grows at its head (`e->next = head; head =
// e`), whose entries' names don't change once they are in it. Finding an entry by walking
// such a list costs a walk of everything before it, and a program of thousands of types and
// templates makes thousands of such walks (O(n^2): about 60% of zc's time at 3000 modules).
//
// A lookup answers what the walk from the head would: the newest entry of that name. Before
// answering, the index catches up with the list by walking from its current head to the
// head it saw last, so the code that adds entries needs no change. If that head isn't found
// (the list was replaced), it indexes the whole list again.
//
// `name_of` gives an entry's name, or NULL for one the lookup must not find (the walk it
// replaces skips it). `next_offset` is offsetof(Entry, next).

typedef const char *(*ListIndexName)(const void *entry);

typedef struct ListIndexSlot
{
    const char *name; // NULL: empty
    void **entries;   // every entry of this name, oldest first
    unsigned count, cap;
} ListIndexSlot;

typedef struct ListIndex
{
    const void *head; // the list's head when the index last caught up
    ListIndexSlot *slots;
    unsigned cap, used;
} ListIndex;

// The newest entry named `name` in the list starting at `head`, or NULL.
void *list_index_find(ListIndex *x, const void *head, size_t next_offset, ListIndexName name_of,
                      const char *name);

// Every entry named `name`, oldest first: *count of them (a walk from the head meets them
// newest first). The array is the index's own and changes when the list grows; take *count
// before calling anything that can add to the list.
void **list_index_all(ListIndex *x, const void *head, size_t next_offset, ListIndexName name_of,
                      const char *name, unsigned *count);

#endif
