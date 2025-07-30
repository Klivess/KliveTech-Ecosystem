#ifndef TASKWRAPPER_H
#define TASKWRAPPER_H
#include <string>
#include <vector>
#include "BluetoothSerial.h"
#include "ArduinoJson.h"


struct TaskWrapper
{
    static void IntTask(void *param)
    {
        auto *taskData = static_cast<std::pair<std::function<void(int)>, int> *>(param);
        taskData->first(taskData->second);
        delete taskData; // Clean up
        vTaskDelete(NULL);
    }

    static void StringTask(void *param)
    {
        auto *taskData = static_cast<std::pair<std::function<void(const char *)>, const char *> *>(param);
        taskData->first(taskData->second);
        delete taskData; // Clean up
        vTaskDelete(NULL);
    }

    static void BoolTask(void *param)
    {
        auto *taskData = static_cast<std::pair<std::function<void(bool)>, bool> *>(param);
        taskData->first(taskData->second);
        delete taskData; // Clean up
        vTaskDelete(NULL);
    }

    static void NoParamTask(void *param)
    {
        auto *taskData = static_cast<std::function<void()> *>(param);
        (*taskData)();
        delete taskData; // Clean up
        vTaskDelete(NULL);
    }
};
#endif