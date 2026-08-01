# Интеграция с Confluence — обзор

Реализация: `src/confluence/confluence_client.h` (REST-клиент) и `src/confluence/confluence_parser.h` (парсер HTML). Используется эндпоинтами `load_confluence` и `parse_confluence`.

## Возможности

- **Загрузка страницы по ID или URL** (`getPageById`, `getPageByUrl`).
- **Загрузка дерева подстраниц** (`loadPageSubtree`) с защитой от циклов.
- **Рекурсивное раскрытие include-макросов** (`getPageWithIncludes`): `ac:structured-macro ac:name="include"` заменяется содержимым включаемой страницы.
- **Поиск DrawIO-вложений** по содержимому и имени (`getDrawioAttachmentsByContent`, `getDrawioXmlFromAttachment`).
- **Извлечение диаграмм** PlantUML/DrawIO из storage-HTML (в связке с `ConfluenceParser`).

## Поддерживаемые версии Confluence

### On-premises: Server / Data Center (по умолчанию)

- **REST API v1**, базовый путь `/rest/api`.
- Параметр `CONFLUENCE_API_TYPE`: `server`, `onprem`, `datacenter`, `dc` (или пусто/неизвестно) → v1.
- Примеры вызовов:
  - `GET /rest/api/content/{id}?expand=body.storage`
  - `GET /rest/api/content/{id}/child/page?limit=100&start={n}&expand=page`
  - `GET /rest/api/content/{id}/child/attachment`
  - `GET /rest/api/content?title={title}&limit=1` (поиск страницы по заголовку — только v1)

### Atlassian Cloud

- **REST API v2**, базовый путь `/wiki/api/v2`.
- Параметр `CONFLUENCE_API_TYPE`: `cloud` или `v2`.
- Примеры вызовов:
  - `GET /wiki/api/v2/pages/{id}?body-format=storage`
  - `GET /wiki/api/v2/pages/{id}/children?limit=250` (+ пагинация через `_links.next`)
- Отличия от v1: пагинация через `_links.next`, поле тела — `body-format=storage`, у детей нет обёртки `content` (в v2 `id` сразу в элементе).

> ⚠️ Поиск страницы по **заголовку** (`getPageIdByTitle`) реализован только для v1 (Server). На Cloud include-макросы должны ссылаться на числовой ID.

## Авторизация

По умолчанию используется **Personal Access Token (PAT)** как Bearer-токен (рекомендуемый способ для Confluence Server/DC и Cloud):

```
Authorization: Bearer {CONFLUENCE_TOKEN}
```

Режим задаётся переменной `CONFLUENCE_AUTH_TYPE`:

| Значение | Заголовок | Когда использовать |
|---|---|---|
| `pat` (по умолчанию) | `Bearer {CONFLUENCE_TOKEN}` | Confluence PAT (Server/DC, Cloud) |
| `basic` | `Basic base64(CONFLUENCE_USER:CONFLUENCE_TOKEN)` | учётная запись + пароль / API-токен (устаревший режим) |

Рекомендации:
- **Server/DC**: создайте PAT в профиле пользователя (с правами просмотра страниц) и передайте его в `CONFLUENCE_TOKEN`; `CONFLUENCE_USER` не нужен.
- **Cloud**: `CONFLUENCE_TOKEN` — API-токен Atlassian; режим `pat` (Bearer) работает и для него.
- Режим `basic` — только если инфраструктура требует Basic auth (например, пароль служебной учётной записи).

## Контекстный путь (context path)

Многие on-prem установки доступны не в корне домена, а в подкаталоге, например `https://confluence.corp.local/confluence`.

`CONFLUENCE_URL` задаётся с учётом контекста: `https://confluence.corp.local/confluence` (без завершающего `/`).

Клиент автоматически:
- берёт `path` базового URL (`/confluence`);
- если запрашиваемый путь (`/rest/api/...`) не начинается с этого префикса — добавляет его: `https://host/confluence/rest/api/...`.

## Параметры соединения

| Переменная | По умолчанию | Описание |
|---|---|---|
| `CONFLUENCE_URL` | — (обязательна) | Базовый URL без завершающего `/` |
| `CONFLUENCE_TOKEN` | — (обязательна) | Personal Access Token (PAT), отправляется как `Authorization: Bearer <token>` |
| `CONFLUENCE_AUTH_TYPE` | `pat` | `pat` (по умолчанию, Bearer) или `basic` (Basic `user:token`) |
| `CONFLUENCE_USER` | — | Логин; используется только при `CONFLUENCE_AUTH_TYPE=basic` |
| `CONFLUENCE_API_TYPE` | `server` | `server/onprem/datacenter/dc` (v1) или `cloud/v2` |
| `CONFLUENCE_TIMEOUT` | `30` | Таймаут запроса, сек |
| `CONFLUENCE_SSL_VERIFY` | `true` | Проверка SSL-сертификатов |

## Защитные лимиты (все — через переменные окружения)

| Переменная | По умолчанию | Описание |
|---|---|---|
| `CONFLUENCE_RESPONSE_MAX_BYTES` | 52428800 (50 МБ) | Максимальный размер ответа API |
| `CONFLUENCE_ATTACHMENT_MAX_BYTES` | 8388608 (8 МБ) | Максимальный размер загружаемого вложения |
| `CONFLUENCE_MAX_TREE_DEPTH` | 200 | Максимальная глубина дерева подстраниц |
| `CONFLUENCE_MAX_TREE_NODES` | 5000 | Максимальное число узлов дерева |
| `CONFLUENCE_MAX_INCLUDE_DEPTH` | 20 | Максимальная глубина вложенности include-макросов |
| `CONFLUENCE_MAX_INCLUDE_MACROS` | 2000 | Максимальное число раскрытых include-макросов |
| `CONFLUENCE_MAX_EXPANDED_BODY_BYTES` | 16777216 (16 МБ) | Максимальный размер тела после раскрытия includes |
| `CONFLUENCE_PARSE_PAGE_MAX_BYTES` | 4194304 (4 МБ) | Максимальный размер тела страницы для парсинга (эндпоинт `parse_confluence`) |

## Запрос HTTP (детали реализации `doGet`)

1. Проверка: `CONFLUENCE_TOKEN` и `CONFLUENCE_URL` заданы, иначе `std::runtime_error("CONFLUENCE_TOKEN/URL is not set")`.
2. К path добавляется контекстный путь (см. выше).
3. SSL-контекст: `VERIFY_STRICT` или `VERIFY_NONE` (по `CONFLUENCE_SSL_VERIFY`).
4. `HTTPSClientSession` с таймаутом; заголовки `User-Agent: PocoAIServer/1.0 (Diagram Processing Server)`, `Accept: application/json`, `Authorization`.
5. Ответ читается с ограничением размера (`copyToStringWithLimit`); для `/download/attachments/` лимит = `CONFLUENCE_ATTACHMENT_MAX_BYTES`, для остальных — `CONFLUENCE_RESPONSE_MAX_BYTES`.
6. При статусе ≠ 200 — `std::runtime_error("Confluence API error: {status} - {url}")`, тело логируется.

> ⚠️ Некоторые Confluence (за WAF/reverse-proxy) отвечают `403` на запросы **без заголовка `User-Agent`** — поэтому клиент всегда отправляет `User-Agent: PocoAIServer/1.0`. Это обязательное требование для ряда on-prem установок.

## Обработка ошибок в эндпоинтах

- Ошибка с текстом `CONFLUENCE` → HTTP `503 Service Unavailable` (`"Confluence service not configured"`).
- Прочие `runtime_error` (например, `"Confluence API error: 404 …"`) → HTTP `502 Bad Gateway`.
- Сбой загрузки одной из подстраниц (в цикле по дереву) **не прерывает** обход — страница пропускается (`catch (...) { continue; }`).
