/*-------------------------------------------------------------------------
 *
 * pg_systemone.c
 *    PostgreSQL C extension for local SystemOne decision model inference
 *    (Julia-1, Lev, Kev decision models via HTTP POST /v1/systemone).
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
#include "catalog/pg_type.h"

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

/* Function prototypes */
PGDLLEXPORT void _PG_init(void);
PGDLLEXPORT void _PG_fini(void);

PGDLLEXPORT Datum systemone_choice(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum systemone_prob(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum systemone_score(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum systemone_match_score(PG_FUNCTION_ARGS);

/* Aliases for backwards compatibility */
PGDLLEXPORT Datum jev_choice(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum jev_prob(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum jev_score(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum jev_match_score(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(systemone_choice);
PG_FUNCTION_INFO_V1(systemone_prob);
PG_FUNCTION_INFO_V1(systemone_score);
PG_FUNCTION_INFO_V1(systemone_match_score);

PG_FUNCTION_INFO_V1(jev_choice);
PG_FUNCTION_INFO_V1(jev_prob);
PG_FUNCTION_INFO_V1(jev_score);
PG_FUNCTION_INFO_V1(jev_match_score);

/*
 * Module initialization: define GUCs and initialize libcurl.
 */
void
_PG_init(void)
{
	/* Primary GUC: systemone.api_url */
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
		"Batch size hint for future batching",
		"Number of rows per batch for batched inference",
		&systemone_batch_size,
		20,
		1,
		100,
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
}

/*
 * Module cleanup: clean up libcurl global state.
 */
void
_PG_fini(void)
{
	curl_global_cleanup();
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
			return 0; /* Causes CURLE_WRITE_ERROR in libcurl */

		buf->data = new_data;
		buf->capacity = new_cap;
	}

	memcpy(buf->data + buf->size, contents, realsize);
	buf->size += realsize;
	buf->data[buf->size] = '\0';

	return realsize;
}

/*
 * Execute HTTP POST request using libcurl with full error checking,
 * headers, timeouts, and memory safety.
 * Returns response payload as a palloc'd C string in PostgreSQL memory context.
 */
static char *
http_post_json(const char *url, const char *api_key, const char *json_body)
{
	CURL *curl = NULL;
	CURLcode res;
	struct curl_slist *headers = NULL;
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

	curl = curl_easy_init();
	if (!curl)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("pg_systemone: failed to initialize curl handle")));
	}

	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "Accept: application/json");
	headers = curl_slist_append(headers, "User-Agent: pg-systemone/1.0");

	if (api_key && api_key[0] != '\0')
	{
		char auth_hdr[512];
		snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", api_key);
		headers = curl_slist_append(headers, auth_hdr);
	}

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *) &resp_buf);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	PG_TRY();
	{
		res = curl_easy_perform(curl);
		if (res != CURLE_OK)
		{
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("pg_systemone: curl request to '%s' failed: %s",
							url, curl_easy_strerror(res))));
		}

		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
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
		if (headers)
			curl_slist_free_all(headers);
		if (curl)
			curl_easy_cleanup(curl);
		if (resp_buf.data)
			free(resp_buf.data);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (headers)
		curl_slist_free_all(headers);
	if (curl)
		curl_easy_cleanup(curl);
	if (resp_buf.data)
		free(resp_buf.data);

	return result_str;
}

/*
 * Helper to extract numeric score / value / probability from a cJSON response.
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

		f = cJSON_GetObjectItemCaseSensitive(item, "noul");
		if (f && cJSON_IsNumber(f))
		{
			*out_value = f->valuedouble;
			return true;
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
	cJSON *root = NULL;
	double score = 0.0;
	bool found = false;

	root = cJSON_Parse(response_text);
	if (!root)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("pg_systemone: failed to parse JSON response: %s", response_text)));
	}

	/* Check "answers" key */
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
				/* Fallback to first field of answers */
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
			target_obj = answers->child; /* Fallback to first question in answers */
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
 *
 * Calls SystemOne with "type": "choice" and returns the selected choice key.
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

	PG_RETURN_TEXT_P(cstring_to_text(selected_choice));
}

/* Alias */
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

	PG_RETURN_FLOAT8(prob);
}

/* Alias */
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

	PG_RETURN_FLOAT8(normalized);
}

/* Alias */
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

	/* Construct question instructions */
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

	/* Normalize 0..2 level range to 0.0..1.0 */
	double normalized_fit = raw_score / 2.0;
	if (normalized_fit < 0.0)
		normalized_fit = 0.0;
	if (normalized_fit > 1.0)
		normalized_fit = 1.0;

	PG_RETURN_FLOAT8(normalized_fit);
}

/* Alias */
Datum
jev_match_score(PG_FUNCTION_ARGS)
{
	return systemone_match_score(fcinfo);
}
