#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define NAPI_VERSION 8
#include "node_api.h"
#include "media.hpp"

namespace {
template <typename Function>
Function hostFunction(const char* name) {
    const auto proc = GetProcAddress(GetModuleHandleW(nullptr), name);
    if (!proc) throw std::runtime_error("Missing Node-API export");
    return reinterpret_cast<Function>(proc);
}

#define NAPI_FUNCTION(name) const auto name = hostFunction<decltype(&::name)>(#name)

void check(napi_status result) {
    if (result != napi_ok) throw std::runtime_error("Node-API call failed");
}

std::string stringValue(napi_env env, napi_value value) {
    NAPI_FUNCTION(napi_get_value_string_utf8);
    size_t length = 0;
    check(napi_get_value_string_utf8(env, value, nullptr, 0, &length));
    std::string result(length + 1, '\0');
    check(napi_get_value_string_utf8(env, value, result.data(), result.size(), &length));
    result.resize(length);
    return result;
}

napi_value decryptDat(napi_env env, napi_callback_info info) {
    try {
        NAPI_FUNCTION(napi_get_cb_info);
        NAPI_FUNCTION(napi_get_value_uint32);
        NAPI_FUNCTION(napi_typeof);
        NAPI_FUNCTION(napi_create_object);
        NAPI_FUNCTION(napi_create_buffer_copy);
        NAPI_FUNCTION(napi_create_string_utf8);
        NAPI_FUNCTION(napi_set_named_property);
        NAPI_FUNCTION(napi_create_uint32);
        NAPI_FUNCTION(napi_get_boolean);
        size_t count = 3;
        napi_value args[3];
        check(napi_get_cb_info(env, info, &count, args, nullptr, nullptr));
        if (count < 2) throw std::runtime_error("Expected DAT path and XOR key");
        uint32_t xorKey;
        check(napi_get_value_uint32(env, args[1], &xorKey));
        if (xorKey > 255) throw std::runtime_error("Invalid XOR key");
        std::string aesKey;
        if (count == 3) {
            napi_valuetype type;
            check(napi_typeof(env, args[2], &type));
            if (type != napi_undefined) aesKey = stringValue(env, args[2]);
        }
        const auto media = wf::decrypt(std::filesystem::u8path(stringValue(env, args[0])),
            static_cast<unsigned char>(xorKey), aesKey);
        napi_value object, value;
        check(napi_create_object(env, &object));
        check(napi_create_buffer_copy(env, media.data.size(), media.data.data(), nullptr, &value));
        check(napi_set_named_property(env, object, "data", value));
        check(napi_create_string_utf8(env, media.ext.c_str(), media.ext.size(), &value));
        check(napi_set_named_property(env, object, "ext", value));
        check(napi_get_boolean(env, media.ext == "wxgf", &value));
        check(napi_set_named_property(env, object, "isWxgf", value));
        const auto number = [&](const char* name, uint32_t input) {
            check(napi_create_uint32(env, input, &value));
            check(napi_set_named_property(env, object, name, value));
        };
        number("version", media.version);
        number("aesSize", media.aesSize);
        number("xorSize", media.xorSize);
        number("rawSize", media.rawSize);
        number("flag", media.flag);
        return object;
    } catch (const std::exception& error) {
        hostFunction<decltype(&napi_throw_error)>("napi_throw_error")(env, nullptr, error.what());
        return nullptr;
    }
}
}

NAPI_MODULE_INIT() {
    try {
        NAPI_FUNCTION(napi_create_function);
        NAPI_FUNCTION(napi_set_named_property);
        napi_value function;
        check(napi_create_function(env, "decryptDatNative", NAPI_AUTO_LENGTH, decryptDat, nullptr, &function));
        check(napi_set_named_property(env, exports, "decryptDatNative", function));
        return exports;
    } catch (const std::exception& error) {
        hostFunction<decltype(&napi_throw_error)>("napi_throw_error")(env, nullptr, error.what());
        return nullptr;
    }
}
