#pragma once

#include <Poco/Net/HTTPRequestHandlerFactory.h>

#include <string>
#include <Poco/Net/HTTPServerRequest.h>

#include "load_confluence_handler.h"
#include "metrics_handler.h"
#include "not_found_handler.h"
#include "parse_confluence_handler.h"
#include "parse_drawio_handler.h"
#include "parse_plantuml_c4_handler.h"
#include "parse_plantuml_sequence_handler.h"
#include "process_with_ai_handler.h"
#include "swagger_handler.h"

namespace handlers {

class RouterFactory : public Poco::Net::HTTPRequestHandlerFactory {
public:
    Poco::Net::HTTPRequestHandler* createRequestHandler(
        const Poco::Net::HTTPServerRequest& request) override {
        const std::string& uri = request.getURI();
        const std::string& method = request.getMethod();

        if (uri == "/api/v1/process_with_ai" && method == "POST") {
            return new ProcessWithAIHandler();
        }
        if (uri == "/api/v1/parse_plantuml_sequence" && method == "POST") {
            return new ParsePlantumlSequenceHandler();
        }
        if (uri == "/api/v1/parse_plantuml_c4" && method == "POST") {
            return new ParsePlantumlC4Handler();
        }
        if (uri == "/api/v1/parse_drawio" && method == "POST") {
            return new ParseDrawioHandler();
        }
        if (method == "GET") {
            size_t q = uri.find('?');
            std::string path = (q != std::string::npos) ? uri.substr(0, q) : uri;
            if (path == "/api/v1/load_confluence") {
                return new LoadConfluenceHandler();
            }
            if (path == "/api/v1/parse_confluence") {
                return new ParseConfluenceHandler();
            }
        }
        if (uri == "/swagger.yaml" && method == "GET") {
            return new SwaggerHandler();
        }
        if (uri == "/metrics" && method == "GET") {
            return new MetricsHandler();
        }
        return new NotFoundHandler();
    }
};

} // namespace handlers
