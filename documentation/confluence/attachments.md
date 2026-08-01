# Confluence: анализ вложений (DrawIO-вложения)

Макрос DrawIO в Confluence часто хранит **не сам XML**, а ссылку на вложение (attached `.drawio` / `.xml` файл). Клиент умеет находить и загружать такие вложения.

## Список вложений страницы

```
GET /rest/api/content/{pageId}/child/attachment     # v1
```

Клиент разбирает ответ (`extractAllAttachments`) и формирует список `{title, path, mediaType}`:

- `title` — имя вложения;
- `path` — путь скачивания из `_links.download` (с ведущим `/`), либо fallback: `/download/attachments/{pageId}/{url-encoded title}`;
- `mediaType` — из `extensions.mediaType` (например, `application/vnd.jgraph.mxfile` для DrawIO-вложений).

## Стратегия «по содержимому» (content-based)

Главная особенность: клиент **не доверяет расширению** и решает, является ли файл DrawIO-диаграммой, **анализируя содержимое**.

### Фильтр по расширению и media type (быстрый отсев)

| Режим | Правило |
|---|---|
| `shouldFetchAttachmentForDrawioDiscovery` | Скачивать файлы с расширением `.drawio`/`.xml`, **или** с media type `application/vnd.jgraph.mxfile` (так Confluence-макрос DrawIO хранит диаграмму как вложение **без расширения**) |
| `shouldFetchAttachmentForDrawioLookup` | Скачивать `.drawio`/`.xml` всегда; media type `application/vnd.jgraph.mxfile` всегда; файлы без расширения — если имя совпадает с именем макроса |
| Заблокированные расширения | `png, jpg, jpeg, docx, xlsx, pdf, ppt, pptx, doc, xls, zip, rar, 7z` — не скачиваются никогда (защита от больших бинарных файлов) |

> ⚠️ Вложения макроса DrawIO в Confluence обычно не имеют расширения: имя = `diagramName`, media type = `application/vnd.jgraph.mxfile`. Черновики (`~имя.tmp`, media type `application/x-drawio-draft`) пропускаются.

### Проверка содержимого

Файл считается DrawIO-диаграммой, если его содержимое содержит `<mxfile` или `<mxGraphModel`.

## Способы получения XML

### 1. Перебор вложений по имени (`getDrawioXmlFromAttachment`)

Используется, когда макрос DrawIO содержит имя диаграммы (`diagramName`/`diagramDisplayName` из `ac:parameter` или `ri:filename`).

Алгоритм:

1. Загрузить список вложений страницы.
2. Нормализовать искомое имя: удалить суффиксы `.drawio`/`.xml`, привести к нижнему регистру.
3. Для каждого вложения вычислить `baseName` (имя без расширения) и сравнить с искомым (`tryGetDrawioFromPage`):
   - точное совпадение `baseName == name`;
   - вхождение подстроки в обе стороны;
   - совпадение с `name + ".drawio"` / `name + ".xml"`.
4. Скачать вложение; проверить содержимое на `<mxfile`/`<mxGraphModel`.
   - если имя совпало и содержимое валидно → вернуть XML;
   - иначе запомнить первый найденный DrawIO-XML как `fallbackXml`.
5. **Fallback**: если точного совпадения не было, но на странице **ровно один** DrawIO-файл — вернуть его (это покрывает случаи, когда макрос не содержит корректного имени).
6. При `includeChildPages=true` — дополнительно искать по всем потомкам страницы (`collectDescendantPageIds`), пока XML не найден.

### 2. Полный перебор всех DrawIO-вложений (`getDrawioAttachmentsByContent`)

Возвращает **все** DrawIO-вложения страницы (и потомков при `includeChildPages=true`):

1. Список вложений страницы → отфильтровать по `shouldFetchAttachmentForDrawioDiscovery` (расширение `.drawio`/`.xml` **или** media type `application/vnd.jgraph.mxfile`).
2. Каждое скачать и проверить содержимое.
3. В результат попадают пары `(baseFilename, xml)` — имя без расширения и полный XML.

## Порядок поиска в `parse_confluence`

В обработчике `appendDiagramsForPage`:

1. Если макрос DrawIO — attachment-заглушка вида `<!-- DrawIO: {имя} (attachment) -->` → `getDrawioXmlFromAttachment(pageId, имя)`; при успехе XML подставляется и `subtype` пересчитывается.
2. Inline-XML в HTML (прямые `<mxfile>`/`<mxGraphModel>`).
3. Остальные диаграммы из `ConfluenceParser`.
4. Затем **дополнительный проход по вложениям**: `getDrawioAttachmentsByContent` — все найденные DrawIO-файлы добавляются как диаграммы, если их хэш ещё не был выдан (дедупликация по `text`).

Это гарантирует, что DrawIO-диаграммы, встроенные только как вложения (без inline-XML), всё равно попадут в результат.

## Пример

Макрос на странице:

```html
<ac:structured-macro ac:name="drawio">
  <ac:parameter ac:name="diagramName">architecture</ac:parameter>
  ...
  <ri:attachment ri:filename="architecture.drawio"/>
</ac:structured-macro>
```

Поведение:
1. `ConfluenceParser` извлекает метаданные → имя `architecture`.
2. `getDrawioXmlFromAttachment("123", "architecture")` ищет вложение `architecture.drawio`.
3. Файл скачивается, проверяется на `<mxfile`/`<mxGraphModel`.
4. XML подставляется в `text` диаграммы; `subtype` определяется по содержимому (например, `c4`, если есть `c4Type`).

## Ограничения

- Вложения более `CONFLUENCE_ATTACHMENT_MAX_BYTES` (8 МБ) не читаются (`copyToStringWithLimit` бросает исключение → вложение пропускается).
- Для discovery-режима нескачиваемые расширения пропускаются заранее.
- Возможен «ложный» DrawIO: файл, содержащий подстроку `<mxfile` где-то в тексте, будет распознан как диаграмма.
