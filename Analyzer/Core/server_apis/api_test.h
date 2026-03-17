#ifndef ANALYZER_SERVER_API_TEST_H
#define ANALYZER_SERVER_API_TEST_H

#include <event2/http.h>

// Test-only API handlers — kept separate from production APIs.
void ApiTestLoadModels(evhttp_request* req, void* arg);

#endif  // ANALYZER_SERVER_API_TEST_H
