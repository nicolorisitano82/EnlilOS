const char dyn_text[] = "[EL0] dynamic ELF avviato correttamente\n";

unsigned long dyn_len(void)
{
    return sizeof(dyn_text) - 1U;
}

const char *dyn_msg(void)
{
    return dyn_text;
}
