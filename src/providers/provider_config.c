/* SPDX-License-Identifier: MIT */
#include "providers/provider_config.h"

#include <ctype.h>
#include <errno.h>
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "diag.h"
#include "trace.h"
#include "xalloc.h"
#include "system/fs.h"
#include "system/path.h"
#include "system/rand.h"
#include "system/spawn.h"
#include "text/placeholder.h"

#define SESSION_PLACEHOLDER "session_id"

/* A key source should hold one line; the cap only stops a mistyped path from streaming a whole
 * file, and the command timeout a credential helper from stalling startup. */
#define API_KEY_MAX_BYTES          (64 * 1024)
#define API_KEY_COMMAND_TIMEOUT_MS 10000

#define FIELD_ANY (PROVIDER_FIELD_OPENAI | PROVIDER_FIELD_ANTHROPIC)
/* Responses fixes its reasoning shape and round-trip on the wire and never sends explicit cache
 * markers, so those knobs apply to Chat Completions only. */
#define FIELD_CACHE_MARKERS (PROVIDER_FIELD_OPENAI_CHAT | PROVIDER_FIELD_ANTHROPIC)

// clang-format off
static const struct provider_field PROVIDER_FIELDS[] = {
    /* `api` is resolved for every def — pinned ones warn about it in resolve_wire rather than
     * here, so the message can say the value is pinned instead of merely unused. */
    {.leaf = "api",                 .classes = FIELD_ANY},
    {.leaf = "base_url",            .classes = FIELD_ANY},
    {.leaf = "port",                .classes = PROVIDER_FIELD_PORT_TEMPLATED},
    {.leaf = "api_key",             .classes = PROVIDER_FIELD_KEYED, .secret = 1},
    {.leaf = "api_key_file",        .classes = PROVIDER_FIELD_KEYED, .secret = 1},
    {.leaf = "api_key_command",     .classes = PROVIDER_FIELD_KEYED, .secret = 1},
    {.leaf = "api_key_env",         .classes = PROVIDER_FIELD_UNPINNED},
    {.leaf = "display_name",        .classes = FIELD_ANY},
    {.leaf = "catalog_id",          .classes = FIELD_ANY},
    {.leaf = "sort_models",         .classes = FIELD_ANY},
    {.leaf = "metadata_api",        .classes = FIELD_ANY},
    {.leaf = "model_apis",          .classes = FIELD_ANY},
    {.leaf = "cache",               .classes = FIELD_CACHE_MARKERS},
    {.leaf = "cache_ttl",           .classes = FIELD_CACHE_MARKERS},
    {.leaf = "send_cache_key",      .classes = PROVIDER_FIELD_OPENAI},
    {.leaf = "request_cost",        .classes = PROVIDER_FIELD_OPENAI_CHAT},
    {.leaf = "reasoning_format",    .classes = PROVIDER_FIELD_OPENAI_CHAT},
    {.leaf = "reasoning_roundtrip", .classes = PROVIDER_FIELD_OPENAI_CHAT},
    {.leaf = "reasoning_required",  .classes = PROVIDER_FIELD_OPENAI_CHAT},
    {.leaf = "max_tokens",          .classes = PROVIDER_FIELD_ANTHROPIC},
    {.leaf = "thinking_mode",       .classes = PROVIDER_FIELD_ANTHROPIC},
    {.leaf = "thinking_budget",     .classes = PROVIDER_FIELD_ANTHROPIC},
    {.leaf = "version",             .classes = PROVIDER_FIELD_ANTHROPIC},
    {.leaf = "extra_body",          .classes = FIELD_ANY},
    {.leaf = "extra_headers",       .classes = FIELD_ANY, .secret = 1},
};
// clang-format on
#define N_PROVIDER_FIELDS (sizeof(PROVIDER_FIELDS) / sizeof(PROVIDER_FIELDS[0]))

const struct provider_field *provider_fields(size_t *n)
{
    *n = N_PROVIDER_FIELDS;
    return PROVIDER_FIELDS;
}

static const struct provider_field *field_find(const char *leaf)
{
    for (size_t i = 0; i < N_PROVIDER_FIELDS; i++)
        if (strcmp(PROVIDER_FIELDS[i].leaf, leaf) == 0)
            return &PROVIDER_FIELDS[i];
    return NULL;
}

static int string_list_has(const char *const *list, const char *value)
{
    for (const char *const *entry = list; entry && *entry; entry++)
        if (strcmp(*entry, value) == 0)
            return 1;
    return 0;
}

/* A value beginning with '$' names an environment variable holding the real value, so a
 * secret can stay out of the config file; "$$" escapes a literal leading '$'. Returns the
 * borrowed resolved value, or NULL when the variable is unset or empty. Resolved values are
 * registered with the trace so they never appear in a HAX_TRACE dump. */
static const char *resolve_env_escape(const char *value)
{
    if (value[0] != '$')
        return value;
    if (value[1] == '$')
        return value + 1;
    const char *resolved = getenv(value + 1);
    if (!resolved || !*resolved)
        return NULL;
    trace_register_secret(resolved);
    return resolved;
}

/* The first non-blank line of a key source, trimmed, as an owned string. Credential helpers often
 * append metadata (`pass show` prints the password first) or a newline; a key is never multi-line.
 * Takes ownership of `text`; returns NULL when the first line is blank. */
static char *key_first_line(char *text)
{
    char *line = text;
    while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n')
        line++;
    size_t len = strcspn(line, "\r\n");
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t'))
        len--;
    char *key = NULL;
    if (len > 0) {
        key = xmalloc(len + 1);
        memcpy(key, line, len);
        key[len] = '\0';
    }
    free(text);
    return key;
}

/* Read a key from disk: `~` expands to $HOME and relative paths resolve against the hax config
 * directory, like prompt files. Returns an owned key, or NULL after warning. */
static char *key_from_file(const char *label, const char *spec)
{
    char *path;
    if (spec[0] == '~')
        path = path_expand_home(spec);
    else if (spec[0] == '/')
        path = xstrdup(spec);
    else
        path = xdg_hax_config_path(spec);
    if (!path) {
        hax_warn("%s: couldn't resolve the path '%s'", label, spec);
        return NULL;
    }

    int truncated = 0;
    char *content = fs_read_file_capped(path, API_KEY_MAX_BYTES, NULL, &truncated);
    if (!content) {
        hax_warn("%s: couldn't read %s: %s — ignoring it", label, path, strerror(errno));
        free(path);
        return NULL;
    }
    char *key = NULL;
    if (truncated) {
        hax_warn("%s: %s exceeds %d bytes — ignoring it", label, path, API_KEY_MAX_BYTES);
        free(content);
    } else {
        key = key_first_line(content);
        if (!key)
            hax_warn("%s: %s holds no key — ignoring it", label, path);
    }
    free(path);
    return key;
}

/* Run a shell command and read a key from its first output line. The command inherits the
 * environment but not stdin, so a helper that insists on a terminal fails instead of hanging. */
static char *key_from_command(const char *label, const char *command)
{
    const char *argv[] = {"/bin/sh", "-c", command, NULL};
    size_t len = 0;
    char *output = spawn_capture_stdout(argv, API_KEY_MAX_BYTES, API_KEY_COMMAND_TIMEOUT_MS, &len);
    if (!output) {
        hax_warn("%s: '%s' failed or printed nothing — ignoring it", label, command);
        return NULL;
    }
    char *key = key_first_line(output);
    if (!key)
        hax_warn("%s: '%s' printed no key — ignoring it", label, command);
    return key;
}

char *provider_api_key(const char *config_prefix, const char *api_key_env)
{
    const char *inline_value = config_scoped_str(config_prefix, "api_key");
    const char *file_spec = config_scoped_str(config_prefix, "api_key_file");
    const char *command = config_scoped_str(config_prefix, "api_key_command");

    char *key = NULL;
    if (inline_value && *inline_value) {
        const char *resolved = resolve_env_escape(inline_value);
        if (resolved && *resolved)
            key = xstrdup(resolved);
    }
    if (!key && file_spec && *file_spec) {
        char *label = xasprintf("%s.api_key_file", config_prefix);
        key = key_from_file(label, file_spec);
        free(label);
    }
    if (!key && command && *command) {
        char *label = xasprintf("%s.api_key_command", config_prefix);
        key = key_from_command(label, command);
        free(label);
    }
    if (!key && api_key_env) {
        const char *env = getenv(api_key_env);
        if (env && *env)
            key = xstrdup(env);
    }
    if (!key)
        return NULL;
    trace_register_secret(key);
    return key;
}

const char *provider_cache_ttl(const char *config_prefix)
{
    const char *value = config_scoped_str(config_prefix, "cache_ttl");
    if (!value || !*value)
        return "1h";
    if (strcasecmp(value, "5m") == 0)
        return "5m";
    if (strcasecmp(value, "1h") == 0)
        return "1h";
    hax_warn("unknown cache_ttl '%s' (5m or 1h) — using 1h", value);
    return "1h";
}

/* Members whose value the request machinery owns: overriding them would desynchronize the
 * parser, the tool loop, or the conversation itself, not just tweak the request. `system` and
 * `instructions` carry the system prompt, which the transcript must keep reflecting (replace
 * it with the system_prompt setting instead); `include` carries the encrypted-reasoning entry
 * Responses continuation needs; `n` is here because the stream parser reads choices[0] only —
 * extra completions would be paid for and silently discarded. */
static const char *const EXTRA_BODY_RESERVED[] = {
    "model", "stream", "messages", "input",          "include",
    "n",     "system", "tools",    "stream_options", "instructions",
};

json_t *provider_extra_body(const char *config_prefix)
{
    if (!config_prefix)
        return NULL;
    char *key = xasprintf("%s.extra_body", config_prefix);
    const json_t *node = config_json_node(key);
    json_t *extra_body = NULL;
    if (json_is_object(node)) {
        extra_body = json_deep_copy(node);
        for (size_t i = 0; i < sizeof(EXTRA_BODY_RESERVED) / sizeof(*EXTRA_BODY_RESERVED); i++) {
            if (json_object_get(extra_body, EXTRA_BODY_RESERVED[i])) {
                hax_warn("%s: '%s' is protocol-owned — ignoring it", key, EXTRA_BODY_RESERVED[i]);
                json_object_del(extra_body, EXTRA_BODY_RESERVED[i]);
            }
        }
    } else if (node) {
        hax_warn("%s must be a JSON object — ignoring it", key);
    }
    free(key);
    return extra_body;
}

/* RFC 7230 field names are tokens; a separator would smuggle in a second header or make curl
 * fail the whole request rather than this one header. */
static int header_name_valid(const char *name)
{
    if (!*name)
        return 0;
    for (const char *byte = name; *byte; byte++) {
        if (!isalnum((unsigned char)*byte) && !strchr("!#$%&'*+-.^_`|~", *byte))
            return 0;
    }
    return 1;
}

/* Field values are visible characters plus space and tab: no CR/LF/DEL or other controls. */
static int header_value_valid(const char *value)
{
    for (const char *byte = value; *byte; byte++) {
        if (((unsigned char)*byte < ' ' && *byte != '\t') || *byte == 0x7f)
            return 0;
    }
    return 1;
}

char **provider_headers_from_object(const json_t *object, const char *label)
{
    if (!object)
        return NULL;
    if (!json_is_object(object)) {
        hax_warn("%s must be a JSON object of name/value members — ignoring it", label);
        return NULL;
    }

    char **headers = xcalloc(json_object_size((json_t *)object) + 1, sizeof(*headers));
    size_t n_headers = 0;
    const char *name;
    json_t *value;
    json_object_foreach((json_t *)object, name, value)
    {
        const char *text = json_string_value(value);
        const char *resolved = text ? resolve_env_escape(text) : NULL;
        /* Validate the resolved value, not the written one: an environment variable holding
         * a newline must not smuggle in a second header. */
        if (!header_name_valid(name))
            hax_warn("%s: invalid header name '%s' — ignoring it", label, name);
        else if (!text)
            hax_warn("%s: header '%s' needs a string value — ignoring it", label, name);
        else if (!resolved)
            hax_warn("%s: header '%s' dropped — %s is not set", label, name, text + 1);
        else if (!*resolved)
            /* curl's spelling for suppressing a header; an empty header cannot be sent. */
            headers[n_headers++] = xasprintf("%s:", name);
        else if (!header_value_valid(resolved))
            hax_warn("%s: header '%s' needs a control-character-free value — ignoring it", label,
                     name);
        else
            headers[n_headers++] = xasprintf("%s: %s", name, resolved);
    }
    if (n_headers == 0) {
        free(headers);
        return NULL;
    }
    return headers;
}

char **provider_extra_header_templates(const char *config_prefix)
{
    if (!config_prefix)
        return NULL;
    char *key = xasprintf("%s.extra_headers", config_prefix);
    char **headers = provider_headers_from_object(config_json_node(key), key);
    free(key);
    return headers;
}

char **provider_extra_headers(const char *config_prefix)
{
    char **templates = provider_extra_header_templates(config_prefix);
    char **kept = provider_headers_merge(NULL, templates);
    char **headers = provider_headers_expand(kept, provider_process_session_id());
    string_array_free(kept);
    string_array_free(templates);
    return headers;
}

const char *provider_process_session_id(void)
{
    static char id[37];
    if (!id[0])
        gen_uuid_v4(id);
    return id;
}

static int header_names_equal(const char *first, const char *second)
{
    const char *first_colon = strchr(first, ':');
    const char *second_colon = strchr(second, ':');
    size_t first_len = first_colon ? (size_t)(first_colon - first) : strlen(first);
    size_t second_len = second_colon ? (size_t)(second_colon - second) : strlen(second);
    return first_len == second_len && strncasecmp(first, second, first_len) == 0;
}

static int headers_contain_name(char *const *headers, const char *header)
{
    for (char *const *entry = headers; entry && *entry; entry++)
        if (header_names_equal(*entry, header))
            return 1;
    return 0;
}

static int header_is_removal(const char *header)
{
    const char *colon = strchr(header, ':');
    return colon && colon[1] == '\0';
}

char **provider_headers_merge(char *const *defaults, char *const *overrides)
{
    size_t capacity = string_array_count((const char *const *)defaults) +
                      string_array_count((const char *const *)overrides);
    if (capacity == 0)
        return NULL;
    char **merged = xcalloc(capacity + 1, sizeof(*merged));
    size_t n_merged = 0;
    for (char *const *entry = defaults; entry && *entry; entry++)
        if (!headers_contain_name(overrides, *entry) && !header_is_removal(*entry))
            merged[n_merged++] = xstrdup(*entry);
    for (char *const *entry = overrides; entry && *entry; entry++)
        if (!header_is_removal(*entry))
            merged[n_merged++] = xstrdup(*entry);
    if (n_merged == 0) {
        free(merged);
        return NULL;
    }
    return merged;
}

char **provider_headers_expand(char *const *templates, const char *session_id)
{
    size_t count = string_array_count((const char *const *)templates);
    if (count == 0)
        return NULL;
    char **expanded = xcalloc(count + 1, sizeof(*expanded));
    for (size_t i = 0; i < count; i++)
        expanded[i] = placeholder_expand(templates[i], SESSION_PLACEHOLDER, session_id);
    return expanded;
}

/* A leaf outside the shared inventory is still consumed when it is a registered per-provider
 * setting — a compiled-in module knob such as providers.llamacpp.port. Inventory fields are
 * deliberately not rescued this way: a registered field the dialect ignores must keep warning. */
static int module_key_registered(const char *name, const char *leaf)
{
    char *key = xasprintf("providers.%s.%s", name, leaf);
    int registered = config_setting_find(key) != NULL;
    free(key);
    return registered;
}

/* A silently ignored member hides a typo or a dialect mix-up. */
void provider_warn_unused_fields(const char *name, const char *api_label, unsigned classes,
                                 const char *const *extra)
{
    char *key = xasprintf("providers.%s", name);
    char **members = NULL;
    size_t n_members = config_object_keys(key, &members);
    free(key);
    for (size_t i = 0; i < n_members; i++) {
        const struct provider_field *field = field_find(members[i]);
        if (string_list_has(extra, members[i])) {
            ; /* consumed by this provider */
        } else if (field) {
            if (field->classes & classes) {
                ; /* consumed by this provider */
            } else if (field->classes & FIELD_ANY) {
                /* The dialect wording wins whenever some wire does consume the field. */
                hax_warn("provider '%s': field '%s' is not used by %s providers", name, members[i],
                         api_label);
            } else {
                hax_warn("provider '%s': field '%s' is not configurable for this provider", name,
                         members[i]);
            }
        } else if (!module_key_registered(name, members[i])) {
            hax_warn("provider '%s': unknown field '%s' (see docs/providers.md)", name, members[i]);
        }
        free(members[i]);
    }
    free(members);
}
