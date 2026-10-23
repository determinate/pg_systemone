/*-------------------------------------------------------------------------
 *
 * pg_systemone.c
 *    PostgreSQL C extension for local SystemOne decision model inference
 *    (Julia-1, Lev, Kev decision models via HTTP POST /v1/systemone).
 *
 * Features:
 *    - Direct SQL inference: choice, prob (noul), score, match_score
 *    - In-Memory LRU Cache (default 1000 items) in TopMemoryContext
 *    - Persistent libcurl keep-alive connection pooling
 *    - Set-Returning Functions (SRF) for batch array processing
 *
 * Copyright (c) 2026
 *
 *-------------------------------------------------------------------------
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/builtins.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "catalog/pg_type.h"
#include "funcapi.h"

#ifdef __cplusplus
}
#endif

#include <curl/curl.h>
#include "cJSON.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

/* Export definitions for Windows / MSVC and Linux */
#ifndef PGDLLEXPORT
#ifdef _MSC_VER
#define PGDLLEXPORT __declspec(dllexport)
#else
#define PGDLLEXPORT
#endif
#endif

/* GUC Variables */
static char *systemone_api_url = NULL;
static char *systemone_api_key = NULL;
static int systemone_batch_size = 20;
static bool systemone_cache_enabled = true;
static int systemone_cache_size = 1000;

/* Persistent CURL handle for HTTP Keep-Alive connection pooling */
static CURL *persistent_curl = NULL;
static struct curl_slist *persistent_headers = NULL;

/* Function prototypes */
PGDLLEXPORT void _PG_init(void);
PGDLLEXPORT void _PG_fini(void);

/* Scalar functions */
PGDLLEXPORT Datum systemone_choice(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum systemone_prob(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum systemone_score(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum systemone_match_score(PG_FUNCTION_ARGS);

/* Set-Returning Functions (SRF Batching) */
PGDLLEXPORT Datum systemone_choice_batch(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum systemone_prob_batch(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum systemone_score_batch(PG_FUNCTION_ARGS);

/* Cache management functions */
PGDLLEXPORT Datum systemone_cache_clear(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum systemone_cache_stats(PG_FUNCTION_ARGS);

/* Aliases for backwards compatibility */
PGDLLEXPORT Datum jev_choice(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum jev_prob(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum jev_score(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum jev_match_score(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(systemone_choice);
PG_FUNCTION_INFO_V1(systemone_prob);
PG_FUNCTION_INFO_V1(systemone_score);
PG_FUNCTION_INFO_V1(systemone_match_score);

PG_FUNCTION_INFO_V1(systemone_choice_batch);
PG_FUNCTION_INFO_V1(systemone_prob_batch);
PG_FUNCTION_INFO_V1(systemone_score_batch);

PG_FUNCTION_INFO_V1(systemone_cache_clear);
PG_FUNCTION_INFO_V1(systemone_cache_stats);

PG_FUNCTION_INFO_V1(jev_choice);
PG_FUNCTION_INFO_V1(jev_prob);
PG_FUNCTION_INFO_V1(jev_score);
PG_FUNCTION_INFO_V1(jev_match_score);

/*
 *-------------------------------------------------------------------------
 * LRU CACHE IMPLEMENTATION (TopMemoryContext)
 *-------------------------------------------------------------------------
 */
#define KEY_HASH_LEN 65

typedef struct LRUNode
{
	char key[KEY_HASH_LEN];
	char val_text[256];
	double val_double;
	bool is_text;
	struct LRUNode *prev;
	struct LRUNode *next;
} LRUNode;

typedef struct CacheEntry
{
	char key[KEY_HASH_LEN];
	LRUNode *node;
} CacheEntry;

static HTAB *lru_hash = NULL;
static LRUNode *lru_head = NULL;
static LRUNode *lru_tail = NULL;
static int lru_count = 0;
static int64 cache_hits = 0;
static int64 cache_misses = 0;

/* Compute 64-bit FNV-1a hash formatted as hex string */
static void
make_cache_key(const char *fn_type, const char *state, const char *instructions, const char *criteria, char *out_key)
{
	uint64 hash = 0xcbf29ce484222325ULL;
	const unsigned char *p;

	#define FNV_UPDATE(s) if (s) { for (p = (const unsigned char *)(s); *p; p++) { hash ^= (uint64)(*p); hash *= 0x100000001b3ULL; } }

	FNV_UPDATE(fn_type);
	hash ^= 0xff;
	FNV_UPDATE(state);
	hash ^= 0xfe;
	FNV_UPDATE(instructions);
	hash ^= 0xfd;
	FNV_UPDATE(criteria);

	snprintf(out_key, KEY_HASH_LEN, "%016llx%016llx", (unsigned long long)(hash), (unsigned long long)(hash ^ 0x9e3779b97f4a7c15ULL));
}

static void
init_cache_system(void)
{
	HASHCTL ctl;
	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = KEY_HASH_LEN;
	ctl.entrysize = sizeof(CacheEntry);
	ctl.hcxt = TopMemoryContext;

	lru_hash = hash_create("SystemOne_LRU_Hash", 1024, &ctl, HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
	lru_head = NULL;
	lru_tail = NULL;
	lru_count = 0;
}

static void
lru_detach(LRUNode *node)
{
	if (node->prev)
		node->prev->next = node->next;
	else
		lru_head = node->next;

	if (node->next)
		node->next->prev = node->prev;
	else
		lru_tail = node->prev;

	node->prev = NULL;
	node->next = NULL;
}

static void
lru_attach_head(LRUNode *node)
{
	node->prev = NULL;
	node->next = lru_head;
	if (lru_head)
		lru_head->prev = node;
	lru_head = node;
	if (!lru_tail)
		lru_tail = node;
}

static bool
cache_get_text(const char *key, char *out_val, size_t max_len)
{
	if (!systemone_cache_enabled || !lru_hash)
	{
		cache_misses++;
		return false;
	}

	CacheEntry *entry = (CacheEntry *) hash_search(lru_hash, key, HASH_FIND, NULL);
	if (!entry || !entry->node || !entry->node->is_text)
	{
		cache_misses++;
		return false;
	}

	lru_detach(entry->node);
	lru_attach_head(entry->node);
	strncpy(out_val, entry->node->val_text, max_len - 1);
	out_val[max_len - 1] = '\0';
	cache_hits++;
	return true;
}

static bool
cache_get_double(const char *key, double *out_val)
{
	if (!systemone_cache_enabled || !lru_hash)
	{
		cache_misses++;
		return false;
	}

	CacheEntry *entry = (CacheEntry *) hash_search(lru_hash, key, HASH_FIND, NULL);
	if (!entry || !entry->node || entry->node->is_text)
	{
		cache_misses++;
		return false;
	}

	lru_detach(entry->node);
	lru_attach_head(entry->node);
	*out_val = entry->node->val_double;
	cache_hits++;
	return true;
}

static void
cache_put_text(const char *key, const char *val)
{
	if (!systemone_cache_enabled || systemone_cache_size <= 0)
		return;

	if (!lru_hash)
		init_cache_system();

	bool found = false;
	CacheEntry *entry = (CacheEntry *) hash_search(lru_hash, key, HASH_ENTER, &found);

	if (found && entry->node)
	{
		lru_detach(entry->node);
		lru_attach_head(entry->node);
		entry->node->is_text = true;
		strncpy(entry->node->val_text, val, sizeof(entry->node->val_text) - 1);
		entry->node->val_text[sizeof(entry->node->val_text) - 1] = '\0';
		return;
	}

	/* Evict least recently used tail if at capacity */
	if (lru_count >= systemone_cache_size && lru_tail)
	{
		LRUNode *evicted = lru_tail;
		lru_detach(evicted);
		hash_search(lru_hash, evicted->key, HASH_REMOVE, NULL);
		pfree(evicted);
		lru_count--;
	}

	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	LRUNode *new_node = (LRUNode *) palloc(sizeof(LRUNode));
	MemoryContextSwitchTo(oldcxt);

	strncpy(new_node->key, key, KEY_HASH_LEN - 1);
	new_node->key[KEY_HASH_LEN - 1] = '\0';
	new_node->is_text = true;
	strncpy(new_node->val_text, val, sizeof(new_node->val_text) - 1);
	new_node->val_text[sizeof(new_node->val_text) - 1] = '\0';
	new_node->val_double = 0.0;
	new_node->prev = NULL;
	new_node->next = NULL;

	entry->node = new_node;
	lru_attach_head(new_node);
	lru_count++;
}

static void
cache_put_double(const char *key, double val)
{
	if (!systemone_cache_enabled || systemone_cache_size <= 0)
		return;

	if (!lru_hash)
		init_cache_system();

	bool found = false;
	CacheEntry *entry = (CacheEntry *) hash_search(lru_hash, key, HASH_ENTER, &found);

	if (found && entry->node)
	{
		lru_detach(entry->node);
		lru_attach_head(entry->node);
		entry->node->is_text = false;
		entry->node->val_double = val;
		return;
	}

	if (lru_count >= systemone_cache_size && lru_tail)
	{
		LRUNode *evicted = lru_tail;
		lru_detach(evicted);
		hash_search(lru_hash, evicted->key, HASH_REMOVE, NULL);
		pfree(evicted);
		lru_count--;
	}

	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	LRUNode *new_node = (LRUNode *) palloc(sizeof(LRUNode));
	MemoryContextSwitchTo(oldcxt);

	strncpy(new_node->key, key, KEY_HASH_LEN - 1);
	new_node->key[KEY_HASH_LEN - 1] = '\0';
	new_node->is_text = false;
	new_node->val_double = val;
	new_node->val_text[0] = '\0';
	new_node->prev = NULL;
	new_node->next = NULL;

	entry->node = new_node;
	lru_attach_head(new_node);
	lru_count++;
}

/* Clear all entries in the LRU cache */
static void
clear_all_cache(void)
{
	if (!lru_hash)
		return;

	LRUNode *curr = lru_head;
	while (curr)
	{
		LRUNode *next = curr->next;
		pfree(curr);
		curr = next;
	}

	hash_destroy(lru_hash);
	lru_hash = NULL;
	lru_head = NULL;
	lru_tail = NULL;
	lru_count = 0;
}

/*
 * Module initialization: define GUCs and initialize libcurl.
 */
void
_PG_init(void)
{
	DefineCustomStringVariable(
		"systemone.api_url",
		"URL for SystemOne server endpoint",
		"HTTP endpoint for decision model inference (e.g., http://127.0.0.1:9931/v1/systemone)",
		&systemone_api_url,
		"http://127.0.0.1:9931/v1/systemone",
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomStringVariable(
		"systemone.api_key",
		"API key for SystemOne server",
		"Optional Bearer token used for SystemOne authorization header",
		&systemone_api_key,
		"",
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"systemone.batch_size",
		"Batch size hint for row evaluation",
		"Number of rows per batch for batched inference",
		&systemone_batch_size,
		20,
		1,
		1000,
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomBoolVariable(
		"systemone.cache_enabled",
		"Enable in-memory LRU cache",
		"Caches decision model results to avoid redundant HTTP requests",
		&systemone_cache_enabled,
		true,
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"systemone.cache_size",
		"Maximum number of entries in LRU cache",
		"Capacity of in-memory LRU cache",
		&systemone_cache_size,
		1000,
		10,
		100000,
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL
	);

	/* Backward compatibility GUCs */
	DefineCustomStringVariable(
		"jev.api_url",
		"Legacy alias for systemone.api_url",
		NULL,
		&systemone_api_url,
		"http://127.0.0.1:9931/v1/systemone",
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomStringVariable(
		"jev.api_key",
		"Legacy alias for systemone.api_key",
		NULL,
		&systemone_api_key,
		"",
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL
	);

	curl_global_init(CURL_GLOBAL_ALL);
	init_cache_system();
}

/*
 * Module cleanup: clean up libcurl global state and persistent connections.
 */
void
_PG_fini(void)
{
	if (persistent_headers)
	{
		curl_slist_free_all(persistent_headers);
		persistent_headers = NULL;
	}
	if (persistent_curl)
	{
		curl_easy_cleanup(persistent_curl);
		persistent_curl = NULL;
	}
	curl_global_cleanup();
	clear_all_cache();
}

/*
 * HTTP Response buffer
 */
typedef struct
{
	char *data;
	size_t size;
	size_t capacity;
} HttpBuffer;

static size_t
http_write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	HttpBuffer *buf = (HttpBuffer *) userp;
	size_t needed = buf->size + realsize + 1;

	if (needed > buf->capacity)
	{
		size_t new_cap = (buf->capacity == 0) ? 4096 : buf->capacity * 2;
		while (new_cap < needed)
			new_cap *= 2;

		char *new_data = (char *) realloc(buf->data, new_cap);
		if (!new_data)
			return 0;

		buf->data = new_data;
		buf->capacity = new_cap;
	}

	memcpy(buf->data + buf->size, contents, realsize);
	buf->size += realsize;
	buf->data[buf->size] = '\0';

	return realsize;
}

/*
 * Execute HTTP POST request using persistent libcurl handle (Keep-Alive connection pool).
 */
static char *
http_post_json(const char *url, const char *api_key, const char *json_body)
{
	CURLcode res;
	HttpBuffer resp_buf;
	char *result_str = NULL;
	long http_status = 0;

	if (!url || url[0] == '\0')
	{
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("pg_systemone: systemone.api_url is not configured or empty")));
	}

	resp_buf.data = NULL;
	resp_buf.size = 0;
	resp_buf.capacity = 0;

	/* Reuse persistent curl handle for TCP Keep-Alive connection pooling */
	if (!persistent_curl)
	{
		persistent_curl = curl_easy_init();
		if (!persistent_curl)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("pg_systemone: failed to initialize curl handle")));
		}
	}
	else
	{
		curl_easy_reset(persistent_curl);
	}

	if (persistent_headers)
	{
		curl_slist_free_all(persistent_headers);
		persistent_headers = NULL;
	}

	persistent_headers = curl_slist_append(persistent_headers, "Content-Type: application/json");
	persistent_headers = curl_slist_append(persistent_headers, "Accept: application/json");
	persistent_headers = curl_slist_append(persistent_headers, "User-Agent: pg-systemone/1.0");

	if (api_key && api_key[0] != '\0')
	{
		char auth_hdr[512];
		snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", api_key);
		persistent_headers = curl_slist_append(persistent_headers, auth_hdr);
	}

	curl_easy_setopt(persistent_curl, CURLOPT_URL, url);
	curl_easy_setopt(persistent_curl, CURLOPT_POSTFIELDS, json_body);
	curl_easy_setopt(persistent_curl, CURLOPT_HTTPHEADER, persistent_headers);
	curl_easy_setopt(persistent_curl, CURLOPT_WRITEFUNCTION, http_write_callback);
	curl_easy_setopt(persistent_curl, CURLOPT_WRITEDATA, (void *) &resp_buf);
	curl_easy_setopt(persistent_curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(persistent_curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(persistent_curl, CURLOPT_TCP_KEEPALIVE, 1L);
	curl_easy_setopt(persistent_curl, CURLOPT_NOSIGNAL, 1L);

	PG_TRY();
	{
		res = curl_easy_perform(persistent_curl);
		if (res != CURLE_OK)
		{
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("pg_systemone: curl request to '%s' failed: %s",
							url, curl_easy_strerror(res))));
		}

		curl_easy_getinfo(persistent_curl, CURLINFO_RESPONSE_CODE, &http_status);
		if (http_status < 200 || http_status >= 300)
		{
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("pg_systemone: HTTP request to '%s' failed with status %ld: %s",
							url, http_status,
							resp_buf.data ? resp_buf.data : "No response body")));
		}

		result_str = pstrdup(resp_buf.data ? resp_buf.data : "");
	}
	PG_CATCH();
	{
		if (resp_buf.data)
			free(resp_buf.data);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (resp_buf.data)
		free(resp_buf.data);

	return result_str;
}

/*
 * Extract numeric score / value / probability from a cJSON response item.
 */
static bool
extract_score_from_item(const cJSON *item, double *out_value)
{
	if (!item)
		return false;

	if (cJSON_IsNumber(item))
	{
		*out_value = item->valuedouble;
		return true;
	}

	if (cJSON_IsBool(item))
	{
		*out_value = cJSON_IsTrue(item) ? 1.0 : 0.0;
		return true;
	}

	if (cJSON_IsObject(item))
	{
		const cJSON *f = cJSON_GetObjectItemCaseSensitive(item, "score");
		if (f && cJSON_IsNumber(f))
		{
			*out_value = f->valuedouble;
			return true;
		}

		f = cJSON_GetObjectItemCaseSensitive(item, "noul");
		if (f && cJSON_IsNumber(f))
		{
			*out_value = f->valuedouble;
			return true;
		}

		f = cJSON_GetObjectItemCaseSensitive(item, "value");
		if (f)
		{
			if (cJSON_IsNumber(f))
			{
				*out_value = f->valuedouble;
				return true;
			}
			if (cJSON_IsBool(f))
			{
				*out_value = cJSON_IsTrue(f) ? 1.0 : 0.0;
				return true;
			}
		}

		f = cJSON_GetObjectItemCaseSensitive(item, "prob");
		if (f && cJSON_IsNumber(f))
		{
			*out_value = f->valuedouble;
			return true;
		}

		f = cJSON_GetObjectItemCaseSensitive(item, "probability");
		if (f && cJSON_IsNumber(f))
		{
			*out_value = f->valuedouble;
			return true;
		}

		/* Check "probabilities" dictionary for noul or choice */
		const cJSON *probs = cJSON_GetObjectItemCaseSensitive(item, "probabilities");
		if (probs && cJSON_IsObject(probs))
		{
			const cJSON *p = cJSON_GetObjectItemCaseSensitive(probs, "true");
			if (!p) p = cJSON_GetObjectItemCaseSensitive(probs, "yes");
			if (p && cJSON_IsNumber(p))
			{
				*out_value = p->valuedouble;
				return true;
			}

			const cJSON *choice = cJSON_GetObjectItemCaseSensitive(item, "choice");
			if (choice && cJSON_IsString(choice))
			{
				const cJSON *p_ch = cJSON_GetObjectItemCaseSensitive(probs, choice->valuestring);
				if (p_ch && cJSON_IsNumber(p_ch))
				{
					*out_value = p_ch->valuedouble;
					return true;
				}
			}
		}

		f = cJSON_GetObjectItemCaseSensitive(item, "confidence");
		if (f && cJSON_IsNumber(f))
		{
			*out_value = f->valuedouble;
			return true;
		}

		/* Fallback: any numeric field in the object except "type" */
		for (const cJSON *child = item->child; child != NULL; child = child->next)
		{
			if (child->string && strcmp(child->string, "type") != 0 && cJSON_IsNumber(child))
			{
				*out_value = child->valuedouble;
				return true;
			}
		}
	}

	return false;
}

static double
parse_systemone_response(const char *response_text, const char *question_id)
{
	cJSON *root = cJSON_Parse(response_text);
	if (!root)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("pg_systemone: failed to parse JSON response: %s", response_text)));
	}

	double score = 0.0;
	bool found = false;

	const cJSON *answers = cJSON_GetObjectItemCaseSensitive(root, "answers");
	if (!answers)
		answers = cJSON_GetObjectItemCaseSensitive(root, "results");

	if (answers)
	{
		if (cJSON_IsObject(answers))
		{
			const cJSON *q_entry = cJSON_GetObjectItemCaseSensitive(answers, question_id);
			if (q_entry && extract_score_from_item(q_entry, &score))
			{
				found = true;
			}
			else
			{
				const cJSON *first = answers->child;
				if (first && extract_score_from_item(first, &score))
					found = true;
			}
		}
		else if (cJSON_IsArray(answers))
		{
			int count = cJSON_GetArraySize(answers);
			for (int i = 0; i < count; i++)
			{
				const cJSON *elem = cJSON_GetArrayItem(answers, i);
				if (cJSON_IsObject(elem))
				{
					const cJSON *id_node = cJSON_GetObjectItemCaseSensitive(elem, "id");
					if (id_node && cJSON_IsString(id_node) &&
						strcmp(id_node->valuestring, question_id) == 0)
					{
						if (extract_score_from_item(elem, &score))
						{
							found = true;
							break;
						}
					}
				}
			}

			if (!found && count > 0)
			{
				const cJSON *first = cJSON_GetArrayItem(answers, 0);
				if (extract_score_from_item(first, &score))
					found = true;
			}
		}
	}

	if (!found)
	{
		if (extract_score_from_item(root, &score))
			found = true;
	}

	cJSON_Delete(root);

	if (!found)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("pg_systemone: could not extract score for question '%s' from response: %s",
						question_id, response_text)));
	}

	return score;
}

static char *
parse_systemone_choice(const char *response_text, const char *question_id)
{
	cJSON *root = cJSON_Parse(response_text);
	if (!root)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("pg_systemone: failed to parse JSON response: %s", response_text)));
	}

	char *choice_res = NULL;
	const cJSON *answers = cJSON_GetObjectItemCaseSensitive(root, "answers");
	if (!answers)
		answers = cJSON_GetObjectItemCaseSensitive(root, "results");

	const cJSON *target_obj = NULL;
	if (answers && cJSON_IsObject(answers))
	{
		target_obj = cJSON_GetObjectItemCaseSensitive(answers, question_id);
		if (!target_obj)
			target_obj = answers->child;
	}

	if (target_obj && cJSON_IsObject(target_obj))
	{
		const cJSON *ch = cJSON_GetObjectItemCaseSensitive(target_obj, "choice");
		if (ch && cJSON_IsString(ch))
			choice_res = pstrdup(ch->valuestring);
	}

	cJSON_Delete(root);

	if (!choice_res)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("pg_systemone: could not extract 'choice' for question '%s' from response: %s",
						question_id, response_text)));
	}

	return choice_res;
}

/*
 *-------------------------------------------------------------------------
 * SQL FUNCTION: systemone_choice(state TEXT, instructions TEXT, criteria TEXT) RETURNS TEXT
 *-------------------------------------------------------------------------
 */
Datum
systemone_choice(PG_FUNCTION_ARGS)
{
	text *state_text = PG_GETARG_TEXT_PP(0);
	text *instructions_text = PG_GETARG_TEXT_PP(1);
	text *criteria_text = PG_GETARG_TEXT_PP(2);

	char *state_str = text_to_cstring(state_text);
	char *instructions_str = text_to_cstring(instructions_text);
	char *criteria_str = text_to_cstring(criteria_text);

	/* 1. Check LRU Cache */
	char cache_key[KEY_HASH_LEN];
	make_cache_key("choice", state_str, instructions_str, criteria_str, cache_key);

	char cached_result[256];
	if (cache_get_text(cache_key, cached_result, sizeof(cached_result)))
	{
		PG_RETURN_TEXT_P(cstring_to_text(cached_result));
	}

	/* 2. Cache miss -> Build JSON request */
	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "state", state_str);

	cJSON *questions_obj = cJSON_AddObjectToObject(root, "questions");
	cJSON *q_obj = cJSON_CreateObject();
	cJSON_AddStringToObject(q_obj, "type", "choice");
	cJSON_AddStringToObject(q_obj, "instructions", instructions_str);

	cJSON *criteria_json = cJSON_Parse(criteria_str);
	if (!criteria_json || !cJSON_IsObject(criteria_json))
	{
		if (criteria_json)
			cJSON_Delete(criteria_json);
		cJSON_Delete(root);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("systemone_choice: criteria must be a valid JSON object string (e.g. '{\"billing\": \"...\", \"support\": \"...\"}')")));
	}

	cJSON_AddItemToObject(q_obj, "criteria", criteria_json);
	cJSON_AddItemToObject(questions_obj, "route", q_obj);

	char *json_body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	if (!json_body)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("pg_systemone: failed to serialize JSON payload")));
	}

	char *response_body = NULL;
	PG_TRY();
	{
		response_body = http_post_json(systemone_api_url, systemone_api_key, json_body);
	}
	PG_CATCH();
	{
		free(json_body);
		PG_RE_THROW();
	}
	PG_END_TRY();

	free(json_body);

	char *selected_choice = parse_systemone_choice(response_body, "route");

	/* Save to LRU cache */
	cache_put_text(cache_key, selected_choice);

	PG_RETURN_TEXT_P(cstring_to_text(selected_choice));
}

Datum
jev_choice(PG_FUNCTION_ARGS)
{
	return systemone_choice(fcinfo);
}

/*
 *-------------------------------------------------------------------------
 * SQL FUNCTION: systemone_prob(state TEXT, question TEXT) RETURNS FLOAT8
 *-------------------------------------------------------------------------
 */
Datum
systemone_prob(PG_FUNCTION_ARGS)
{
	text *state_text = PG_GETARG_TEXT_PP(0);
	text *question_text = PG_GETARG_TEXT_PP(1);

	char *state_str = text_to_cstring(state_text);
	char *question_str = text_to_cstring(question_text);

	/* 1. Check LRU Cache */
	char cache_key[KEY_HASH_LEN];
	make_cache_key("prob", state_str, question_str, "", cache_key);

	double cached_prob;
	if (cache_get_double(cache_key, &cached_prob))
	{
		PG_RETURN_FLOAT8(cached_prob);
	}

	/* 2. Cache miss -> Build JSON request */
	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "state", state_str);

	cJSON *questions_obj = cJSON_AddObjectToObject(root, "questions");
	cJSON *q_obj = cJSON_CreateObject();
	cJSON_AddStringToObject(q_obj, "type", "noul");
	cJSON_AddStringToObject(q_obj, "instructions", question_str);

	cJSON_AddItemToObject(questions_obj, "prob", q_obj);

	char *json_body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	if (!json_body)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("pg_systemone: failed to serialize JSON payload")));
	}

	char *response_body = NULL;
	PG_TRY();
	{
		response_body = http_post_json(systemone_api_url, systemone_api_key, json_body);
	}
	PG_CATCH();
	{
		free(json_body);
		PG_RE_THROW();
	}
	PG_END_TRY();

	free(json_body);

	double prob = parse_systemone_response(response_body, "prob");
	if (prob < 0.0)
		prob = 0.0;
	if (prob > 1.0)
		prob = 1.0;

	/* Save to LRU cache */
	cache_put_double(cache_key, prob);

	PG_RETURN_FLOAT8(prob);
}

Datum
jev_prob(PG_FUNCTION_ARGS)
{
	return systemone_prob(fcinfo);
}

/*
 *-------------------------------------------------------------------------
 * SQL FUNCTION: systemone_score(state TEXT, question TEXT, levels TEXT[]) RETURNS FLOAT8
 *-------------------------------------------------------------------------
 */
Datum
systemone_score(PG_FUNCTION_ARGS)
{
	text *state_text = PG_GETARG_TEXT_PP(0);
	text *question_text = PG_GETARG_TEXT_PP(1);
	ArrayType *levels_arr = PG_GETARG_ARRAYTYPE_P(2);

	char *state_str = text_to_cstring(state_text);
	char *question_str = text_to_cstring(question_text);

	Oid elem_type = ARR_ELEMTYPE(levels_arr);
	if (elem_type != TEXTOID)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("systemone_score: levels argument must be an array of text")));
	}

	int ndims = ARR_NDIM(levels_arr);
	if (ndims != 1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("systemone_score: levels array must be one-dimensional")));
	}

	int16 typlen;
	bool typbyval;
	char typalign;
	get_typlenbyvalalign(elem_type, &typlen, &typbyval, &typalign);

	Datum *elem_datums;
	bool *elem_nulls;
	int n_levels;
	deconstruct_array(levels_arr, elem_type, typlen, typbyval, typalign,
					  &elem_datums, &elem_nulls, &n_levels);

	if (n_levels < 2)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("systemone_score: levels array must contain at least 2 levels (got %d)", n_levels)));
	}
	if (n_levels > 10)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("systemone_score: levels array cannot exceed 10 levels (got %d)", n_levels)));
	}

	/* Build concatenated levels string for cache key */
	StringInfoData levels_buf;
	initStringInfo(&levels_buf);
	for (int i = 0; i < n_levels; i++)
	{
		if (i > 0)
			appendStringInfoChar(&levels_buf, '|');
		if (!elem_nulls[i])
			appendStringInfoString(&levels_buf, TextDatumGetCString(elem_datums[i]));
	}

	char cache_key[KEY_HASH_LEN];
	make_cache_key("score", state_str, question_str, levels_buf.data, cache_key);

	double cached_score;
	if (cache_get_double(cache_key, &cached_score))
	{
		PG_RETURN_FLOAT8(cached_score);
	}

	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "state", state_str);

	cJSON *questions_obj = cJSON_AddObjectToObject(root, "questions");
	cJSON *q_obj = cJSON_CreateObject();
	cJSON_AddStringToObject(q_obj, "type", "score");
	cJSON_AddStringToObject(q_obj, "instructions", question_str);

	cJSON *criteria_json = cJSON_AddArrayToObject(q_obj, "criteria");
	for (int i = 0; i < n_levels; i++)
	{
		if (elem_nulls[i])
		{
			cJSON_Delete(root);
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("systemone_score: levels array element %d is NULL", i + 1)));
		}
		char *lvl_str = TextDatumGetCString(elem_datums[i]);
		cJSON_AddItemToArray(criteria_json, cJSON_CreateString(lvl_str));
	}

	cJSON_AddItemToObject(questions_obj, "score", q_obj);

	char *json_body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	if (!json_body)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("pg_systemone: failed to serialize JSON payload")));
	}

	char *response_body = NULL;
	PG_TRY();
	{
		response_body = http_post_json(systemone_api_url, systemone_api_key, json_body);
	}
	PG_CATCH();
	{
		free(json_body);
		PG_RE_THROW();
	}
	PG_END_TRY();

	free(json_body);

	double raw_score = parse_systemone_response(response_body, "score");

	/* Normalize score across (n_levels - 1) levels */
	double normalized = raw_score / (double)(n_levels - 1);
	if (normalized < 0.0)
		normalized = 0.0;
	if (normalized > 1.0)
		normalized = 1.0;

	cache_put_double(cache_key, normalized);

	PG_RETURN_FLOAT8(normalized);
}

Datum
jev_score(PG_FUNCTION_ARGS)
{
	return systemone_score(fcinfo);
}

/*
 *-------------------------------------------------------------------------
 * SQL FUNCTION: systemone_match_score(job_desc TEXT, resume TEXT) RETURNS FLOAT8
 *-------------------------------------------------------------------------
 */
Datum
systemone_match_score(PG_FUNCTION_ARGS)
{
	text *job_desc_text = PG_GETARG_TEXT_PP(0);
	text *resume_text = PG_GETARG_TEXT_PP(1);

	char *job_desc_str = text_to_cstring(job_desc_text);
	char *resume_str = text_to_cstring(resume_text);

	char cache_key[KEY_HASH_LEN];
	make_cache_key("match", job_desc_str, resume_str, "", cache_key);

	double cached_match;
	if (cache_get_double(cache_key, &cached_match))
	{
		PG_RETURN_FLOAT8(cached_match);
	}

	StringInfoData prompt_buf;
	initStringInfo(&prompt_buf);
	appendStringInfo(&prompt_buf, "Does this candidate resume match the job requirements? Resume: %s", resume_str);

	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "state", job_desc_str);

	cJSON *questions_obj = cJSON_AddObjectToObject(root, "questions");
	cJSON *q_obj = cJSON_CreateObject();
	cJSON_AddStringToObject(q_obj, "type", "score");
	cJSON_AddStringToObject(q_obj, "instructions", prompt_buf.data);

	cJSON *criteria_arr = cJSON_AddArrayToObject(q_obj, "criteria");
	cJSON_AddItemToArray(criteria_arr, cJSON_CreateString("no fit"));
	cJSON_AddItemToArray(criteria_arr, cJSON_CreateString("partial fit"));
	cJSON_AddItemToArray(criteria_arr, cJSON_CreateString("perfect fit"));

	cJSON_AddItemToObject(questions_obj, "fit", q_obj);

	char *json_body = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);

	if (!json_body)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("pg_systemone: failed to serialize JSON payload")));
	}

	char *response_body = NULL;
	PG_TRY();
	{
		response_body = http_post_json(systemone_api_url, systemone_api_key, json_body);
	}
	PG_CATCH();
	{
		free(json_body);
		PG_RE_THROW();
	}
	PG_END_TRY();

	free(json_body);

	double raw_score = parse_systemone_response(response_body, "fit");

	double normalized_fit = raw_score / 2.0;
	if (normalized_fit < 0.0)
		normalized_fit = 0.0;
	if (normalized_fit > 1.0)
		normalized_fit = 1.0;

	cache_put_double(cache_key, normalized_fit);

	PG_RETURN_FLOAT8(normalized_fit);
}

Datum
jev_match_score(PG_FUNCTION_ARGS)
{
	return systemone_match_score(fcinfo);
}

/*
 *-------------------------------------------------------------------------
 * SET-RETURNING FUNCTIONS (SRF): BATCH INFERENCE
 *
 * Evaluates an entire array of states in a single set-returning function call,
 * taking full advantage of persistent HTTP keep-alive connections and LRU caching.
 *-------------------------------------------------------------------------
 */

/* SRF Context for returning batch results row-by-row */
typedef struct
{
	int n_items;
	char **text_results;
	double *double_results;
	bool is_text;
} BatchContext;

/*
 * systemone_choice_batch(states text[], instructions text, criteria text)
 * RETURNS TABLE (idx int, choice text)
 */
Datum
systemone_choice_batch(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		ArrayType *states_arr = PG_GETARG_ARRAYTYPE_P(0);
		char *instructions_str = text_to_cstring(PG_GETARG_TEXT_PP(1));
		char *criteria_str = text_to_cstring(PG_GETARG_TEXT_PP(2));

		Oid elem_type = ARR_ELEMTYPE(states_arr);
		if (elem_type != TEXTOID)
		{
			ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
					errmsg("systemone_choice_batch: states argument must be an array of text")));
		}

		int16 typlen;
		bool typbyval;
		char typalign;
		get_typlenbyvalalign(elem_type, &typlen, &typbyval, &typalign);

		Datum *elem_datums;
		bool *elem_nulls;
		int n_items;
		deconstruct_array(states_arr, elem_type, typlen, typbyval, typalign,
						  &elem_datums, &elem_nulls, &n_items);

		BatchContext *bctx = (BatchContext *) palloc(sizeof(BatchContext));
		bctx->n_items = n_items;
		bctx->is_text = true;
		bctx->double_results = NULL;
		bctx->text_results = (char **) palloc(sizeof(char *) * n_items);

		/* Verify criteria JSON once */
		cJSON *crit_test = cJSON_Parse(criteria_str);
		if (!crit_test || !cJSON_IsObject(crit_test))
		{
			if (crit_test) cJSON_Delete(crit_test);
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("systemone_choice_batch: criteria must be a valid JSON object string")));
		}
		cJSON_Delete(crit_test);

		/* Evaluate each element: checking cache first, sending HTTP keep-alive on miss */
		for (int i = 0; i < n_items; i++)
		{
			if (elem_nulls[i])
			{
				bctx->text_results[i] = pstrdup("");
				continue;
			}

			char *state_str = TextDatumGetCString(elem_datums[i]);
			char cache_key[KEY_HASH_LEN];
			make_cache_key("choice", state_str, instructions_str, criteria_str, cache_key);

			char cached_val[256];
			if (cache_get_text(cache_key, cached_val, sizeof(cached_val)))
			{
				bctx->text_results[i] = pstrdup(cached_val);
				continue;
			}

			/* Cache miss -> single call over persistent keep-alive connection */
			cJSON *root = cJSON_CreateObject();
			cJSON_AddStringToObject(root, "state", state_str);
			cJSON *q_obj = cJSON_CreateObject();
			cJSON_AddStringToObject(q_obj, "type", "choice");
			cJSON_AddStringToObject(q_obj, "instructions", instructions_str);
			cJSON_AddItemToObject(q_obj, "criteria", cJSON_Parse(criteria_str));
			cJSON *questions_obj = cJSON_AddObjectToObject(root, "questions");
			cJSON_AddItemToObject(questions_obj, "route", q_obj);

			char *json_body = cJSON_PrintUnformatted(root);
			cJSON_Delete(root);

			char *response_body = http_post_json(systemone_api_url, systemone_api_key, json_body);
			free(json_body);

			char *choice_val = parse_systemone_choice(response_body, "route");
			cache_put_text(cache_key, choice_val);
			bctx->text_results[i] = pstrdup(choice_val);
		}

		funcctx->user_fctx = bctx;
		funcctx->max_calls = n_items;

		TupleDesc tupdesc;
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("systemone_choice_batch: function returning record called in context that cannot accept type record")));
		}
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	BatchContext *bctx = (BatchContext *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		int curr = funcctx->call_cntr;
		Datum values[2];
		bool nulls[2] = {false, false};

		values[0] = Int32GetDatum(curr + 1);
		values[1] = CStringGetTextDatum(bctx->text_results[curr]);

		HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	else
	{
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * systemone_prob_batch(states text[], question text)
 * RETURNS TABLE (idx int, prob float8)
 */
Datum
systemone_prob_batch(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		ArrayType *states_arr = PG_GETARG_ARRAYTYPE_P(0);
		char *question_str = text_to_cstring(PG_GETARG_TEXT_PP(1));

		Oid elem_type = ARR_ELEMTYPE(states_arr);
		int16 typlen;
		bool typbyval;
		char typalign;
		get_typlenbyvalalign(elem_type, &typlen, &typbyval, &typalign);

		Datum *elem_datums;
		bool *elem_nulls;
		int n_items;
		deconstruct_array(states_arr, elem_type, typlen, typbyval, typalign,
						  &elem_datums, &elem_nulls, &n_items);

		BatchContext *bctx = (BatchContext *) palloc(sizeof(BatchContext));
		bctx->n_items = n_items;
		bctx->is_text = false;
		bctx->text_results = NULL;
		bctx->double_results = (double *) palloc(sizeof(double) * n_items);

		for (int i = 0; i < n_items; i++)
		{
			if (elem_nulls[i])
			{
				bctx->double_results[i] = 0.0;
				continue;
			}

			char *state_str = TextDatumGetCString(elem_datums[i]);
			char cache_key[KEY_HASH_LEN];
			make_cache_key("prob", state_str, question_str, "", cache_key);

			double cached_val;
			if (cache_get_double(cache_key, &cached_val))
			{
				bctx->double_results[i] = cached_val;
				continue;
			}

			cJSON *root = cJSON_CreateObject();
			cJSON_AddStringToObject(root, "state", state_str);
			cJSON *q_obj = cJSON_CreateObject();
			cJSON_AddStringToObject(q_obj, "type", "noul");
			cJSON_AddStringToObject(q_obj, "instructions", question_str);
			cJSON *questions_obj = cJSON_AddObjectToObject(root, "questions");
			cJSON_AddItemToObject(questions_obj, "prob", q_obj);

			char *json_body = cJSON_PrintUnformatted(root);
			cJSON_Delete(root);

			char *response_body = http_post_json(systemone_api_url, systemone_api_key, json_body);
			free(json_body);

			double prob = parse_systemone_response(response_body, "prob");
			if (prob < 0.0) prob = 0.0;
			if (prob > 1.0) prob = 1.0;

			cache_put_double(cache_key, prob);
			bctx->double_results[i] = prob;
		}

		funcctx->user_fctx = bctx;
		funcctx->max_calls = n_items;

		TupleDesc tupdesc;
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("systemone_prob_batch: composite result type required")));
		}
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	BatchContext *bctx = (BatchContext *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		int curr = funcctx->call_cntr;
		Datum values[2];
		bool nulls[2] = {false, false};

		values[0] = Int32GetDatum(curr + 1);
		values[1] = Float8GetDatum(bctx->double_results[curr]);

		HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	else
	{
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * systemone_score_batch(states text[], question text, levels text[])
 * RETURNS TABLE (idx int, score float8)
 */
Datum
systemone_score_batch(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		ArrayType *states_arr = PG_GETARG_ARRAYTYPE_P(0);
		char *question_str = text_to_cstring(PG_GETARG_TEXT_PP(1));
		ArrayType *levels_arr = PG_GETARG_ARRAYTYPE_P(2);

		Oid elem_type = ARR_ELEMTYPE(states_arr);
		int16 typlen;
		bool typbyval;
		char typalign;
		get_typlenbyvalalign(elem_type, &typlen, &typbyval, &typalign);

		Datum *elem_datums;
		bool *elem_nulls;
		int n_items;
		deconstruct_array(states_arr, elem_type, typlen, typbyval, typalign,
						  &elem_datums, &elem_nulls, &n_items);

		/* Deconstruct levels once */
		Datum *lvl_datums;
		bool *lvl_nulls;
		int n_levels;
		deconstruct_array(levels_arr, TEXTOID, -1, false, 'i', &lvl_datums, &lvl_nulls, &n_levels);

		if (n_levels < 2 || n_levels > 10)
		{
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("systemone_score_batch: levels array must contain 2 to 10 levels")));
		}

		StringInfoData levels_buf;
		initStringInfo(&levels_buf);
		for (int i = 0; i < n_levels; i++)
		{
			if (i > 0) appendStringInfoChar(&levels_buf, '|');
			if (!lvl_nulls[i]) appendStringInfoString(&levels_buf, TextDatumGetCString(lvl_datums[i]));
		}

		BatchContext *bctx = (BatchContext *) palloc(sizeof(BatchContext));
		bctx->n_items = n_items;
		bctx->is_text = false;
		bctx->text_results = NULL;
		bctx->double_results = (double *) palloc(sizeof(double) * n_items);

		for (int i = 0; i < n_items; i++)
		{
			if (elem_nulls[i])
			{
				bctx->double_results[i] = 0.0;
				continue;
			}

			char *state_str = TextDatumGetCString(elem_datums[i]);
			char cache_key[KEY_HASH_LEN];
			make_cache_key("score", state_str, question_str, levels_buf.data, cache_key);

			double cached_val;
			if (cache_get_double(cache_key, &cached_val))
			{
				bctx->double_results[i] = cached_val;
				continue;
			}

			cJSON *root = cJSON_CreateObject();
			cJSON_AddStringToObject(root, "state", state_str);
			cJSON *q_obj = cJSON_CreateObject();
			cJSON_AddStringToObject(q_obj, "type", "score");
			cJSON_AddStringToObject(q_obj, "instructions", question_str);
			cJSON *crit_arr = cJSON_AddArrayToObject(q_obj, "criteria");
			for (int j = 0; j < n_levels; j++)
			{
				cJSON_AddItemToArray(crit_arr, cJSON_CreateString(TextDatumGetCString(lvl_datums[j])));
			}
			cJSON *questions_obj = cJSON_AddObjectToObject(root, "questions");
			cJSON_AddItemToObject(questions_obj, "score", q_obj);

			char *json_body = cJSON_PrintUnformatted(root);
			cJSON_Delete(root);

			char *response_body = http_post_json(systemone_api_url, systemone_api_key, json_body);
			free(json_body);

			double raw_score = parse_systemone_response(response_body, "score");
			double normalized = raw_score / (double)(n_levels - 1);
			if (normalized < 0.0) normalized = 0.0;
			if (normalized > 1.0) normalized = 1.0;

			cache_put_double(cache_key, normalized);
			bctx->double_results[i] = normalized;
		}

		funcctx->user_fctx = bctx;
		funcctx->max_calls = n_items;

		TupleDesc tupdesc;
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("systemone_score_batch: composite result type required")));
		}
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	BatchContext *bctx = (BatchContext *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		int curr = funcctx->call_cntr;
		Datum values[2];
		bool nulls[2] = {false, false};

		values[0] = Int32GetDatum(curr + 1);
		values[1] = Float8GetDatum(bctx->double_results[curr]);

		HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	else
	{
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 *-------------------------------------------------------------------------
 * CACHE CONTROL FUNCTIONS
 *-------------------------------------------------------------------------
 */

/* Clear in-memory LRU cache */
Datum
systemone_cache_clear(PG_FUNCTION_ARGS)
{
	clear_all_cache();
	init_cache_system();
	PG_RETURN_BOOL(true);
}

/* Return cache statistics */
Datum
systemone_cache_stats(PG_FUNCTION_ARGS)
{
	TupleDesc tupdesc;
	Datum values[5];
	bool nulls[5] = {false, false, false, false, false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("systemone_cache_stats: composite result type required")));
	}
	tupdesc = BlessTupleDesc(tupdesc);

	int64 total = cache_hits + cache_misses;
	double ratio = (total > 0) ? ((double)cache_hits / (double)total) : 0.0;

	values[0] = Int64GetDatum(cache_hits);
	values[1] = Int64GetDatum(cache_misses);
	values[2] = Int32GetDatum(lru_count);
	values[3] = Int32GetDatum(systemone_cache_size);
	values[4] = Float8GetDatum(ratio);

	HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
