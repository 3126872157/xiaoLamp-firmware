// Production tool table with the fixture NVS; never touches a physical device.
#include <cassert>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include "lamp_protocol.h"
#include "lamp_tool_table.h"
#include "settings.h"

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
static Json Parse(const std::string& text) {
    return Json(cJSON_ParseWithOpts(text.c_str(), nullptr, true), cJSON_Delete);
}
static std::string Table(const std::string& tools, const std::string& extra = "") {
    return "{\"protocol_version\":\"lamp-tools/1\"" + extra + ",\"tools\":[" + tools + "]}";
}
static std::string Tool(const std::string& name, const std::string& description,
                        const std::string& schema, const std::string& extra = "") {
    return "{\"name\":\"" + name + "\",\"description\":\"" + description + "\"" + extra +
           ",\"schema\":" + schema + "}";
}
static std::set<std::string> Names(const std::string& tools) {
    auto parsed = Parse(tools);
    assert(cJSON_IsArray(parsed.get()));
    std::set<std::string> names;
    for (auto* tool = parsed->child; tool; tool = tool->next)
        names.insert(lamp::Text(cJSON_GetObjectItemCaseSensitive(tool, "function"), "name"));
    return names;
}
static std::string Description(const std::string& tools, const std::string& name) {
    auto parsed = Parse(tools);
    for (auto* tool = parsed->child; tool; tool = tool->next) {
        auto* function = cJSON_GetObjectItemCaseSensitive(tool, "function");
        if (lamp::Text(function, "name") == name)
            return lamp::Text(function, "description");
    }
    return "";
}
static void Rejects(const std::string& table) {
    std::string error;
    assert(!lamp::ToolTableValidate(table, error));
    assert(!error.empty());
}
static void Rejects(const std::string& table, const std::string& expected) {
    std::string error;
    if (lamp::ToolTableValidate(table, error) || error.find(expected) == std::string::npos) {
        std::cerr << "expected '" << expected << "' but got '" << error << "' for "
                  << table.substr(0, 240) << std::endl;
        assert(false);
    }
}
static void Accepts(const std::string& table) {
    std::string error;
    assert(lamp::ToolTableValidate(table, error));
    assert(error.empty());
}

int main() {
    Settings::data.clear();
    lamp::ToolTableReset();
    const auto defaults = lamp::ToolTableTools();
    auto names = Names(defaults);
    assert(names.size() == 10);
    assert(names.count("lamp_get_state") && names.count("lamp_end_conversation") &&
           names.count("lamp_rest") && names.count("lamp_set_mode") && names.count("lamp_stop") &&
           names.count("lamp_get_task") && names.count("lamp_set_torque") &&
           names.count("lamp_record_calibration") && names.count("lamp_express") &&
           names.count("lamp_run_sequence"));
    assert(!names.count("lamp_prepare_expression"));
    assert(!Description(defaults, "lamp_rest").empty());
    assert(lamp::ToolTableOperation("lamp_stop") == "cancel");
    assert(lamp::ToolTableOperation("lamp_set_torque") == "set_torque");
    assert(lamp::ToolTableOperation("lamp_prepare_expression") == "execute_group");
    assert(lamp::ToolTableOperation("lamp_unknown").empty());
    assert(lamp::ToolTableArgsTemplate("lamp_run_sequence").empty());
    auto status = lamp::ToolTableStatus();
    assert(!status.custom && status.tools == 10 && status.added == 0 && status.error.empty());
    assert(lamp::ToolTableJson().empty());
    assert(lamp::ToolTableInstructions().empty());

    const std::string empty_schema = R"({"type":"object","properties":{}})",
                      closed_schema =
                          R"({"type":"object","additionalProperties":false,"properties":{}})";
    const std::string nod =
        Tool("lamp_nod_twice", "点两下头", closed_schema,
             R"(,"operation":"execute_group","args":{"steps":[{"action_id":"execute_primitive","skill_id":"A01","options":{"repetitions":2}}]})");
    const std::string where = Tool("lamp_where", "查询缓存状态", closed_schema,
                                   R"(,"operation":"get_state")");

    // Rejected tables keep the compiled capability set.
    Rejects(R"({"protocol_version":"lamp-tools/2","tools":[]})", "protocol_version");
    Rejects(R"({"protocol_version":"lamp-tools/1","extra":1})", "unsupported keys");
    Rejects(R"({"protocol_version":"lamp-tools/1","instructions":"","tools":[]})", "instructions");
    Rejects(R"({"protocol_version":"lamp-tools/1","tools":{}})", "tools must be an array");
    Rejects(Table(Tool("bad", "x", empty_schema, R"(,"operation":"get_state")")), "lamp_ followed");
    Rejects(Table(Tool("lamp_stop", "x", empty_schema, R"(,"operation":"execute_group")")),
            "cannot change its operation");
    Rejects(Table(Tool("lamp_stop", "x", empty_schema, R"(,"args":{"task_id":"abc"})")),
            "cannot carry an args template");
    Rejects(Table(Tool("lamp_prepare_expression", "x", empty_schema)), "Only advertised");
    Rejects(Table(Tool("lamp_new", "x", empty_schema, R"(,"operation":"reboot")")),
            "compiled operations");
    Rejects(Table(Tool("lamp_new", "x", R"({"type":"array"})", R"(,"operation":"get_state")")),
            "must be object");
    Rejects(Table(Tool("lamp_new", "x", empty_schema, R"(,"operation":"")")), "compiled operations");
    Rejects(Table(Tool("lamp_new", "x", R"({"type":"object","properties":{"n":{"type":"integer"}}})",
                       R"(,"operation":"get_task")")),
            "minimum and maximum");
    Rejects(Table(Tool(
                "lamp_new", "x",
                R"({"type":"object","properties":{"n":{"type":"string","enum":[],"maxLength":4}}})",
                R"(,"operation":"get_state")")),
            "one to sixteen");
    Rejects(Table(Tool("lamp_new", "x",
                       R"({"type":"object","additionalProperties":true,"properties":{}})",
                       R"(,"operation":"get_state")")),
            "additional properties");
    Rejects(Table(Tool(
                "lamp_new", "x",
                R"({"type":"object","properties":{"n":{"type":"integer","minimum":0,"maximum":99999}}})",
                R"(,"operation":"get_state")")),
            "too wide");
    Rejects(Table(Tool("lamp_new", "x", empty_schema,
                       R"(,"operation":"execute_group","args":{"context":{"source":"dialogue"}})")),
            "context");
    Rejects(Table(Tool(
                "lamp_new", "x",
                R"({"type":"object","properties":{"steps":{"type":"string","maxLength":8}}})",
                R"(,"operation":"execute_group","args":{"steps":[{"action_id":"execute_primitive","skill_id":"A01"}]})")),
            "must not fix a schema property");
    Rejects(Table(Tool("lamp_new", "x", empty_schema,
                       R"(,"operation":"execute_group","args":{"steps":[{"action_id":"execute_primitive","skill_id":"Z99"}]})")),
            "");
    Rejects(Table(Tool("lamp_new", "x", empty_schema,
                       R"(,"operation":"cancel","args":{"task_id":"bad\tid"})")),
            "task_id");
    Rejects(Table(Tool("lamp_new", std::string(400, 'x'), empty_schema,
                       R"(,"operation":"get_state")")),
            "description");
    std::string many;
    for (int i = 0; i < 9; ++i)
        many += (i ? "," : "") + Tool("lamp_extra_" + std::to_string(i), "x", empty_schema,
                                      R"(,"operation":"get_state")");
    Rejects(Table(many), "too many tools");
    std::string wide;
    for (int i = 0; i < 8; ++i)
        wide += (i ? "," : "") + Tool("lamp_wide_" + std::to_string(i), std::string(300, 'x'),
                                      empty_schema, R"(,"operation":"get_state")");
    Rejects(Table(wide, ",\"instructions\":\"" + std::string(1400, 'y') + "\""), "Table exceeds");
    Rejects(Table(nod + "," + nod), "unique");

    const auto stored =
        Table(nod + "," + where + "," + Tool("lamp_rest", "改过的描述", empty_schema),
              ",\"instructions\":\"你是被存储表覆盖的台灯\"");
    Accepts(stored);
    std::string error;
    assert(lamp::ToolTableSave(stored, error));
    assert(error.empty());
    assert(lamp::ToolTableJson() == stored);
    status = lamp::ToolTableStatus();
    assert(status.custom && status.tools == 12 && status.added == 2 && status.error.empty());
    assert(status.custom_instructions);
    const auto advertised = lamp::ToolTableTools();
    names = Names(advertised);
    assert(names.size() == 12 && names.count("lamp_nod_twice") && names.count("lamp_where"));
    assert(Description(advertised, "lamp_rest") == "改过的描述");
    assert(Description(advertised, "lamp_nod_twice") == "点两下头");
    assert(Description(advertised, "lamp_stop") == Description(defaults, "lamp_stop"));
    assert(lamp::ToolTableInstructions() == "你是被存储表覆盖的台灯");
    assert(lamp::ToolTableOperation("lamp_nod_twice") == "execute_group");
    assert(lamp::ToolTableOperation("lamp_where") == "get_state");
    assert(lamp::ToolTableOperation("lamp_stop") == "cancel");
    assert(lamp::ToolTableOperation("lamp_set_torque") == "set_torque");
    const auto templated = lamp::ToolTableArgsTemplate("lamp_nod_twice");
    assert(templated.find("A01") != std::string::npos &&
           templated.find("repetitions") != std::string::npos);
    assert(lamp::ToolTableArgsTemplate("lamp_where").empty());
    assert(lamp::ToolTableArgsTemplate("lamp_rest").empty());
    assert(!Settings::data["lamp_tools"]["table_v1"].empty());

    // A stored table survives a reboot, and a broken one falls back to the compiled set.
    lamp::ToolTableReload();
    assert(lamp::ToolTableStatus().custom && lamp::ToolTableStatus().added == 2);
    Settings::data["lamp_tools"]["table_v1"] = R"({"protocol_version":"lamp-tools/9"})";
    lamp::ToolTableReload();
    status = lamp::ToolTableStatus();
    assert(!status.custom && status.added == 0 && status.tools == 10);
    assert(status.error.find("protocol_version") != std::string::npos);
    assert(Names(lamp::ToolTableTools()) == Names(defaults));
    assert(lamp::ToolTableInstructions().empty());
    assert(lamp::ToolTableOperation("lamp_nod_twice").empty());

    assert(lamp::ToolTableSave("", error));
    assert(!lamp::ToolTableStatus().custom);
    assert(lamp::ToolTableTools() == defaults);
    std::cout << "PASS tool table" << std::endl;
    return 0;
}
