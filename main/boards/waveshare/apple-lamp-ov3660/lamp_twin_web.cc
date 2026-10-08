#include "sdkconfig.h"
#if CONFIG_APPLE_LAMP_TWIN_WEB
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <cstring>
#include <string>
#include "lamp_twin_page.h"
#include "lamp_twin_web.h"
#if CONFIG_APPLE_LAMP_OMNI_REALTIME
#include "lamp_omni_page.h"
#include "omni_realtime_protocol.h"
#endif
namespace {
httpd_handle_t server = nullptr;
LampHardwareExecutor* device = nullptr;
std::string secret;
std::string Header(httpd_req_t* req, const char* name) {
    const auto size = httpd_req_get_hdr_value_len(req, name);
    if (!size || size > 200)
        return "";
    std::string value(size + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, name, value.data(), value.size()) != ESP_OK)
        return "";
    value.resize(size);
    return value;
}
esp_err_t Json(httpd_req_t* req, const std::string& data) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, data.data(), data.size());
}
esp_err_t Page(httpd_req_t* req) {
    std::string page = kLampTwinPage;
    page.replace(page.find("__TOKEN__"), 9, secret);
#if CONFIG_APPLE_LAMP_OMNI_REALTIME
    page.insert(page.find("</main>"), kLampOmniPage);
#endif
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                       "frame-ancestors 'none'; connect-src 'self'");
    return httpd_resp_send(req, page.data(), page.size());
}
esp_err_t Api(httpd_req_t* req) {
    if (Header(req, "X-Lamp-Token") != secret) {
        httpd_resp_set_status(req, "403 Forbidden");
        return Json(
            req,
            "{\"ok\":false,\"error\":{\"message\":\"Open the device page to obtain a session\"}}");
    }
    const auto origin = Header(req, "Origin"), host = Header(req, "Host");
    if (!origin.empty() && origin != "http://" + host) {
        httpd_resp_set_status(req, "403 Forbidden");
        return Json(req, "{\"ok\":false,\"error\":{\"message\":\"Cross-origin control denied\"}}");
    }
    std::string path(req->uri);
    path = path.substr(0, path.find('?'));
    if (req->method == HTTP_GET) {
#if CONFIG_APPLE_LAMP_OMNI_REALTIME
        if (path == "/api/lamp/omni")
            return Json(req, OmniPanel());
#if CONFIG_APPLE_LAMP_EDGE_RUNTIME
        if (path == "/api/lamp/runtime")
            return Json(req, OmniRuntimeState());
#endif
#endif
        if (path == "/api/lamp/history") {
            char query[64]{}, value[24]{};
            uint64_t after = 0;
            if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                httpd_query_key_value(query, "after", value, sizeof(value)) == ESP_OK) {
                char* end = nullptr;
                after = strtoull(value, &end, 10);
                if (!end || *end || after > 9007199254740991ULL)
                    return Json(req, "{\"ok\":false}");
            }
            return Json(req, device->Diagnostics(after));
        }
        if (path == "/api/lamp/state")
            return Json(req, device->State());
        if (path == "/api/lamp/config")
            return Json(req, device->PanelConfig());
        const std::string prefix = "/api/lamp/tasks/";
        if (path.starts_with(prefix)) {
            auto id = path.substr(prefix.size());
            if (id.starts_with("hardware-"))
                id = id.substr(9);
            if (id.empty() || id.size() > 87)
                return Json(req, "{\"ok\":false}");
            return Json(req, device->Inspect(id));
        }
    } else if (req->method == HTTP_POST) {
        if (req->content_len <= 0 || req->content_len > 2048 ||
            !Header(req, "Content-Type").starts_with("application/json")) {
            httpd_resp_set_status(req, "400 Bad Request");
            return Json(req,
                        "{\"ok\":false,\"error\":{\"message\":\"Expected bounded JSON body\"}}");
        }
        std::string body(req->content_len, '\0');
        size_t received = 0;
        while (received < body.size()) {
            int n = httpd_req_recv(req, body.data() + received, body.size() - received);
            if (n <= 0)
                return ESP_FAIL;
            received += n;
        }
        if (path == "/api/lamp/calibration/preview")
            return Json(req, device->PanelConfig(body));
        if (path == "/api/lamp/command")
            return Json(req, device->PanelSubmit(body));
#if CONFIG_APPLE_LAMP_OMNI_REALTIME
        if (path == "/api/lamp/omni")
            return Json(req, OmniPanel(body));
#endif
    }
    httpd_resp_set_status(req, "404 Not Found");
    return Json(req, "{\"ok\":false,\"error\":{\"message\":\"Unknown API\"}}");
}
}  // namespace
void StartLampTwinWeb(LampHardwareExecutor& executor) {
    if (server)
        return;
    char token[33];
    for (unsigned i = 0; i < 4; i++)
        snprintf(token + i * 8, 9, "%08lx", static_cast<unsigned long>(esp_random()));
    secret = token;
    device = &executor;
#if CONFIG_APPLE_LAMP_OMNI_REALTIME && !CONFIG_APPLE_LAMP_EDGE_RUNTIME
    OmniSetStateReader([&executor] { return executor.State(); });
#endif
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.ctrl_port = 32769;
    config.stack_size = 12288;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;
    if (httpd_start(&server, &config) != ESP_OK) {
        server = nullptr;
        ESP_LOGE("LampTwin", "HTTP start failed; UART execution unaffected");
        return;
    }
    httpd_uri_t root{};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = Page;
    httpd_uri_t get{};
    get.uri = "/api/lamp/*";
    get.method = HTTP_GET;
    get.handler = Api;
    httpd_uri_t post = get;
    post.method = HTTP_POST;
    if (httpd_register_uri_handler(server, &root) != ESP_OK ||
        httpd_register_uri_handler(server, &get) != ESP_OK ||
        httpd_register_uri_handler(server, &post) != ESP_OK) {
        httpd_stop(server);
        server = nullptr;
        ESP_LOGE("LampTwin", "HTTP route registration failed");
        return;
    }
    esp_netif_ip_info_t ip{};
    auto* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK)
        ESP_LOGI("LampTwin", "Test panel: http://" IPSTR ":8080/ ; no startup motion",
                 IP2STR(&ip.ip));
}
#endif
