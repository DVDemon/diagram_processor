# Конфигурация

Вся конфигурация задаётся через **переменные окружения**. Для локального запуска используется файл `.env` (копия из `.env_example`), который подхватывает `docker-compose`.

## Сервер

| Переменная | По умолчанию | Описание |
|---|---|---|
| `PORT` | `8080` | Порт HTTP-сервера |
| `LOG_LEVEL` | `information` | Уровень лога: `trace`, `debug`, `information`/`info`, `notice`, `warning`/`warn`, `error`, `critical`, `fatal`, `none` |

## AI (OpenAI / DeepSeek) — эндпоинт `process_with_ai`

| Переменная | По умолчанию | Описание |
|---|---|---|
| `OPENAI_API_KEY` | — (обязательна) | API-ключ. Если не задан — `503`. |
| `OPENAI_API_URL` | `https://api.deepseek.com` | Базовый URL API. Транспорт выбирается по схеме: `https://` → TLS, `http://` → plain HTTP (для локальных серверов). Путь `/v1/chat/completions` дописывается автоматически (если URL уже оканчивается на `/v1` — дописывается `/chat/completions`). |
| `OPENAI_MODEL` | `deepseek-chat` | Имя модели |
| `OPENAI_SYSTEM_PROMPT` | — | Системный промпт (отправляется с ролью `system`) |
| `OPENAI_TIMEOUT` | `60` | Таймаут запроса, сек |
| `OPENAI_SSL_VERIFY` | `false` | `true`/`1`/`yes` — проверка SSL-сертификатов (по умолчанию отключена для совместимости) |

### Асинхронные задачи AI (`process_with_ai_async` / `async_ai_status` / `async_ai_result`)

| Переменная | По умолчанию | Описание |
|---|---|---|
| `OPENAI_ASYNC_MAX_THREADS` | `4` | Размер пула потоков (`Poco::ThreadPool`) |
| `OPENAI_MAX_RETRIES` | `3` | Число повторных пересылок запроса при транзиентных ошибках AI (`429`/`5xx`) |
| `OPENAI_ASYNC_MAX_JOBS` | `10000` | Максимум хранимых задач в памяти (старые завершённые удаляются) |

## Confluence — эндпоинты `load_confluence` / `parse_confluence`

### Основные

| Переменная | По умолчанию | Описание |
|---|---|---|
| `CONFLUENCE_URL` | — (обязательна) | Базовый URL без завершающего `/`. On-prem: `https://confluence.corp.local` или с контекстом `https://host/confluence`. Cloud: `https://tenant.atlassian.net`. |
| `CONFLUENCE_TOKEN` | — (обязательна) | Personal Access Token (PAT), отправляется как `Authorization: Bearer <token>` |
| `CONFLUENCE_AUTH_TYPE` | `pat` | `pat` (по умолчанию, Bearer-токен) или `basic` (Basic auth `user:token`) |
| `CONFLUENCE_USER` | — | Логин; используется только при `CONFLUENCE_AUTH_TYPE=basic` |
| `CONFLUENCE_API_TYPE` | `server` | `server`, `onprem`, `datacenter`, `dc` → REST v1 (`/rest/api`). `cloud`, `v2` → REST v2 (`/wiki/api/v2`). |
| `CONFLUENCE_TIMEOUT` | `30` | Таймаут запроса, сек |
| `CONFLUENCE_SSL_VERIFY` | `true` | `true`/`1`/`yes` — проверка SSL |

### Лимиты и защита

| Переменная | По умолчанию | Описание |
|---|---|---|
| `CONFLUENCE_RESPONSE_MAX_BYTES` | `52428800` (50 МБ) | Максимальный размер ответа REST API |
| `CONFLUENCE_ATTACHMENT_MAX_BYTES` | `8388608` (8 МБ) | Максимальный размер скачиваемого вложения |
| `CONFLUENCE_MAX_TREE_DEPTH` | `200` | Максимальная глубина дерева подстраниц |
| `CONFLUENCE_MAX_TREE_NODES` | `5000` | Максимальное число узлов дерева |
| `CONFLUENCE_MAX_INCLUDE_DEPTH` | `20` | Максимальная вложенность include-макросов |
| `CONFLUENCE_MAX_INCLUDE_MACROS` | `2000` | Максимальное число раскрытых include-макросов |
| `CONFLUENCE_MAX_EXPANDED_BODY_BYTES` | `16777216` (16 МБ) | Лимит тела после раскрытия includes |
| `CONFLUENCE_PARSE_PAGE_MAX_BYTES` | `4194304` (4 МБ) | Лимит тела страницы для `parse_confluence` (страницы больше лимита пропускаются/отклоняются) |
| `CONFLUENCE_REGEX_PARSING_ENABLED` | `false` | `true`/`1` — включить regex-парсер макросов (`ConfluenceParser`) в дополнение к безопасному пути. Рекомендуется `false` для нестабильных страниц. |

## Пример `.env`

```bash
PORT=8080
LOG_LEVEL=information

OPENAI_API_KEY=sk-...
OPENAI_API_URL=https://api.deepseek.com
OPENAI_MODEL=deepseek-chat
OPENAI_SYSTEM_PROMPT=From now on, prioritize accuracy...
OPENAI_TIMEOUT=60
OPENAI_SSL_VERIFY=false

CONFLUENCE_URL=https://confluence.corp.local
CONFLUENCE_USER=service-account
CONFLUENCE_TOKEN=...
CONFLUENCE_API_TYPE=server
CONFLUENCE_TIMEOUT=30
CONFLUENCE_SSL_VERIFY=false
CONFLUENCE_REGEX_PARSING_ENABLED=false
```

## Как применить

```bash
cp .env_example .env
# отредактируйте .env

# через docker compose (подхватывает .env автоматически):
docker compose up --build

# или вручную:
set -a && source .env && set +a
./build/poco_template_server
```
