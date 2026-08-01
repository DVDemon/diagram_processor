# Алгоритм: DrawIO C4

Реализация: `src/drawio/drawio_parser.h` → `DrawioParser::parse`.
Основан на https://github.com/DVDemon/drawio. Обрабатывает графические «ошибки»: линии, не касающиеся прямоугольников, вложенные прямоугольники.

## Общая схема

```
xmlInput
   │
   ├─ 1. Проверка: содержит <mxfile> или <mxGraphModel>?  ── нет → ошибка
   │
   ├─ 2. extractRootXml()  — извлечь содержимое <diagram>
   │      ├─ несжатый XML (начинается с '<')
   │      └─ сжатый: base64 → zlib inflate (raw, -15) → percent-decode
   │
   ├─ 3. parseObjects()    — <object c4Type=...>: компоненты и связи Relationship
   ├─ 4. parseMxCells()    — <mxCell> со стилем edgeStyle= → «сломанные» связи
   ├─ 5. applyLabelsToBroken() — подтянуть подписи к «сломанным» связям
   ├─ 6. fillParentId()    — иерархия по геометрии (вложенность прямоугольников)
   ├─ 7. fixBrokenRelations() — восстановить связи по sourcePoint/targetPoint
   ├─ 8. fixMissingRelations() — дедупликация пар (source,target), отбрасывание неизвестных
   │
   └─ 9. buildJson() → {success, components, requests, parent_child}
```

## Шаг 1. Проверка формата

Если во входе нет ни `<mxfile`, ни `<mxGraphModel` — ошибка:
```
"Not a DrawIO diagram. Expected mxfile or mxGraphModel XML."
```

## Шаг 2. Извлечение корневого XML

**Специально реализовано строковым поиском (без `std::regex`), чтобы избежать stack overflow на больших диаграммах.**

1. Найти `<diagram` (регистронезависимо), затем закрывающий `>` и `</diagram>`.
2. Содержимое между ними:
   - если начинается с `<` — это уже XML (несжатый формат), вернуть как есть;
   - иначе — **сжатый формат**: строка из base64-алфавита (`0-9A-Za-z-_`), которую DrawIO хранит в `<diagram>`.
3. Декодирование сжатого формата:
   - `Poco::Base64Decoder` → байты;
   - `zlib::inflateInit2(&strm, -15)` (raw deflate) → распаковать в буфер (ёмкость = `сжатый размер × 64`, минимум 128 КБ);
   - **percent-decoding**: последовательности `%XX` превращаются в байты;
   - результат — искомый XML.
4. Если `<diagram>` не найден, но есть `<root>` — вернуть весь XML целиком (для случаев, когда диаграмма передана без обёртки).
5. Иначе — ошибка `"Could not extract diagram content from DrawIO XML."`.

## Шаг 3. parseObjects — компоненты и связи

Проходим по всем `<object …>…</object>` блокам:

### Компоненты (c4Type ≠ "Relationship")

Атрибуты `<object>`:
- `id` — идентификатор;
- `c4Name` — отображаемое имя;
- `c4Type` — тип C4 (по умолчанию `"Component"`, если пусто);
- `c4Description` — описание (используется как имя, если нет `c4Name`).

Геометрия берётся из вложенного `<mxGeometry>` (`x`, `y`, `width`, `height`) → вычисляются `left/top/right/bottom`; флаг `hasGeometry = true`.

### Связи (c4Type == "Relationship")

`source`/`target` ищутся **во внутреннем `<mxCell>`** (вложенном в `<object>`), а не в атрибутах `<object>`:

```xml
<object id="rel1" c4Type="Relationship" c4Description="Запрос на оплату">
  <mxCell parent="1" source="sys1" target="sys2"/>
</object>
```

- Если `source` и `target` непустые → готовая связь `Relation`.
- Иначе — «сломанная» связь `BrokenRelation`: запоминаются координаты `mxGeometry` и точки `mxPoint` с `as="source"/"target"` (sourcePoint/targetPoint).

## Шаг 4. parseMxCells — связи по стилю `edgeStyle=`

Все `<mxCell>` со стилем, содержащим `edgeStyle=`, но без атрибутов `source`/`target`, добавляются в `broken`. Так восстанавливаются связи, сохранённые DrawIO не как C4 `Relationship`, а как обычные ребра.

## Шаг 5. applyLabelsToBroken — подписи рёбер

Собираются `<mxCell>` со стилем `edgeLabel` (атрибуты `parent` + `value`). Для «сломанных» связей с совпадающим `id` (parent ребра = id связи) `c4Description` заменяется на подпись.

## Шаг 6. fillParentId — иерархия по геометрии

Для каждой пары компонентов `(inner, outer)`:

```
inner.left >= outer.left && inner.top >= outer.top &&
inner.right <= outer.right && inner.bottom <= outer.bottom
```

Если внутренний полностью помещается во внешнем — `inner.parentId = outer.id`. Берется первый подходящий родитель.

## Шаг 7. fixBrokenRelations — восстановление связей

Для каждой «сломанной» связи:

- если `source` пуст и есть `sourcePoint` (`hasSrcPoint`) — найти компонент, содержащий точку `(srcX, srcY)`; при нескольких — выбрать **наименьший по площади** (самый вложенный);
- то же для `target` по `targetPoint`;
- если источник и цель найдены — сформировать `Relation`.

Точка попадает в компонент, если `x ∈ [left, right]` и `y ∈ [top, bottom]`.

## Шаг 8. fixMissingRelations — фильтрация и дедупликация

- Отбрасываются связи, где источник/цель неизвестны (не в `components`).
- Дублирующиеся пары `(source, target)` удаляются (остаётся первая).

## Шаг 9. buildJson — вывод

- `components` — все, кроме `c4Type == "SystemScopeBoundary"` / `"ContainerScopeBoundary"` (границы исключаются). Имя: `c4Name` → `c4Description` → `c4Type`.
- `requests` — связи с нумерацией `request_id` с 1.
- `parent_child` — только для компонентов, у которых `parentId` известен и родитель **не является границей**:

```json
{ "hierarchy_id": 1, "parent_id": "sys1", "child_id": "cont1" }
```

## Особенности и ограничения

- Рекурсивная вложенность (несколько уровней) поддерживается через геометрию; при пересекающихся прямоугольниках победит первый найденный родитель.
- Связь DrawIO «родитель-ребёнок» через атрибут `parent` в `mxCell` не используется — только геометрия.
- Для очень больших XML: корневой парсинг — строковый (безопасно), но `parseObjects`/`findTagContent` используют `std::regex`, что накладывает ограничение на размер без переполнения стека.
- Валидность `Relation` требует, чтобы `source`/`target` существовали в `components`; связи «в никуда» отбрасываются.

## Тесты

- `test/fixtures/drawio/simple_c4.drawio` — базовый случай (две сущности + связь).
- `test/fixtures/drawio/multi_component.drawio` — несколько компонентов.
- `test/fixtures/drawio/complex.drawio` — вложенность, «сломанные» связи, восстановление по точкам.
