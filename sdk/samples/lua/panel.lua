-- 示例 3：读写整个 MOD 的一页面板（Lua 版的 pbz_mod_panel）
--
-- 申请读、写、命令、UI 页四项权限（玩家在「派生插件」列表里逐项点开；没点开前对应的调用返回 nil, -10, "DENIED"）。
-- 授权后画一页：绑定模块属性的控件由宿主自己读写，脚本不用写事件代码；另有一个自己处理的按钮和一块状态文字。

pbz.Init{
    name = "模块面板（Lua）",
    version = "1.0",
    author = "豪之刃",
    description = "用模块接口读写整个 MOD，并在面板上画一页",
    min_revision = 3,
    caps = { "read", "write", "command", "ui" },
    caps_reason = "读写模块属性、订阅事件、在面板上显示一页",
}

local last_event = "（还没有事件）"
local page_up = false

-- 这个 MOD 里有没有这个模块属性 / 动作：页上只放真有的。
local function has(list, module, name)
    local items = list(module)
    if not items then return false end
    for _, item in ipairs(items) do
        if item.name == name then return true end
    end
    return false
end

local function show(text)
    last_event = text
    if page_up then pbz.UiSetText("main", "info", text) end
end

local function try_page()
    if page_up or not pbz.Caps then return end
    local items = {}
    if has(pbz.PropList, "host", "mod_version") then
        items[#items + 1] = { kind = "label", id = "version", label = "MOD 版本", bind = "host.mod_version" }
    end
    if has(pbz.PropList, "combo", "show_toast") then
        items[#items + 1] = { kind = "toggle", id = "toast", label = "派生提示", bind = "combo.show_toast" }
    end
    if has(pbz.PropList, "combo", "chain_hud") then
        items[#items + 1] = { kind = "choice", id = "hud", label = "派生显示", bind = "combo.chain_hud" }
    end
    if has(pbz.ActionList, "npc_look", "restore") then
        items[#items + 1] = { kind = "button", id = "restore", label = "还原主角", bind = "npc_look:restore" }
    end
    items[#items + 1] = { kind = "button", id = "dump", label = "列出模块（写日志）" }
    items[#items + 1] = { kind = "text", id = "info", text = last_event }
    local ok, code = pbz.UiPage{ id = "main", title = "模块面板（Lua）", items = items }
    if ok then page_up = true elseif code ~= -10 then pbz.Log("UiPage refused: " .. tostring(code)) end
end

local function subscribe()
    pbz.Subscribe("npc_look", "look_changed")
    pbz.Subscribe("character", "swap_changed")
    pbz.Subscribe("weapons", "weapon_changed")
end

-- 玩家改了授权：再画页、再订阅。
pbz.OnCaps(function(mask, caps)
    pbz.Log("the player now allows: read=" .. tostring(caps.read) .. " write=" .. tostring(caps.write) .. " ui=" .. tostring(caps.ui))
    subscribe()
    try_page()
end)

pbz.OnUi(function(e)
    if e.item == "dump" then
        for _, m in ipairs(pbz.ModuleList()) do
            pbz.Log("module " .. m.name .. " (" .. m.label .. ", " .. m.thread .. "): " .. m.props .. " props, " .. m.actions .. " actions")
        end
    end
end)

pbz.OnModuleEvent(function(e)
    show(e.module .. "/" .. e.event .. ": " .. e.text)
end)

subscribe()
try_page()
