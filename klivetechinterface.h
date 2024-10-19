#ifndef KliveTechInterface_H
#define KliveTechInterface_H
#include <string>
#include <vector>
#include "BluetoothSerial.h"
#include "ArduinoJson.h"
using namespace std;

enum ActionParameterType
{
    Integer,
    String,
    Bool
};

enum OperationNumber{
    ExecuteAction,
    GetActions
};

struct Actions
{
    ActionParameterType type;
    const char *name;
    const char *paramDescription;
    std::function<void(int)> intFunction;
    std::function<void(const char *)> StringFunction;
    std::function<void(bool)> BoolFunction;
};
struct OmnipotentResponse
{
    OperationNumber operation;
    int ID;
    const char *data;
    bool ResponseExpected;
};

class KliveTech
{
public:
    BluetoothSerial SerialBT;
    void CreateKliveTechGadget(const char *name);

    void CallLoop();

    void CreateActionWithIntegerParam(const char *actname, std::function<void(int)> func, const char *paramDescription);
    void CreateActionWithStringParam(const char *actname, std::function<void(const char *)> func, const char *paramDescription);
    void CreateActionWithBoolParam(const char *actname, std::function<void(bool)> func, const char *paramDescription);

private:
    vector<Actions> possibleActions;

    std::string startCommand = "{startComm}";
    std::string endCommand = "{endComm}";

    void SendData(const char *data);
    const char *FormulateResponse(int ID, JsonObject obj, const char *status, bool isResponseExpected);
    OmnipotentResponse DeserializeResponse(const char *response);
    bool hasEnding(std::string const &fullString, std::string const &ending);
    const char *ReadCommand();
    inline const char *const BoolToString(bool b)
    {
        return b ? "true" : "false";
    }
};

#endif