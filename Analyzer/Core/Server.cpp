#include "Server.h"

#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/http_struct.h>
#include <json/json.h>
#include <json/value.h>
#include <thread>
#include "Control.h"
#include "Config.h"
#include "Scheduler.h"
#include "Utils/Log.h"
#include "Utils/Common.h"

using namespace AVSAnalyzer;

#define RECV_BUF_MAX_SIZE 1024*8


void HTTPServerStart(Scheduler &scheduler) {
    scheduler.SetState(true);

    std::thread([&]() {

        LOGI("启动分析器服务：http://0.0.0.0:%d", scheduler.GetConfig()->analyzer_port);
        
        event_config* evt_config = event_config_new();
        event_base* base = event_base_new_with_config(evt_config);
        evhttp* http = evhttp_new(base);
        evhttp_set_default_content_type(http, "text/html; charset=utf-8");

        evhttp_set_timeout(http, 0);
        // 设置路由
        evhttp_set_cb(http, "/", ApiIndex, nullptr);
        evhttp_set_cb(http, "/api/health", ApiHealth, nullptr);
        evhttp_set_cb(http, "/api/controls", ApiAllMonitors, &scheduler);
        evhttp_set_cb(http, "/api/control", ApiMonitor, &scheduler);
        evhttp_set_cb(http, "/api/control/add", ApiAddMonitor, &scheduler);
        evhttp_set_cb(http, "/api/control/cancel", ApiRemoveMonitor, &scheduler);

        evhttp_bind_socket(http, "0.0.0.0",
            scheduler.GetConfig()->analyzer_port);
        event_base_dispatch(base);

        event_base_free(base);
        evhttp_free(http);
        event_config_free(evt_config);

        scheduler.SetState(false);

    }).detach();

}

void ApiIndex(evhttp_request* req, void* arg) {
   
    Json::Value result_urls;
    result_urls["/api"] = "this api version 1.0";
    result_urls["/api/health"] = "check health";
    result_urls["/api/controls"] = "get all control being analyzed";
    result_urls["/api/control"] = "get control being analyzed";
    result_urls["/api/control/add"] = "add control";
    result_urls["/api/control/cancel"] = "cancel control";
    result_urls["/api/largeModelCalcu"] = "largeModelCalcu";
    
    
    Json::Value result;
    result["urls"] = result_urls;

    evbuffer* buff = evbuffer_new();
    evbuffer_add_printf(buff, "%s", result.toStyledString().c_str());
    evhttp_send_reply(req, HTTP_OK, nullptr, buff);
    evbuffer_free(buff);

}
void ApiHealth(evhttp_request* req, void* arg) {
    int result_code = 0;
    std::string result_msg = "error";

    // 健康检测
    result_code = 1000;
    result_msg = "current service health";


    Json::Value result;
    result["msg"] = result_msg;
    result["code"] = result_code;

    evbuffer* buff = evbuffer_new();
    evbuffer_add_printf(buff, "%s", result.toStyledString().c_str());
    evhttp_send_reply(req, HTTP_OK, nullptr, buff);
    evbuffer_free(buff);

}
void ApiAllMonitors(evhttp_request* req, void* arg) {
    

    auto scheduler = static_cast<Scheduler *>(arg);
    char buf[RECV_BUF_MAX_SIZE];
    ParsePostRequest(req, buf);

    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    JSONCPP_STRING errs;

    int result_code = 0;
    std::string result_msg = "error";
    Json::Value result;

    if (reader->parse(buf, buf + std::strlen(buf), &root, &errs) && errs.empty()) {
        if (auto monitors = scheduler->GetAllMonitors(); !monitors.empty()) {
            Json::Value result_data_item;
            Json::Value result_data;
            int64_t curTimestamp = getCurTimestamp();
            int64_t startTimestamp = 0;
            for (auto monitor : monitors)
            {
                startTimestamp = monitor->startTimestamp;

                result_data_item["code"] = monitor->code.data();
                result_data_item["streamUrl"] = monitor->streamUrl.data();
                result_data_item["pushStream"] = monitor->pushStream;
                result_data_item["pushStreamUrl"] = monitor->pushStreamUrl.data();
                result_data_item["algorithmCode"] = monitor->algorithmCode.data();
                result_data_item["objectCode"] = monitor->objectCode.data();
                result_data_item["recognitionRegion"] = monitor->recognitionRegion.data();

                result_data_item["checkFps"] = monitor->checkFps;
                result_data_item["startTimestamp"] = startTimestamp;
                result_data_item["liveMilliseconds"] = curTimestamp - startTimestamp;


                result_data.append(result_data_item);
            }
            result["data"] = result_data;
            result_code = 1000;
            result_msg = "success";
        }
        else {
            result_msg = "the number of control exector is empty";
        }


    }
    else {
        result_msg = "invalid request parameter";
    }
    result["msg"] = result_msg;
    result["code"] = result_code;

    //LOGI("\n \t request:%s \n \t response:%s", root.toStyledString().data(), result.toStyledString().data());


    evbuffer* buff = evbuffer_new();
    evbuffer_add_printf(buff, "%s", result.toStyledString().c_str());
    evhttp_send_reply(req, HTTP_OK, nullptr, buff);
    evbuffer_free(buff);

}
void ApiMonitor(evhttp_request* req, void* arg) {

    auto scheduler = static_cast<Scheduler *>(arg);
    char buf[RECV_BUF_MAX_SIZE];
    ParsePostRequest(req, buf);

    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    JSONCPP_STRING errs;

    Json::Value result_monitor;
    int result_code = 0;
    std::string result_msg = "error";
    

    if (reader->parse(buf, buf + std::strlen(buf), &root, &errs) && errs.empty()) {

        Monitor* monitor = nullptr;
        if (root["code"].isString()) {
            std::string code = root["code"].asCString();
            monitor = scheduler->GetMonitorByCode(code);
        }

        if (monitor) {
            result_monitor["code"] = monitor->code;
            result_monitor["checkFps"] = monitor->checkFps;

            result_code = 1000;
            result_msg = "success";

        }
        else {
            result_msg = "the monitor does not exist";
        }
    }
    else {
        result_msg = "invalid request parameter";
    }

    Json::Value result;
    result["control"] = result_monitor;
    result["msg"] = result_msg;
    result["code"] = result_code;

    LOGI("\n \t request:%s \n \t response:%s", root.toStyledString().data(), result.toStyledString().data());


    evbuffer* buff = evbuffer_new();
    evbuffer_add_printf(buff, "%s", result.toStyledString().c_str());
    evhttp_send_reply(req, HTTP_OK, nullptr, buff);
    evbuffer_free(buff);

}
void ApiAddMonitor(evhttp_request* req, void* arg) {

    const auto scheduler = static_cast<Scheduler *>(arg);
    char buf[RECV_BUF_MAX_SIZE];
    ParsePostRequest(req, buf);

    const Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    JSONCPP_STRING errs;

    int result_code = 0;
    std::string result_msg;


    if (reader->parse(buf, buf + std::strlen(buf), &root, &errs) && errs.empty()) {
        const auto objects_str = root["objects"].asString();
        const auto objects_v = split(objects_str, ",");
        Monitor control{
            root["code"].asCString(),
        root["streamUrl"].asString(),
        root["pushStream"].asBool(),
        root["pushStreamUrl"].asString(),
        root["algorithmCode"].asString(),
        objects_str,
        objects_v,
        static_cast<int>(objects_v.size()),
        root["objectCode"].asString(),
        root["recognitionRegion"].asString(),
        };

        if (root["minInterval"].isString()) {
            control.minInterval = stoi(root["minInterval"].asString()) * 1000;//客户端传递过的该参数单位是秒
        }
        // TODO: put minimum interval of Alarm into json to make it configurable
        //强制设置报警时间最大不能超过3分钟=180000毫秒
        if (control.minInterval > 180000) {
            control.minInterval = 180000;
        }


        if (control.validateAdd(result_msg)) {
            scheduler->MonitorAdd(&control, result_code, result_msg);
        }
    }
    else {
        result_msg = "invalid request parameter";
    }

    Json::Value result;
    result["msg"] = result_msg;
    result["code"] = result_code;

    LOGI("\n \t request:%s \n \t response:%s", root.toStyledString().data(), result.toStyledString().data());

    struct evbuffer* buff = evbuffer_new();
    evbuffer_add_printf(buff, "%s", result.toStyledString().c_str());
    evhttp_send_reply(req, HTTP_OK, nullptr, buff);
    evbuffer_free(buff);


}

void ApiRemoveMonitor(evhttp_request* req, void* arg) {
    auto scheduler = static_cast<Scheduler *>(arg);
    char buf[RECV_BUF_MAX_SIZE];
    ParsePostRequest(req, buf);

    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());

    Json::Value root;
    JSONCPP_STRING errs;

    int result_code = 0;
    std::string result_msg = "error";

    if (reader->parse(buf, buf + std::strlen(buf), &root, &errs) && errs.empty()) {

        Monitor control;

        if (root["code"].isString()) {
            control.code = root["code"].asCString();
        }
        if (control.validateCancel(result_msg)) {
            scheduler->MonitorRemove(&control, result_code, result_msg);
        }

    }
    else {
        result_msg = "invalid request parameter";
    }

    Json::Value result;
    result["msg"] = result_msg;
    result["code"] = result_code;

    LOGI("\n \t request:%s \n \t response:%s", root.toStyledString().data(), result.toStyledString().data());

    struct evbuffer* buff = evbuffer_new();
    evbuffer_add_printf(buff, "%s", result.toStyledString().c_str());
    evhttp_send_reply(req, HTTP_OK, nullptr, buff);
    evbuffer_free(buff);


}
void ParseGetRequest(struct evhttp_request* req, struct evkeyvalq* params) {
    if (req == nullptr) {
        return;
    }
    const char* url = evhttp_request_get_uri(req);
    if (url == nullptr) {
        return;
    }
    struct evhttp_uri* decoded = evhttp_uri_parse(url);
    if (!decoded) {
        return;
    }
    const char* path = evhttp_uri_get_path(decoded);
    if (path == nullptr) {
        path = "/";
    }
    char* query = (char*)evhttp_uri_get_query(decoded);
    if (query == nullptr) {
        return;
    }
    evhttp_parse_query_str(query, params);
}
void ParsePostRequest(struct evhttp_request* req, char* buf) {
    size_t post_size = 0;

    post_size = evbuffer_get_length(req->input_buffer);
    if (post_size <= 0) {
        //        printf("====line:%d,post msg is empty!\n",__LINE__);
        return;

    }
    else {
        size_t copy_len = post_size > RECV_BUF_MAX_SIZE ? RECV_BUF_MAX_SIZE : post_size;
        //        printf("====line:%d,post len:%d, copy_len:%d\n",__LINE__,post_size,copy_len);
        memcpy(buf, evbuffer_pullup(req->input_buffer, -1), copy_len);
        buf[post_size] = '\0';
        //        printf("====line:%d,post msg:%s\n",__LINE__,buf);
    }

}