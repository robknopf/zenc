/* A library a module links (@link): compiled once for every version, so its state lasts. */
int lib_count(void)
{
    static int n;
    return ++n;
}
