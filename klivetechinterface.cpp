#include "klivetechinterface.h"
#include <string>
#include <sstream>
#include <regex>
#include <cstdlib> // for atoi
#include <cstring> // for strstr, strchr, strncpy
#include "taskwrapper.h"

using namespace std;
void KliveTech::CreateKliveTechGadget(const char *name)
{
    Serial.begin(115200);
    if (!SerialBT.begin(name))
    {
        Serial.println("An error occurred initialising Bluetooth.");
        pinMode(2, OUTPUT);
        while(true){
            delay(500);
            digitalWrite(2, HIGH);
            delay(500);
            digitalWrite(2, LOW);
        }
    }
    else
    {
        pinMode(2, OUTPUT);
        digitalWrite(2, HIGH);
        Serial.println("Bluetooth Set Up");
    }
    xTaskCreate(
        [](void *param)
        {
            static_cast<KliveTech *>(param)->CallLoop();
        },                      /* Task function. */
        "KliveTech Call Loop", /* name of task. */
        1024 * 2,                  /* Stack size of task */
        NULL,                  /* parameter of the task */
        2,                     /* priority of the task */
        NULL                   /* Task handle to keep track of created task */
    );
    Serial.println("KliveTech Gadget Created");
}
// FOR SOME REASON, THE NORMAL const char* == const char* DOESN'T WORK??? SO I HAVE TO MAKE THIS?? Whatever I don't even care enough
static bool stringCompare(const char *one, const char *two)
{
    return strcmp(one, two) == 0;
}

void KliveTech::CallLoop()
{
    // Indefinitely do this
    for (;;)
    {
        auto comm = ReadCommand();
        Serial.println("comm received: ");
        OmnipotentResponse resp = DeserializeResponse(comm);
        JsonDocument docResp;
        JsonObject finalResp = docResp.to<JsonObject>();
        Serial.print("ID: ");
        Serial.println(resp.ID);
        Serial.print("Data: ");
        Serial.println(resp.data);
        Serial.print("Operation: ");
        Serial.println(resp.operation);
        Serial.print("Response Expected: ");
        Serial.println(resp.ResponseExpected);
        if (resp.operation == GetActions)
        {
            Serial.println("Get Actions Received");

            JsonArray actions = finalResp.createNestedArray("Actions");
            for (size_t i = 0; i < possibleActions.size(); i++)
            {
                JsonObject action = actions.createNestedObject();
                action["Name"] = possibleActions[i].name;
                action["ParamDescription"] = possibleActions[i].paramDescription;
                action["Type"] = possibleActions[i].type;
            }
            auto formedResp = FormulateResponse(resp.ID, finalResp, "200", "false");
            SendData(formedResp);
            return;
        }
        if (resp.operation == ExecuteAction)
        {
            Serial.println("Execute Action Received");
            JsonDocument messageDoc;
            deserializeJson(messageDoc, std::string(resp.data));
            const char *actionName = messageDoc["ActionName"].as<const char *>();
            // find the action
            Actions *action = nullptr;
            for (size_t i = 0; i < possibleActions.size(); i++)
            {
                if (stringCompare(possibleActions[i].name, actionName))
                {
                    action = &possibleActions[i];
                    break;
                }
            }
            // Check action type and execute corresponding function
            if (action->type == ActionParameterType::Integer)
            {
                auto param = messageDoc["Param"].as<int>();
                auto *taskData = new std::pair<std::function<void(int)>, int>(action->intFunction, param);
                xTaskCreate(TaskWrapper::IntTask, "KliveTech Action", 1024, taskData, 1, NULL);
                auto formedResp = FormulateResponse(resp.ID, finalResp, "200", "false");
                SendData(formedResp);
            }
            else if (action->type == ActionParameterType::String)
            {
                auto param = messageDoc["Param"].as<const char *>();
                auto *taskData = new std::pair<std::function<void(const char *)>, const char *>(action->StringFunction, param);
                xTaskCreate(TaskWrapper::StringTask, "KliveTech Action", 1024, taskData, 1, NULL);
                Serial.print("Executing ");
                Serial.println(actionName);
                auto formedResp = FormulateResponse(resp.ID, finalResp, "200", "false");
                SendData(formedResp);
            }
            else if (action->type == ActionParameterType::Bool)
            {
                auto param = messageDoc["Param"].as<bool>();
                Serial.print("Executing ");
                Serial.println(actionName);
                auto *taskData = new std::pair<std::function<void(bool)>, bool>(action->BoolFunction, param);
                xTaskCreate(TaskWrapper::BoolTask, "KliveTech Action", 1024, taskData, 1, NULL);
                auto formedResp = FormulateResponse(resp.ID, finalResp, "200", "false");
                SendData(formedResp);
            }
            else if (action->type == ActionParameterType::None)
            {
                Serial.print("Executing ");
                Serial.println(actionName);
                auto *taskData = new std::function<void()>(action->NoParamFunction);
                xTaskCreate(TaskWrapper::NoParamTask, "KliveTech Action", 1024, taskData, 1, NULL);
                auto formedResp = FormulateResponse(resp.ID, finalResp, "200", "false");
                SendData(formedResp);
            }
            else
            {
                auto formedResp = FormulateResponse(resp.ID, finalResp, "500", "false");
                SendData(formedResp);
            }
            return;
        }
        if (resp.operation == Ping)
        {
            auto formedResp = FormulateResponse(resp.ID, finalResp, "200", "false");
            SendData(formedResp);
            return;
        }
        Serial.println("No Command");
        if (resp.ResponseExpected == true)
        {
            auto formedResp = FormulateResponse(resp.ID, finalResp, "500", "false");
            SendData(formedResp);
        }
    }
}
OmnipotentResponse KliveTech::DeserializeResponse(const char *message)
{
    // Expected message: {ID{string} DATA{string} RESPEXPECT{true/false}}
    OmnipotentResponse resp;

    JsonDocument doc;
    deserializeJson(doc, std::string(message));
    resp.ID = doc["ID"].as<int>();
    resp.operation = static_cast<OperationNumber>(doc["OP"].as<int>());
    resp.data = doc["DATA"].as<const char *>();
    resp.ResponseExpected = doc["RESPEXPECT"].as<bool>();

    return resp;
}
bool KliveTech::hasEnding(std::string const &fullString, std::string const &ending)
{
    if (fullString.length() >= ending.length())
    {
        return (0 == fullString.compare(fullString.length() - ending.length(), ending.length(), ending));
    }
    else
    {
        return false;
    }
}
const char *KliveTech::FormulateResponse(int ID, JsonObject obj, const char *status, bool isResponseExpected)
{
    // Write arduino json code that serialises the parameters and returns the json as a string
    //  Expected message: {ID{string} DATA{string} RESPEXPECT{true/false}}
    StaticJsonDocument<2048> doc;
    doc["ID"] = ID;
    // add obj as nested data to DATA
    JsonObject data = doc.createNestedObject("DATA");
    for (JsonPair kv : obj)
    {
        data[kv.key()] = kv.value();
    }
    doc["STATUS"] = status;
    doc["RESPEXPECT"] = isResponseExpected;
    // Return doc as json string
    std::string response;
    serializeJson(doc, response);
    return response.c_str();
}

void KliveTech::SendData(const char *data)
{
    std::string str(data);
    std::stringstream ss;
    ss << startCommand << data << endCommand;
    std::string command = ss.str();
    for (size_t i = 0; i < command.size(); i++)
    {
        SerialBT.write(command[i]);
    }
}
const char *KliveTech::ReadCommand()
{
    std::string command = "";
    while (true)
    {
        auto read = SerialBT.read();
        if (read != -1)
        {
            auto b = char(read);
            command = command + b;
            if (command == startCommand)
            {
                command = "";
            }
            if (hasEnding(command, endCommand))
            {
                // command = command.replace(0, startCommand.size(), "", startCommand.size());
                command = command.replace(command.size() - endCommand.size(), command.size(), "", endCommand.size());
                break;
            }
        }
    }
    return command.c_str();
}
void KliveTech::CreateActionWithIntegerParam(const char *actname, std::function<void(int)> func, const char *paramDesc)
{
    Actions action;
    action.type = ActionParameterType::Integer;
    action.name = actname;
    action.intFunction = func;
    action.paramDescription = paramDesc;
    this->possibleActions.emplace_back(action);
}
void KliveTech::CreateActionWithStringParam(const char *actname, std::function<void(const char *)> func, const char *paramDesc)
{
    Actions action;
    action.type = ActionParameterType::String;
    action.name = actname;
    action.StringFunction = func;
    action.paramDescription = paramDesc;
    this->possibleActions.emplace_back(action);
}
void KliveTech::CreateActionWithBoolParam(const char *actname, std::function<void(bool)> func, const char *paramDesc)
{
    Actions action;
    action.type = ActionParameterType::Bool;
    action.name = actname;
    action.BoolFunction = func;
    action.paramDescription = paramDesc;
    this->possibleActions.emplace_back(action);
}

void KliveTech::CreateActionWithNoParam(const char *actname, std::function<void()> func)
{
    Actions action;
    action.type = ActionParameterType::None;
    action.name = actname;
    action.NoParamFunction = func;
    this->possibleActions.emplace_back(action);
}