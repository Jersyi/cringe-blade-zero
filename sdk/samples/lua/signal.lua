-- 示例 2：插件信号（Lua 版的 pbz_weapon_actions 的一小部分）
--
-- 把主角具体的武器动作变成一个“按键”：这里是名字里带 _Air_A 的技能（空中轻攻击）。
-- 派生链里把「空中轻攻击（Lua）」用作自定义按键起端，空中轻攻击时就放派生链的招式；
-- 派生链此刻在等这个信号时，取消游戏原本的空中轻攻击（on_hero_ability 返回 true）。
-- 只用派生能力，不申请额外权限。

pbz.Init{
    name = "武器动作信号（Lua）",
    version = "1.0",
    author = "豪之刃",
    description = "空中轻攻击变成信号",
}

pbz.RegisterSignal{ id = "air_light", name = "空中轻攻击（Lua）", group = "武器动作（Lua）", description = "主角放出名字里带 _Air_A 的技能时按下" }

local seen = {}  -- 每种新技能在日志里写一行，方便照着改规则

pbz.OnHeroAbility(function(ability)
    if ability.chain_own then return false end
    if not seen[ability.name] then
        seen[ability.name] = true
        pbz.Log("ability seen: " .. ability.name)
    end
    if string.find(ability.name, "_Air_A", 1, true) and pbz.SignalWanted("air_light") then
        pbz.FireSignal("air_light")
        return true  -- 取消游戏原本的动作，让派生链的招式代替它
    end
    return false
end)
