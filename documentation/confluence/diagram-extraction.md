# Confluence: извлечение диаграмм из HTML

Здесь описано, как из storage-HTML страницы (`body.storage.value`) извлекаются PlantUML и DrawIO-диаграммы. Задействованы два уровня:

- **`ConfluenceParser`** (`confluence_parser.h`) — «регексный» парсер макросов.
- **`ParseConfluenceHandler`** (`parse_confluence_handler.h`) — безопасный (без regex) путь + дедупликация + вложения.

## Включаемые/выключаемые пути

Режим определяется переменной `CONFLUENCE_REGEX_PARSING_ENABLED` (по умолчанию `false`):

| Значение | Что используется |
|---|---|
| `false` (по умолчанию) | **Безопасный путь**: inline-поиск по строкам (`@startuml…@enduml`, `<mxfile…</mxfile>`, `<mxGraphModel…</mxGraphModel>`) + DrawIO-вложения. `ConfluenceParser` **не вызывается**. Рекомендуется для нестабильных/больших страниц. |
| `true` | Дополнительно вызывается `ConfluenceParser` (regex-поиск макросов). |

Фактически оба пути дополняют друг друга: даже при `false` inline-код и вложения извлекаются; при `true` добавляются диаграммы из аккуратно разобранных `ac:structured-macro`.

## Структура данных диаграммы

```cpp
enum class DiagramFormat { PlantUML, DrawIO };

struct Diagram {
    std::string text;        // PlantUML-код или DrawIO-XML
    DiagramFormat format;
    std::string subtype;     // "sequence", "c4", "component", "usecase", "attachment", ...
    std::string sectionTitle;
};
```

## 1. PlantUML-макросы

`ConfluenceParser::extractPlantUML` ищет макросы `ac:structured-macro` с `ac:name="plantuml"`. Регексы допускают **любой порядок атрибутов** (Confluence часто пишет `ac:macro-id` первым, а не `ac:name`):

```
<ac:structured-macro ac:macro-id="..." ac:name="plantuml" ...> ... </ac:structured-macro>
```

Код извлекается в порядке приоритета:
1. `<!\[CDATA\[ ... \]\]>` внутри макроса;
2. `<ac:plain-text-body> ... </ac:plain-text-body>`.

`sectionTitle` — заголовок последнего `<h2>` (или `<h1>`, если `<h2>` нет) **перед** макросом в HTML.

> ⚠️ В реальных страницах макрос PlantUML может быть **пустым** (только параметр `output=svg`, без CDATA/`ac:plain-text-body`) — тогда кода в `body.storage` нет, и диаграмма не извлекается. Это особенность хранения конкретной страницы/плагина, а не парсера.

## 2. DrawIO-макросы

Ищутся макросы с `ac:name` равным `drawio`, `draw.io`, `draw-io` (атрибут `ac:name` в любом порядке относительно остальных).

- **XML** извлекается в порядке: CDATA → `ac:plain-text-body` → вложенный `<mxfile…</mxfile>` → `<mxGraphModel…</mxGraphModel>`.
- **Если XML нет** — макрос считается вложением: `text` = `"<!-- DrawIO: {diagramName} (attachment) -->"` (или `"<!-- DrawIO diagram (attachment) -->"`), `subtype = "attachment"`.
  - `diagramName` берётся из `ac:parameter ac:name="diagramName"`, затем `diagramDisplayName`, затем `ri:filename`.
  - На этапе обработчика (`appendDiagramsForPage`) этот текст-заглушка распознаётся (префикс `<!-- DrawIO: `), и XML реально подгружается из вложения.

## 3. Inline-код (безопасный путь, regex отключён)

Независимо от `CONFLUENCE_REGEX_PARSING_ENABLED`, обработчик дополнительно сканирует HTML напрямую:

- `@startuml … @enduml` — блоки PlantUML (до 200 штук);
- `<mxfile … </mxfile>` и `<mxGraphModel … </mxGraphModel>` — блоки DrawIO (до 200 штук каждого).

Чтобы не дублировать диаграммы, уже найденные макросами, выполняется сверка по хэшу: inline-блок добавляется, только если его хэш ещё не встречался среди извлечённых диаграмм этого формата.

## 4. DrawIO-вложения

Описанный в [attachments.md](attachments.md) проход `getDrawioAttachmentsByContent` добавляет все DrawIO-файлы страницы (и подстраниц при `include_subpages=1`) как диаграммы. Поиск ведётся **не только по расширению** `.drawio`/`.xml`, но и по media type `application/vnd.jgraph.mxfile` — так распознаются вложения макроса DrawIO **без расширения** (имя = `diagramName`). У диаграмм:
- `text` = полный XML;
- `subtype` = `detectDrawIOType(xml)`;
- `sectionTitle` = имя файла (без расширения).

## Определение подтипов

### PlantUML (`detectPlantUMLType`)

«Голосование» по ключевым словам (приоритет — максимальная сумма баллов):

| Ключевое слово | +баллы в категорию |
|---|---|
| `participant` | sequence +3 |
| `activate` | sequence +1 |
| `->` или `-->` | sequence +1 |
| `component` | component +2 |
| `interface` | component +1 |
| `person` | c4 +1 |
| `system` | c4 +1 |
| `container` | c4 +1 |
| `usecase` | usecase +3 |
| `actor` | usecase +1 |
| `extends` / `includes` | usecase +1 |

Победившая категория → `subtype` (`sequence`/`component`/`c4`/`usecase`); если баллов нет — `unknown`.

> Та же функция дублирована в обработчике (`detectPlantUmlTypeInHandler`), чтобы не зависеть от включения `ConfluenceParser`.

### DrawIO (`detectDrawIOType`)

- Содержит `c4Type` → `c4`;
- содержит `uml`/`sequence` → `uml`;
- `flowchart`/`flow` → `flowchart`;
- `architecture`/`component` → `architecture`;
- иначе → `unknown`.

## Дедупликация (глобальная)

В `ParseConfluenceHandler` ведётся множество `emittedDiagramHashes` — хэши `std::hash(text) ^ format_const`:

- константы: PlantUML → `0x243f6a8885a308d3`, DrawIO → `0x9e3779b97f4a7c15` (чтобы одинаковый текст в разных форматах не склеивался);
- диаграмма попадает в ответ только если её хэш ещё не встречался;
- это устраняет дубли при попадании одной диаграммы через макрос, inline и вложение одновременно, а также при повторении на нескольких страницах (`include_subpages=1`);
- отдельное множество `resolvedKeys` (`pageId|имяФайла`) исключает повторное разрешение одного DrawIO-вложения.

## Итоговый порядок сборки (в `appendDiagramsForPage`)

1. (Если `CONFLUENCE_REGEX_PARSING_ENABLED=true`) `ConfluenceParser::parse(html)` → диаграммы из макросов.
2. Разрешение attachment-заглушек → подстановка XML из вложений.
3. Дополнение inline-DrawIO-блоками (по хэшу).
4. Дополнение inline-PlantUML-блоками (по хэшу).
5. Добавление диаграмм из макросов (глобальный дедуп-хэш).
6. Дополнение DrawIO-вложениями (глобальный дедуп-хэш + `resolvedKeys`).

## Логирование прогресса

При `include_subpages=1` обработчик логирует прогресс для каждой страницы и каждого вызова REST API:

```
parse_confluence progress: depth={d} resources_loaded={n} pages_processed={m} stage=child_list page_id={id}
parse_confluence progress: depth={d} resources_loaded={n} pages_processed={m} stage=page_body page_id={id}
parse_confluence progress: depth={d} resources_loaded={n} pages_processed={m} stage=page_parsed page_id={id}
```

Это помогает отслеживать длительные обходы больших деревьев.

## Ограничения

- Ограничение `maxParsePageBytes()` (4 МБ по умолчанию) — слишком большие страницы пропускаются.
- Каждый вид inline-поиска ограничен 200 диаграммами.
- `ConfluenceParser` использует `std::regex`, поэтому для очень больших HTML при `CONFLUENCE_REGEX_PARSING_ENABLED=true` возможен stack overflow — это причина, по которой безопасный путь по умолчанию.
