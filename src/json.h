#pragma once

#include "common.h"

enum JsonType {
    JSON_NULL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_BOOLEAN,
    JSON_ARRAY,
    JSON_OBJECT,
};

struct StringJsonPair;

struct Json {
    Json* next;
    JsonType type;
    union {
        f32 number;
        char* string;
        b32 boolean;
        Json* array_first;
        StringJsonPair* object_first;
    };
};

struct StringJsonPair {
    StringJsonPair* next;
    char* string;
    Json* json;
};

Json* parse_json_string(Arena* arena, char* str);

int json_len(Json* j);
Json* json_query(Json* j, char* str);

#define JSON_FOREACH(arr, name) for (Json* name = arr->array_first; name; name = name->next)
