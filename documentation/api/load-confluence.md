# GET /api/v1/load_confluence

Загружает страницу из Confluence по `page_id` и возвращает её storage-HTML и исходный JSON страницы из REST API.

## Запрос

- **Метод:** `GET`
- **Query-параметры:**
  - `page_id` *(обязательный)* — числовой ID страницы Confluence.
  - `include_subpages` *(опциональный)* — `1` или `true`, чтобы раскрыть include-макросы и **рекурсивно** загрузить дерево прямых дочерних страниц.

```
GET /api/v1/load_confluence?page_id=123456789
GET /api/v1/load_confluence?page_id=123456789&include_subpages=1
```

## Успешный ответ — `200`

### Без `include_subpages`

```json
{
  "page_id": "123456789",
  "html": "…storage-HTML страницы (body.storage.value)…",
  "page": { "…": "…" },
  "children": []
}
```

- `html` — HTML в storage-формате Confluence.
- `page` — сырой JSON ответа REST API (`GET /rest/api/content/{id}?expand=body.storage` или `GET /wiki/api/v2/pages/{id}?body-format=storage` для Cloud).
- `children` — всегда `[]` (пусто).

### С `include_subpages=1`

Тот же объект, но `children` содержит массив дочерних страниц той же структуры (рекурсивно):

```json
{
  "page_id": "123456789",
  "html": "…",
  "page": { "…": "…" },
  "children": [
    {
      "page_id": "987654321",
      "html": "…",
      "page": { "…": "…" },
      "children": []
    }
  ]
}
```

Порядок раскрытия: сначала на странице раскрываются include-макросы, затем загружаются прямые дочерние страницы (для каждой — тот же алгоритм).

### Особый случай: цикл в иерархии

Если при обходе дерева встречена уже посещённая страница, узел помечается:

```json
{
  "page_id": "…",
  "error": "circular_reference_in_page_hierarchy",
  "children": []
}
```

## Ошибки

| Статус | Условие | Тело |
|---|---|---|
| `400` | отсутствует `page_id` | `{"error":"page_id query parameter is required"}` |
| `502` | ошибка API Confluence, пустой `html` | `{"error":"Failed to load page content"}` |
| `503` | `CONFLUENCE_*` не настроены | `{"error":"Confluence service not configured"}` |
| `500` | внутренняя ошибка | `{"error":"Internal error"}` |
| `405` | метод ≠ GET | `{"error":"Method not allowed"}` |

## Настройки

Для работы нужны переменные `CONFLUENCE_URL`, `CONFLUENCE_USER`/`CONFLUENCE_TOKEN` (и опционально `CONFLUENCE_API_TYPE`, `CONFLUENCE_TIMEOUT`, `CONFLUENCE_SSL_VERIFY`). Подробно — [configuration.md](../configuration.md) и [confluence/overview.md](../confluence/overview.md).

### Пример

```bash
curl 'localhost:8080/api/v1/load_confluence?page_id=123456789&include_subpages=1'
```
