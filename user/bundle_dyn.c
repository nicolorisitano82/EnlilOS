const char bundle_dyn_text[] = "bundle-lib-ok\n";

unsigned long dyn_len(void)
{
    return sizeof(bundle_dyn_text) - 1U;
}

const char *dyn_msg(void)
{
    return bundle_dyn_text;
}
