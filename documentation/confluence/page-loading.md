# Confluence: загрузка страниц

В этом разделе — как клиент загружает страницы, раскрывает include-макросы и обходит дерево подстраниц.

## Загрузка одной страницы

### По ID

```
GET {base}/rest/api/content/{pageId}?expand=body.storage          # v1 (on-prem)
GET {base}/wiki/api/v2/pages/{pageId}?body-format=storage          # v2 (cloud)
```

`getPageById` возвращает **сырой JSON** ответа. Тело страницы извлекается функцией `extractBodyFromPageJson`: из `body.storage.value` (для v2 — тоже `body.storage.value`, т.к. формат body одинаков).

### По URL

`getPageByUrl` извлекает числовой ID из URL регулярным выражением `/pages/(\d+)(?:/|$|\?)`:

- `https://confluence.example.com/pages/123456789/Page+Name` → `123456789`
- `https://confluence.example.com/pages/123456789`

> ⚠️ В публичных HTTP-эндпоинтах сейчас принимается только `page_id` (query-параметр). Метод `getPageByUrl` — внутренний API клиента.

## Include-макросы (`ac:name="include"`)

Confluence позволяет встраивать страницу в страницу макросом «Include page»:

```html
<ac:structured-macro ac:name="include" ac:schema-version="1">
  <ac:parameter ac:name="page">123456</ac:parameter>
  ...
</ac:structured-macro>
```

`getPageWithIncludes` выполняет **рекурсивное** раскрытие:

1. Загружает страницу (`getPageById`).
2. Если тело больше `CONFLUENCE_MAX_EXPANDED_BODY_BYTES` (16 МБ) — не пытается раскрывать.
3. `findIncludeMacros` — находит все макросы include (регекс допускает любой порядок атрибутов: `ac:name` не обязан быть первым). Идентификатор включаемой страницы извлекается в порядке приоритета:
   - `ri:content-id="…"` (стандартный формат);
   - `ri:page-id="…"` (альтернатива);
   - `<ac:parameter ac:name="page">…</ac:parameter>` (числовой ID или заголовок);
   - `ri:content-title="…"` (заголовок — требует поиска по заголовку).
4. `resolvePageId`: если это чистый номер — используется как ID; иначе `getPageIdByTitle` (поиск по заголовку, **только v1**).
5. Если ID не разрешился — макрос заменяется на `<div class="include-error">[Error: page not found: …]</div>`.
6. **Защита от циклов**: `processed` — множество уже раскрытых страниц. Повторное вхождение (A → B → A) пропускается с логом `"Circular include detected for page: …"`.
7. Загружается включаемая страница, её содержимое **само рекурсивно раскрывается** (`processIncludesRecursively`, глубина ≤ `CONFLUENCE_MAX_INCLUDE_DEPTH` = 20, всего макросов ≤ `CONFLUENCE_MAX_INCLUDE_MACROS` = 2000).
8. Оригинальный макрос заменяется раскрытым HTML (с проверкой нового размера против лимита).

Итог — JSON той же структуры, что `getPageById`, но с `body.storage.value`, где все include-макросы заменены содержимым.

### Ограничения раскрытия

- Глубина вложенности includes — не более 20 уровней.
- Общее число раскрытых макросов за вызов — не более 2000.
- Если на каком-то уровне размер превысит 16 МБ — раскрытие прекращается, макрос заменяется на `<div class="include-error">[Include expansion limit reached]</div>`.
- Ошибка загрузки включаемой страницы → `<div class="include-error">[Error loading included page: …]</div>` (не прерывает обработку).

## Дерево подстраниц

### Список прямых детей

`fetchAllDirectChildPageIds` (с пагинацией):

- **v1**: `GET /rest/api/content/{id}/child/page?limit=100&start={start}&expand=page` — постранично до тех пор, пока не вернётся меньше `limit` элементов. ID извлекается из `results[].content.id` (в v1 элемент обёрнут в `content`).
- **v2**: `GET /wiki/api/v2/pages/{id}/children?limit=250` — пагинация через `_links.next` (следующий относительный путь). ID берётся из `results[].id` напрямую.

`extractChildPageIds` умеет разбирать оба формата: ищет `id`, затем `content.id`, затем `page.id`.

### Загрузка поддерева (`loadPageSubtree`)

Рекурсивный обход в глубину (`loadPageSubtreeImpl`):

- Для страницы: если `resolveIncludes=true` — `getPageWithIncludes`, иначе `getPageById`.
- Затем рекурсивно загружаются прямые дети (с тем же правилом).
- **Защита от циклов**: `visited`-множество. Повторное вхождение возвращает узел с `circularSkip = true` (эндпоинт `load_confluence` отдаёт `"error": "circular_reference_in_page_hierarchy"`).
- **Лимиты**: глубина ≤ `CONFLUENCE_MAX_TREE_DEPTH` (200), суммарное число узлов ≤ `CONFLUENCE_MAX_TREE_NODES` (5000).
- Ошибка загрузки детей конкретной страницы не прерывает обход.

Структура результата (`LoadedConfluencePage`):

```cpp
struct LoadedConfluencePage {
    std::string pageId;
    std::string pageJson;                                // сырой JSON REST API
    std::vector<std::unique_ptr<LoadedConfluencePage>> children;
    bool circularSkip;                                   // true — цикл
};
```

### Использование в эндпоинтах

- `load_confluence?include_subpages=1` → `loadPageSubtree(pageId, true)`, результат сериализуется в JSON-дерево: `{page_id, html, page, children: [...]}`.
- `parse_confluence?include_subpages=1` → сначала **BFS** (`collectPageIdsWithDepth`) собирает упорядоченный список `(pageId, depth)` всех страниц дерева, затем для каждой загружает тело с includes и извлекает диаграммы. BFS реализован в обработчике (не в клиенте) и защищён `seen`-множеством от циклов.
