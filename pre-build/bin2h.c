
#include "bin2h.h"
#include <stdio.h>

//-------------------------------------------------------------------------------------------------
bool bin2h(const char* filename, const char* variable, const void* buffer, size_t length)
{
    FILE *f = fopen(filename, "wt");
    if (f == NULL)
        return false;

    fprintf(f, "#ifndef __%s__H__\n", variable);
    fprintf(f, "#define __%s__H__\n\n", variable);
    fprintf(f, "#include <stdint.h>\n");
    fprintf(f, "#include <stddef.h>\n\n");
    fprintf(f, "const size_t %s_size = %zu;\n", variable, length);
    fprintf(f, "const uint8_t %s[%zu] =\n{\n    ", variable, length);

    const uint8_t* input = (const uint8_t*)buffer;
    size_t index = 0;

    while (index<length-1)
    {
        fprintf(f, "0x%02X, ", input[index++]);
        if (index%32 == 0)
            fprintf(f, "\n    ");
    }

    fprintf(f, "0x%02X\n};\n#endif\n", input[index]);
    fclose(f);
    return true;
}

//-------------------------------------------------------------------------------------------------
bool string2h(const char* filename, const char* variable, const char* string, size_t length)
{
    FILE *f = fopen(filename, "wt");
    if (f == NULL)
        return false;

    fprintf(f, "#ifndef __%s__H__\n", variable);
    fprintf(f, "#define __%s__H__\n\n", variable);
    fprintf(f, "#include <stddef.h>\n\n");
    fprintf(f, "const size_t %s_size = %zu;\n", variable, length);
    fprintf(f, "const char* %s =\n", variable);

    size_t index = 0;
    while (index<length)
    {
        fprintf(f, "\n    \"");
        while (string[index] != '\n' && index < length)
            putc(string[index++], f);
        
        fprintf(f, "\\n\"");
        index++;
    }

    fprintf(f, "\n;\n#endif\n");
    fclose(f);

    return true;
}

//-------------------------------------------------------------------------------------------------
bool uint2h(const char* filename, const char* variable, const uint32_t* buffer, size_t length)
{
    FILE *f = fopen(filename, "wt");
    if (f == NULL)
        return false;

    fprintf(f, "#ifndef __%s__H__\n", variable);
    fprintf(f, "#define __%s__H__\n\n", variable);
    fprintf(f, "const uint32_t %s[] =\n{\n    ", variable);
    
    size_t index = 0;
    
    while (index<length-1)
    {
        fprintf(f, "0x%08X, ", buffer[index++]);
        if (index%8 == 0)
            fprintf(f, "\n    ");
    }

    fprintf(f, "0x%08X\n};\n#endif\n", buffer[index]);
    fclose(f);
    return true;
}

//-------------------------------------------------------------------------------------------------
bool copy_file(const char *src, const char *dst) 
{
    FILE *in = fopen(src, "rb");
    if (in == NULL)
        return false;

    FILE *out = fopen(dst, "wb");
    if (out==NULL) 
    { 
        fclose(in); 
        return false;
    }

    unsigned char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);

    fclose(in);
    fclose(out);
    return true;
}


