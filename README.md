**English** | [🇷🇺 Документация на русском](README_RU.md)

---

# pg_systemone

[![PostgreSQL](https://img.shields.io/badge/PostgreSQL-14%20|%2015%20|%2016%20|%2017-336791?logo=postgresql&logoColor=white)](https://www.postgresql.org/)
[![License](https://img.shields.io/badge/License-PostgreSQL%20%2F%20MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%20|%20Linux-lightgrey.svg)]()
[![Model](https://img.shields.io/badge/Decision%20Models-Julia--1%20|%20Lev%20|%20Kev%20|%20SystemOne-brightgreen.svg)](https://huggingface.co/ggml-org/Julia-1-GGUF)
[![Inference Engine](https://img.shields.io/badge/Engine-llama.cpp%20server-orange.svg)](https://github.com/ggerganov/llama.cpp)
[![Cache](https://img.shields.io/badge/Cache-LRU%20TopMemoryContext-success.svg)]()

> **Fast, local System 1 Decision Model inference directly inside PostgreSQL SQL queries.**  
> Native C extension connecting PostgreSQL to `llama-server` (`/v1/systemone`) running **Julia-1**, **Lev**, or **Kev** models with **In-Memory LRU Caching**, **Keep-Alive Connection Pooling**, and **Set-Returning Function (SRF) Batching** for millions of rows.



## 💡 Why Decision Models in PostgreSQL?

Traditional generative LLMs (System 2, like LLaMA-3 or Mistral) stream tokens autoregressively, are slow (500ms–5000ms), non-deterministic, and frequently break JSON schemas under production loads.

**Decision models** (System 1, e.g. **Julia-1** by Supersonic Labs, based on `mmBERT`) evaluate input text against structured questions in a **single forward pass**:
* ⚡ **Sub-10ms Latency**: No generative token loop. Single tensor forward pass.
* 🎯 **Strict Guarantees**: Produces exact categorical labels, calibrated probabilities, and continuous rubric scores.
* 🐘 **Zero Data Movement**: Classify, route, and score millions of rows directly in `SELECT`, `WHERE`, and `ORDER BY` without moving gigabytes of customer data to external Python microservices or Kafka pipelines.
* 🛡️ **In-Memory LRU Cache**: Identical queries resolve in **< 0.05ms** without hitting the inference server.
* 📦 **Batch SRF Processing**: Chunk 1,000,000+ rows into batches of 20–50 using Set-Returning Functions without overloading HTTP connections.

---

| Feature | System 2 (Generative LLMs) | Vector Search (pgvector) | **pg_systemone (Decision Models)** |
|---|---|---|---|
| **Speed per row** | 500 ms – 5000 ms | < 5 ms | **5 ms – 15 ms** (0.05 ms with LRU cache) |
| **Output Type** | Markdown / Unstructured text | Distance metric (Float) | **Exact Categorical Label or Calibrated Probability** |
| **Deterministic** | ❌ Prone to hallucinations | ✔ Geometric distance | **✔ Fully deterministic & calibrated** |
| **Model Size** | 7B – 70B parameters | 50M – 300M parameters | **144M parameters (ultra-lightweight)** |
| **Hardware Required**| Dedicated High-End GPU | CPU / RAM | **Runs easily on CPU or entry-level GPU** |

---

## ⚡ Features

* **Multi-Class Routing (`systemone_choice`)**: Route customer tickets, classify feedback, or tag content using arbitrary JSON criteria.
* **Calibrated Probabilities (`systemone_prob`)**: Evaluate yes/no propositions (`noul` decision format) returning continuous confidence `0.0 .. 1.0`.
* **Rubric & Ordinal Scoring (`systemone_score`)**: Score candidate text against 2 to 10 ordered discrete levels with probability-weighted continuous interpolation.
* **Candidate Fit Scoring (`systemone_match_score`)**: Instant semantic match score between job vacancy and candidate resume for ATS & recruitment platforms.
* **High-Throughput Set-Returning Batch Functions (`systemone_*_batch`)**: Collect arrays of text and evaluate in batches to prevent socket exhaustion and pipeline 1M+ rows.
* **Built-in LRU Cache**: Thread-safe TopMemoryContext hash table storing up to 1000 items with O(1) eviction and sub-millisecond lookups.
* **HTTP Keep-Alive Connection Pooling**: Reuses persistent `CURL*` handles to eliminate TCP/TLS handshake overhead on every query.
* **Full Windows & Linux Support**:
  * **Windows**: Compiles natively with MSVC `cl.exe` / `link.exe` against official PostgreSQL 14–17 x64.
  * **Linux**: Standard PostgreSQL PGXS `make && sudo make install`.
* **Legacy Backward Compatibility**: Seamlessly alias legacy `jev_*` functions and GUC settings.

---

## 🚀 Quickstart: Running Julia-1 with llama.cpp

### 1. Download Model Weights (GGUF)
`Julia-1` is an open-source 144M parameter decision model. Official GGUF quantized models are hosted at [ggml-org/Julia-1-GGUF](https://huggingface.co/ggml-org/Julia-1-GGUF).

```bash
# Download Q8_0 or BF16 GGUF weights
curl -L -o julia-1-q8_0.gguf "https://huggingface.co/ggml-org/Julia-1-GGUF/resolve/main/julia-1-q8_0.gguf"
```

### 2. Start llama-server
Launch `llama-server` pointing to the GGUF model and specify your port (e.g. `9931`):

```bash
# Windows
llama-server.exe -m julia-1-q8_0.gguf --port 9931 --host 127.0.0.1

# Linux / macOS
./llama-server -m julia-1-q8_0.gguf --port 9931 --host 127.0.0.1
```

> **Note:** `llama.cpp` automatically exposes the `POST /v1/systemone` endpoint when a decision model architecture is detected.

### 3. Verify Local Endpoint

**PowerShell:**
```powershell
$body = @{
    state = "I was charged twice for my subscription this month. Please refund!"
    questions = @{
        route = @{
            type = "choice"
            instructions = "Which department should handle this request?"
            criteria = @{
                billing = "Payment issues, duplicate charges, refunds, invoices"
                support = "Technical bugs, server downtime, login issues"
            }
        }
    }
} | ConvertTo-Json -Depth 5

$response = Invoke-RestMethod -Uri "http://127.0.0.1:9931/v1/systemone" -Method Post -ContentType "application/json" -Body $body
$response | ConvertTo-Json -Depth 5
```

---

## 🛠 Building and Installation

### Windows (MSVC cl.exe & link.exe)

> **Requirements:** Visual Studio 2019/2022 with *Desktop development with C++*, and 64-bit PostgreSQL (14–17).

1. Launch **x64 Native Tools Command Prompt for VS 2022** (Must be **x64**).
2. Navigate to repository root:
   ```cmd
   cd /d f:\projects\ERP\pg-open-jev
   ```
3. Set your PostgreSQL installation path:
   ```cmd
   set PG_DIR=C:\Program Files\PostgreSQL\17
   ```
4. Generate `libcurl.lib` from PostgreSQL's bundled `libcurl.dll`:
   ```cmd
   echo EXPORTS > libcurl.def
   for /f "skip=19 tokens=4" %A in ('dumpbin /exports "%PG_DIR%\bin\libcurl.dll"') do if not "%A"=="" echo %A >> libcurl.def
   lib.exe /def:libcurl.def /machine:x64 /out:libcurl.lib
   ```
5. Compile C source files:
   ```cmd
   cl.exe /c /O2 /MD /W3 /wd4996 ^
      /DWIN32 /D_WINDOWS ^
      /I"src" ^
      /I"%PG_DIR%\include\server\port\win32_msvc" ^
      /I"%PG_DIR%\include\server\port\win32" ^
      /I"%PG_DIR%\include\server" ^
      /I"%PG_DIR%\include" ^
      src\pg_systemone.c src\cJSON.c
   ```
6. Link into `pg_systemone.dll`:
   ```cmd
   link.exe /DLL /OUT:pg_systemone.dll ^
      pg_systemone.obj cJSON.obj ^
      "%PG_DIR%\lib\postgres.lib" ^
      libcurl.lib ^
      /NODEFAULTLIB:libcmt.lib
   ```
7. Copy extension files to PostgreSQL directories (Run as Administrator):
   ```cmd
   copy /Y pg_systemone.dll "%PG_DIR%\lib\"
   copy /Y pg_systemone.control "%PG_DIR%\share\extension\"
   copy /Y pg_systemone--1.0.sql "%PG_DIR%\share\extension\"
   ```

*(Alternative build via CMake)*:
```cmd
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64 -DPostgreSQL_ROOT="C:/Program Files/PostgreSQL/17"
cmake --build . --config Release
```

---

### Linux (Ubuntu / Debian / RHEL / Fedora)

```bash
# 1. Install PostgreSQL headers and libcurl
sudo apt-get update
sudo apt-get install -y build-essential postgresql-server-dev-17 libcurl4-openssl-dev

# 2. Build and install via PGXS
cd pg_systemone
make
sudo make install
```

---

## ⚙️ Configuration (GUC Parameters)

Connect to your database via `psql` or your preferred GUI client:

```sql
-- 1. Load extension
CREATE EXTENSION IF NOT EXISTS pg_systemone;

-- 2. Configure inference endpoint
SET systemone.api_url = 'http://127.0.0.1:9931/v1/systemone';

-- Or persist globally for all sessions:
ALTER SYSTEM SET systemone.api_url = 'http://127.0.0.1:9931/v1/systemone';
SELECT pg_reload_conf();

-- Verify settings:
SHOW systemone.api_url;
SHOW systemone.cache_enabled;
SHOW systemone.cache_size;
```

### Complete GUC Options Reference:

| Parameter | Type | Default | Description |
|---|---|---|---|
| `systemone.api_url` | string | `http://127.0.0.1:9931/v1/systemone` | URL of the `llama-server` / Julia-1 endpoint |
| `systemone.api_key` | string | `""` | Optional Bearer authorization token |
| `systemone.batch_size` | int | `20` | Suggested batch chunk size for table processing |
| `systemone.cache_enabled` | bool | `true` | Enable/disable in-memory LRU cache |
| `systemone.cache_size` | int | `1000` | Max entries in the in-memory LRU cache |

*(Legacy aliases `jev.api_url`, `jev.api_key`, `jev.batch_size` are fully supported).*

---

## 🚀 In-Memory LRU Cache & High-Load Optimization

When evaluating recurring user inquiries, categories, or duplicate strings, querying `llama-server` on every row wastes CPU cycles.

`pg_systemone` includes an **in-memory LRU cache** operating in PostgreSQL's `TopMemoryContext` with FNV-1a 64-bit hashing:
- **Cache Hit Latency:** **< 0.05 milliseconds**
- **Capacity:** Default 1,000 entries (configurable via `systemone.cache_size`)

### Cache Management Functions:

```sql
-- View cache hit ratio, capacity, and active entries
SELECT * FROM systemone_cache_stats();
/*
 hits | misses | entries | max_entries | hit_ratio 
------+--------+---------+-------------+-----------
  842 |    158 |     158 |        1000 |    0.8420
*/

-- Flush the cache
SELECT systemone_cache_clear();
```

---

## ⚡ Handling 1,000,000 Rows: Batching via SRF

Running a naive `SELECT systemone_choice(text...) FROM table_with_1m_rows` opens 1,000,000 individual HTTP requests and can overwhelm the inference server.

Use the **Set-Returning Function (SRF) batching API**:
* `systemone_choice_batch(states text[], instructions text, criteria text) RETURNS TABLE(idx int, choice text)`
* `systemone_prob_batch(states text[], question text) RETURNS TABLE(idx int, prob float8)`
* `systemone_score_batch(states text[], question text, levels text[]) RETURNS TABLE(idx int, score float8)`

### High-Throughput Pattern (Chunking by 20 rows):

```sql
-- Batch processing 1,000,000 customer reviews in chunks of 20 using array_agg and LATERAL:
WITH chunked AS (
    SELECT 
        (row_number() OVER () - 1) / 20 AS batch_id,
        id,
        review_text
    FROM customer_reviews
),
batched AS (
    SELECT 
        batch_id,
        array_agg(review_text ORDER BY id) AS texts,
        array_agg(id ORDER BY id) AS ids
    FROM chunked
    GROUP BY batch_id
)
SELECT 
    b.ids[res.idx] AS review_id,
    res.choice AS sentiment
FROM batched b
CROSS JOIN LATERAL systemone_choice_batch(
    b.texts,
    'What is the sentiment of this customer review?',
    '{
        "positive": "Praise, high satisfaction, loves product",
        "neutral": "Informational inquiry, balanced comments",
        "negative": "Complaints, defects, anger, poor service"
    }'
) res;
```

---

## 📖 SQL Function Reference

### 1. `systemone_choice(state TEXT, instructions TEXT, criteria TEXT) RETURNS TEXT`
Classifies input text (`state`) into one of the choices defined in the `criteria` JSON object.

```sql
SELECT systemone_choice(
    'My card was charged twice for order #49102. Please issue a refund immediately.',
    'Which team should handle this ticket?',
    '{
        "billing": "Invoices, charges, duplicate billing, refunds",
        "tech_support": "Application errors, site downtime, login bugs",
        "sales": "Pricing, new enterprise plans, upgrades"
    }'
) AS assigned_team;
-- Returns: 'billing'
```

---

### 2. `systemone_prob(state TEXT, question TEXT) RETURNS FLOAT8`
Evaluates binary proposition query (`noul`), returning calibrated probability `0.0 .. 1.0`.

```sql
SELECT systemone_prob(
    'CRITICAL: Main PostgreSQL primary database is unresponsive and dropping client connections!',
    'Is this incident a critical outage (P1/Blocker)?'
) AS is_critical_prob;
-- Returns: 0.974 (97.4% probability)
```

---

### 3. `systemone_score(state TEXT, question TEXT, levels TEXT[]) RETURNS FLOAT8`
Evaluates text against 2 to 10 ordered discrete rubric levels, returning a normalized score `0.0 .. 1.0`.

```sql
SELECT systemone_score(
    'Senior engineer with 8 years building distributed microservices in Go, Kafka, PostgreSQL, Kubernetes',
    'What is the technical qualification seniority of this engineer?',
    ARRAY['junior', 'middle', 'senior', 'lead_architect']
) AS seniority_score;
-- Returns: ~ 0.89 (strong senior / lead)
```

---

### 4. `systemone_match_score(job_desc TEXT, resume TEXT) RETURNS FLOAT8`
Calculates calibrated ATS match score between vacancy requirements and candidate resume.

```sql
SELECT candidate_name,
       ROUND(systemone_match_score(vacancy_requirements, resume_text)::numeric, 3) AS fit_score
FROM candidate_applications
WHERE systemone_match_score(vacancy_requirements, resume_text) >= 0.75
ORDER BY fit_score DESC;
```

---

## 🧪 Interactive SQL Test Script

Copy and run this in your database to verify all features:

```sql
-- 1. Setup sample support tickets
DROP TABLE IF EXISTS support_tickets;
CREATE TABLE support_tickets (
    id SERIAL PRIMARY KEY,
    user_name TEXT,
    message TEXT
);

INSERT INTO support_tickets (user_name, message) VALUES
('Alex', 'I was charged twice for my subscription this month. Please refund!'),
('Sarah', 'HTTP 500 Internal Server Error when exporting PDF reports.'),
('Michael', 'Where can I download the wholesale price list for corporate orders?'),
('Elena', 'CRITICAL! Auth cluster is down, store clerks cannot log into POS registers!'),
('David', 'Funds were deducted from my card 20 minutes ago, but order status is still unpaid.');

-- 2. Execute Decision Model classification with Urgency Scoring
SELECT 
    id,
    user_name,
    message,
    systemone_choice(
        message,
        'Assign target support department',
        '{
            "billing": "Payments, card charges, refunds, invoice issues",
            "support": "Bugs, 500 errors, system down, broken features",
            "sales": "Price lists, quotes, enterprise accounts"
        }'
    ) AS department,
    ROUND(systemone_score(
        message,
        'Rate ticket urgency',
        ARRAY['low', 'medium', 'high', 'critical']
    )::numeric, 3) AS urgency_score
FROM support_tickets
ORDER BY urgency_score DESC;

-- 3. Verify Cache Hit Statistics
SELECT * FROM systemone_cache_stats();
```

---

## 🔍 SEO Keywords & Search Indexing

This project is indexed for developers searching for:
- *PostgreSQL LLM extension*
- *In-database decision models Julia-1*
- *PostgreSQL classification using llama.cpp*
- *Local SystemOne inference in PostgreSQL*
- *pg_systemone vs pgvector*
- *Compile PostgreSQL extension Windows MSVC cl.exe*
- *Fast SQL text routing without Python*
- *Resume parsing and ATS scoring inside Postgres*

---

## 📄 License

Licensed under the **PostgreSQL License / MIT**.  
Contains bundled [`cJSON`](https://github.com/DaveGamble/cJSON) under the MIT License.
