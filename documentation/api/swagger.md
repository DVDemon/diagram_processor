# GET /swagger.yaml

Возвращает OpenAPI 3.0.3 спецификацию API.

## Запрос

```
GET /swagger.yaml
```

- **Content-Type ответа:** `application/x-yaml`
- **Статус:** `200`

Спецификация описывает все публичные эндпоинты, схемы запросов/ответов и примеры. Она хранится в `src/handler/swagger_handler.h` (строка `SWAGGER_YAML`) и отдаётся как есть.

## Использование

- Откройте в Swagger UI / Swagger Editor:
  - `https://editor.swagger.io/?url=http://localhost:8080/swagger.yaml`
- Или импортируйте в Postman/Insomnia как OpenAPI.

## Содержимое спецификации

| Путь | Описание |
|---|---|
| `/api/v1/process_with_ai` | POST — AI-обработка текста |
| `/api/v1/load_confluence` | GET — загрузка страницы Confluence |
| `/api/v1/parse_confluence` | GET — извлечение диаграмм из страницы Confluence |
| `/api/v1/parse_plantuml_sequence` | POST — разбор PlantUML Sequence |
| `/api/v1/parse_plantuml_c4` | POST — разбор PlantUML C4 |
| `/api/v1/parse_drawio` | POST — разбор DrawIO C4 |

> ⚠️ В спецификации НЕ описаны служебные эндпоинты `/metrics` и `/swagger.yaml`.
