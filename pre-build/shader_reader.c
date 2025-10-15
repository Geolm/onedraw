#include "shader_reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#define IS_QUOTE(x) ((x=='\"')||(x=='<')||(x=='>'))
#define INCLUDE_TAG "#include"

//----------------------------------------------------------------------------------------------------------------------------
char* read_shader(const char* filename)
{
    FILE* f = fopen(filename, "r");
    if (f != NULL)
    {
        fseek(f, 0, SEEK_END);
        long filesize = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        char* buffer = (char*) malloc(filesize +1);
        fread(buffer, filesize, 1, f);
        buffer[filesize] = 0;
        fclose(f);
        return buffer;
    }
    return NULL;
}

//----------------------------------------------------------------------------------------------------------------------------
static inline char* string_concat(const char* a, size_t strlen_a, const char* b, size_t strlen_b)
{
    char* output = (char*) malloc(strlen_a + strlen_b + 1);
    char* ptr = output;

    for(size_t i=0; i<strlen_a; i++)
        *(ptr++) = a[i];

    for(size_t i=0; i<strlen_b; i++)
        *(ptr++) = b[i];

    *ptr = 0;

    return output;
}

//----------------------------------------------------------------------------------------------------------------------------
char* remplace_include(char* shader_buffer, const char* include_buffer, char* include, char* end_of_include, char** next)
{
    char* new_buffer = (char*) malloc(strlen(shader_buffer) + strlen(include_buffer) + 1 - (end_of_include - include));
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

//----------------------------------------------------------------------------------------------------------------------------
char* read_shader_include(const char* include_path, const char* filename)
{
    size_t include_path_length = strlen(include_path);
    char* shader_full = string_concat(include_path, include_path_length, filename, strlen(filename));
    char* buffer = read_shader(shader_full);

    if (buffer != NULL)
    {
        // no fancy lexer or parser : we just look for #include string as the code is assumed to be a (simple) shader 
        // if we can open the included file we remplace the include statement with the code of the included file
        // otherwise we don't do anything
        char* include = strstr(buffer, INCLUDE_TAG);
        char* current = include;

        while (include != NULL)
        {
            while (!IS_QUOTE(*current)) current++;

            char* include_filename = ++current;
            
            while (!IS_QUOTE(*current)) current++;

            size_t include_filename_length = current - include_filename;
            char* include_full = string_concat(include_path, include_path_length, include_filename, include_filename_length);
            char* include_buffer = read_shader(include_full);
            current++;

            if (include_buffer != NULL)
            {
                char* after_include;
                char* new_buffer = remplace_include(buffer, include_buffer, include, current, &after_include);
                
                free(buffer);
                free(include_buffer);

                buffer = new_buffer;
                current = after_include;
            }

            free(include_full);

            include = strstr(current, INCLUDE_TAG);
            current = include;
        }
    }
    
    free(shader_full);
    return buffer;
}
