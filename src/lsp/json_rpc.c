#include "json_rpc.h"
#include "cJSON.h"
#include "lsp_project.h"
#include "lsp_formatter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lsp_check_file(const char *uri, const char *src, int id);
void lsp_goto_definition(const char *uri, int line, int col, int id);
void lsp_hover(const char *uri, int line, int col, int id);
void lsp_completion(const char *uri, int line, int col, int id);
void lsp_document_symbol(const char *uri, int id);
void lsp_references(const char *uri, int line, int col, int id);
// Prototype
void lsp_signature_help(const char *uri, int line, int col, int id);
void lsp_rename(const char *uri, int line, int col, const char *new_name, int id);
void lsp_code_action(const char *uri, cJSON *diagnostics, int id);

static int lsp_position_to_offset(const char *src, int line, int character, size_t *out_offset)
{
    if (!src || !out_offset || line < 0 || character < 0)
    {
        return 0;
    }

    const char *p = src;
    int current_line = 0;
    while (*p && current_line < line)
    {
        if (*p == '\n')
        {
            current_line++;
        }
        p++;
    }

    if (current_line != line)
    {
        return 0;
    }

    int current_char = 0;
    while (*p && *p != '\n' && current_char < character)
    {
        p++;
        current_char++;
    }

    *out_offset = (size_t)(p - src);
    return 1;
}

static char *lsp_apply_change(const char *base, cJSON *change)
{
    if (!base || !change)
    {
        return NULL;
    }

    cJSON *text = cJSON_GetObjectItem(change, "text");
    if (!text || !text->valuestring)
    {
        return NULL;
    }

    cJSON *range = cJSON_GetObjectItem(change, "range");
    if (!range)
    {
        return strdup(text->valuestring);
    }

    cJSON *start = cJSON_GetObjectItem(range, "start");
    cJSON *end = cJSON_GetObjectItem(range, "end");
    if (!start || !end)
    {
        return NULL;
    }

    cJSON *start_line = cJSON_GetObjectItem(start, "line");
    cJSON *start_char = cJSON_GetObjectItem(start, "character");
    cJSON *end_line = cJSON_GetObjectItem(end, "line");
    cJSON *end_char = cJSON_GetObjectItem(end, "character");
    if (!start_line || !start_char || !end_line || !end_char)
    {
        return NULL;
    }

    size_t start_offset = 0;
    size_t end_offset = 0;
    if (!lsp_position_to_offset(base, start_line->valueint, start_char->valueint, &start_offset) ||
        !lsp_position_to_offset(base, end_line->valueint, end_char->valueint, &end_offset) ||
        end_offset < start_offset)
    {
        return NULL;
    }

    size_t base_len = strlen(base);
    size_t repl_len = strlen(text->valuestring);
    if (end_offset > base_len)
    {
        return NULL;
    }

    size_t out_len = start_offset + repl_len + (base_len - end_offset);
    char *out = malloc(out_len + 1);
    if (!out)
    {
        return NULL;
    }

    memcpy(out, base, start_offset);
    memcpy(out + start_offset, text->valuestring, repl_len);
    memcpy(out + start_offset + repl_len, base + end_offset, base_len - end_offset);
    out[out_len] = '\0';

    return out;
}

// Helper to extract textDocument params
static void get_params(cJSON *root, char **uri, int *line, int *col)
{
    cJSON *params = cJSON_GetObjectItem(root, "params");

    if (!params)
    {
        return;
    }

    cJSON *doc = cJSON_GetObjectItem(params, "textDocument");
    if (doc)
    {
        cJSON *u = cJSON_GetObjectItem(doc, "uri");
        if (u && u->valuestring)
        {
            *uri = strdup(u->valuestring);
        }
    }

    cJSON *pos = cJSON_GetObjectItem(params, "position");
    if (pos)
    {
        cJSON *l = cJSON_GetObjectItem(pos, "line");
        cJSON *c = cJSON_GetObjectItem(pos, "character");
        if (l)
        {
            *line = l->valueint;
        }
        if (c)
        {
            *col = c->valueint;
        }
    }
}

void handle_request(const char *json_str)
{
    cJSON *json = cJSON_Parse(json_str);
    if (!json)
    {
        return;
    }

    int id = 0;
    cJSON *id_item = cJSON_GetObjectItem(json, "id");
    if (id_item)
    {
        id = id_item->valueint;
    }

    cJSON *method_item = cJSON_GetObjectItem(json, "method");
    if (!method_item || !method_item->valuestring)
    {
        cJSON_Delete(json);
        return;
    }
    char *method = method_item->valuestring;

    if (strcmp(method, "initialize") == 0)
    {
        cJSON *params = cJSON_GetObjectItem(json, "params");
        char *root = NULL;
        if (params)
        {
            cJSON *rp = cJSON_GetObjectItem(params, "rootPath");
            if (rp && rp->valuestring)
            {
                root = strdup(rp->valuestring);
            }
            else
            {
                cJSON *ru = cJSON_GetObjectItem(params, "rootUri");
                if (ru && ru->valuestring)
                {
                    root = strdup(ru->valuestring);
                }
            }
        }

        if (root && strncmp(root, "file://", 7) == 0)
        {
            char *clean = strdup(root + 7);
            free(root);
            root = clean;
        }

        lsp_project_init(root ? root : ".");
        if (root)
        {
            free(root);
        }

        const char *response =
            "{\"jsonrpc\":\"2.0\",\"id\":0,\"result\":{"
            "\"serverInfo\":{\"name\":\"ZenC LS\",\"version\": \"1.0.0\"},"
            "\"capabilities\":{\"textDocumentSync\":{\"openClose\":true,\"change\":2},"
            "\"definitionProvider\":true,\"hoverProvider\":true,"
            "\"referencesProvider\":true,\"documentSymbolProvider\":true,"
            "\"renameProvider\":true,\"codeActionProvider\":true,"
            "\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\"]},"
            "\"completionProvider\":{"
            "\"triggerCharacters\":[\".\"]},"
            "\"semanticTokensProvider\":{\"legend\":{\"tokenTypes\":[\"variable\",\"function\","
            "\"struct\",\"keyword\",\"string\",\"number\",\"comment\",\"type\",\"enum\",\"member\","
            "\"operator\",\"parameter\",\"macro\",\"typeParameter\"],\"tokenModifiers\":["
            "\"declaration\",\"definition\",\"readonly\","
            "\"static\",\"deprecated\",\"abstract\",\"async\",\"modification\",\"documentation\","
            "\"defaultLibrary\"]},\"full\":true}"
            "}}}";

        // Dynamically construct response with correct ID
        cJSON *res_json = cJSON_Parse(response);
        if (!res_json)
        {
            fprintf(stderr, "zls: Failed to construct initialize response\n");
            return;
        }
        cJSON_DeleteItemFromObject(res_json, "id");
        cJSON_AddNumberToObject(res_json, "id", id);

        char *str = cJSON_PrintUnformatted(res_json);
        fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", strlen(str), str);
        fflush(stdout);
        free(str);
        cJSON_Delete(res_json);
        fflush(stdout);
    }
    else if (strcmp(method, "initialized") == 0)
    {
        lsp_project_index_workspace();
    }
    else if (strcmp(method, "textDocument/didOpen") == 0 ||
             strcmp(method, "textDocument/didChange") == 0)
    {
        cJSON *params = cJSON_GetObjectItem(json, "params");
        if (params)
        {
            cJSON *doc = cJSON_GetObjectItem(params, "textDocument");
            if (doc)
            {
                cJSON *uri = cJSON_GetObjectItem(doc, "uri");
                cJSON *text = cJSON_GetObjectItem(doc, "text");
                if (!uri || !uri->valuestring)
                {
                    cJSON_Delete(json);
                    return;
                }

                if (strcmp(method, "textDocument/didOpen") == 0)
                {
                    if (text && text->valuestring)
                    {
                        lsp_check_file(uri->valuestring, text->valuestring, id);
                    }
                }
                else
                {
                    cJSON *changes = cJSON_GetObjectItem(params, "contentChanges");
                    int change_count = (changes && cJSON_IsArray(changes)) ? cJSON_GetArraySize(changes) : 0;
                    if (change_count <= 0)
                    {
                        cJSON_Delete(json);
                        return;
                    }

                    ProjectFile *pf = lsp_project_get_file(uri->valuestring);
                    char *current =
                        (pf && pf->source) ? strdup(pf->source) : ((text && text->valuestring)
                                                                       ? strdup(text->valuestring)
                                                                       : strdup(""));

                    for (int i = 0; i < change_count; i++)
                    {
                        cJSON *change = cJSON_GetArrayItem(changes, i);
                        char *next = lsp_apply_change(current, change);
                        if (!next)
                        {
                            cJSON *fallback_text = change ? cJSON_GetObjectItem(change, "text") : NULL;
                            if (fallback_text && fallback_text->valuestring)
                            {
                                next = strdup(fallback_text->valuestring);
                            }
                        }

                        if (next)
                        {
                            free(current);
                            current = next;
                        }
                    }

                    if (current)
                    {
                        lsp_check_file(uri->valuestring, current, id);
                        free(current);
                    }
                }
            }
        }
    }
    else if (strcmp(method, "textDocument/definition") == 0)
    {
        char *uri = NULL;
        int line = 0, col = 0;
        get_params(json, &uri, &line, &col);
        if (uri)
        {
            lsp_goto_definition(uri, line, col, id);
            free(uri);
        }
    }
    else if (strcmp(method, "textDocument/hover") == 0)
    {
        char *uri = NULL;
        int line = 0, col = 0;
        get_params(json, &uri, &line, &col);
        if (uri)
        {
            lsp_hover(uri, line, col, id);
            free(uri);
        }
    }
    else if (strcmp(method, "textDocument/completion") == 0)
    {
        char *uri = NULL;
        int line = 0, col = 0;
        get_params(json, &uri, &line, &col);
        if (uri)
        {
            lsp_completion(uri, line, col, id);
            free(uri);
        }
    }
    else if (strcmp(method, "textDocument/documentSymbol") == 0)
    {
        char *uri = NULL;
        int line = 0, col = 0; // Unused for outline
        get_params(json, &uri, &line, &col);
        if (uri)
        {
            lsp_document_symbol(uri, id);
            free(uri);
        }
    }
    else if (strcmp(method, "textDocument/references") == 0)
    {
        char *uri = NULL;
        int line = 0, col = 0;
        get_params(json, &uri, &line, &col);
        if (uri)
        {
            lsp_references(uri, line, col, id);
            free(uri);
        }
    }
    else if (strcmp(method, "textDocument/signatureHelp") == 0)
    {
        char *uri = NULL;
        int line = 0, col = 0;
        get_params(json, &uri, &line, &col);
        if (uri)
        {
            lsp_signature_help(uri, line, col, id);
            free(uri);
        }
    }
    else if (strcmp(method, "textDocument/codeAction") == 0)
    {
        cJSON *params = cJSON_GetObjectItem(json, "params");
        if (params)
        {
            cJSON *uri_obj = cJSON_GetObjectItem(params, "textDocument");
            cJSON *uri = cJSON_GetObjectItem(uri_obj, "uri");
            cJSON *context = cJSON_GetObjectItem(params, "context");
            cJSON *diagnostics = cJSON_GetObjectItem(context, "diagnostics");

            if (uri && diagnostics)
            {
                lsp_code_action(uri->valuestring, diagnostics, id);
            }
        }
    }
    else if (strcmp(method, "textDocument/semanticTokens/full") == 0)
    {
        cJSON *params = cJSON_GetObjectItem(json, "params");
        cJSON *doc = cJSON_GetObjectItem(params, "textDocument");
        if (doc)
        {
            cJSON *uri_item = cJSON_GetObjectItem(doc, "uri");
            if (uri_item && uri_item->valuestring)
            {
                char *resp = lsp_semantic_tokens_full(uri_item->valuestring);
                if (resp)
                {
                    cJSON *res_json = cJSON_CreateObject();
                    cJSON_AddStringToObject(res_json, "jsonrpc", "2.0");
                    cJSON_AddNumberToObject(res_json, "id", id);
                    cJSON *result = cJSON_Parse(resp);
                    if (result)
                    {
                        cJSON_AddItemToObject(res_json, "result", result);
                    }
                    else
                    {
                        // fallback empty
                        cJSON_AddItemToObject(res_json, "result", cJSON_CreateObject());
                    }
                    free(resp);

                    char *str = cJSON_PrintUnformatted(res_json);
                    fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", strlen(str), str);
                    fflush(stdout);
                    free(str);
                    cJSON_Delete(res_json);
                }
            }
        }
    }
    else if (strcmp(method, "textDocument/rename") == 0)
    {
        char *uri = NULL;
        int line = 0, col = 0;
        get_params(json, &uri, &line, &col);

        cJSON *params = cJSON_GetObjectItem(json, "params");
        cJSON *nn = cJSON_GetObjectItem(params, "newName");
        char *new_name = nn ? nn->valuestring : NULL;

        if (uri && new_name)
        {
            lsp_rename(uri, line, col, new_name, id);
            free(uri);
        }
    }
    else if (strcmp(method, "textDocument/formatting") == 0)
    {
        cJSON *params = cJSON_GetObjectItem(json, "params");
        cJSON *doc = cJSON_GetObjectItem(params, "textDocument");
        if (doc)
        {
            cJSON *uri_item = cJSON_GetObjectItem(doc, "uri");
            if (uri_item && uri_item->valuestring)
            {
                ProjectFile *pf = lsp_project_get_file(uri_item->valuestring);
                if (pf && pf->source)
                {
                    char *formatted = lsp_format_source(pf->source);
                    if (formatted)
                    {
                        cJSON *res_json = cJSON_CreateObject();
                        cJSON_AddStringToObject(res_json, "jsonrpc", "2.0");
                        cJSON_AddNumberToObject(res_json, "id", id);

                        cJSON *result = cJSON_CreateArray();
                        cJSON *edit = cJSON_CreateObject();

                        // Replace whole document
                        cJSON *range = cJSON_CreateObject();
                        cJSON *start = cJSON_CreateObject();
                        cJSON_AddNumberToObject(start, "line", 0);
                        cJSON_AddNumberToObject(start, "character", 0);

                        // Count lines in source
                        int lines = 0;
                        const char *p = pf->source;
                        while (*p)
                        {
                            if (*p == '\n')
                            {
                                lines++;
                            }
                            p++;
                        }

                        cJSON *end = cJSON_CreateObject();
                        cJSON_AddNumberToObject(end, "line", lines + 1);
                        cJSON_AddNumberToObject(end, "character", 0);

                        cJSON_AddItemToObject(range, "start", start);
                        cJSON_AddItemToObject(range, "end", end);
                        cJSON_AddItemToObject(edit, "range", range);
                        cJSON_AddStringToObject(edit, "newText", formatted);

                        cJSON_AddItemToArray(result, edit);
                        cJSON_AddItemToObject(res_json, "result", result);

                        char *str = cJSON_PrintUnformatted(res_json);
                        fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", strlen(str), str);
                        fflush(stdout);
                        free(str);
                        cJSON_Delete(res_json);
                        free(formatted);
                    }
                }
            }
        }
    }
    else if (strcmp(method, "shutdown") == 0)
    {
        cJSON *res_json = cJSON_CreateObject();
        cJSON_AddStringToObject(res_json, "jsonrpc", "2.0");
        cJSON_AddNumberToObject(res_json, "id", id);
        cJSON_AddNullToObject(res_json, "result");

        char *str = cJSON_PrintUnformatted(res_json);
        fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", strlen(str), str);
        fflush(stdout);
        free(str);
        cJSON_Delete(res_json);
    }
    else if (strcmp(method, "exit") == 0)
    {
        // For notification, no ID. Just exit.
        exit(0);
    }

    cJSON_Delete(json);
}
