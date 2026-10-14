---
title: "`.dfe` —— 可复用的发射器"
description: "`.dfe` —— 可复用的发射器"
---

<a id="dfe--a-reusable-emitter"></a>

# `.dfe` —— 可复用的发射器

一个 `.dfe` 装一个 `Emitter` 块,写法和它内联在 `.dfs` 里时一模一样:

```cpp
Emitter(Name="Emitters/E_MoonFlashCard", Root="Plugin.DreamFX")
{
    Settings = { SimTarget = CPU; LocalSpace = true; QualityLevelMask = 31; }

    EmitterUpdate = {
        EmitterState(LifeCycleMode = Self, LoopBehavior = Once, LoopDuration = 0.12);
        SpawnBurst_Instantaneous(SpawnCount = 1);
    }

    ParticleSpawn  = { ... }
    ParticleUpdate = { ... }

    SpriteRenderer Card { ... }
}
```

`QualityLevelMask` 是可扩展性位掩码,它住在发射器的 `FNiagaraPlatformSet` 里而不是顶层。
属性位于结构体内的设置,内部用点号路径寻址;而 DSL 名取叶子,因为那才是作者设置的那一部分。

一个 `.dfe` 单独构建什么都不产(DFX5097)。它的存在是为了被拉进一个系统:

```cpp
Emitter Flash from "../Emitters/E_MoonFlashCard"
{
    EmitterUpdate = {
        EmitterState(LifeCycleMode = Self, LoopBehavior = Once, LoopDuration = 0.08);
    }
}
```

路径先相对引用它的文件解析,再相对每一个 DFX 根解析。扩展名可省略。

<a id="copy-not-inheritance"></a>

## 拷贝,而不是继承

`from` 取的是快照(R3)。之后再改 `.dfe`,已经拷贝过它的系统不会有任何变化,直到那些系统重建;生成出来的资产也不带回指的链接。

这个决定也是反编译出的系统永远自包含的原因:拷贝一旦发生,
「这些里面哪些是覆盖」就无法恢复了,所以导出无法重建 `from`。它是单向的语法糖。

<a id="what-the-override-block-replaces"></a>

## 覆盖块替换什么

合并粒度是块,不是模块:

| 部分 | 合并方式 |
| --- | --- |
| `Settings` | 逐键 —— 名字没有歧义,所以覆盖只替换那一个设置 |
| 每个栈 | 如果宿主声明了就整块替换;否则保留 `.dfe` 的 |
| 具名 `Stage` 栈 | 按名称匹配;覆盖会原地替换该阶段,新名称按声明顺序追加 |
| `Defaults` | 按完整参数名匹配;覆盖会原地替换值,新名称追加,未提及的默认值保留。覆盖必须保持基础参数的类型(DFX3048) |
| 渲染器 | 全有或全无 —— 声明任何一个渲染器就替换掉全部 |

栈之所以整块替换,是因为按模块合并需要一个这门语言没有的模块身份:同一个栈里对同一个模块的两次调用无法区分,所以合并就只能猜,而猜错会静默地把别人的特效重新排序。

渲染器的全有或全无是同一个理由的另一面:它们按声明顺序寻址,所以替换三个里的一个会静默地把另外两个重新编号。

构建和验证会把被引用 `.dfe` 文件的内容纳入源指纹。修改基础文件会让每一个使用它的宿主失效,即便宿主文本没有变化。

<a id="user-parameters"></a>

## 用户参数

一个 `.dfe` 可以读取 `User.*`。宿主系统必须声明这些参数,而检查发生在 `from` 那一行,而不是在 `.dfe` 内部 —— 因为「把它拉进来」这个决定是在那里做的(DFX3043)。

```
NS_Host.dfs(7,5): error DFX3043: '../Emitters/E_Flash' reads user parameters this system does not
declare: User.FlashCount. Add them to the Properties block.
```
