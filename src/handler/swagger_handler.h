#pragma once

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Logger.h>
#include <Poco/Timestamp.h>

#include "request_counter.h"

#include <string>

namespace handlers {

inline const std::string SWAGGER_YAML = R"(openapi: 3.0.3
info:
  title: Poco Template Server API
  description: REST API для разбора диаграмм (PlantUML, DrawIO C4), интеграции с Confluence и AI
  version: 1.0.0
servers:
  - url: /
paths:
  /api/v1/process_with_ai:
    post:
      summary: Process with AI
      description: Send text to AI (DeepSeek/OpenAI API) and get response
      operationId: postProcessWithAI
      requestBody:
        required: true
        content:
          application/json:
            schema:
              type: object
              description: One of text, prompt or message is required
              properties:
                text:
                  type: string
                  description: User prompt/message to AI
                prompt:
                  type: string
                  description: Alternative field for user prompt
                message:
                  type: string
                  description: Alternative field for user message
            example:
              text: "Привет! Расскажи коротко о себе."
      responses:
        '200':
          description: Success
          content:
            application/json:
              schema:
                type: object
                properties:
                  result:
                    type: string
                    description: AI response text
                  success:
                    type: boolean
                    example: true
              example:
                result: "Я AI-ассистент..."
                success: true
        '400':
          description: Invalid request (missing text/prompt/message, empty body, invalid JSON)
        '502':
          description: AI API error
        '503':
          description: AI service not configured
  /api/v1/process_with_ai_async:
    post:
      summary: Start async AI processing
      description: Запускает асинхронную AI-задачу и сразу возвращает request_id (HTTP 202). Статус и результат опрашиваются через /api/v1/async_ai_status и /api/v1/async_ai_result.
      operationId: postProcessWithAIAsync
      requestBody:
        required: true
        content:
          application/json:
            schema:
              type: object
              description: One of text, prompt or message is required
              properties:
                text: { type: string }
                prompt: { type: string }
                message: { type: string }
            example:
              text: "Привет! Коротко опиши себя."
      responses:
        '202':
          description: Task accepted
          content:
            application/json:
              schema:
                type: object
                properties:
                  request_id:
                    type: integer
                    format: int64
                  status:
                    type: string
                    example: running
        '400':
          description: Invalid request (missing text/prompt/message, empty body, invalid JSON)
        '503':
          description: AI service not configured
  /api/v1/async_ai_status:
    get:
      summary: Get async AI task status
      description: Возвращает статус задачи (running/completed/failed), время старта, число пересылок и отправленные байты.
      operationId: getAsyncAIStatus
      parameters:
        - name: request_id
          in: query
          required: true
          schema:
            type: integer
            format: int64
      responses:
        '200':
          description: Status
          content:
            application/json:
              schema:
                type: object
                properties:
                  request_id:
                    type: integer
                    format: int64
                  status:
                    type: string
                    enum: [running, completed, failed]
                  start_time_ms:
                    type: integer
                    format: int64
                  retries:
                    type: integer
                  bytes_sent:
                    type: integer
        '400':
          description: Missing or invalid request_id
        '404':
          description: request_id not found
  /api/v1/async_ai_result:
    get:
      summary: Get async AI task result
      description: Возвращает ответ LLM (completed), описание ошибки (failed) или HTTP 202, если задача ещё выполняется.
      operationId: getAsyncAIResult
      parameters:
        - name: request_id
          in: query
          required: true
          schema:
            type: integer
            format: int64
      responses:
        '200':
          description: Completed or failed
          content:
            application/json:
              schema:
                type: object
                properties:
                  request_id:
                    type: integer
                    format: int64
                  status:
                    type: string
                    enum: [running, completed, failed]
                  result:
                    type: string
                    description: Ответ LLM при status=completed
                  error:
                    type: string
                    description: Описание ошибки при status=failed
        '202':
          description: Task is still running
        '400':
          description: Missing or invalid request_id
        '404':
          description: request_id not found
  /api/v1/load_confluence:
    get:
      summary: Load Confluence page
      description: Load page from Confluence by page ID. With include_subpages=1, resolves include macros, then recursively loads each direct child page (same rules on every level). Response includes a children array per node.
      operationId: getLoadConfluence
      parameters:
        - name: page_id
          in: query
          required: true
          schema:
            type: string
            example: "123456789"
          description: Confluence page ID
        - name: include_subpages
          in: query
          required: false
          schema:
            type: string
            enum: ["1", "true"]
          description: If 1 or true, resolve include macros and recursively load child pages (each child loaded with includes then its own children)
      responses:
        '200':
          description: Success — page_id, html, page; children is a nested array of the same shape when include_subpages=1 (empty array when include_subpages is off)
          content:
            application/json:
              schema:
                type: object
                properties:
                  page_id:
                    type: string
                  html:
                    type: string
                    description: Confluence storage HTML (include macros resolved when include_subpages=1)
                  page:
                    type: object
                    description: Raw Confluence REST API page JSON
                  children:
                    type: array
                    description: Direct child pages (recursive structure), only populated when include_subpages=1
                    items:
                      type: object
        '400':
          description: Missing page_id parameter
        '502':
          description: Confluence API error or empty page body
        '503':
          description: Confluence not configured
  /api/v1/parse_confluence:
    get:
      summary: Parse Confluence page and extract diagrams
      description: Load page; with include_subpages=1, resolve includes and walk the child-page tree (same as load_confluence), collecting diagrams from every page in the tree.
      operationId: getParseConfluence
      parameters:
        - name: page_id
          in: query
          required: true
          schema:
            type: string
            example: "123456789"
          description: Confluence page ID
        - name: include_subpages
          in: query
          required: false
          schema:
            type: string
            enum: ["1", "true"]
          description: If 1 or true, resolve includes and parse every page in the child hierarchy (recursive)
      responses:
        '200':
          description: Success, returns diagrams array
          content:
            application/json:
              schema:
                type: object
                properties:
                  diagrams:
                    type: array
                    items:
                      type: object
                      properties:
                        text:
                          type: string
                          description: Diagram source (PlantUML code or DrawIO XML)
                        format:
                          type: string
                          enum: [plantuml, drawio]
                        subtype:
                          type: string
                          description: Diagram subtype (sequence, c4, component, etc.)
                        sectionTitle:
                          type: string
                        source_page_id:
                          type: string
                          description: Confluence page ID the diagram was taken from
                  count:
                    type: integer
        '400':
          description: Missing page_id parameter
        '502':
          description: Confluence API error
        '503':
          description: Confluence not configured
  /api/v1/parse_plantuml_sequence:
    post:
      summary: Parse PlantUML Sequence diagram
      description: Разбор PlantUML Sequence диаграммы. Извлекает participants/actors и их взаимодействия. JSON с полем text, plantuml или content. Либо raw PlantUML в body.
      operationId: postParsePlantumlSequence
      requestBody:
        required: true
        content:
          application/json:
            schema:
              type: object
              description: One of text, plantuml or content is required
              properties:
                text:
                  type: string
                  description: PlantUML Sequence diagram source
                plantuml:
                  type: string
                  description: Alternative field for diagram source
                content:
                  type: string
                  description: Alternative field for diagram source
            example:
              text: |
                @startuml
                participant User
                participant Frontend
                participant Backend
                User -> Frontend: Login request
                Frontend -> Backend: Authenticate
                Backend --> Frontend: Auth token
                @enduml
      responses:
        '200':
          description: Success
          content:
            application/json:
              schema:
                type: object
                properties:
                  success:
                    type: boolean
                    example: true
                  components:
                    type: array
                    items:
                      type: object
                      properties:
                        id: { type: string }
                        code: { type: string }
                        name: { type: string }
                  requests:
                    type: array
                    items:
                      type: object
                      properties:
                        request_id: { type: integer }
                        component_source_id: { type: string }
                        component_target_id: { type: string }
                        description: { type: string }
              example:
                success: true
                components:
                  - { id: "User", code: "User", name: "User" }
                  - { id: "Frontend", code: "Frontend", name: "Frontend" }
                  - { id: "Backend", code: "Backend", name: "Backend" }
                requests:
                  - { request_id: 1, component_source_id: "User", component_target_id: "Frontend", description: "Login request" }
                  - { request_id: 2, component_source_id: "Frontend", component_target_id: "Backend", description: "Authenticate" }
                  - { request_id: 3, component_source_id: "Backend", component_target_id: "Frontend", description: "Auth token" }
        '400':
          description: Not a sequence diagram or invalid input
  /api/v1/parse_plantuml_c4:
    post:
      summary: Parse PlantUML C4 diagram
      description: Разбор PlantUML C4 диаграммы (rectangle, C4-PlantUML Person/System/Rel). JSON с полем text, plantuml или content. Либо raw PlantUML в body.
      operationId: postParsePlantumlC4
      requestBody:
        required: true
        content:
          application/json:
            schema:
              type: object
              description: One of text, plantuml or content is required
              properties:
                text:
                  type: string
                  description: PlantUML C4 diagram source
                plantuml:
                  type: string
                  description: Alternative field for diagram source
                content:
                  type: string
                  description: Alternative field for diagram source
            example:
              text: |
                @startuml
                rectangle "==GlassFish\n<size:10>[Software System]</size>" <<GlassFish>> as GlassFish
                rectangle "==DSTK\n<size:10>[Software System]</size>" <<DSTK>> as DSTK
                GlassFish .[#707070,thickness=2].> DSTK : "deactivate"
                @enduml
      responses:
        '200':
          description: Success
          content:
            application/json:
              schema:
                type: object
                properties:
                  success:
                    type: boolean
                    example: true
                  components:
                    type: array
                    items:
                      type: object
                      properties:
                        id: { type: string }
                        code: { type: string }
                        name: { type: string }
                        c4_type:
                          type: string
                          description: C4 type (Person, SoftwareSystem, Container, Component)
                  requests:
                    type: array
                    items:
                      type: object
                      properties:
                        request_id: { type: integer }
                        component_source_id: { type: string }
                        component_target_id: { type: string }
                        description: { type: string }
                  parent_child:
                    type: array
                    description: Иерархия родитель-потомок (Container/Component внутри System/Container)
                    items:
                      type: object
                      properties:
                        hierarchy_id: { type: integer }
                        parent_id: { type: string }
                        child_id: { type: string }
              example:
                success: true
                components:
                  - { id: "GlassFish", code: "GlassFish", name: "GlassFish", c4_type: "SoftwareSystem" }
                  - { id: "DSTK", code: "DSTK", name: "DSTK", c4_type: "SoftwareSystem" }
                requests:
                  - { request_id: 1, component_source_id: "GlassFish", component_target_id: "DSTK", description: "deactivate" }
                parent_child: []
        '400':
          description: Not a C4 diagram or invalid input
  /api/v1/parse_drawio:
    post:
      summary: Parse DrawIO C4 diagram
      description: |
        Разбор DrawIO C4 диаграммы. JSON с полем text, xml, content или drawio (XML диаграммы).
        Либо raw XML в body. source/target связей должны быть в атрибутах внутреннего mxCell.
        Поддерживает сжатый (base64+zlib) и несжатый формат XML.
      operationId: postParseDrawio
      requestBody:
        required: true
        content:
          application/json:
            schema:
              type: object
              description: One of text, xml, content or drawio is required
              properties:
                text:
                  type: string
                  description: DrawIO XML (несжатый или base64+zlib)
                xml:
                  type: string
                  description: Alternative field for DrawIO XML
                content:
                  type: string
                  description: Alternative field for DrawIO XML
                drawio:
                  type: string
                  description: Alternative field for DrawIO XML
            example:
              text: |
                <mxfile host="app.diagrams.net">
                <diagram><mxGraphModel><root>
                <mxCell id="0"/><mxCell id="1" parent="0"/>
                <object id="sys1" c4Name="Интернет-магазин" c4Type="SoftwareSystem">
                  <mxCell parent="1"/><mxGeometry x="20" y="40" width="120" height="60" as="geometry"/>
                </object>
                <object id="sys2" c4Name="Платёжная система" c4Type="SoftwareSystem">
                  <mxCell parent="1"/><mxGeometry x="200" y="40" width="120" height="60" as="geometry"/>
                </object>
                <object id="rel1" c4Type="Relationship" c4Description="Запрос на оплату">
                  <mxCell parent="1" source="sys1" target="sys2"/>
                </object>
                </root></mxGraphModel></diagram></mxfile>
      responses:
        '200':
          description: Success
          content:
            application/json:
              schema:
                type: object
                properties:
                  success:
                    type: boolean
                    example: true
                  components:
                    type: array
                    items:
                      type: object
                      properties:
                        id: { type: string }
                        code: { type: string }
                        name: { type: string }
                        c4_type:
                          type: string
                          description: C4 type (Container, SoftwareSystem, Person, etc.)
                  requests:
                    type: array
                    items:
                      type: object
                      properties:
                        request_id: { type: integer }
                        component_source_id: { type: string }
                        component_target_id: { type: string }
                        description: { type: string }
                  parent_child:
                    type: array
                    description: Иерархия родитель-потомок (по геометрии)
                    items:
                      type: object
                      properties:
                        hierarchy_id: { type: integer }
                        parent_id: { type: string }
                        child_id: { type: string }
              example:
                success: true
                components:
                  - { id: "sys1", code: "sys1", name: "Интернет-магазин", c4_type: "SoftwareSystem" }
                  - { id: "sys2", code: "sys2", name: "Платёжная система", c4_type: "SoftwareSystem" }
                requests:
                  - { request_id: 1, component_source_id: "sys1", component_target_id: "sys2", description: "Запрос на оплату" }
                parent_child: []
        '400':
          description: Invalid DrawIO XML or not a diagram
)";

class SwaggerHandler : public Poco::Net::HTTPRequestHandler {
public:
    void handleRequest(Poco::Net::HTTPServerRequest& request,
                       Poco::Net::HTTPServerResponse& response) override {
        Poco::Timestamp start;
        if (g_httpRequests) g_httpRequests->inc();

        response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
        response.setContentType("application/x-yaml");
        response.setContentLength(static_cast<std::streamsize>(SWAGGER_YAML.size()));
        std::ostream& ostr = response.send();
        ostr << SWAGGER_YAML;

        Poco::Timespan elapsed = Poco::Timestamp() - start;
        double seconds = static_cast<double>(elapsed.totalMicroseconds()) / 1000000.0;
        if (g_httpDuration) g_httpDuration->observe(seconds);
        auto& logger = Poco::Logger::get("Server");
        logger.information("200 GET /swagger.yaml from %s, %.2f ms",
                          request.clientAddress().toString(), seconds * 1000.0);
    }
};

} // namespace handlers
