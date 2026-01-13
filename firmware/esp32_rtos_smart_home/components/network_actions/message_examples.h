#pragma once

// Message examples for various smart home devices
// These are working configurations that can be copied/modified as needed
//
// TO ADD A NEW EXAMPLE: Just add one line to MESSAGE_EXAMPLE_LIST below
// Format: EXAMPLE(NAME, json string)

namespace MessageExamples {

// ============================================================================
// X-Macro List - Define each example ONCE
// ============================================================================
#define MESSAGE_EXAMPLE_LIST \
    /* Philips Hue Bridge (10.0.0.101) - HTTP REST API */ \
    /* Living Room (Group 84) */ \
    EXAMPLE(HUE_LIVING_ROOM_OFF, R"([{"name":"Living Room Off","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/84/action","method":"PUT","body":"{\"on\":false}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(HUE_LIVING_ROOM_ON, R"([{"name":"Living Room On","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/84/action","method":"PUT","body":"{\"on\":true}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(HUE_LIVING_ROOM_DIMMED, R"([{"name":"Living Room Dimmed","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/84/action","method":"PUT","body":"{\"scene\":\"7D-uDH-nPx-UAXWq\"}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(HUE_LIVING_ROOM_COOL_BRIGHT, R"([{"name":"Living Room Cool Bright","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/84/action","method":"PUT","body":"{\"scene\":\"htWF1mlooiTmMn5x\"}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(HUE_LIVING_ROOM_WARM_BRIGHT, R"([{"name":"Living Room Warm Bright","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/84/action","method":"PUT","body":"{\"scene\":\"eopN9qL7LrGVran2\"}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(HUE_LIVING_ROOM_NIGHTLIGHT, R"([{"name":"Living Room Nightlight","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/84/action","method":"PUT","body":"{\"scene\":\"WUCLsVrk5kAiP5ZT\"}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    /* Bedroom (Group 83) */ \
    EXAMPLE(HUE_BEDROOM_OFF, R"([{"name":"Bedroom Off","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/83/action","method":"PUT","body":"{\"on\":false}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(HUE_BEDROOM_ON, R"([{"name":"Bedroom On","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/83/action","method":"PUT","body":"{\"on\":true}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(HUE_BEDROOM_BRIGHT, R"([{"name":"Bedroom Bright","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/83/action","method":"PUT","body":"{\"scene\":\"PxXknmpgC1mhLxKN\"}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(HUE_BEDROOM_DIMMED, R"([{"name":"Bedroom Dimmed","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/83/action","method":"PUT","body":"{\"scene\":\"uKqYX1Z4mXE6co5o\"}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(HUE_BEDROOM_NIGHTLIGHT, R"([{"name":"Bedroom Nightlight","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/83/action","method":"PUT","body":"{\"scene\":\"EE2RDN4XrzSvAPXF\"}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(HUE_BEDROOM_COOL_BRIGHT, R"([{"name":"Bedroom Cool Bright","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/83/action","method":"PUT","body":"{\"scene\":\"UpWdmp4QNmBLELpl\"}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    /* Bathroom (Group 81) */ \
    EXAMPLE(HUE_BATHROOM_OFF, R"([{"name":"Bathroom Off","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/81/action","method":"PUT","body":"{\"on\":false}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(HUE_BATHROOM_ON, R"([{"name":"Bathroom On","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/81/action","method":"PUT","body":"{\"on\":true}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(HUE_BATHROOM_DIMMED, R"([{"name":"Bathroom Dimmed","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/81/action","method":"PUT","body":"{\"scene\":\"YASLkUGp8VLu1f31\"}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(HUE_BATHROOM_NIGHTLIGHT, R"([{"name":"Bathroom Nightlight","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/81/action","method":"PUT","body":"{\"scene\":\"iUH35iqqQRHIRVWb\"}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(HUE_BATHROOM_COOL_BRIGHT, R"([{"name":"Bathroom Cool Bright","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/81/action","method":"PUT","body":"{\"scene\":\"ob5uV3HFaSxTMdj1\"}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    /* Kitchen/Fridge (Group 82) */ \
    EXAMPLE(HUE_FRIDGE_OFF, R"([{"name":"Fridge Off","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/82/action","method":"PUT","body":"{\"on\":false}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(HUE_FRIDGE_ON, R"([{"name":"Fridge On","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/82/action","method":"PUT","body":"{\"on\":true}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(HUE_FRIDGE_ENERGIZE, R"([{"name":"Fridge Energize","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/82/action","method":"PUT","body":"{\"scene\":\"wdIDZsY7FW8h8NFi\"}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(HUE_FRIDGE_NIGHTLIGHT, R"([{"name":"Fridge Nightlight","url":"http://10.0.0.101:80/api/3XQGgYfdSPuErtNKtA3iRYKLxP3etEF6QPOMRnct/groups/82/action","method":"PUT","body":"{\"scene\":\"aPnYT85mZErseE15\"}","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    /* Dwelo Hub (10.0.0.183:8080) - HTTP POST API */ \
    EXAMPLE(DWELO_KITCHEN_ON, R"([{"name":"Kitchen On","url":"http://10.0.0.183:8080/dwelo/kitchen/on","method":"POST","body":" ","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(DWELO_KITCHEN_OFF, R"([{"name":"Kitchen Off","url":"http://10.0.0.183:8080/dwelo/kitchen/off","method":"POST","body":" ","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(DWELO_ENTRYWAY_ON, R"([{"name":"Entryway On","url":"http://10.0.0.183:8080/dwelo/entryway/on","method":"POST","body":" ","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(DWELO_ENTRYWAY_OFF, R"([{"name":"Entryway Off","url":"http://10.0.0.183:8080/dwelo/entryway/off","method":"POST","body":" ","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(DWELO_LOCK_FRONT_DOOR, R"([{"name":"Lock Front Door","url":"http://10.0.0.183:8080/dwelo/frontdoor/lock","method":"POST","body":" ","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    EXAMPLE(DWELO_UNLOCK_FRONT_DOOR, R"([{"name":"Unlock Front Door","url":"http://10.0.0.183:8080/dwelo/frontdoor/unlock","method":"POST","body":" ","headers":["Content-Type: application/json"],"timeout_ms":10000}])") \
    /* Shelly Relays - HTTP GET API */ \
    EXAMPLE(SHELLY_STUDY_LAMP_ON, R"([{"name":"Study Lamp On","url":"http://10.0.0.45:80/relay/0?turn=on","method":"GET","body":"","headers":[],"timeout_ms":10000}])") \
    EXAMPLE(SHELLY_STUDY_LAMP_OFF, R"([{"name":"Study Lamp Off","url":"http://10.0.0.45:80/relay/0?turn=off","method":"GET","body":"","headers":[],"timeout_ms":10000}])") \
    EXAMPLE(SHELLY_DESK_ELECTRONICS_ON, R"([{"name":"Desk Electronics On","url":"http://10.0.0.239:80/relay/0?turn=on","method":"GET","body":"","headers":[],"timeout_ms":10000}])") \
    EXAMPLE(SHELLY_DESK_ELECTRONICS_OFF, R"([{"name":"Desk Electronics Off","url":"http://10.0.0.239:80/relay/0?turn=off","method":"GET","body":"","headers":[],"timeout_ms":10000}])") \
    /* Atmosphere (10.0.0.156) - JSON-RPC 2.0 */ \
    EXAMPLE(AZM_ZONE_MUTE_TCP, R"([{"name":"Zone Mute","host":"10.0.0.156","port":5321,"data":"{\"jsonrpc\":\"2.0\",\"method\":\"set\",\"params\":{\"param\":\"ZoneMute_0\",\"val\":1},\"id\":1}\n\n","timeout_ms":5000}])") \
    EXAMPLE(AZM_ZONE_UNMUTE_TCP, R"([{"name":"Zone Unmute","host":"10.0.0.156","port":5321,"data":"{\"jsonrpc\":\"2.0\",\"method\":\"set\",\"params\":{\"param\":\"ZoneMute_0\",\"val\":0},\"id\":1}\n\n","timeout_ms":5000}])") \
    EXAMPLE(AZM_MSG_DOOR_DIRTY_TCP, R"([{"name":"Shut The Front Door NOT Dirty","host":"10.0.0.156","port":5321,"data":"{\"jsonrpc\":\"2.0\",\"method\":\"set\",\"params\":{\"param\":\"PlayMessage_3\",\"val\":1},\"id\":1}\n\n","timeout_ms":5000}])") \
    EXAMPLE(AZM_MSG_DOOR_MARK_TCP, R"([{"name":"Shut The Front Door Mark Ruffalo","host":"10.0.0.156","port":5321,"data":"{\"jsonrpc\":\"2.0\",\"method\":\"set\",\"params\":{\"param\":\"PlayMessage_2\",\"val\":1},\"id\":1}\n\n","timeout_ms":5000}])") \
    EXAMPLE(AZM_MSG_DOOR_CLOSE_COMEDIAN_TCP, R"([{"name":"Door Closed Comedian","host":"10.0.0.156","port":5321,"data":"{\"jsonrpc\":\"2.0\",\"method\":\"set\",\"params\":{\"param\":\"PlayMessage_4\",\"val\":1},\"id\":1}\n\n","timeout_ms":5000}])") \
    EXAMPLE(AZM_BEDROOM_BT_WS, R"([{"name":"Hit the Bluetooth Button","url":"ws://10.0.0.156:1235","message":"{\"jsonrpc\":\"2.0\",\"method\":\"set\",\"params\":{\"obj\":\"Control Wallplate\",\"param\":\"Bluetooth Button\",\"device\":3,\"val\":1},\"id\":1}\n\n","timeout_ms":10000}])") \
    EXAMPLE(AZM_PEGASUS_BT_WS, R"([{"name":"Activate Pegasus BT Source","url":"ws://10.0.0.156:1235","message":"{\"jsonrpc\":\"2.0\",\"method\":\"set\",\"params\":{\"obj\":\"Control Wallplate\",\"param\":\"Bluetooth Button\",\"device\":2,\"val\":1},\"id\":1}\n\n","timeout_ms":10000}])")

    // Generate constexpr constants from the macro list
#define EXAMPLE(name, value) constexpr const char* name = value;
MESSAGE_EXAMPLE_LIST
#undef EXAMPLE

// Generate array of all examples from the macro list
inline constexpr const char* const ALL_EXAMPLES[] = {
#define EXAMPLE(name, value) name,
    MESSAGE_EXAMPLE_LIST
#undef EXAMPLE
};

inline constexpr size_t ALL_EXAMPLES_COUNT = sizeof(ALL_EXAMPLES) / sizeof(ALL_EXAMPLES[0]);

} // namespace MessageExamples
