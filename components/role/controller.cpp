#include <string.h>
#include "esp_err.h"
#include "cJSON.h"
#include "logger.hpp"
#include "device.hpp"
#include "database.hpp"
#include "role.hpp"

namespace role
{
    Role::Role()
    {
        this->Name = NULL;
        this->Devices = (const char **)calloc(1, sizeof(char *));
        this->Emoji = NULL;
        this->Creator = NULL;
        this->CreatedAt = 0;
    }

    Role::Role(const char *name, const char **devices, const char *emoji,
               const char *creator, time_t createdAt)
    {
        this->Name = strdup(name);

        int size = 0;
        while (devices[size] != NULL)
            size++;
        this->Devices = (const char **)malloc((size + 1) * sizeof(char *));
        for (int i = 0; i < size; i++)
            this->Devices[i] = strdup(devices[i]);
        this->Devices[size] = NULL;

        this->Emoji = strdup(emoji);
        this->Creator = strdup(creator);
        this->CreatedAt = createdAt;
    }

    Role::Role(cJSON *src)
    {
        this->Name = strdup(cJSON_GetObjectItem(src, "name")->valuestring);

        cJSON *devices = cJSON_GetObjectItem(src, "devices");
        int size = cJSON_GetArraySize(devices);
        this->Devices = (const char **)malloc((size + 1) * sizeof(char *));
        for (int i = 0; i < size; i++)
            this->Devices[i] = strdup(cJSON_GetArrayItem(devices, i)->valuestring);
        this->Devices[size] = NULL;

        this->Emoji = strdup(cJSON_GetObjectItem(src, "emoji")->valuestring);
        this->Creator = strdup(cJSON_GetObjectItem(src, "creator")->valuestring);
        this->CreatedAt = cJSON_GetObjectItem(src, "created_at")->valueint;
    }

    Role::~Role()
    {
        free((void *)this->Name);

        for (int i = 0; this->Devices[i] != NULL; i++)
            free((void *)this->Devices[i]);
        free((void *)this->Devices);

        free((void *)this->Emoji);
        free((void *)this->Creator);
    }

    Role &Role::operator=(const Role &other)
    {
        if (this == &other)
            return *this;

        free((void *)this->Name);

        for (int i = 0; this->Devices[i] != NULL; i++)
            free((void *)this->Devices[i]);
        free((void *)this->Devices);

        free((void *)this->Emoji);
        free((void *)this->Creator);

        this->Name = strdup(other.Name);

        int size = 0;
        while (other.Devices[size] != NULL)
            size++;
        this->Devices = (const char **)malloc((size + 1) * sizeof(char *));
        for (int i = 0; i < size; i++)
            this->Devices[i] = strdup(other.Devices[i]);
        this->Devices[size] = NULL;

        this->Emoji = strdup(other.Emoji);
        this->Creator = strdup(other.Creator);
        this->CreatedAt = other.CreatedAt;

        return *this;
    }

    cJSON *Role::JSON()
    {
        cJSON *root = cJSON_CreateObject();

        cJSON_AddStringToObject(root, "name", this->Name);

        int size = 0;
        while (this->Devices[size] != NULL)
            size++;
        cJSON *devices = cJSON_AddArrayToObject(root, "devices");
        for (int i = 0; i < size; i++)
            cJSON_AddItemToArray(devices, cJSON_CreateString(this->Devices[i]));

        cJSON_AddStringToObject(root, "emoji", this->Emoji);
        cJSON_AddStringToObject(root, "creator", this->Creator);
        cJSON_AddNumberToObject(root, "created_at", this->CreatedAt);

        return root;
    }

    bool Role::Equals(const char *name) const
    {
        return !strcmp(this->Name, name);
    }

    bool Role::Equals(Role *other) const
    {
        return this->Equals(other->Name);
    }

    bool Role::Equals(const Role *other) const
    {
        return this->Equals(other->Name);
    }

    Controller *Controller::New(logger::Logger *logger, database::Database *database)
    {
        if (Instance != NULL)
            return Instance;

        Instance = new Controller();

        // Inject dependencies
        Instance->logger = logger;
        Instance->db = database->Open(DB_NAMESPACE);

        // Create or reset default system roles
        Instance->Set((Role *)(&System::Admin));
        Instance->Set((Role *)(&System::Guest));

        return Instance;
    }

    uint32_t Controller::Count()
    {
        uint32_t count;

        ESP_ERROR_CHECK(this->db->Count(&count));

        return count;
    }

    Role *Controller::Get(const char *name)
    {
        Role *role = NULL;

        cJSON *roleJSON = NULL;
        ESP_ERROR_CHECK(this->db->Get(name, &roleJSON));

        if (roleJSON != NULL)
            role = new Role(roleJSON);

        cJSON_Delete(roleJSON);

        return role;
    }

    Role *Controller::List(uint32_t *size)
    {
        uint32_t count = this->Count();
        *size = count;
        if (count < 1)
            return NULL;

        Role *list = new Role[count];
        Role *aux = list;

        ESP_ERROR_CHECK(this->db->Find(
            [](const char *key, void *context) -> bool
            {
                Role *list = *(Role **)context;

                cJSON *roleJSON = NULL;
                ESP_ERROR_CHECK(Instance->db->Get(key, &roleJSON));

                *list = Role(roleJSON);
                (*(Role **)context)++;

                cJSON_Delete(roleJSON);
                return false;
            },
            &aux));

        return list;
    }

    void Controller::Set(Role *role)
    {
        cJSON *roleJSON = role->JSON();
        ESP_ERROR_CHECK(this->db->Set(role->Name, roleJSON));
        cJSON_Delete(roleJSON);
    }

    void Controller::Delete(const char *name)
    {
        ESP_ERROR_CHECK(this->db->Delete(name));
    }

    void Controller::RemoveDeviceFromAllRoles(const char *device)
    {
        // Get all roles, notice that updating an NVS entry while iterating is not possible
        uint32_t size;
        Role *roles = this->List(&size);

        // Remove device from role if included
        for (int i = 0; i < size; i++)
        {
            if (this->Includes(&roles[i], device))
            {
                int devicesSize = 0;
                while (roles[i].Devices[devicesSize] != NULL)
                    devicesSize++;

                int newDevicesSize = devicesSize - 1;

                const char **devices = (const char **)malloc((newDevicesSize + 1) * sizeof(char *));

                devicesSize = 0;
                newDevicesSize = 0;
                while (roles[i].Devices[devicesSize] != NULL)
                {
                    if (strcmp(roles[i].Devices[devicesSize], device))
                    {
                        devices[newDevicesSize] = strdup(roles[i].Devices[devicesSize]);
                        newDevicesSize++;
                    }
                    devicesSize++;
                }
                devices[newDevicesSize] = NULL;

                for (int j = 0; roles[i].Devices[j] != NULL; j++)
                    free((void *)roles[i].Devices[j]);
                free((void *)roles[i].Devices);

                // Passing freeing ownership
                roles[i].Devices = devices;

                this->Set(&roles[i]);
            }
        }

        delete[] roles;
    }

    void Controller::Drop()
    {
        ESP_ERROR_CHECK(this->db->Drop());
    }

    bool Controller::Includes(const char *role, const char *device)
    {
        Role *roleAux = this->Get(role);
        bool ret = this->Includes(roleAux, device);
        delete roleAux;

        return ret;
    }

    bool Controller::Includes(Role *role, const char *device)
    {
        for (int i = 0; role->Devices[i] != NULL; i++)
            if (!strcmp(role->Devices[i], device))
                return true;

        return false;
    }

    bool Controller::Includes(const char *role, device::Device *device)
    {
        return this->Includes(role, device->Name);
    }

    bool Controller::Includes(Role *role, device::Device *device)
    {
        return this->Includes(role, device->Name);
    }
}