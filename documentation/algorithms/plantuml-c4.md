# Алгоритм: PlantUML C4

Реализация: `src/plantuml/plantuml_c4.h` → `PlantUmlC4Parser::parse`.

## Общая схема

```
plantumlText
   │
   ├─ 1. isC4Diagram()?  ── нет → {error, success:false}
   │
   ├─ 2. extractSystems()
   │      ├─ extractSystemsRectangle()  — rectangle "==Name" <<Type>> as alias
   │      └─ extractSystemsC4Lib()      — Person/System/Container/Component(...)
   │
   ├─ 3. extractDependencies()
   │      ├─ extractDependenciesArrow() — src .[#color].> tgt : "msg"
   │      └─ extractDependenciesRel()   — Rel(src, tgt, "msg")
   │
   └─ 4. buildJson() → {success, components, requests, parent_child}
```

## Шаг 1. Определение типа диаграммы

Диаграмма считается C4, если выполняется хотя бы одно из условий (в нижнем регистре):

1. есть `rectangle` **и** (`.[` и `].>`) — синтаксис стрелок-зависимостей;
2. есть `rectangle` **и** C4-индикатор: `software system`, `container`, `person`, `system boundary`;
3. есть `person(` или `system(`/`system_ext(` **и** `rel(` — синтаксис C4-библиотеки.

Иначе — ошибка:
```
"Not a C4 diagram. Expected rectangle systems and .[].> dependencies."
```

## Шаг 2. Извлечение компонентов

### 2.1. Rectangle-стиль

Два регулярных выражения (первое — с уточнённой меткой, второе — запасное):

```
rectangle\s+"==([^"\\]+)(?:\n<size:\d+>\[([^\]]+)\]</size>)?(?:\n.*?)?"\s+<<[^>]+>>\s+as\s+([^\s]+)
```

- Группа 1 — имя (после `==`).
- Группа 2 (опционально) — текст в квадратных скобках метки, например `[Software System]`. По нему определяется `c4_type`:
  - содержит `software system` → `SoftwareSystem`
  - `container` → `Container`
  - `component` → `Component`
  - `person` → `Person`
  - `system boundary` → `SystemBoundary`
  - `container boundary` → `ContainerBoundary`
  - иначе → `SoftwareSystem`
- Группа 3 — псевдоним после `as` (становится `id`/`code`).

Второе выражение (без `<size:…>`-сегмента) подхватывает варианты, которые не прошли первое, с `c4_type = "SoftwareSystem"` (если псевдоним ещё не зарегистрирован).

### 2.2. C4-библиотека (C4-PlantUML)

Таблица шаблонов:

| Шаблон | c4_type | Вложенный (hasParent) |
|---|---|---|
| `Person(alias, "Name", …)` | Person | нет |
| `Person_Ext(alias, "Name", …)` | Person | нет |
| `System(alias, "Name", …)` | SoftwareSystem | нет |
| `System_Ext(alias, "Name", …)` | SoftwareSystem | нет |
| `Container(alias, "Name", …)` | Container | **да** |
| `Container_Ext(alias, "Name", …)` | Container | **да** |
| `Component(alias, "Name", …)` | Component | **да** |
| `Component_Ext(alias, "Name", …)` | Component | **да** |

- Для не-вложенных: `id = арг1` (alias), `name = арг2`.
- Для вложенных (`Container`/`Component`): **первый аргумент — это родитель**, второй — имя. `id = арг2`, `parentId = арг1`. (Это семантика C4-PlantUML: `Container(parent, alias, "Name")`.)
- Третий аргумент (описание) не сохраняется в компонент.

## Шаг 3. Извлечение зависимостей

### 3.1. Arrow-стиль

```
([^\s]+)\s+\.[^\]]*\]\.>\s+([^\s]+)\s*:\s*"([^"]*)"
```

- Группа 1 — источник, группа 2 — цель, группа 3 — описание (в кавычках).
- Паттерн соответствует `sys1 .[#red,thickness=2].> ext1 : "REST API"` (между `.` и `].>` могут быть любые атрибуты стиля).
- Зависимость сохраняется, **только если и источник, и цель есть среди компонентов**.
- Совпадения сортируются по позиции; `request_id` нумеруется с 1.

### 3.2. `Rel(...)`-стиль

```
Rel\s*\(\s*([^,]+)\s*,\s*([^,]+)\s*,\s*"([^"]*)"\s*\)
```

- Группы: источник, цель, описание.
- Сохраняется только при известных компонентах (регистронезависимо: `Rel`, `Rel_Neighbor`, `Rel_Back` — попадают по префиксу `Rel`).

## Шаг 4. Формирование JSON

```json
{
  "success": true,
  "components": [
    { "id": "user", "code": "user", "name": "Customer", "c4_type": "Person" },
    { "id": "webapp", "code": "webapp", "name": "Web Application", "c4_type": "SoftwareSystem" }
  ],
  "requests": [
    { "request_id": 1, "component_source_id": "user", "component_target_id": "webapp", "description": "Browses" }
  ],
  "parent_child": [
    { "hierarchy_id": 1, "parent_id": "webapp", "child_id": "api" }
  ]
}
```

`parent_child` строится только для вложенных компонентов (`parentId` непустой и родитель существует).

## Особенности и ограничения

- `SystemBoundary`/`ContainerBoundary` из rectangle-стиля попадают в `components` (в отличие от DrawIO, где границы исключаются).
- C4-библиотека: вложенность `Container(parent,…)` определяет `parent_child`; но если используются `SystemBoundary`-блоки в rectangle-стиле, иерархия не восстанавливается.
- Описания (`4-й параметр` функций C4-библиотеки, текст после `:`) — извлекаются как `description` связей только для `Rel` и arrow-стиля.
- Значения с кавычками внутри строк (например, `"msg with \"quotes\""`) поддерживаются частично.

## Тесты

- `test/fixtures/plantuml_c4/rectangle_c4.puml` — rectangle-стиль с цветной стрелкой.
- `test/fixtures/plantuml_c4/c4_lib.puml` — C4-библиотека: `Person`, `System`, `System_Ext`, `Rel`.
