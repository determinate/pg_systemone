**English** | [🇷🇺 Документация на русском](README_RU.md)

---

# pg_systemone

[![PostgreSQL](https://img.shields.io/badge/PostgreSQL-14%20|%2015%20|%2016%20|%2017-336791?logo=postgresql&logoColor=white)](https://www.postgresql.org/)
[![License](https://img.shields.io/badge/License-PostgreSQL%20%2F%20MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%20|%20Linux-lightgrey.svg)]()
[![Model](https://img.shields.io/badge/Decision%20Models-Julia--1%20|%20Lev%20|%20Kev%20|%20SystemOne-brightgreen.svg)](https://huggingface.co/ggml-org/Julia-1-GGUF)

> High-performance, cross-platform PostgreSQL C extension enabling local **System One decision model inference** (such as **Julia-1**, **Lev**, and **Kev**) directly inside SQL queries (`SELECT`, `WHERE`, `ORDER BY`) via the `llama.cpp` `POST /v1/systemone` endpoint.

---

## 💡 Why Decision Models in PostgreSQL?

Traditional generative LLMs (System 2) stream tokens slowly, are non-deterministic, and frequently fail to produce strict schemas under heavy concurrency. 

**Decision models** (System 1, e.g., Supersonic Labs' `Julia-1` built on `mmBERT`) evaluate an input state against structured questions in a **single forward pass**:
* ⚡ **Sub-10ms execution**: No autoregressive token generation loop.
* 🎯 **Strict typing**: Returns calibrated probabilities, exact category choices, or continuous ratings.
* 🐘 **Native database integration**: Filter millions of records, route support tickets, or re-rank candidate resumes directly in your SQL statements without roundtripping through an external Python API or message queue.

---

## ⚡ Features

* **Categorical Routing (`systemone_choice`)**: Route tickets, classify customer inquiries, or assign tags using JSON criteria mappings.
* **Calibrated Probabilities (`systemone_prob`)**: Evaluate binary propositions (`noul` type) returning a `0.0 .. 1.0` float probability.
* **Ordinal Rubric Scoring (`systemone_score`)**: Position unstructured text along an ordered scale of 2 to 10 discrete levels.
* **Resume-to-Job Re-Ranking (`systemone_match_score`)**: Evaluate resume fit for recruitment and ATS systems.
* **Zero Bloat & Cross-Platform**:
  * **Windows**: Compiles cleanly with MSVC (`cl.exe` / `link.exe` x64 or CMake) to `pg_systemone.dll`.
  * **Linux**: Compiles via standard PostgreSQL PGXS (`make && sudo make install`) to `pg_systemone.so`.
* **Embedded JSON Parser**: Bundled [`cJSON`](https://github.com/DaveGamble/cJSON) to eliminate external header dependencies.
* **Dynamic GUC Configuration**: Switch inference endpoints per-session (`SET systemone.api_url`) or globally (`ALTER SYSTEM`).
* **Parallel Safe & Memory Resilient**: Leverages PostgreSQL memory contexts (`palloc`), exception safety (`PG_TRY` / `PG_CATCH`), and `PARALLEL SAFE` markers.
* **Backward Compatible**: Supports legacy `jev_*` function and GUC aliases out-of-the-box.

---

## 🚀 Quickstart: Running Julia-1 with llama.cpp

### 1. Download Model Weights (GGUF)
`Julia-1` is an open-source, ultra-lightweight (144M parameters) decision model. Official GGUF quantized models are hosted on HuggingFace at [ggml-org/Julia-1-GGUF](https://huggingface.co/ggml-org/Julia-1-GGUF).

Download the quantized model (e.g., `Q8_0` or `BF16`):
```bash
# Direct download via curl or huggingface-cli
curl -L -o julia-1-q8_0.gguf "https://huggingface.co/ggml-org/Julia-1-GGUF/resolve/main/julia-1-q8_0.gguf"
```

### 2. Launch llama-server
Start `llama-server` pointing to the downloaded GGUF model and specify your port (e.g. `9931`):

```bash
# Windows
llama-server.exe -m julia-1-q8_0.gguf --port 9931 --host 127.0.0.1

# Linux / macOS
./llama-server -m julia-1-q8_0.gguf --port 9931 --host 127.0.0.1
```

> **Note:** When a decision model is loaded, `llama.cpp` automatically exposes the `POST /v1/systemone` endpoint.

### 3. Verify the Local Inference Endpoint

**Using PowerShell:**
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

**Using cURL:**
```bash
curl -X POST http://127.0.0.1:9931/v1/systemone \
  -H "Content-Type: application/json" \
  -d '{
    "state": "I was charged twice for my subscription this month. Please refund!",
    "questions": {
      "route": {
        "type": "choice",
        "instructions": "Which department should handle this request?",
        "criteria": {
          "billing": "Payment issues, duplicate charges, refunds",
          "support": "Technical bugs, server downtime, login issues"
        }
      }
    }
  }'
```

---

## 🛠 Building and Installation

### Windows (MSVC cl.exe & link.exe)

> **Prerequisites:** Visual Studio 2019/2022 with *Desktop development with C++* installed, and an official 64-bit PostgreSQL (14–17) installation.

1. Open the **x64 Native Tools Command Prompt for VS 2022** (ensure you are using **x64**, not x86).
2. Navigate to the project root:
   ```cmd
   cd /d f:\projects\ERP\pg-open-jev
   ```
3. Set your PostgreSQL installation path:
   ```cmd
   set PG_DIR=C:\Program Files\PostgreSQL\17
   ```
4. Generate `libcurl.lib` directly from the `libcurl.dll` included with PostgreSQL:
   ```cmd
   echo EXPORTS > libcurl.def
   for /f "skip=19 tokens=4" %A in ('dumpbin /exports "%PG_DIR%\bin\libcurl.dll"') do if not "%A"=="" echo %A >> libcurl.def
   lib.exe /def:libcurl.def /machine:x64 /out:libcurl.lib
   ```
5. Compile the C source files into object files:
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
7. Copy the extension files into your PostgreSQL directories (requires Administrator rights):
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

1. Install PostgreSQL development headers and `libcurl`:
   ```bash
   # Ubuntu / Debian (PostgreSQL 14, 15, 16, or 17)
   sudo apt-get update
   sudo apt-get install -y build-essential postgresql-server-dev-17 libcurl4-openssl-dev
   ```
2. Build and install using PGXS:
   ```bash
   cd pg-open-jev
   make
   sudo make install
   ```

---

## ⚙️ Configuration (GUC Parameters)

Connect to PostgreSQL using `psql`, DBeaver, or DataGrip:

```sql
-- 1. Enable extension in your database
CREATE EXTENSION IF NOT EXISTS pg_systemone;

-- 2. Configure target inference server URL (for current session)
SET systemone.api_url = 'http://127.0.0.1:9931/v1/systemone';

-- Or persist globally across the PostgreSQL cluster:
ALTER SYSTEM SET systemone.api_url = 'http://127.0.0.1:9931/v1/systemone';
SELECT pg_reload_conf();

-- Verify current configuration:
SHOW systemone.api_url;
```

### Available GUC Variables:

| Parameter | Type | Default | Description |
|---|---|---|---|
| `systemone.api_url` | string | `http://127.0.0.1:9931/v1/systemone` | Endpoint of the local `llama-server` / Julia-1 instance |
| `systemone.api_key` | string | `""` | Optional Bearer authorization token |
| `systemone.batch_size` | int | `20` | Batch hint for SRF row evaluation |

*(Legacy aliases `jev.api_url`, `jev.api_key`, `jev.batch_size` are also supported).*

---

## 📖 SQL Function Reference

### 1. `systemone_choice(state TEXT, instructions TEXT, criteria TEXT) RETURNS TEXT`
Classifies input text (`state`) into one of the options defined in the `criteria` JSON map. Returns the string key of the selected choice.

```sql
SELECT systemone_choice(
    'My card was charged twice for the same order, please issue a refund.',
    'Which team should handle this ticket?',
    '{
        "billing": "Invoices, payments, charges, duplicate billing, refunds",
        "support": "Application errors, site downtime, login issues",
        "sales": "Pricing, new plans, enterprise upgrades"
    }'
) AS assigned_team;
-- Returns: 'billing'
```

---

### 2. `systemone_prob(state TEXT, question TEXT) RETURNS FLOAT8`
Evaluates a binary proposition (`noul` / yes-no query). Returns a calibrated float probability from `0.0` to `1.0`.

```sql
SELECT systemone_prob(
    'Production database cluster is down, POS registers cannot record customer sales!',
    'Is this incident a critical outage (P1/Blocker)?'
) AS is_critical;
-- Returns: 0.963... (96.3% probability)
```

---

### 3. `systemone_score(state TEXT, question TEXT, levels TEXT[]) RETURNS FLOAT8`
Evaluates unstructured text across an ordered scale of 2 to 10 discrete levels. Returns a normalized, probability-weighted expected score from `0.0` (lowest level) to `1.0` (highest level).

```sql
SELECT systemone_score(
    'Application occasionally lags for 1-2 seconds when rendering large PDF exports',
    'How severe is this bug?',
    ARRAY['trivial', 'minor', 'major', 'critical']
) AS severity_score;
-- Returns: ~ 0.28 (between trivial and minor)
```

---

### 4. `systemone_match_score(job_desc TEXT, resume TEXT) RETURNS FLOAT8`
Designed for HRTech and ATS: scores how closely a candidate resume matches a job description from `0.0` (no match) to `1.0` (perfect match).

```sql
SELECT title, 
       ROUND(systemone_match_score(job_desc, '5 years of experience: Go, PostgreSQL, Kafka, Kubernetes, Highload microservices')::numeric, 3) AS fit_score
FROM job_vacancies
WHERE systemone_match_score(job_desc, '5 years of experience: Go, PostgreSQL, Kafka, Kubernetes, Highload microservices') >= 0.70
ORDER BY fit_score DESC;
```

---

## 🗄 Test Dataset and Seed Script (SQL)

Copy and run this script in your PostgreSQL editor to immediately test all extension capabilities:

```sql
-- 1. Create support tickets table
DROP TABLE IF EXISTS support_tickets;
CREATE TABLE support_tickets (
    id SERIAL PRIMARY KEY,
    user_name TEXT,
    message TEXT,
    created_at TIMESTAMP DEFAULT NOW()
);

-- Seed test data
INSERT INTO support_tickets (user_name, message) VALUES
('Alex', 'Test message: I was charged twice for my subscription. Please refund!'),
('Sarah', 'Getting a blank screen and HTTP 500 Internal Server Error when exporting PDF reports.'),
('Michael', 'Hello, where can I download wholesale price lists for enterprise accounts?'),
('Elena', 'URGENT! Auth cluster is down, store clerks cannot log into POS terminals!'),
('David', 'Funds were deducted from my bank card 15 minutes ago, but order status still shows unpaid.');

-- 2. Run automated classification, routing, and urgency prioritization:
SELECT 
    id,
    user_name,
    message,
    -- Multi-class categorical routing
    systemone_choice(
        message,
        'Which team should handle this ticket?',
        '{
            "billing": "Payments, credit card charges, refunds, invoice issues",
            "support": "Bugs, 500 errors, system down, broken features",
            "sales": "Price lists, quotes, enterprise purchases"
        }'
    ) AS target_department,
    -- Urgency scoring (0.0 .. 1.0)
    ROUND(systemone_score(
        message,
        'Rate ticket urgency',
        ARRAY['low', 'medium', 'high', 'critical']
    )::numeric, 3) AS urgency_score
FROM support_tickets
ORDER BY urgency_score DESC;
```

---

## ⚡ Performance: Two-Stage Hybrid Search

Evaluating decision models sequentially across millions of rows can saturate inference threads. For maximum query throughput, use a **two-stage retrieval pipeline**:
1. **Stage 1 (Vector ANN / Full-Text Search)**: Retrieve top candidates using `pgvector` or tsvector (`LIMIT 100`).
2. **Stage 2 (`systemone_match_score` / `systemone_score`)**: Re-rank the preselected candidates using the decision model.

```sql
SELECT id, title, fit_score
FROM (
    SELECT id, title, job_desc,
           systemone_match_score(job_desc, 'Candidate: 4 years Python, FastAPI, Docker, PostgreSQL') AS fit_score
    FROM (
        SELECT id, title, job_desc
        FROM job_vacancies
        ORDER BY embedding <-> $candidate_embedding
        LIMIT 100
    ) preselected
) ranked
WHERE fit_score >= 0.75
ORDER BY fit_score DESC;
```

---

## 📄 License

This extension is licensed under the **PostgreSQL License / MIT**.  
Includes embedded [`cJSON`](https://github.com/DaveGamble/cJSON) under the MIT License.
