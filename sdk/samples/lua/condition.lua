-- 示例 1：条件判断节点（Lua 版的 pbz_conditions 的一小部分）
--
-- 放进 version.dll 旁的 PBZPlugins\scripts\，在「招式派生」页打开「派生插件」，脚本随即载入（改完脚本：列表里「重新读取脚本」）。
-- 在派生链里：「+ 插件节点」，选「Lua · 主角血量」这类动作。判断节点按 decide 返回的出口走：0 是第一个出口。
-- 这个脚本只用派生能力（不申请任何额外权限）。

pbz.Init{
    name = "条件判断（Lua）",
    version = "1.0",
    author = "豪之刃",
    description = "主角血量、目标距离、概率：Lua 写的判断节点",
}

-- 主角血量低于 value % 走出口 0，否则出口 1。
pbz.RegisterAction({
    id = "hero_hp", name = "主角血量", group = "条件（Lua）", kind = "decide",
    outputs = { "低于", "不低于" },
    value = { label = "血量低于", unit = "%", min = 1, max = 100, step = 5, default = 30 },
}, function(call)
    local hero = pbz.GetHero()
    if not hero or not hero.valid or hero.health_max <= 0 then return 1 end
    return (hero.health * 100 / hero.health_max < call.value) and 0 or 1
end)

-- 目标在 value 厘米内走出口 0，更远走 1，没有目标走 2。
pbz.RegisterAction({
    id = "target_distance", name = "目标距离", group = "条件（Lua）", kind = "decide",
    outputs = { "在距离内", "更远", "没有目标" },
    value = { label = "距离", unit = "厘米", min = 50, max = 3000, step = 50, default = 400 },
}, function(call)
    local hero = pbz.GetHero()
    if not hero or not hero.has_target then return 2 end
    return (hero.target_distance <= call.value) and 0 or 1
end)

-- 按概率走出口 0。判断节点要没有副作用，所以随机数只在这里用，不改别的东西。
pbz.RegisterAction({
    id = "chance", name = "概率", group = "条件（Lua）", kind = "decide",
    outputs = { "命中", "未命中" },
    value = { label = "命中概率", unit = "%", min = 0, max = 100, step = 5, default = 50 },
}, function(call)
    return (math.random(100) <= call.value) and 0 or 1
end)

-- 一个动作节点：走到这里时在画面右下角提示数值。
pbz.RegisterAction({
    id = "notice", name = "提示", group = "动作（Lua）", kind = "run",
    value = { label = "数值", min = -1000, max = 1000, step = 1, default = 0 },
}, function(call)
    pbz.Notify("Lua 节点 " .. call.node .. "，数值 " .. call.value)
end)
