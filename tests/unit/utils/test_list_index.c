// SPDX-License-Identifier: MIT
// Unit tests of src/utils/list_index.c: a lookup finds what a walk from the head would.
#include "utils/list_index.h"
#include <stddef.h>
#include <string.h>
#include "unit_test.h"

typedef struct Entry
{
    const char *name;
    int skip; // not to be found (its name_of gives NULL)
    struct Entry *next;
} Entry;

static const char *entry_name(const void *e)
{
    const Entry *x = e;
    return x->skip ? NULL : x->name;
}

static Entry *push(Entry *head, Entry *e)
{
    e->next = head;
    return e;
}

// What the index replaces: the first entry of that name, from the head.
static Entry *walk(Entry *head, const char *name)
{
    for (; head; head = head->next)
    {
        if (!head->skip && strcmp(head->name, name) == 0)
        {
            return head;
        }
    }
    return NULL;
}

static Entry *find(ListIndex *x, Entry *head, const char *name)
{
    return list_index_find(x, head, offsetof(Entry, next), entry_name, name);
}

UTEST(list_index, finds_what_a_walk_finds_as_the_list_grows)
{
    ListIndex x;
    memset(&x, 0, sizeof(x));
    static Entry e[200];
    static char names[200][8];
    Entry *head = NULL;
    ASSERT_TRUE(find(&x, head, "a") == NULL);
    for (int i = 0; i < 200; i++)
    {
        snprintf(names[i], sizeof names[i], "n%d", i % 37); // names repeat: newest wins
        e[i].name = names[i];
        e[i].skip = (i % 11 == 0);
        head = push(head, &e[i]);
        // a lookup after every addition, and one after several, as the parser does
        if (i % 3 == 0)
        {
            for (int k = 0; k < 40; k++)
            {
                char n[8];
                snprintf(n, sizeof n, "n%d", k);
                ASSERT_TRUE(find(&x, head, n) == walk(head, n));
            }
        }
    }
}

UTEST(list_index, gives_every_entry_of_a_name_oldest_first)
{
    ListIndex x;
    memset(&x, 0, sizeof(x));
    Entry a = {"s", 0, NULL}, b = {"t", 0, NULL}, c = {"s", 0, NULL}, d = {"s", 1, NULL};
    Entry *head = push(push(push(push(NULL, &a), &b), &c), &d);
    unsigned n = 0;
    void **all = list_index_all(&x, head, offsetof(Entry, next), entry_name, "s", &n);
    ASSERT_EQ(2u, n);
    ASSERT_TRUE(all[0] == &a);
    ASSERT_TRUE(all[1] == &c);
    Entry e2 = {"s", 0, NULL};
    head = push(head, &e2);
    all = list_index_all(&x, head, offsetof(Entry, next), entry_name, "s", &n);
    ASSERT_EQ(3u, n);
    ASSERT_TRUE(all[2] == &e2);
}

UTEST(list_index, indexes_a_replaced_list_again)
{
    ListIndex x;
    memset(&x, 0, sizeof(x));
    Entry a = {"a", 0, NULL}, b = {"b", 0, NULL};
    Entry *one = push(NULL, &a);
    ASSERT_TRUE(find(&x, one, "a") == &a);
    Entry *two = push(NULL, &b); // another list: "a" is not in it
    ASSERT_TRUE(find(&x, two, "a") == NULL);
    ASSERT_TRUE(find(&x, two, "b") == &b);
    ASSERT_TRUE(find(&x, NULL, "b") == NULL);
}
