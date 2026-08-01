# GET /api/v1/parse_confluence

Загружает страницу Confluence, раскрывает include-макросы и **извлекает из HTML все диаграммы** PlantUML и DrawIO (включая DrawIO-вложения). Возвращает список диаграмм с их исходным текстом.

Это ключевой эндпоинт для анализа архитектуры, описанной на страницах Confluence.

## Запрос

- **Метод:** `GET`
- **Query-параметры:**
  - `page_id` *(обязательный)* — числовой ID страницы.
  - `include_subpages` *(опциональный)* — `1`/`true`: раскрыть include-макросы и пройтись по дереву всех дочерних страниц, собирая диаграммы из каждой.

```
GET /api/v1/parse_confluence?page_id=123456789
GET /api/v1/parse_confluence?page_id=123456789&include_subpages=1
```

## Успешный ответ — `200`

```json
{
  "diagrams": [
    {
      "text": "@startuml\nparticipant User\n…\n@enduml",
      "format": "plantuml",
      "subtype": "sequence",
      "sectionTitle": "Login Flow",
      "source_page_id": "123456789"
    },
    {
      "text": "<mxfile …>…</mxfile>",
      "format": "drawio",
      "subtype": "c4",
      "sectionTitle": "Architecture",
      "source_page_id": "123456789"
    }
  ],
  "count": 2
}
```

Поля элемента `diagrams[]`:

| Поле | Описание |
|---|---|
| `text` | Исходник диаграммы: PlantUML-код (`@startuml…@enduml`) или DrawIO-XML (`<mxfile>…` / `<mxGraphModel>…`). Для DrawIO-вложения подставляется полный XML из файла. |
| `format` | `plantuml` или `drawio` |
| `subtype` | Подтип: `sequence`, `c4`, `component`, `usecase`, `unknown` (PlantUML); `c4`, `uml`, `flowchart`, `architecture`, `attachment`, `unknown` (DrawIO). |
| `sectionTitle` | Заголовок раздела страницы (последний `<h2>`/`<h1>` перед макросом), в котором диаграмма расположена. |
| `source_page_id` | ID страницы, из которой взята диаграмма (важно при `include_subpages=1`). |

### Дедупликация

Диаграммы **глобально дедуплицируются** по хэшу `text + format`. Одна и та же диаграмма (например, попавшая на страницу через include-макрос и напрямую, или одинаковый DrawIO-файл на разных страницах) вернётся один раз.

## Ошибки

| Статус | Условие | Тело |
|---|---|---|
| `400` | отсутствует `page_id` | `{"error":"page_id query parameter is required"}` |
| `502` | ошибка API / пустое тело страницы / тело больше `CONFLUENCE_PARSE_PAGE_MAX_BYTES` (4 МБ) | `{"error":"Failed to load page content"}` / `{"error":"Page body too large for parsing"}` |
| `503` | `CONFLUENCE_*` не настроены | `{"error":"Confluence service not configured"}` |
| `500` | внутренняя ошибка | `{"error":"Internal error"}` |
| `405` | метод ≠ GET | `{"error":"Method not allowed"}` |

## Источники диаграмм (что попадает в результат)

1. **PlantUML-макросы** (`<ac:structured-macro ac:name="plantuml">`) — код из CDATA / `ac:plain-text-body`.
2. **DrawIO-макросы** (`ac:name="drawio"` / `"draw.io"` / `"draw-io"`):
   - если макрос содержит XML — возвращается сам XML;
   - если макрос ссылается на вложение — XML загружается из вложения (`getDrawioXmlFromAttachment`), а `subtype` пересчитывается по содержимому.
3. **Inline-код прямо в HTML** — блоки `@startuml…@enduml` и `<mxfile…</mxfile>` / `<mxGraphModel…</mxGraphModel>`, найденные вне макросов.
4. **DrawIO-вложения** — все вложения страницы (и дочерних при `include_subpages=1`), чьё содержимое является DrawIO-XML. Поиск идёт по расширению `.drawio`/`.xml` **и** по media type `application/vnd.jgraph.mxfile` (вложения макроса DrawIO часто не имеют расширения).

Подробно — [confluence/diagram-extraction.md](../confluence/diagram-extraction.md).

## Ограничения по размеру

- `CONFLUENCE_PARSE_PAGE_MAX_BYTES` (по умолчанию `4194304`, 4 МБ) — страницы с телом больше этого размера пропускаются (при `include_subpages=1`) или возвращают `502` (для корневой страницы).
- Для каждого типа диаграмм из inline-кода берётся не более 200 штук (защита от деградации).

### Пример

```bash
curl 'localhost:8080/api/v1/parse_confluence?page_id=123456789&include_subpages=1'
```
