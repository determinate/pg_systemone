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
