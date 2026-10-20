#include "shader_reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

#define IS_QUOTE(x) ((x=='\"')||(x=='<')||(x=='>'))
#define INCLUDE_TAG "#include"

//----------------------------------------------------------------------------------------------------------------------------
char* read_shader(Arena* arena, const char* filename)
{
    FILE* f = fopen(filename, "r");
    if (f != NULL)
    {
        fseek(f, 0, SEEK_END);
        long filesize = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (filesize < 0 || (size_t)filesize == 0)
        {
            fprintf(stderr, "error: '%s' is empty\n", filename);
            fclose(f);
            return NULL;
        }

        char* buffer = (char*) arena_alloc(arena, (size_t)filesize +1);
        if (fread(buffer, (size_t)filesize, 1, f) != 1)
        {
            fprintf(stderr, "error: failed to read '%s'\n", filename);
            fclose(f);
            return NULL;
        }

        buffer[filesize] = 0;
        fclose(f);
        return buffer;
    }

    fprintf(stderr, "error: could not find '%s'\n", filename);
    return NULL;
}

//----------------------------------------------------------------------------------------------------------------------------
static inline char* string_concat(Arena* arena, const char* a, size_t strlen_a, const char* b, size_t strlen_b)
{
    char* output = (char*) arena_alloc(arena, strlen_a + strlen_b + 1);
    char* ptr = output;

    for(size_t i=0; i<strlen_a; i++)
        *(ptr++) = a[i];

    for(size_t i=0; i<strlen_b; i++)
        *(ptr++) = b[i];

    *ptr = 0;

    return output;
}

//----------------------------------------------------------------------------------------------------------------------------
char* remplace_include(Arena* arena, char* shader_buffer, const char* include_buffer, char* include, char* end_of_include, char** next)
{
    char* new_buffer = (char*) arena_alloc(arena, strlen(shader_buffer) + strlen(include_buffer) + 1 - (end_of_include - include));
    char* output = new_buffer;

    while (shader_buffer < include)
        *(output++) = *(shader_buffer++);

    while (*include_buffer != 0)
        *(output++) = *(include_buffer++);

    *next = output;

    while (*end_of_include != 0)
        *(output++) = *(end_of_include++);

    *output = 0;
    return new_buffer;
}

// returns true if `line` contains only whitespace characters
static bool is_whitespace_only(const char* line, size_t length)
{
    for (size_t i=0; i<length; i++)
        if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
            return false;

    return true;
}

// given the position of the #include tag, locate the opening delimiter.
// returns a pointer to the opening delimiter, or NULL if none is found before end of string.
static char* find_include_delimiter(const char* include)
{
    const char* p = include + strlen(INCLUDE_TAG);
    while (*p != 0 && *p != '\'' && *p != '"' && *p != '<')
        p++;

    if (*p == 0)
        return NULL;

    return (char*)p;
}

// returns true if the #include at `include` is a real directive (at the start of a line, only whitespace before it)
static bool is_include_directive(const char* buffer, const char* include)
{
    const char* line_start = include;
    while (line_start > buffer && line_start[-1] != '\n')
        line_start--;

    return is_whitespace_only(line_start, (size_t)(include - line_start));
}

//----------------------------------------------------------------------------------------------------------------------------
char* read_shader_include(Arena* arena, const char* include_path, const char* filename)
{
    size_t include_path_length = strlen(include_path);
    char* shader_full = string_concat(arena, include_path, include_path_length, filename, strlen(filename));
    char* buffer = read_shader(arena, shader_full);

    if (buffer == NULL)
        return NULL; // read_shader reported the error

    // no fancy lexer or parser : we just look for #include directives, as the code is assumed to be a (simple) shader
    // - #include "file"   : a project file, inlined here. a missing file fails the build.
    // - #include <header> : a system header (e.g. metal_stdlib), left untouched, it is resolved by the compiler
    char* pos = buffer;
    char* include = strstr(pos, INCLUDE_TAG);

    while (include != NULL)
    {
        if (!is_include_directive(buffer, include))
        {
            // not a real directive (e.g. the word "include" inside a comment or string), keep scanning
            pos = include + strlen(INCLUDE_TAG);
            include = strstr(pos, INCLUDE_TAG);
            continue;
        }

        char* open = find_include_delimiter(include);
        if (open == NULL)
            break; // malformed directive, nothing to do

        if (*open != '"')
        {
            // <system header> (or a 'old-style' include): skip past the matching closing delimiter and keep going
            char close = (*open == '<') ? '>' : '\'';
            char* end = open + 1;
            while (*end != 0 && *end != close)
                end++;

            if (*end == 0)
                break; // malformed

            pos = end + 1;
            include = strstr(pos, INCLUDE_TAG);
            continue;
        }

        // double-quoted include : a project file that must exist
        char* include_filename = open + 1;
        char* end = open + 1;
        while (*end != 0 && *end != '"')
            end++;

        if (*end == 0)
        {
            fprintf(stderr, "error: malformed #include in %s\n", filename);
            return NULL;
        }

        size_t include_filename_length = end - include_filename;
        char* include_full = string_concat(arena, include_path, include_path_length, include_filename, include_filename_length);
        char* include_buffer = read_shader(arena, include_full);
        end++;

        if (include_buffer == NULL)
        {
            fprintf(stderr, "error: could not resolve include \"%s%s\" in %s\n", include_path, include_filename, filename);
            return NULL; // a shader with a missing project include is broken, fail the whole build
        }

        char* after_include;
        char* new_buffer = remplace_include(arena, buffer, include_buffer, include, end, &after_include);

        buffer = new_buffer;
        pos = after_include;

        include = strstr(pos, INCLUDE_TAG);
    }

    return buffer;
}
