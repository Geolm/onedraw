
#include "bin2h.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

// Reports any failure that happens after a header file has been written, then closes it.
static bool finish_header(FILE* f, const char* filename)
{
    if (fflush(f) != 0 || ferror(f))
    {
        fprintf(stderr, "error: failed to write '%s' (%s)\n", filename, strerror(errno));
        fclose(f);
        return false;
    }

    if (fclose(f) != 0)
    {
        fprintf(stderr, "error: failed to close '%s' (%s)\n", filename, strerror(errno));
        return false;
    }

    return true;
}

// Opens a header file for writing, reporting and returning false on failure.
static FILE* open_header(const char* filename)
{
    FILE* f = fopen(filename, "wt");
    if (f == NULL)
        fprintf(stderr, "error: could not open '%s' for writing (%s)\n", filename, strerror(errno));

    return f;
}

//-------------------------------------------------------------------------------------------------
bool bin2h(const char* filename, const char* variable, const void* buffer, size_t length)
{
    if (length == 0)
    {
        fprintf(stderr, "error: cannot write '%s': the buffer is empty\n", filename);
        return false;
    }

    FILE *f = open_header(filename);
    if (f == NULL)
        return false;

    fprintf(f, "#ifndef __%s__H__\n", variable);
    fprintf(f, "#define __%s__H__\n\n", variable);
    fprintf(f, "#include <stdint.h>\n");
    fprintf(f, "#include <stddef.h>\n\n");
    fprintf(f, "static const size_t %s_size = %zu;\n", variable, length);
    fprintf(f, "static const uint8_t %s[] =\n{\n    ", variable);

    const uint8_t* input = (const uint8_t*)buffer;
    size_t index = 0;

    while (index<length-1)
    {
        fprintf(f, "0x%02X, ", input[index++]);
        if (index%32 == 0)
            fprintf(f, "\n    ");
    }

    fprintf(f, "0x%02X\n};\n#endif\n", input[index]);

    return finish_header(f, filename);
}

//-------------------------------------------------------------------------------------------------
bool string2h(const char* filename, const char* variable, const char* string, size_t length)
{
    if (length == 0)
    {
        fprintf(stderr, "error: cannot write '%s': the buffer is empty\n", filename);
        return false;
    }

    FILE* f = open_header(filename);
    if (f == NULL)
        return false;

    fprintf(f, "#ifndef __%s__H__\n", variable);
    fprintf(f, "#define __%s__H__\n\n", variable);
    fprintf(f, "#include <stddef.h>\n\n");
    fprintf(f, "static const size_t %s_size = %zu;\n", variable, length);
    fprintf(f, "static const char %s[] =\n", variable);

    const unsigned char* bytes = (const unsigned char*)string;
    size_t i = 0;

    while (i < length)
    {
        fprintf(f, "    \"");
        while (i < length && bytes[i] != '\n')
        {
            unsigned char c = bytes[i++];
            switch (c)
            {
                case '\"': fprintf(f, "\\\""); break;
                case '\\': fprintf(f, "\\\\"); break;
                case '\r': fprintf(f, "\\r"); break;
                case '\t': fprintf(f, "\\t"); break;
                default:
                    if (isprint(c))
                        putc(c, f);
                    else
                        fprintf(f, "\\x%02X", c);
                    break;
            }
        }

        fprintf(f, "\\n\"\n");
        if (i < length && bytes[i] == '\n')
            i++;
    }

    fprintf(f, ";\n\n#endif // __%s__H__\n", variable);

    return finish_header(f, filename);
}


//-------------------------------------------------------------------------------------------------
bool uint2h(const char* filename, const char* variable, const uint32_t* buffer, size_t length)
{
    if (length == 0)
    {
        fprintf(stderr, "error: cannot write '%s': the buffer is empty\n", filename);
        return false;
    }

    FILE *f = open_header(filename);
    if (f == NULL)
        return false;

    fprintf(f, "#ifndef __%s__H__\n", variable);
    fprintf(f, "#define __%s__H__\n\n", variable);
    fprintf(f, "#include <stdint.h>\n");
    fprintf(f, "#include <stddef.h>\n\n");
    fprintf(f, "static const size_t %s_size = %zu;\n", variable, length);
    fprintf(f, "static const uint32_t %s[] =\n{\n    ", variable);

    size_t index = 0;

    while (index<length-1)
    {
        fprintf(f, "0x%08X, ", buffer[index++]);
        if (index%8 == 0)
            fprintf(f, "\n    ");
    }

    fprintf(f, "0x%08X\n};\n#endif\n", buffer[index]);

    return finish_header(f, filename);
}

//-------------------------------------------------------------------------------------------------
bool copy_file(const char *src, const char *dst) 
{
    FILE *in = fopen(src, "rb");
    if (in == NULL)
    {
        fprintf(stderr, "error: could not open '%s' (%s)\n", src, strerror(errno));
        return false;
    }

    FILE *out = fopen(dst, "wb");
    if (out==NULL) 
    { 
        fprintf(stderr, "error: could not open '%s' for writing (%s)\n", dst, strerror(errno));
        fclose(in);
        return false;
    }

    unsigned char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    {
        if (fwrite(buf, 1, n, out) != n)
        {
            fprintf(stderr, "error: failed to write '%s' (%s)\n", dst, strerror(errno));
            fclose(in);
            fclose(out);
            return false;
        }
    }

    if (ferror(in))
    {
        fprintf(stderr, "error: failed to read '%s' (%s)\n", src, strerror(errno));
        fclose(in);
        fclose(out);
        return false;
    }

    if (fclose(in) != 0 || fclose(out) != 0)
    {
        fprintf(stderr, "error: failed to close file (%s)\n", strerror(errno));
        return false;
    }

    return true;
}
