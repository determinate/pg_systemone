[🇬🇧 English Version](README.md) | **🇷🇺 Документация на русском**

---

# pg_systemone

[![PostgreSQL](https://img.shields.io/badge/PostgreSQL-14%20|%2015%20|%2016%20|%2017-336791?logo=postgresql&logoColor=white)](https://www.postgresql.org/)
[![License](https://img.shields.io/badge/License-PostgreSQL%20%2F%20MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%20|%20Linux-lightgrey.svg)]()
[![Model](https://img.shields.io/badge/Decision%20Models-Julia--1%20|%20Lev%20|%20Kev%20|%20SystemOne-brightgreen.svg)](https://huggingface.co/ggml-org/Julia-1-GGUF)
[![Inference Engine](https://img.shields.io/badge/Engine-llama.cpp%20server-orange.svg)](https://github.com/ggerganov/llama.cpp)
[![Cache](https://img.shields.io/badge/Cache-LRU%20TopMemoryContext-success.svg)]()

> **Быстрый локальный инференс моделей принятия решений (System 1 Decision Models) прямо внутри SQL-запросов PostgreSQL.**  
> Нативное C-расширение для PostgreSQL, связывающее СУБД с `llama-server` (`/v1/systemone`) для моделей **Julia-1**, **Lev** и **Kev** со встроенным **LRU-кешем в оперативной памяти**, **пулом постоянных Keep-Alive HTTP-соединений** и **батчингом через Set-Returning Functions (SRF)** для обработки миллионов строк.


## 💡 Зачем нужны Decision Models в PostgreSQL?

Обычные генеративные нейросети (System 2, такие как LLaMA, Mistral, ChatGPT) генерируют текст токен за токеном, работают медленно (от 500 мс до 5 секунд на запрос), подвержены галлюцинациям и часто ломают строгую валидацию JSON при высоких нагрузках.

**Модели принятия решений** (System 1, например, **Julia-1** от Supersonic Labs на архитектуре `mmBERT`) оценивают входной текст и вопросы за **один прямой проход (forward pass)**:
* ⚡ **Суб-10мс инференс**: нет медленного цикла генерации текста.
* 🎯 **Гарантированная типизация**: возвращаются точные категории, калиброванные вероятности и численные скоры по шкале.
* 🐘 **Без перегонки данных**: классификация и скоринг выполняются прямо в базе (`SELECT`, `WHERE`, `ORDER BY`), не перекачивая терабайты в Python-микросервисы или очереди Kafka.
* 🛡️ **Встроенный LRU-кеш**: повторяющиеся запросы отдаются за **< 0.05 мс** без нагрузки на `llama-server`.
* 📦 **Батчинг на миллионы строк**: функции SRF позволяют пакетировать вызовы по 20–50 строк, предотвращая исчерпание сокетов.

---

| Критерий | Генеративные LLM (System 2) | Векторный поиск (pgvector) | **pg_systemone (Decision Models)** |
|---|---|---|---|
| **Скорость на строку** | 500 мс – 5000 мс | < 5 мс | **5 мс – 15 мс** (0.05 мс из кеша) |
| **Результат** | Неструктурированный текст | Векторная дистанция (float) | **Точная категория или калиброванная вероятность** |
| **Детерминированность** | ❌ Возможны галлюцинации | ✔ Геометрическая близость | **✔ Строгая детерминированность** |
| **Размер модели** | 7B – 70B параметров | 50M – 300M параметров | **144M параметров (очень легкая)** |
| **Требования к железу**| Мощный GPU с 16+ ГБ VRAM | CPU / RAM | **Легко запускается на обычном CPU** |

---

## ⚡ Основные возможности

* **Категоризация и маршрутизация (`systemone_choice`)**: маршрутизация тикетов поддержки, распределение лидов или тегирование по произвольным критериям.
* **Калиброванная вероятность (`systemone_prob`)**: проверка бинарных гипотез (`noul`), возвращает числовую вероятность от `0.0` до `1.0`.
* **Оценка по шкале уровней (`systemone_score`)**: позиционирование текста по упорядоченной шкале от 2 до 10 градаций (например: грейды, срочность, серьезность багов).
* **Скоринг резюме для HR/ATS (`systemone_match_score`)**: оценка соответствия резюме кандидата требованиям вакансии от `0.0` до `1.0`.
* **Пакетная обработка (`systemone_*_batch`)**: Set-Returning Functions (SRF) для быстрой обработки массивов строк.
* **LRU-кеш в памяти**: кеш на 1000 записей в `TopMemoryContext` PostgreSQL с FNV-1a 64-битным хешированием.
* **Пул Keep-Alive соединений**: многократное переиспользование дескриптора `CURL*` без повторных TCP-рукопожатий.
* **Кроссплатформенность**:
  * **Windows**: чистая сборка через MSVC `cl.exe` / `link.exe` или CMake.
  * **Linux**: сборка через PGXS `make && sudo make install`.
* **Обратная совместимость**: поддержка устаревших алиасов `jev_*`.

---

## 🚀 Быстрый старт: Запуск Julia-1 через llama.cpp

### 1. Скачивание весов модели (GGUF)
Модель `Julia-1` доступна на HuggingFace: [ggml-org/Julia-1-GGUF](https://huggingface.co/ggml-org/Julia-1-GGUF).

```bash
# Скачивание квантованной модели (Q8_0 или BF16)
curl -L -o julia-1-q8_0.gguf "https://huggingface.co/ggml-org/Julia-1-GGUF/resolve/main/julia-1-q8_0.gguf"
```

### 2. Запуск llama-server
Запустите `llama-server` с указанием порта (например, `9931`):

```bash
# Windows
llama-server.exe -m julia-1-q8_0.gguf --port 9931 --host 127.0.0.1

# Linux / macOS
./llama-server -m julia-1-q8_0.gguf --port 9931 --host 127.0.0.1
```

> **Примечание:** `llama-server` автоматически открывает эндпоинт `POST /v1/systemone`, когда видит модель класса decision model.

### 3. Проверка эндпоинта через PowerShell

```powershell
$body = @{
    state = "Опыт 6 лет: проектирование распределенных систем на Go, Kafka, Kubernetes, clickhouse, highload"
    questions = @{
        seniority = @{
            type = "score"
            instructions = "Каков уровень квалификации инженера?"
            criteria = @("junior", "middle", "senior", "lead / architect")
        }
    }
} | ConvertTo-Json -Depth 5

$response = Invoke-RestMethod -Uri "http://127.0.0.1:9931/v1/systemone" -Method Post -ContentType "application/json" -Body $body
$response | ConvertTo-Json -Depth 5
```

---

## 🛠 Сборка и установка

### Windows (MSVC cl.exe & link.exe)

> **Требования:** Visual Studio 2019/2022 (*Desktop development with C++*) и официальный 64-битный PostgreSQL 14–17.

1. Откройте **x64 Native Tools Command Prompt for VS 2022** (обязательно **x64**).
2. Перейдите в каталог проекта:
   ```cmd
   cd /d f:\projects\ERP\pg-open-jev
   ```
3. Задайте путь к вашей установке PostgreSQL:
   ```cmd
   set PG_DIR=C:\Program Files\PostgreSQL\17
   ```
4. Сгенерируйте файл импорта `libcurl.lib` из встроенной в PostgreSQL библиотеки `libcurl.dll`:
   ```cmd
   echo EXPORTS > libcurl.def
   for /f "skip=19 tokens=4" %A in ('dumpbin /exports "%PG_DIR%\bin\libcurl.dll"') do if not "%A"=="" echo %A >> libcurl.def
   lib.exe /def:libcurl.def /machine:x64 /out:libcurl.lib
   ```
5. Скомпилируйте C-файлы:
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
6. Скомпонуйте DLL:
   ```cmd
   link.exe /DLL /OUT:pg_systemone.dll ^
      pg_systemone.obj cJSON.obj ^
      "%PG_DIR%\lib\postgres.lib" ^
      libcurl.lib ^
      /NODEFAULTLIB:libcmt.lib
   ```
7. Скопируйте файлы расширения в каталоги PostgreSQL (от имени Администратора):
   ```cmd
   copy /Y pg_systemone.dll "%PG_DIR%\lib\"
   copy /Y pg_systemone.control "%PG_DIR%\share\extension\"
   copy /Y pg_systemone--1.0.sql "%PG_DIR%\share\extension\"
   ```

---

### Linux (Ubuntu / Debian / RHEL / Fedora)

```bash
# Установка заголовочных файлов PostgreSQL и libcurl
sudo apt-get update
sudo apt-get install -y build-essential postgresql-server-dev-17 libcurl4-openssl-dev

# Сборка через PGXS
cd pg_systemone
make
sudo make install
```

---

## ⚙️ Настройка параметров (GUC)

Подключитесь к базе через `psql` или любой SQL-клиент:

```sql
-- 1. Подключение расширения
CREATE EXTENSION IF NOT EXISTS pg_systemone;

-- 2. Настройка адреса инференс-сервера
SET systemone.api_url = 'http://127.0.0.1:9931/v1/systemone';

-- Для сохранения на уровне всего кластера:
ALTER SYSTEM SET systemone.api_url = 'http://127.0.0.1:9931/v1/systemone';
SELECT pg_reload_conf();

-- Просмотр настроек:
SHOW systemone.api_url;
SHOW systemone.cache_enabled;
SHOW systemone.cache_size;
```

---

## 🚀 Встроенный LRU-кеш в оперативной памяти

При повторном анализе однотипных сообщений или повторяющихся текстов обращение к `llama-server` не требуется. `pg_systemone` кеширует результат в `TopMemoryContext`:
* **Время ответа из кеша:** **< 0.05 мс**
* **Размер кеша по умолчанию:** 1000 записей

```sql
-- Просмотр статистики кеша (хиты, промахи, процент попадания)
SELECT * FROM systemone_cache_stats();
/*
 hits | misses | entries | max_entries | hit_ratio 
------+--------+---------+-------------+-----------
  842 |    158 |     158 |        1000 |    0.8420
*/

-- Очистка кеша
SELECT systemone_cache_clear();
```

---

## ⚡ Обработка 1 000 000 строк: батчинг через SRF

Прямой вызов `SELECT systemone_choice(...) FROM table_1m` создает по одному HTTP-запросу на каждую строку и перегрузит сокеты. Для миллионных таблиц используйте **пакетные функции (SRF)**:

```sql
-- Обработка таблицы с 1 000 000 отзывов пачками по 20 строк:
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
    'Какова тональность отзыва покупателя?',
    '{
        "positive": "Положительный отзыв, благодарность, всё понравилось",
        "neutral": "Нейтральный вопрос по доставке или товару",
        "negative": "Жалоба, брак, гнев, требование возврата"
    }'
) res;
```

---

## 📖 Справочник SQL-функций

### 1. `systemone_choice(state TEXT, instructions TEXT, criteria TEXT) RETURNS TEXT`
Классифицирует входящий текст (`state`) по вариантам из JSON-карты `criteria`.

```sql
SELECT systemone_choice(
    'У меня списали оплату за подписку дважды, верните деньги!',
    'В какой отдел направить обращение?',
    '{
        "billing": "Оплата, списания, двойные платежи, счета, возвраты",
        "support": "Ошибки системы, сбои, не работает сайт",
        "sales": "Тарифы, оптовые закупки, корпоративные клиенты"
    }'
) AS department;
-- Результат: 'billing'
```

---

### 2. `systemone_prob(state TEXT, question TEXT) RETURNS FLOAT8`
Оценивает бинарную вероятность вопроса (`noul`), возвращая число от `0.0` до `1.0`.

```sql
SELECT systemone_prob(
    'База данных PostgreSQL упала, кассовые узлы в магазинах заблокированы!',
    'Является ли инцидент критической аварией (P1/Blocker)?'
) AS is_critical_prob;
-- Результат: 0.974 (97.4% вероятность)
```

---

### 3. `systemone_score(state TEXT, question TEXT, levels TEXT[]) RETURNS FLOAT8`
Скоринг по упорядоченной шкале от 2 до 10 градаций. Возвращает нормализованную непрерывную оценку от `0.0` до `1.0`.

```sql
SELECT systemone_score(
    'Ведущий инженер, 8 лет опыта: Go, Kafka, PostgreSQL, Kubernetes, highload',
    'Каков грейд инженера?',
    ARRAY['junior', 'middle', 'senior', 'lead_architect']
) AS seniority_score;
-- Результат: ~ 0.89 (уверенный Senior/Lead)
```

---

### 4. `systemone_match_score(job_desc TEXT, resume TEXT) RETURNS FLOAT8`
Скоринг соответствия резюме кандидата требованиям вакансии (ATS скоринг).

```sql
SELECT candidate_name,
       ROUND(systemone_match_score(job_desc, resume_text)::numeric, 3) AS fit_score
FROM candidates
WHERE systemone_match_score(job_desc, resume_text) >= 0.75
ORDER BY fit_score DESC;
```

---

## 🧪 Тестовый скрипт для проверки в PostgreSQL

```sql
-- Создание таблицы обращений
DROP TABLE IF EXISTS support_tickets;
CREATE TABLE support_tickets (
    id SERIAL PRIMARY KEY,
    user_name TEXT,
    message TEXT
);

INSERT INTO support_tickets (user_name, message) VALUES
('Алексей', 'Списали деньги дважды за подписку. Срочно верните!'),
('Ирина', 'При экспорте отчета в PDF выдает ошибку 500 Internal Server Error.'),
('Михаил', 'Здравствуйте, где скачать оптовый прайс-лист для юрлиц?'),
('Елена', 'СРОЧНО! Упал сервер авторизации, кассиры не могут пробить чеки!'),
('Данияр', 'Деньги с карты ушли 20 минут назад, а статус заказа до сих пор не оплачен.');

-- Маршрутизация и скоринг срочности
SELECT 
    id,
    user_name,
    message,
    systemone_choice(
        message,
        'Определите отдел',
        '{
            "billing": "Счета, списания, возвраты, проблемы с картами",
            "support": "Ошибки, падение серверов, баги",
            "sales": "Прайс-листы, закупки, корпоративные тарифы"
        }'
    ) AS department,
    ROUND(systemone_score(
        message,
        'Оцените срочность тикета',
        ARRAY['низкая', 'средняя', 'высокая', 'критическая']
    )::numeric, 3) AS urgency_score
FROM support_tickets
ORDER BY urgency_score DESC;

-- Проверка кеша
SELECT * FROM systemone_cache_stats();
```

---

## 📄 Лицензия

Расширение распространяется под лицензией **PostgreSQL License / MIT**.  
Включает встроенный парсер [`cJSON`](https://github.com/DaveGamble/cJSON) под лицензией MIT.
