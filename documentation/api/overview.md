# API — общие соглашения

## Базовый URL

- Локально: `http://localhost:8080` (порт задаётся переменной `PORT`).
- Все эндпоинты монтируются в корень, префикса версии не имеют (версия зашита в путь эндпоинта: `/api/v1/...`).

## Форматы тела запроса

### JSON (рекомендуется)

Все POST-эндпоинты принимают `application/json`. Для диаграмм источник задаётся одним из полей:

| Эндпоинт | Допустимые ключи |
|---|---|
| `parse_plantuml_sequence` | `text`, `plantuml`, `content` |
| `parse_plantuml_c4` | `text`, `plantuml`, `content` |
| `parse_drawio` | `text`, `xml`, `content`, `drawio` |
| `process_with_ai` | `text`, `prompt`, `message` |

Если ни одного из ключей нет — `400 Bad Request`.

### Raw-текст

Если `Content-Type` не содержит `application/json`, тело запроса интерпретируется как исходный текст диаграммы (PlantUML или XML) / текст для AI. Это удобно для `curl --data-binary`:

```bash
curl -X POST localhost:8080/api/v1/parse_plantuml_c4 \
  -H 'Content-Type: text/plain' \
  --data-binary '@diagram.puml'
```

## Формат успешного ответа парсеров

```json
{
  "success": true,
  "components": [ { "id": "...", "code": "...", "name": "...", "c4_type": "..." } ],
  "requests":   [ { "request_id": 1, "component_source_id": "...", "component_target_id": "...", "description": "..." } ],
  "parent_child": [ { "hierarchy_id": 1, "parent_id": "...", "child_id": "..." } ]
}
```

- `components` — уникальные сущности диаграммы (id, code — псевдоним; name — отображаемое имя).
- `requests` — упорядоченные связи/взаимодействия (для Sequence — порядок строк диаграммы).
- `parent_child` — иерархия «родитель-потомок» (только C4/DrawIO, для Sequence отсутствует).
- Поле `c4_type` — только у C4-парсеров.

### Ошибки парсеров

Если вход не распознан как диаграмма нужного типа, возвращается:

```json
{ "error": "Not a sequence diagram. Only participant/actor and -> interactions are supported.",
  "success": false }
```
с HTTP-статусом `400`.

## HTTP-статусы

| Код | Значение | Примеры |
|---|---|---|
| `200` | Успех | `success: true` |
| `400` | Неверный запрос | отсутствует поле, пустое тело, не JSON, не диаграмма |
| `404` | Не найдено | неизвестный URI |
| `405` | Метод не разрешён | GET на POST-эндпоинт и наоборот |
| `500` | Внутренняя ошибка | непойманное исключение |
| `502` | Ошибка вышестоящего сервиса | ошибка Confluence/AI API, пустое тело страницы |
| `503` | Сервис не настроен | нет `OPENAI_API_KEY` / `CONFLUENCE_*` |

## JSON-экранирование

Обработчики парсеров при ошибках формируют JSON вручную и экранируют `"`, `\`, `\n`, `\r`, `\t` (функция `escapeJson`). Успешные ответы сериализуются через `Poco::JSON::Stringifier`.

## Логирование

Каждый запрос логируется на уровне `information` (или `warning` для 404) в формате:

```
{status} {METHOD} /api/v1/... from {client_ip}, {elapsed_ms} ms
```

Внутренние клиенты (Confluence, OpenAI) пишут в собственные логгеры `ConfluenceClient` / `OpenAIClient`.

## Метрики

Все запросы учитываются в Prometheus-метриках. Подробно — [metrics.md](metrics.md).

## OpenAPI

Полная спецификация доступна на лету: `GET /swagger.yaml`. См. [swagger.md](swagger.md).

## Postman

В репозитории есть коллекция `postman/Poco Template Server.postman_collection.json` с примерами всех запросов.
