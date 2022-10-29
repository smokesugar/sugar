#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

enum TokenType {
    TOKEN_ERROR,
    TOKEN_EOF,
    TOKEN_NULL,
    TOKEN_COLON,
    TOKEN_COMMA,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LSQUARE,
    TOKEN_RSQUARE,
    TOKEN_BOOLEAN,
    TOKEN_NUMBER,
    TOKEN_STRING,
};

struct Token {
    TokenType type;
    char* ptr;
    int len;
    int line;
};

struct Lexer {
    char* ptr;
    int line;
};

internal char char_advance(Lexer* l) {
    char c = *l->ptr;

    if (c != '\0') {
        ++l->ptr;
    }

    if (c == '\n') {
        ++l->line;
    }

    return c;
}

internal TokenType check_keyword(char* start, char* keyword, Lexer* l, TokenType type) {
    int len = (int)strlen(keyword);
    if (strncmp(start, keyword, len) == 0) {
        l->ptr = start + len;
        return type;
    }
    return TOKEN_ERROR;
}

internal Token token_advance(Lexer* l) {
    while (isspace(*l->ptr)) {
        char_advance(l);
    }

    char* start = l->ptr;
    int line = l->line;
    char c = char_advance(l);

    TokenType type = TOKEN_ERROR;

    switch (c) {
        case '\0': type = TOKEN_EOF; break;
        case ':': type = TOKEN_COLON; break;
        case ',': type = TOKEN_COMMA; break;
        case '{': type = TOKEN_LBRACE; break;
        case '}': type = TOKEN_RBRACE; break;
        case '[': type = TOKEN_LSQUARE; break;
        case ']': type = TOKEN_RSQUARE; break;
        case 't':
            type = check_keyword(start, "true", l, TOKEN_BOOLEAN);
            break;
        case 'f':
            type = check_keyword(start, "false", l, TOKEN_BOOLEAN);
            break;
        case 'n':
            type = check_keyword(start, "null", l, TOKEN_NULL);
            break;
        case '"': {
            while (*l->ptr != '"' && *l->ptr != '\0') {
                char_advance(l);
            }
            
            if (*l->ptr == '\0') {
                system_message_box("Unterminated json string");
                assert(false);
            }

            char_advance(l);

            type = TOKEN_STRING;
        } break;
        default:
            if (isdigit(c) || c == '-') {
                (void)strtof(start, &l->ptr);
                type = TOKEN_NUMBER;
            }
    }

    Token tok;
    tok.type = type;
    tok.ptr = start;
    tok.len = (int)(l->ptr - start);
    tok.line = line;

    return tok;
}

internal Token token_peek(Lexer* l) {
    Lexer temp = *l;
    return token_advance(&temp);
}

internal Json* new_json(Arena* arena, JsonType type) {
    Json* j = arena_push_struct_zero(arena, Json);
    j->type = type;
    return j;
}

internal char* extract_token_string(Arena* arena, Token tok) {
    assert(tok.type == TOKEN_STRING);
    int len = tok.len - 2;
    char* str = (char*)arena_push(arena, len + 1);
    memcpy(str, tok.ptr + 1, len);
    str[len] = '\0';
    return str;
}

internal Token token_match(Lexer* l, TokenType type) {
    Token tok = token_advance(l);
    if (tok.type != type) {
        system_message_box("Unexpected token '%.*s' (line %d)", tok.len, tok.ptr, tok.line);
    }
    return tok;
}

internal Json* parse(Arena* arena, Lexer* l) {
    Token tok = token_advance(l);

    switch (tok.type) {
        case TOKEN_NULL:
            return new_json(arena, JSON_NULL);
        case TOKEN_BOOLEAN: {
            Json* j = new_json(arena, JSON_BOOLEAN);
            j->boolean = tok.ptr[0] == 't';
            return j;
        }
        case TOKEN_NUMBER: {
            Json* j = new_json(arena, JSON_NUMBER);
            j->number = strtof(tok.ptr, 0);
            return j;
        }
        case TOKEN_STRING: {
            Json* j = new_json(arena, JSON_STRING);
            j->string = extract_token_string(arena, tok);
            return j;
        }
        case TOKEN_LSQUARE: {
            Json head = {};
            Json* cur = &head;

            bool first = true;

            while (token_peek(l).type != TOKEN_RSQUARE && token_peek(l).type != TOKEN_EOF) {
                if (first) {
                    first = false;
                }
                else {
                    token_match(l, TOKEN_COMMA);
                }

                cur->next = parse(arena, l);
                cur = cur->next;
            }

            token_match(l, TOKEN_RSQUARE);

            Json* j = new_json(arena, JSON_ARRAY);
            j->array_first = head.next;

            return j;
        }
        case TOKEN_LBRACE: {
            StringJsonPair head = {};
            StringJsonPair* cur = &head;

            bool first = true;

            while (token_peek(l).type != TOKEN_RBRACE && token_peek(l).type != TOKEN_EOF) {
                if (first) {
                    first = false;
                }
                else {
                    token_match(l, TOKEN_COMMA);
                }

                char* string = extract_token_string(arena, token_match(l, TOKEN_STRING));
                token_match(l, TOKEN_COLON);
                Json* json = parse(arena, l);

                StringJsonPair* pair = arena_push_struct_zero(arena, StringJsonPair);
                pair->string = string;
                pair->json = json;

                cur->next = pair;
                cur = cur->next;
            }

            token_match(l, TOKEN_RBRACE);

            Json* j = new_json(arena, JSON_OBJECT);
            j->object_first = head.next;

            return j;
        };
        default:
            system_message_box("Unrecognised token '%.*s' (line %d).", tok.len, tok.ptr, tok.line);
            assert(false);
            return 0;
    }
}

Json* parse_json_string(Arena* arena, char* str) {
    Lexer lexer;
    lexer.line = 1;
    lexer.ptr = str;

    return parse(arena, &lexer);
}

int json_len(Json* j) {
    assert(j->type == JSON_ARRAY);
    
    int len = 0;
    for (Json* n = j->array_first; n; n = n->next) {
        ++len;
    }

    return len;
};

Json* json_query(Json* j, char* str) {
    assert(j->type == JSON_OBJECT);
    
    for (StringJsonPair* pair = j->object_first; pair; pair = pair->next) {
        if (strcmp(pair->string, str) == 0) {
            return pair->json;
        }
    }

    return 0;
}
