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
              required: [text]
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
          description: Invalid request (missing text, invalid JSON)
        '502':
          description: AI API error
        '503':
          description: AI service not configured
  /api/v1/load_confluence:
    get:
      summary: Load Confluence page
      description: Load page from Confluence by page ID. With include_subpages=1, recursively resolves include macros (sub-pages).
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
          description: If 1 or true, resolve ac:structured-macro include macros and embed sub-page content
      responses:
        '200':
          description: Success, returns Confluence page JSON
          content:
            application/json:
              schema:
                type: object
        '400':
          description: Missing page_id parameter
        '502':
          description: Confluence API error
        '503':
          description: Confluence not configured
  /api/v1/parse_confluence:
    get:
      summary: Parse Confluence page and extract diagrams
      description: Load page (with optional subpages via include_subpages=1), parse content, return list of PlantUML and DrawIO diagrams.
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
          description: If 1 or true, resolve include macros and embed sub-page content before parsing
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
      description: Разбор PlantUML Sequence диаграммы. Извлекает participants/actors и их взаимодействия.
      operationId: postParsePlantumlSequence
      requestBody:
        required: true
        content:
          application/json:
            schema:
              type: object
              required: [text]
              properties:
                text:
                  type: string
                  description: PlantUML Sequence diagram source
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
      description: Разбор PlantUML C4 диаграммы (rectangle, C4-PlantUML Person/System/Rel).
      operationId: postParsePlantumlC4
      requestBody:
        required: true
        content:
          application/json:
            schema:
              type: object
              required: [text]
              properties:
                text:
                  type: string
                  description: PlantUML C4 diagram source
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
        Разбор DrawIO C4 диаграммы. Принимает JSON с полем text, xml, content или drawio.
        Имена компонентов берутся из c4Name, при отсутствии — из c4Description.
        Поддерживает сжатый и несжатый формат XML.
      operationId: postParseDrawio
      requestBody:
        required: true
        content:
          application/json:
            schema:
              type: object
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
                <object id="rel1" c4Type="Relationship" c4Description="Запрос на оплату" c4Technology="REST" source="sys1" target="sys2">
                  <mxCell parent="1"/>
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
