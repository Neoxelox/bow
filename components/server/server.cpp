#include <errno.h>
#include <string.h>
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "esp_http_server.h"
#include "http_parser.h"
#include "cJSON.h"
#include "esp_vfs_fat.h"
#include "logger.hpp"
#include "database.hpp"
#include "provisioner.hpp"
#include "chron.hpp"
#include "device.hpp"
#include "user.hpp"
#include "server.hpp"

namespace server
{
    Server *Server::New(logger::Logger *logger, database::Database *database, provisioner::Provisioner *provisioner,
                        chron::Controller *chron, device::Transmitter *transmitter, device::Receiver *receiver,
                        user::Controller *user)
    {
        if (Instance != NULL)
            return Instance;

        Instance = new Server();

        // Inject dependencies
        Instance->logger = logger;
        Instance->database = database;
        Instance->provisioner = provisioner;
        Instance->chron = chron;
        Instance->transmitter = transmitter;
        Instance->receiver = receiver;
        Instance->user = user;

        Instance->espServer = NULL;

        // Register Wi-Fi station/softAP and LwIP event callbacks
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, Instance->staFunc, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, WIFI_EVENT_AP_START, Instance->apFunc, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, WIFI_EVENT_AP_STOP, Instance->apFunc, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, Instance->ipFunc, NULL, NULL));

        // Mount FAT filesystem from static partition
        esp_vfs_fat_mount_config_t cfg = {
            .format_if_mount_failed = false,
            .max_files = MAX_CLIENTS,
            .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        };
        ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl(ROOT, PARTITION, &cfg, &Instance->fsHandle));

        // Increase HTTPD log level, is too verbose
        esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
        esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
        esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

        return Instance;
    }

    void Server::start()
    {
        if (this->espServer != NULL)
            return;

        // Configure HTTP server
        httpd_config_t cfg = {
            .task_priority = 9,
            .stack_size = 4 * 1024,
            .core_id = tskNO_AFFINITY,
            .server_port = PORT,
            .ctrl_port = 32768,
            .max_open_sockets = MAX_CLIENTS,
            .max_uri_handlers = 5,
            .max_resp_headers = 10,
            .backlog_conn = 5,
            .lru_purge_enable = true,
            .recv_wait_timeout = 5,
            .send_wait_timeout = 5,
            .uri_match_fn = httpd_uri_match_wildcard,
        };

        // Start HTTP server
        ESP_ERROR_CHECK(httpd_start(&this->espServer, &cfg));

        // Register error handlers, note that there is no way to register all at the same time
        ESP_ERROR_CHECK(httpd_register_err_handler(this->espServer, HTTPD_400_BAD_REQUEST, this->errorHandler));
        ESP_ERROR_CHECK(httpd_register_err_handler(this->espServer, HTTPD_401_UNAUTHORIZED, this->errorHandler));
        ESP_ERROR_CHECK(httpd_register_err_handler(this->espServer, HTTPD_403_FORBIDDEN, this->errorHandler));
        ESP_ERROR_CHECK(httpd_register_err_handler(this->espServer, HTTPD_404_NOT_FOUND, this->errorHandler));
        ESP_ERROR_CHECK(httpd_register_err_handler(this->espServer, HTTPD_405_METHOD_NOT_ALLOWED, this->errorHandler));
        ESP_ERROR_CHECK(httpd_register_err_handler(this->espServer, HTTPD_408_REQ_TIMEOUT, this->errorHandler));
        ESP_ERROR_CHECK(httpd_register_err_handler(this->espServer, HTTPD_411_LENGTH_REQUIRED, this->errorHandler));
        ESP_ERROR_CHECK(httpd_register_err_handler(this->espServer, HTTPD_414_URI_TOO_LONG, this->errorHandler));
        ESP_ERROR_CHECK(httpd_register_err_handler(this->espServer, HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE, this->errorHandler));
        ESP_ERROR_CHECK(httpd_register_err_handler(this->espServer, HTTPD_500_INTERNAL_SERVER_ERROR, this->errorHandler));
        ESP_ERROR_CHECK(httpd_register_err_handler(this->espServer, HTTPD_501_METHOD_NOT_IMPLEMENTED, this->errorHandler));
        ESP_ERROR_CHECK(httpd_register_err_handler(this->espServer, HTTPD_505_VERSION_NOT_SUPPORTED, this->errorHandler));

        // TODO: logger middleware

        // Register api handlers
        ESP_ERROR_CHECK(httpd_register_uri_handler(this->espServer, &this->apiPostRegisterURIHandler));
        ESP_ERROR_CHECK(httpd_register_uri_handler(this->espServer, &this->apiPostLoginURIHandler));
        ESP_ERROR_CHECK(httpd_register_uri_handler(this->espServer, &this->apiGetUsersURIHandler));

        // Register frontend handler, note that it has to be the last one to catch all other URLs
        ESP_ERROR_CHECK(httpd_register_uri_handler(this->espServer, &this->frontURIHandler));

        this->logger->Debug(TAG, "Started HTTP server on port :%d", cfg.server_port);
    }

    void Server::stop()
    {
        if (this->espServer == NULL)
            return;

        // Stop HTTP server
        ESP_ERROR_CHECK(httpd_stop(this->espServer));

        this->espServer = NULL;

        this->logger->Debug(TAG, "Stopped HTTP server");
    }

    esp_err_t Server::sendFile(httpd_req_t *request, const char *path, const char *status)
    {
        esp_err_t err;

        // Set appropiate content type, encoding and status
        err = httpd_resp_set_hdr(request, Headers::ContentEncoding, ContentEncodings::GZIP);
        if (err != ESP_OK)
            return err;

        err = httpd_resp_set_status(request, status);
        if (err != ESP_OK)
            return err;

        err = httpd_resp_set_type(request, ContentTypes::TextHTML);
        if (err != ESP_OK)
            return err;

        // Open file
        FILE *file = fopen(path, "r");
        if (file == NULL)
        {
            return ESP_FAIL;
        }

        // Read and send file in chunks
        char chunk[1024];
        size_t size;
        do
        {
            size = fread(chunk, 1, sizeof(chunk), file);
            if (httpd_resp_send_chunk(request, chunk, size) != ESP_OK)
            {
                // Close file
                fclose(file);

                // On large responses client may reset the connection suddenly, ignore it and do not panic
                if (errno == ECONNRESET || errno == ENOTCONN)
                    return ESP_OK;

                // Abort sending file
                err = httpd_resp_send_chunk(request, NULL, 0);
                if (err != ESP_OK)
                    return err;

                return ESP_FAIL;
            }
        } while (size != 0);

        // Close file
        fclose(file);

        // Close connection for large responses to free the underlying socket instantly
        err = httpd_resp_set_hdr(request, Headers::Connection, "close");
        if (err != ESP_OK)
            return err;

        // Respond with an empty chunk to signal HTTP response completion
        err = httpd_resp_send_chunk(request, NULL, 0);
        if (err != ESP_OK)
            return err;

        return ESP_OK;
    }

    esp_err_t Server::sendJSON(httpd_req_t *request, cJSON *json, const char *status)
    {
        esp_err_t err;

        // Set appropiate content type and status
        err = httpd_resp_set_status(request, status);
        if (err != ESP_OK)
            return err;

        err = httpd_resp_set_type(request, ContentTypes::ApplicationJSON);
        if (err != ESP_OK)
            return err;

        // Send all body at once
        const char *body = cJSON_PrintUnformatted(json);
        err = httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
        free((void *)body);
        if (err != ESP_OK)
            return err;

        return ESP_OK;
    }

    esp_err_t Server::sendError(httpd_req_t *request, Error error, const char *message)
    {
        esp_err_t err;

        cJSON *root = cJSON_CreateObject();

        cJSON_AddStringToObject(root, "code", error.Code);
        if (message != NULL)
            cJSON_AddStringToObject(root, "message", message);

        err = Instance->sendJSON(request, root, error.Status);
        cJSON_Delete(root);
        if (err != ESP_OK)
            return err;

        return ESP_OK;
    }

    esp_err_t Server::recvJSON(httpd_req_t *request, cJSON **json)
    {
        esp_err_t err;

        char body[MAX_REQUEST_CONTENT_SIZE + 1];

        // Check if request content fits in body buffer
        if (request->content_len > MAX_REQUEST_CONTENT_SIZE)
            return ESP_ERR_NO_MEM;

        // Trying to receive with no content generates an error
        if (request->content_len < 2)
        {
            *json = cJSON_Parse("{}");
            return ESP_OK;
        }

        err = httpd_req_recv(request, body, request->content_len);
        if (err <= 0)
        {
            // On large responses client may reset the connection suddenly, ignore it and do not panic
            if (err == HTTPD_SOCK_ERR_TIMEOUT)
                return ESP_OK;

            return ESP_FAIL;
        }

        // Ensure NULL-terminated string
        body[request->content_len] = '\0';

        // Parse request JSON content
        *json = cJSON_Parse(body);

        return ESP_OK;
    }

    user::User *Server::checkToken(httpd_req_t *request)
    {
        user::User *user = NULL;

        char header[MAX_REQUEST_HEADER_SIZE + 1];

        uint32_t size = httpd_req_get_hdr_value_len(request, Headers::Authorization);

        // Check if request headers fits in header buffer
        if (size > MAX_REQUEST_HEADER_SIZE)
            return NULL;

        // Authorization header should be present and at least user::TOKEN_SIZE
        if (size < user::TOKEN_SIZE)
            return NULL;

        // Read Authorization header
        ESP_ERROR_CHECK(httpd_req_get_hdr_value_str(request, Headers::Authorization, header, size + 1));

        // Search for name:token delimiter
        char *del = strchr(header, ':');

        // Replace delimiter with NULL to split the header
        *del = '\0';

        char *token = del + 1;
        char *name = header;

        // Get user
        user = Instance->user->Get(name);
        if (user == NULL)
            return NULL;

        // Check if tokens match
        if (strcmp(token, user->Token))
        {
            delete user;
            return NULL;
        }

        return user;
    }

    void Server::apFunc(void *args, esp_event_base_t base, int32_t id, void *data)
    {
        // Start HTTP server when Wi-Fi softAP has started
        if (id == WIFI_EVENT_AP_START)
            Instance->start();

        // Stop HTTP server when Wi-Fi softAP has stopped because underlying sockets are freed
        else // WIFI_EVENT_AP_STOP
            Instance->stop();
    }

    void Server::staFunc(void *args, esp_event_base_t base, int32_t id, void *data)
    {
        // Stop HTTP server when Wi-Fi station is disconnected because underlying sockets are freed
        // WIFI_EVENT_STA_DISCONNECTED
        Instance->stop();
    }

    void Server::ipFunc(void *args, esp_event_base_t base, int32_t id, void *data)
    {
        // Start HTTP server when IP is got
        // IP_EVENT_STA_GOT_IP
        Instance->start();
    }

    esp_err_t Server::errorHandler(httpd_req_t *request, httpd_err_code_t error)
    {
        const char *status;
        cJSON *root = cJSON_CreateObject();

        switch (error)
        {
        case HTTPD_400_BAD_REQUEST:
            status = Errors::InvalidRequest.Status;
            cJSON_AddStringToObject(root, "code", Errors::InvalidRequest.Code);
            break;

        case HTTPD_404_NOT_FOUND:
            status = Errors::NotFound.Status;
            cJSON_AddStringToObject(root, "code", Errors::NotFound.Code);
            break;

        default:
            status = Errors::ServerGeneric.Status;
            cJSON_AddStringToObject(root, "code", Errors::ServerGeneric.Code);
            break;
        }

        const char *body = cJSON_PrintUnformatted(root);

        ESP_ERROR_CHECK(httpd_resp_set_status(request, status));
        ESP_ERROR_CHECK(httpd_resp_set_type(request, ContentTypes::ApplicationJSON));

        // Framework-necessary code
#ifdef CONFIG_HTTPD_ERR_RESP_NO_DELAY
        int fd = httpd_req_to_sockfd(request);
        int nodelay = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0)
        {
            Instance->logger->Warn(TAG, "error calling setsockopt: %d", errno);
            nodelay = 0;
        }
#endif
        // ------------------------

        ESP_ERROR_CHECK(httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN));

        free((void *)body);
        cJSON_Delete(root);

        // Framework-necessary code
#ifdef CONFIG_HTTPD_ERR_RESP_NO_DELAY
        if (nodelay == 1)
        {
            nodelay = 0;
            if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0)
            {
                Instance->logger->Error(TAG, "error calling setsockopt: %d", errno);
                return ESP_ERR_INVALID_STATE;
            }
        }
#endif
        // ------------------------

        return ESP_FAIL;
    }

    esp_err_t Server::frontHandler(httpd_req_t *request)
    {
        // On home path serve the Gzipped frontend from FATFS static partition
        if (!strcmp(request->uri, "/") || !strcmp(request->uri, ""))
        {
            // Set Cache Control header so clients load the frontend faster
            ESP_ERROR_CHECK(httpd_resp_set_hdr(request, Headers::CacheControl, "max-age=604800, stale-while-revalidate=86400, stale-if-error=86400"));
            ESP_ERROR_CHECK(Instance->sendFile(request, FRONTEND, Statuses::_200));

            return ESP_OK;
        }

        char *location = NULL;
        provisioner::Credentials *creds = Instance->provisioner->GetCreds();

        // If there are no stored Wi-Fi credentials redirect to onboarding page /#/onboarding
        if (creds == NULL)
            ESP_ERROR_CHECK(httpd_resp_set_hdr(request, Headers::Location, "/#/onboarding"));
        // Otherwise redirect to the frontend hash-based navigation: /#/:URI
        else
        {
            location = (char *)malloc(strlen(request->uri) + 2 + 1);
            strcpy(location, "/#");
            ESP_ERROR_CHECK(httpd_resp_set_hdr(request, Headers::Location, strcat(location, request->uri)));
        }

        delete creds;

        // If provisioner mode is AP use temporary redirects with content instead
        if (Instance->provisioner->GetMode() == WIFI_MODE_AP)
        {
            ESP_ERROR_CHECK(httpd_resp_set_status(request, Statuses::_302));
            // iOS requires content in the response to detect a captive portal
            ESP_ERROR_CHECK(httpd_resp_set_type(request, ContentTypes::TextHTML));
            ESP_ERROR_CHECK(httpd_resp_send(request, "<h1>Redirecting to the onboarding...</h1>", HTTPD_RESP_USE_STRLEN));
        }
        else
        {
            ESP_ERROR_CHECK(httpd_resp_set_status(request, Statuses::_301));
            ESP_ERROR_CHECK(httpd_resp_send(request, NULL, 0));
        }

        free((void *)location);

        return ESP_OK;
    }

    esp_err_t Server::apiPostRegisterHandler(httpd_req_t *request)
    {
        // Bind request JSON
        cJSON *reqJSON = NULL;
        ESP_ERROR_CHECK(Instance->recvJSON(request, &reqJSON));

        // Check if a user with the same name already exists
        user::User *exUser = Instance->user->Get(cJSON_GetObjectItem(reqJSON, "name")->valuestring);
        if (exUser != NULL)
        {
            cJSON_Delete(reqJSON);
            delete exUser;
            ESP_ERROR_CHECK(Instance->sendError(request, Errors::InvalidRequest, "User already exists"));
            return ESP_FAIL;
        }

        // Generate new authentication token
        char token[user::TOKEN_SIZE + 1];
        Instance->user->GenerateToken(token);

        // Create the new user
        user::User newUser(
            cJSON_GetObjectItem(reqJSON, "name")->valuestring,
            cJSON_GetObjectItem(reqJSON, "password")->valuestring,
            token,
            Instance->user->Count() > 0 ? "MEMBER" : "ADMIN", // TODO: Get default system roles from role component
            cJSON_GetObjectItem(reqJSON, "emoji")->valuestring,
            Instance->chron->Now());

        cJSON_Delete(reqJSON);

        Instance->user->Set(&newUser);

        // Send response JSON
        cJSON *resJSON = newUser.JSON();
        cJSON_DeleteItemFromObject(resJSON, "password");
        ESP_ERROR_CHECK(Instance->sendJSON(request, resJSON, Statuses::_200));
        cJSON_Delete(resJSON);

        return ESP_OK;
    }

    esp_err_t Server::apiPostLoginHandler(httpd_req_t *request)
    {
        // Bind request JSON
        cJSON *reqJSON = NULL;
        ESP_ERROR_CHECK(Instance->recvJSON(request, &reqJSON));

        // Get user
        user::User *user = Instance->user->Get(cJSON_GetObjectItem(reqJSON, "name")->valuestring);
        if (user == NULL)
        {
            cJSON_Delete(reqJSON);
            ESP_ERROR_CHECK(Instance->sendError(request, Errors::InvalidRequest, "User doesn't exist"));
            return ESP_FAIL;
        }

        // Check if passwords match
        if (strcmp(cJSON_GetObjectItem(reqJSON, "password")->valuestring, user->Password))
        {
            cJSON_Delete(reqJSON);
            delete user;
            ESP_ERROR_CHECK(Instance->sendError(request, Errors::InvalidRequest, "Passwords don't match"));
            return ESP_FAIL;
        }

        cJSON_Delete(reqJSON);

        // Generate new authentication token
        char token[user::TOKEN_SIZE + 1];
        Instance->user->GenerateToken(token);
        free((void *)user->Token);
        user->Token = strdup(token);

        // Save user
        Instance->user->Set(user);

        // Send response JSON
        cJSON *resJSON = user->JSON();
        delete user;
        cJSON_DeleteItemFromObject(resJSON, "password");
        ESP_ERROR_CHECK(Instance->sendJSON(request, resJSON, Statuses::_200));
        cJSON_Delete(resJSON);

        return ESP_OK;
    }

    esp_err_t Server::apiGetUsersHandler(httpd_req_t *request)
    {
        // Authenticate request user
        user::User *reqUser = Instance->checkToken(request);
        if (reqUser == NULL)
        {
            ESP_ERROR_CHECK(Instance->sendError(request, Errors::Unauthorized, NULL));
            return ESP_FAIL;
        }

        // Send response JSON
        cJSON *resJSON = reqUser->JSON();
        delete reqUser;
        cJSON_DeleteItemFromObject(resJSON, "password");
        ESP_ERROR_CHECK(Instance->sendJSON(request, resJSON, Statuses::_200));
        cJSON_Delete(resJSON);

        return ESP_OK;
    }
}