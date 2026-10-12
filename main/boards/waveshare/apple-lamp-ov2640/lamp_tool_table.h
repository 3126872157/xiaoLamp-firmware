#pragma once
#include <string>

// Bounded tool table for the Omni edge runtime.
// The compiled table is the capability anchor: a stored table may rewrite descriptions
// and schemas, and may add new tool names, but only by referring to an operation that
// lamp_tool_ops.h declares at compile time.
namespace lamp {
inline constexpr size_t kMaxTableBytes = 3000;
inline constexpr size_t kMaxInstructionsBytes = 1500;
inline constexpr size_t kMaxDescriptionBytes = 320;
inline constexpr size_t kMaxSchemaBytes = 1200;
inline constexpr int kMaxTableTools = 16;
inline constexpr int kMaxAddedTools = 8;

struct ToolTableState {
    int tools = 0;
    int added = 0;
    bool custom = false;
    bool custom_instructions = false;
    std::string error;
};

// Reads the stored table from NVS once; absent or invalid data keeps the compiled defaults.
void ToolTableLoad();
// Re-reads the stored table, as after a panel update or a fresh boot.
void ToolTableReload();
// Pure validation of a candidate table. An empty error means the table is accepted.
bool ToolTableValidate(const std::string& table, std::string& error);
// Validates, stores and activates a candidate table. An empty table clears the override.
bool ToolTableSave(const std::string& table, std::string& error);
std::string ToolTableTools();
// Stored instructions, or empty when the compiled default still applies.
std::string ToolTableInstructions();
std::string ToolTableJson();
ToolTableState ToolTableStatus();
// Built-in mapping plus stored additions.
std::string ToolTableOperation(const std::string& name);
// Fixed argument template of a stored addition, or empty for built-in tools.
std::string ToolTableArgsTemplate(const std::string& name);
// Back to the compiled defaults, without touching NVS.
void ToolTableReset();
}  // namespace lamp
