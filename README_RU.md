[🇬🇧 English Version](README.md) | **🇷🇺 Документация на русском**

---

# pg_systemone

[![PostgreSQL](https://img.shields.io/badge/PostgreSQL-14%20|%2015%20|%2016%20|%2017-336791?logo=postgresql&logoColor=white)](https://www.postgresql.org/)
[![License](https://img.shields.io/badge/License-PostgreSQL%20%2F%20MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%20|%20Linux-lightgrey.svg)]()
[![Model](https://img.shields.io/badge/Decision%20Models-Julia--1%20|%20Lev%20|%20Kev%20|%20SystemOne-brightgreen.svg)](https://huggingface.co/ggml-org/Julia-1-GGUF)

> Высокопроизводительное кроссплатформенное C-расширение для **PostgreSQL (14–17)**, позволяющее вызывать локальные модели принятия решений (**Decision Models: Julia-1, Lev, Kev**) прямо внутри SQL-запросов (`SELECT`, `WHERE`, `ORDER BY`) через эндпоинт `POST /v1/systemone` сервера `llama.cpp`.

---

## ⚡ Особенности

* **Decision Models прямо в SQL**: маршрутизация тикетов, классификация текста, скоринг резюме и оценка вероятностей в операторах `SELECT`, `WHERE` и `ORDER BY`.
* **Суб-10мс инференс**: модели класса System 1 (например, `Julia-1` на базе mmBERT) выполняют классификацию в один прямой проход (forward pass) без генерации текста токен-за-токеном.
* **Полная кроссплатформенность**:
  * **Windows** (MSVC `cl.exe` / `link.exe`, CMake) -> `pg_systemone.dll`
  * **Linux** (GCC, PGXS Makefile) -> `pg_systemone.so`
* **Минимум зависимостей**: в проект встроен легковесный парсер `cJSON`, а HTTP-запросы выполняются через надежный `libcurl`.
* **Динамическая настройка эндпоинтов (GUC)**: возможность менять адрес сервера локально через `SET systemone.api_url` или глобально через `ALTER SYSTEM`.
* **Parallel Safe & TOAST-Aware**: безопасная работа в параллельных планах запросов PostgreSQL с контекстами памяти `palloc`/`PG_TRY`.
* **Обратная совместимость**: поддерживаются устаревшие алиасы функций `jev_*`.

---

## 🚀 Быстрый старт: Запуск Julia-1 через llama.cpp

### 1. Скачивание модели Julia-1 (GGUF)
Модель `Julia-1` разработана Supersonic Labs специально для задач классификации и маршрутизации. Официальные веса в формате GGUF размещены в репозитории [ggml-org/Julia-1-GGUF](https://huggingface.co/ggml-org/Julia-1-GGUF).

Скачайте квантованную модель (например, `Q8_0` или `BF16`):
```bash
# Пример скачивания через curl / huggingface-cli
curl -L -o julia-1-q8_0.gguf "https://huggingface.co/ggml-org/Julia-1-GGUF/resolve/main/julia-1-q8_0.gguf"
```

### 2. Запуск llama-server
Запустите сервер `llama.cpp` с указанием модели и желаемого порта (например, `9931`):

```bash
# Windows
llama-server.exe -m julia-1-q8_0.gguf --port 9931 --host 127.0.0.1

# Linux / macOS
./llama-server -m julia-1-q8_0.gguf --port 9931 --host 127.0.0.1
```

> **Важно:** Сервер автоматически активирует эндпоинт `POST /v1/systemone`, если загруженная модель имеет архитектуру decision model.

### 3. Проверка работы сервера через PowerShell / curl

**PowerShell:**
```powershell
$body = @{
    state = "Тестовое сообщение: у меня списали деньги дважды. Срочно верните!"
    questions = @{
        route = @{
            type = "choice"
            instructions = "Куда направить обращение?"
            criteria = @{
                billing = "Проблемы с оплатой и возвраты"
                support = "Техническая поддержка"
            }
        }
    }
} | ConvertTo-Json -Depth 5

$response = Invoke-RestMethod -Uri "http://127.0.0.1:9931/v1/systemone" -Method Post -ContentType "application/json" -Body $body
$response | ConvertTo-Json -Depth 5
```

**cURL:**
```bash
curl -X POST http://127.0.0.1:9931/v1/systemone \
  -H "Content-Type: application/json" \
  -d '{
    "state": "У меня списали деньги дважды.",
    "questions": {
      "route": {
        "type": "choice",
        "instructions": "Куда направить обращение?",
        "criteria": {
          "billing": "Проблемы с оплатой",
          "support": "Техподдержка"
        }
      }
    }
  }'
```

---

## 🛠 Сборка и установка

### Вариант А: Windows (через MSVC cl.exe / link.exe)

> Требуется: установленный Visual Studio 2019/2022 с компонентом «Разработка классических приложений на C++» и установленный PostgreSQL (x64).

1. Откройте **x64 Native Tools Command Prompt for VS 2022** (строго 64-битную консоль!).
2. Перейдите в каталог проекта:
   ```cmd
   cd /d f:\projects\ERP\pg-open-jev
   ```
3. Задайте путь к вашей установке PostgreSQL 17:
   ```cmd
   set PG_DIR=C:\Program Files\PostgreSQL\17
   ```
4. Если `libcurl.dll` уже есть в `%PG_DIR%\bin`, сгенерируйте файл библиотеки `libcurl.lib` прямо из нее:
   ```cmd
   echo EXPORTS > libcurl.def
   for /f "skip=19 tokens=4" %A in ('dumpbin /exports "%PG_DIR%\bin\libcurl.dll"') do if not "%A"=="" echo %A >> libcurl.def
   lib.exe /def:libcurl.def /machine:x64 /out:libcurl.lib
   ```
5. Скомпилируйте исходный код:
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
6. Соберите DLL библиотеку:
   ```cmd
   link.exe /DLL /OUT:pg_systemone.dll ^
      pg_systemone.obj cJSON.obj ^
      "%PG_DIR%\lib\postgres.lib" ^
      libcurl.lib ^
      /NODEFAULTLIB:libcmt.lib
   ```
7. Установите файлы расширения в каталог PostgreSQL (требуются права администратора):
   ```cmd
   copy /Y pg_systemone.dll "%PG_DIR%\lib\"
   copy /Y pg_systemone.control "%PG_DIR%\share\extension\"
   copy /Y pg_systemone--1.0.sql "%PG_DIR%\share\extension\"
   ```

*(Опционально через CMake)*:
```cmd
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64 -DPostgreSQL_ROOT="C:/Program Files/PostgreSQL/17"
cmake --build . --config Release
```

---

### Вариант Б: Linux (Ubuntu / Debian / CentOS / Alpine)

1. Установите пакеты разработки PostgreSQL и `libcurl`:
   ```bash
   # Ubuntu / Debian для PostgreSQL 16/17
   sudo apt-get update
   sudo apt-get install -y build-essential postgresql-server-dev-17 libcurl4-openssl-dev
   ```
2. Соберите и установите расширение:
   ```bash
   cd pg-open-jev
   make
   sudo make install
   ```

---

## ⚙️ Настройка в PostgreSQL

Подключитесь к базе данных через `psql`, DBeaver или pgAdmin:

```sql
-- 1. Подключение расширения
CREATE EXTENSION IF NOT EXISTS pg_systemone;

-- 2. Задание эндпоинта llama-server (на текущую сессию)
SET systemone.api_url = 'http://127.0.0.1:9931/v1/systemone';

-- Или глобально для всего кластера PostgreSQL:
ALTER SYSTEM SET systemone.api_url = 'http://127.0.0.1:9931/v1/systemone';
SELECT pg_reload_conf();

-- Проверка настроек
SHOW systemone.api_url;
```

### Параметры GUC:
| Параметр | По умолчанию | Описание |
|---|---|---|
| `systemone.api_url` | `http://127.0.0.1:9931/v1/systemone` | Адрес HTTP эндпоинта сервера llama.cpp / Julia-1 |
| `systemone.api_key` | `""` | Опциональный Bearer токен авторизации |
| `systemone.batch_size` | `20` | Размер пакета для оптимизации запросов |

*(Алиасы `jev.api_url`, `jev.api_key` также сохраняют работоспособность).*

---

## 📖 SQL Функции и примеры использования

### 1. `systemone_choice(state TEXT, instructions TEXT, criteria TEXT) RETURNS TEXT`
Классифицирует входной текст (`state`) по словарю категорий `criteria` (JSON-объект) и возвращает строковый ключ выбранной категории.

```sql
SELECT systemone_choice(
    'У меня дважды списали деньги за одну подписку, верните средства!',
    'Куда направить обращение клиента?',
    '{
        "billing": "Проблемы с оплатой, списания, двойные платежи, возвраты",
        "tech_support": "Ошибки в приложении, сбои, не работает сайт",
        "sales": "Консультация по новым тарифам и покупка продуктов"
    }'
) AS category;
-- Результат: 'billing'
```

---

### 2. `systemone_prob(state TEXT, question TEXT) RETURNS FLOAT8`
Оценивает вероятность бинарного вопроса (тип `noul`: Да/Нет). Возвращает число с плавающей точкой от `0.0` до `1.0`.

```sql
SELECT systemone_prob(
    'Клиент пишет: база лежит, клиенты в магазинах не могут пробить чеки на кассах!',
    'Является ли инцидент критической аварией (P1/Blocker)?'
) AS is_critical_incident;
-- Результат: 0.963... (96.3% вероятность)
```

---

### 3. `systemone_score(state TEXT, question TEXT, levels TEXT[]) RETURNS FLOAT8`
Оценивает текст по упорядоченной шкале уровней (от 2 до 10 значений) от худшего/минимального к лучшему/максимальному. Возвращает средневзвешенную оценку на непрерывном отрезке от `0.0` до `1.0`.

```sql
SELECT systemone_score(
    'Приложение иногда зависает на 2 секунды при открытии тяжелого отчета',
    'Оцените уровень критичности проблемы',
    ARRAY['незначительная', 'средняя', 'высокая', 'критическая']
) AS severity_score;
-- Результат: ~ 0.28 (между незначительной и средней)
```

---

### 4. `systemone_match_score(job_desc TEXT, resume TEXT) RETURNS FLOAT8`
Специализированная функция для HR-систем: сопоставляет текст резюме кандидата с требованиями вакансии и возвращает оценку соответствия от `0.0` (не подходит) до `1.0` (идеальное совпадение).

```sql
SELECT title, 
       ROUND(systemone_match_score(job_desc, 'Опыт 5 лет Go, PostgreSQL, Kafka, Kubernetes, микросервисы')::numeric, 3) AS fit
FROM job_vacancies
WHERE systemone_match_score(job_desc, 'Опыт 5 лет Go, PostgreSQL, Kafka, Kubernetes, микросервисы') >= 0.70
ORDER BY fit DESC;
```

---

## 🗄 Сидер данных для тестирования (SQL)

Скопируйте и выполните этот скрипт в вашей базе данных, чтобы протестировать все возможности расширения:

```sql
-- 1. Таблица обращений в службу заботы
DROP TABLE IF EXISTS support_tickets;
CREATE TABLE support_tickets (
    id SERIAL PRIMARY KEY,
    user_name TEXT,
    message TEXT,
    created_at TIMESTAMP DEFAULT NOW()
);

INSERT INTO support_tickets (user_name, message) VALUES
('Айдар', 'Тестовое сообщение: у меня списали деньги дважды. Срочно верните!'),
('Алина', 'При попытке экспортировать отчет в PDF вылетает белая страница и ошибка 500'),
('Марат', 'Здравствуйте, где можно скачать прайс-лист для оптовых закупок?'),
('Елена', 'Срочно! Сервер авторизации лежит, кассиры не могут войти в систему!'),
('Ольга', 'Деньги с карты снялись 10 минут назад, но статус заказа до сих пор "Не оплачен"');

-- 2. Запрос автоматической классификации и приоритизации тикетов:
SELECT 
    id,
    user_name,
    message,
    -- Определение отдела для маршрутизации
    systemone_choice(
        message,
        'В какой отдел направить заявку?',
        '{
            "billing": "Проблемы с оплатой, списания, возврат средств",
            "support": "Ошибки, баги, падения сервера, не работает функционал",
            "sales": "Прайс-листы, тарифы, покупки"
        }'
    ) AS target_department,
    -- Оценка срочности (0.0 .. 1.0)
    ROUND(systemone_score(
        message,
        'Оцените уровень срочности обращения',
        ARRAY['низкий', 'средний', 'высокий', 'авария']
    )::numeric, 3) AS urgency_score
FROM support_tickets
ORDER BY urgency_score DESC;
```

---

## ⚡ Оптимизация (Двухэтапный гибридный поиск)

При работе с таблицами на миллионы записей последовательный вызов LLM на каждой строке может создавать избыточную нагрузку на CPU/GPU. Рекомендуется использовать **двухэтапный отбор**:

```sql
-- 1-й этап: быстрый векторный отбор (pgvector) или Full-Text Search (LIMIT 100)
-- 2-й этап: точный скоринг кандидатов моделью Julia-1
SELECT id, title, fit_score
FROM (
    SELECT id, title, job_desc,
           systemone_match_score(job_desc, 'Кандидат: 4 года Python, FastAPI, Docker') AS fit_score
    FROM (
        SELECT id, title, job_desc
        FROM job_vacancies
        ORDER BY embedding <-> $candidate_embedding
        LIMIT 100
    ) preselected
) ranked
WHERE fit_score >= 0.70
ORDER BY fit_score DESC;
```

---

## 📄 Лицензия

Расширение распространяется под лицензией **PostgreSQL License / MIT**.
Включает код парсера [cJSON](https://github.com/DaveGamble/cJSON) (MIT License).
