#include "server_apis/api_test.h"

#include "scheduler.h"
#include "server_apis/api_common.h"

// Model runtimes are preloaded by DetectorPool when the Scheduler is
// constructed. This endpoint reports which models are currently loaded and
// how many inference sessions each one owns.
void ApiTestLoadModels(evhttp_request* req, void* arg) {
  auto* scheduler = static_cast<avs_analyzer::Scheduler*>(arg);
  const nlohmann::json models = scheduler->GetModelStatus();
  const bool all_ok = !models.empty();

  nlohmann::json resp;
  resp["code"] = all_ok ? 1000 : 0;
  resp["msg"] = all_ok ? "models loaded" : "no models loaded";
  resp["models"] = models;
  send_json(req, HTTP_OK, resp.dump());
}
