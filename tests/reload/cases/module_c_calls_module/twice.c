/* A C file the module links that calls the module back: it can't be an image of its own (the
   module's names are a version's), so the Reloader compiles it with the module, as before. */
int module_value(void);
int lib_twice(void)
{
    return module_value() * 2;
}
