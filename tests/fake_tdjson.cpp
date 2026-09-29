// A stand-in for tdjson.dll used only by tst_tdruntime. Built three ways:
//   fake_tdjson_ok       reports version 1.8.67 and exports every entry point
//   fake_tdjson_oldver   reports another version
//   fake_tdjson_partial  omits td_send
// It speaks just enough of the JSON interface for the loader's own probes.
#include <cstring>

#ifndef FAKE_TD_VERSION
#define FAKE_TD_VERSION "1.8.67"
#endif

#define EXPORT extern "C" __declspec(dllexport)

EXPORT int td_create_client_id() { return 1; }

#ifndef FAKE_TD_OMIT_SEND
EXPORT void td_send(int, const char*) {}
#endif

EXPORT const char* td_receive(double) { return nullptr; }

EXPORT const char* td_execute(const char* request)
{
    static char reply[128];
    if (request && std::strstr(request, "getOption")) {
        std::strcpy(reply, "{\"@type\":\"optionValueString\",\"value\":\"" FAKE_TD_VERSION "\"}");
        return reply;
    }
    return nullptr;
}

EXPORT void td_set_log_message_callback(int, void (*)(int, const char*)) {}
