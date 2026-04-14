#include <ctype.h>

int isalpha(int c)
{
    unsigned int ch = (unsigned int)(unsigned char)c;

    return ((ch >= (unsigned int)'A' && ch <= (unsigned int)'Z') ||
            (ch >= (unsigned int)'a' && ch <= (unsigned int)'z')) ? 1 : 0;
}

int isdigit(int c)
{
    unsigned int ch = (unsigned int)(unsigned char)c;

    return (ch >= (unsigned int)'0' && ch <= (unsigned int)'9') ? 1 : 0;
}

int isalnum(int c)
{
    return (isalpha(c) || isdigit(c)) ? 1 : 0;
}

int isspace(int c)
{
    unsigned int ch = (unsigned int)(unsigned char)c;

    return (ch == (unsigned int)' '  ||
            ch == (unsigned int)'\t' ||
            ch == (unsigned int)'\n' ||
            ch == (unsigned int)'\r' ||
            ch == (unsigned int)'\f' ||
            ch == (unsigned int)'\v') ? 1 : 0;
}

int isprint(int c)
{
    unsigned int ch = (unsigned int)(unsigned char)c;

    return (ch >= 32U && ch < 127U) ? 1 : 0;
}

int tolower(int c)
{
    unsigned int ch = (unsigned int)(unsigned char)c;

    if (ch >= (unsigned int)'A' && ch <= (unsigned int)'Z')
        return (int)(ch - (unsigned int)'A' + (unsigned int)'a');
    return c;
}

int toupper(int c)
{
    unsigned int ch = (unsigned int)(unsigned char)c;

    if (ch >= (unsigned int)'a' && ch <= (unsigned int)'z')
        return (int)(ch - (unsigned int)'a' + (unsigned int)'A');
    return c;
}
