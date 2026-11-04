#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>

// How the supplement page presents the catalog (debug_supplement_catalog.generated.h): our own sections and cards,
// not the Blueprint's categories. A card is one function; its options are the catalog items that set it (on / off,
// a level, a target). Every catalog index appears exactly once (tests/supplement_layout_test.cpp). The cards of the
// 播放Seq list (one per level) are generated with the catalog (debug_supplement_playseq.generated.h).
namespace SupplementLayout {
inline constexpr size_t kMaxOptions = 48;  // the longest level's sequence list; the page keeps a 64-bit pending mask
struct Option { const wchar_t* label = nullptr; int16_t item = -1; };
struct Card {
    const wchar_t* title;
    const wchar_t* note;
    bool confirm;    // changes saves, progress or the map: the first click arms, a second click within 3 s runs it
    bool remembers;  // highlights the option last applied this session (switches and levels; not one-off actions)
    std::array<Option, kMaxOptions> options;
};
#include "debug_supplement_playseq.generated.h"
static_assert(kPlaySeqMaxOptions <= kMaxOptions, "a level has more sequences than a card holds");
struct Section { const wchar_t* title; const wchar_t* subtitle; const Card* cards; size_t count; };

inline constexpr Card kCombat[] = {
    {L"不被击中", L"怪物的攻击不再命中主角", false, true, {{{L"开启", 157}, {L"关闭", 158}}}},
    {L"不会死亡", L"生命归零时保留最后一口气", false, true, {{{L"开启", 154}, {L"关闭", 153}}}},
    {L"秒杀", L"每次点击在开与关之间切换", false, false, {{{L"切换", 89}}}},
    {L"无限杀气", L"杀气不再消耗", false, true, {{{L"开启", 65}, {L"关闭", 66}}}},
    {L"无限意识之影", L"意识之影不再消耗", false, true, {{{L"开启", 67}, {L"关闭", 68}}}},
    {L"必定完美格挡", L"“全局”同时作用于所有格挡来源", false, true, {{{L"关闭", 71}, {L"开启", 69}, {L"全局", 70}}}},
    {L"低杀气完美格挡", L"杀气低于所选比例时，格挡必定完美", false, true, {{{L"关闭", 75}, {L"30%", 72}, {L"50%", 73}, {L"70%", 74}}}},
    {L"攻击力", L"每次点击调整一档", false, false, {{{L"减小", 98}, {L"增大", 97}}}},
    {L"精纯杀气", L"提高上限或立即回满", false, false, {{{L"提高上限", 155}, {L"回满", 156}}}},
    {L"游戏难度", L"立即切换当前存档的难度", true, true, {{{L"入阵者", 94}, {L"破局者", 95}, {L"逆天者", 96}}}},
};
inline constexpr Card kMovement[] = {
    {L"飞行", L"自由飞行，穿越地形", false, true, {{{L"开启", 0}, {L"关闭", 1}}}},
    {L"移动速度", L"主角移动速度倍率", false, true, {{{L"默认", 88}, {L"2 倍", 86}, {L"3 倍", 87}}}},
};
inline constexpr Card kDisplay[] = {
    {L"帧率", L"在画面上显示或关闭帧率", false, false, {{{L"切换", 162}}}},
    {L"隐藏界面", L"三种原版隐藏方式；再次点击恢复", false, false, {{{L"全部", 164}, {L"宣传片", 115}, {L"展会", 114}}}},
    {L"相机模式", L"打开游戏的相机模式", false, false, {{{L"打开", 55}}}},
    {L"设置界面", L"打开游戏的设置界面", false, false, {{{L"打开", 165}}}},
    {L"动态模糊", L"", false, true, {{{L"开启", 120}, {L"关闭", 121}}}},
    {L"展会演示模式", L"TGA 演示版的画面设置", false, true, {{{L"开启", 118}, {L"关闭", 119}}}},
    {L"DLSS", L"超分辨率与插帧倍数", false, true, {{{L"关闭", 76}, {L"开启", 77}, {L"超分", 78}, {L"超分+插帧", 79}, {L"插帧 2×", 80}, {L"插帧 3×", 81}, {L"插帧 4×", 82}}}},
    {L"主机画质模式", L"PS5 画质档位（调试版）", false, true, {{{L"性能", 83}, {L"平衡", 84}, {L"质量", 85}}}},
    {L"主机画质设置", L"PS5 画质档位（设置版）", false, true, {{{L"性能", 122}, {L"平衡", 123}, {L"质量", 124}}}},
    {L"魂的发色肤色", L"魂的头发与身体灰阶", false, true, {{{L"黑色", 198}, {L"灰白", 199}, {L"白", 200}, {L"彻底白", 201}}}},
};
inline constexpr Card kAudio[] = {
    {L"音乐", L"", false, true, {{{L"开启", 103}, {L"关闭", 102}}}},
    {L"拼刀声", L"兵刃相交的声音风格", false, true, {{{L"超低沉", 104}, {L"低沉", 105}, {L"默认", 106}, {L"明亮", 107}}}},
    {L"混音", L"按播放设备选择", false, true, {{{L"扬声器", 108}, {L"耳机", 109}, {L"家庭影院", 110}}}},
    {L"声音风格", L"", false, true, {{{L"默认", 111}, {L"风格化", 112}, {L"极致", 113}}}},
};
inline constexpr Card kProgress[] = {
    {L"获取物品", L"直接放入背包", true, false, {{{L"全部武器", 59}, {L"全部药物", 62}, {L"全部饰品", 61}, {L"升级材料", 64}, {L"全部道具", 4}, {L"装备条件", 63}}}},
    {L"天赋", L"", true, false, {{{L"激活全部节点", 50}, {L"获取所需材料", 51}}}},
    {L"意识尘埃", L"一次十万", true, false, {{{L"增加", 47}, {L"减少", 48}}}},
    {L"剩余天数", L"", true, false, {{{L"减少 20 天", 45}, {L"只剩 1 天", 46}}}},
    {L"解锁与激活", L"", true, false, {{{L"装备升级", 49}, {L"镜花水月", 52}, {L"心脉修复", 53}, {L"全部铃铛", 163}}}},
    {L"地图标记", L"在地图上显示收集品与地点", false, false, {{{L"黑石碎片", 125}, {L"琥珀碎片", 126}, {L"记忆之花", 127}, {L"重要宝箱", 128}, {L"钥匙", 129}, {L"挑战地点", 130}, {L"全部探测", 131}}}},
    {L"存档", L"立即存档再读回，或读取服务器存档", true, false, {{{L"存档并读档", 60}, {L"读取服务器存档", 54}}}},
    {L"删除存档", L"删除本机的设置或游戏进度存档", true, false, {{{L"设置存档", 9}, {L"游戏进度存档", 10}}}},
    {L"通关本周目", L"直接完成当前周目", true, false, {{{L"通关", 58}}}},
    {L"角色死亡", L"主角立即死亡", true, false, {{{L"立即死亡", 5}}}},
    {L"新游戏", L"清空整个存档，跳过序章从溪谷开始，沿用当前难度", true, false, {{{L"从溪谷开始", 191}}}},
};
inline constexpr Card kCultivation[] = {
    {L"武器升级", L"背包里的全部武器（含杀气武器）提升所选等级", true, false, {{{L"+1", 169}, {L"+2", 170}, {L"+3", 171}, {L"+5", 172}, {L"+10", 173}, {L"+20", 174}, {L"+30", 175}}}},
    {L"武器回退", L"等级高于 1 的武器回到 1 级，返还材料和尘埃", true, false, {{{L"全部回退", 176}}}},
    {L"武器铸魂", L"", true, false, {{{L"全部铸魂", 177}}}},
    {L"药瓶与饰品", L"已拥有的升到满级；或初始化全部药瓶并给升级材料", true, false, {{{L"已有的升满级", 178}, {L"全部药瓶与材料", 179}}}},
    {L"升级测试材料", L"全部武器、装备条件、材料、饰品与药物，100 个武器回退道具，100 万尘埃", true, false, {{{L"领取", 180}}}},
    {L"黑石与琥珀", L"各加所选数量", true, false, {{{L"1 块", 181}, {L"2 块", 182}, {L"3 块", 183}, {L"5 块", 184}, {L"12 块", 185}}}},
    {L"传送符", L"", true, false, {{{L"100 个", 186}}}},
    {L"饰品槽", L"激活四个加槽天赋", true, false, {{{L"解锁 6 个饰品槽", 187}}}},
    {L"前三关收集物", L"地图 xigu、fujizhilin、lujiacun 的全部收集物", true, false, {{{L"带拾取界面", 188}, {L"不带拾取界面", 189}}}},
    {L"商店", L"解锁全部商品标记", true, false, {{{L"全部商品", 190}}}},
};
inline constexpr Card kWorld[] = {
    {L"重开地图", L"重新载入当前地图", true, false, {{{L"保留位置", 8}, {L"回到起点", 7}}}},
    {L"加载范围", L"世界分区的流送距离（厘米）", false, true, {{{L"6400", 167}, {L"12800", 168}, {L"25600", 33}, {L"51200", 34}, {L"102400", 35}, {L"153600", 36}, {L"204800", 37}}}},
    {L"HLOD", L"远景合批模型", false, true, {{{L"开启", 100}, {L"关闭", 101}}}},
    {L"加载", L"打印当前范围到游戏日志，或加快流送", false, false, {{{L"打印范围", 99}, {L"加快加载", 166}}}},
    {L"铃铛", L"退出全部残破铃铛状态，或取消激活 Bell_LZSJ_009", true, false, {{{L"清空残破状态", 192}, {L"取消激活 LZSJ_009", 193}}}},
};
inline constexpr Card kStory[] = {
    {L"陆三迁 · 一、二", L"", true, false, {{{L"一 · 开门", 15}, {L"一 · 飞过去", 22}, {L"二 · 山洞飞过去", 16}, {L"二 · 有药", 17}, {L"二 · 没药", 18}}}},
    {L"陆三迁 · 三", L"", true, false, {{{L"飞过去", 19}, {L"宗祠 · 喝药", 20}, {L"宗祠 · 不喝药", 21}}}},
    {L"头痛哥", L"", true, false, {{{L"飞过去", 32}, {L"二阶段", 23}}}},
    {L"江湖人士", L"", true, false, {{{L"飞过去", 24}, {L"二阶段", 25}}}},
    {L"柳义", L"", true, false, {{{L"飞过去", 159}, {L"二阶段", 30}, {L"拾取拉杆", 31}}}},
    {L"登顶山道", L"主线", true, false, {{{L"过桥标记", 40}, {L"过桥飞过去", 41}, {L"拾取宗祠钥匙", 42}, {L"开门", 43}, {L"Boss 前飞过去", 44}}}},
    {L"雷当", L"", true, false, {{{L"有真解药", 26}}}},
    {L"快速推进", L"", true, false, {{{L"击杀钩者", 27}, {L"跳过教学", 28}, {L"雨夜左殇 · 马车战", 29}}}},
    {L"游戏模式", L"", true, true, {{{L"普通", 38}, {L"剑阵", 39}}}},
    {L"任务工具", L"", false, false, {{{L"Narrative Quest", 6}, {L"已激活事件标记", 160}}}},
};
inline constexpr Card kSequence[] = {
    {L"可跳过", L"让所有过场都能跳过", false, true, {{{L"全部可跳过", 134}, {L"恢复", 135}}}},
    {L"流送源", L"", false, true, {{{L"节省", 151}, {L"昂贵", 152}, {L"不生成", 132}}}},
    {L"布料重置", L"过场切镜时重置布料", false, false, {{{L"关闭", 133}}}},
    {L"编辑器全屏", L"", false, true, {{{L"开启", 136}, {L"关闭", 137}}}},
    {L"预创建玩家角色", L"序章专用", false, true, {{{L"开启", 138}, {L"关闭", 139}}}},
    {L"全局双实例", L"", false, true, {{{L"开启", 141}, {L"关闭", 140}}}},
    {L"调试信息", L"", false, true, {{{L"精简", 142}, {L"详细", 143}}}},
    {L"懒生成", L"", false, true, {{{L"开启", 144}, {L"关闭", 145}}}},
    {L"保留角色原位置", L"录制飞碟过场时开启", false, true, {{{L"开启", 146}, {L"关闭", 147}}}},
    {L"分帧创建", L"", false, true, {{{L"开启", 148}, {L"关闭", 149}}}},
    {L"重播", L"再播放上一段过场", false, false, {{{L"播放上一个", 150}}}},
};
inline constexpr Card kDiagnostics[] = {
    {L"打印到日志", L"写入游戏日志", false, false, {{{L"全部属性", 2}, {L"当前坐标", 3}, {L"存档信息", 161}}}},
    {L"性能采集", L"", false, false, {{{L"Trace", 90}, {L"相机性能测试", 91}, {L"内存报告", 92}, {L"GPU 分析", 93}}}},
    {L"歌词", L"", false, true, {{{L"开启", 11}, {L"关闭", 12}}}},
    {L"标记广播优化", L"", false, true, {{{L"开启", 13}, {L"关闭", 14}}}},
    {L"交互导航网格检查", L"", false, true, {{{L"开启", 116}, {L"关闭", 117}}}},
    {L"编辑器", L"", false, false, {{{L"事件编辑器", 56}, {L"地图收集编辑器", 57}}}},
    {L"交互调试", L"显示交互调试信息并放宽候选检查", false, true, {{{L"开启", 194}, {L"关闭", 195}}}},
    {L"打印到面板", L"在游戏的打印面板显示", false, false, {{{L"当前交互对象", 196}, {L"主武器战斗属性", 197}}}},
};

inline constexpr Section kSections[] = {
    {L"战斗", L"", kCombat, std::size(kCombat)},
    {L"移动", L"", kMovement, std::size(kMovement)},
    {L"画面与界面", L"", kDisplay, std::size(kDisplay)},
    {L"音频", L"", kAudio, std::size(kAudio)},
    {L"进度与存档", L"会改写存档：点一次后，三秒内再点一次才执行", kProgress, std::size(kProgress)},
    {L"养成", L"会改写存档：点一次后，三秒内再点一次才执行", kCultivation, std::size(kCultivation)},
    {L"地图与加载", L"", kWorld, std::size(kWorld)},
    {L"剧情与事件", L"跳过或推进任务节点，会写入任务进度", kStory, std::size(kStory)},
    {L"过场动画", L"", kSequence, std::size(kSequence)},
    {L"过场播放", L"点两次播放：传送到过场位置，不在这张地图时先载入地图；可搜索序列名", kPlaySeq, std::size(kPlaySeq)},
    {L"诊断与工具", L"", kDiagnostics, std::size(kDiagnostics)},
};
}
