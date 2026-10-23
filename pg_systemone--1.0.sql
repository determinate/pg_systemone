-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_systemone" to load this file. \quit

-- systemone_choice: Categorical classification returning selected option key (e.g. 'billing', 'support')
CREATE FUNCTION systemone_choice(state text, instructions text, criteria text)
RETURNS text
AS 'MODULE_PATHNAME', 'systemone_choice'
LANGUAGE C STRICT PARALLEL SAFE;

-- systemone_prob: Calibrated probability for binary/noul decisions (0.0 to 1.0)
CREATE FUNCTION systemone_prob(state text, question text)
RETURNS float8
AS 'MODULE_PATHNAME', 'systemone_prob'
LANGUAGE C STRICT PARALLEL SAFE;

-- systemone_score: Generic calibrated score across discrete ordered levels (0.0 to 1.0)
CREATE FUNCTION systemone_score(state text, question text, levels text[])
RETURNS float8
AS 'MODULE_PATHNAME', 'systemone_score'
LANGUAGE C STRICT PARALLEL SAFE;

-- systemone_match_score: Score candidate resume against job description (0.0 to 1.0)
CREATE FUNCTION systemone_match_score(job_desc text, resume text)
RETURNS float8
AS 'MODULE_PATHNAME', 'systemone_match_score'
LANGUAGE C STRICT PARALLEL SAFE;

-- =========================================================================
-- Set-Returning Functions (SRF Batching)
-- =========================================================================

-- systemone_choice_batch: Batch categorical choice classification
CREATE FUNCTION systemone_choice_batch(states text[], instructions text, criteria text)
RETURNS TABLE(idx int, choice text)
AS 'MODULE_PATHNAME', 'systemone_choice_batch'
LANGUAGE C STRICT PARALLEL SAFE;

-- systemone_prob_batch: Batch calibrated probability
CREATE FUNCTION systemone_prob_batch(states text[], question text)
RETURNS TABLE(idx int, prob float8)
AS 'MODULE_PATHNAME', 'systemone_prob_batch'
LANGUAGE C STRICT PARALLEL SAFE;

-- systemone_score_batch: Batch calibrated scoring
CREATE FUNCTION systemone_score_batch(states text[], question text, levels text[])
RETURNS TABLE(idx int, score float8)
AS 'MODULE_PATHNAME', 'systemone_score_batch'
LANGUAGE C STRICT PARALLEL SAFE;

-- =========================================================================
-- In-Memory LRU Cache Management
-- =========================================================================

-- systemone_cache_clear: Clears in-memory LRU cache
CREATE FUNCTION systemone_cache_clear()
RETURNS bool
AS 'MODULE_PATHNAME', 'systemone_cache_clear'
LANGUAGE C VOLATILE;

-- systemone_cache_stats: Cache statistics (hits, misses, entries, max_entries, hit_ratio)
CREATE FUNCTION systemone_cache_stats()
RETURNS TABLE(hits int8, misses int8, entries int, max_entries int, hit_ratio float8)
AS 'MODULE_PATHNAME', 'systemone_cache_stats'
LANGUAGE C VOLATILE;

-- =========================================================================
-- Backwards compatibility aliases
-- =========================================================================
CREATE FUNCTION jev_choice(state text, instructions text, criteria text)
RETURNS text
AS 'MODULE_PATHNAME', 'jev_choice'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION jev_prob(state text, question text)
RETURNS float8
AS 'MODULE_PATHNAME', 'jev_prob'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION jev_score(state text, question text, levels text[])
RETURNS float8
AS 'MODULE_PATHNAME', 'jev_score'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION jev_match_score(job_desc text, resume text)
RETURNS float8
AS 'MODULE_PATHNAME', 'jev_match_score'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION jev_choice_batch(states text[], instructions text, criteria text)
RETURNS TABLE(idx int, choice text)
AS 'MODULE_PATHNAME', 'systemone_choice_batch'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION jev_prob_batch(states text[], question text)
RETURNS TABLE(idx int, prob float8)
AS 'MODULE_PATHNAME', 'systemone_prob_batch'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION jev_score_batch(states text[], question text, levels text[])
RETURNS TABLE(idx int, score float8)
AS 'MODULE_PATHNAME', 'systemone_score_batch'
LANGUAGE C STRICT PARALLEL SAFE;
